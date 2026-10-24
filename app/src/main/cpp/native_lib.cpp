// JNI bridge + minimal GLES3 renderer. Stage 0: scan a game folder, parse RES archives,
// show diagnostics, and render a loaded mesh (or a test triangle) with a touch-orbit camera.
#include <jni.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sys/stat.h>
#include <vector>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <string>
#include "igi_formats.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "IGI", __VA_ARGS__)

namespace {

std::mutex g_mu;
igi::Mesh g_mesh;
bool g_meshDirty = false;
float g_yaw = 0.5f, g_pitch = 0.3f, g_dist = 3.0f;
int g_w = 1, g_h = 1;
GLuint g_prog = 0, g_vbo = 0, g_ibo = 0;
GLsizei g_indexCount = 0, g_vertCount = 0;
GLint g_uMvp = -1;

igi::Bytes readFile(const std::string& path, size_t maxBytes = (size_t)600 << 20) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    igi::Bytes out;
    if (n > 0 && (size_t)n <= maxBytes) {
        out.resize((size_t)n);
        if (std::fread(out.data(), 1, (size_t)n, f) != (size_t)n) out.clear();
    }
    std::fclose(f);
    return out;
}

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, nullptr, log); LOG("shader error: %s", log); }
    return s;
}

void mul(const float* a, const float* b, float* o) {
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) {
        o[c * 4 + r] = 0;
        for (int k = 0; k < 4; k++) o[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
    }
}

void perspective(float fovy, float aspect, float n, float f, float* m) {
    float t = 1.0f / std::tan(fovy / 2);
    for (int i = 0; i < 16; i++) m[i] = 0;
    m[0] = t / aspect; m[5] = t; m[10] = (f + n) / (n - f); m[11] = -1; m[14] = 2 * f * n / (n - f);
}

void lookAtOrbit(float yaw, float pitch, float dist, float* m) {
    float cx = dist * std::cos(pitch) * std::sin(yaw), cy = dist * std::sin(pitch), cz = dist * std::cos(pitch) * std::cos(yaw);
    float fx = -cx, fy = -cy, fz = -cz, fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    fx /= fl; fy /= fl; fz /= fl;
    float sx = fy * 0 - fz * 1, sy = fz * 0 - fx * 0, sz = fx * 1 - fy * 0;   // f x up(0,1,0)
    float sl = std::sqrt(sx * sx + sy * sy + sz * sz); if (sl < 1e-6f) sl = 1;
    sx /= sl; sy /= sl; sz /= sl;
    float ux = sy * fz - sz * fy, uy = sz * fx - sx * fz, uz = sx * fy - sy * fx;
    float v[16] = {sx, ux, -fx, 0, sy, uy, -fy, 0, sz, uz, -fz, 0,
                   -(sx * cx + sy * cy + sz * cz), -(ux * cx + uy * cy + uz * cz), (fx * cx + fy * cy + fz * cz), 1};
    std::memcpy(m, v, sizeof(v));
}

void uploadMesh(const igi::Mesh& m) {
    if (!g_vbo) { glGenBuffers(1, &g_vbo); glGenBuffers(1, &g_ibo); }
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, m.verts.size() * 4, m.verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m.indices.size() * 2, m.indices.data(), GL_STATIC_DRAW);
    g_indexCount = (GLsizei)m.indices.size();
    g_vertCount = (GLsizei)(m.verts.size() / 8);
}

void listRes(const std::string& root, const std::string& rel, int depth, std::vector<std::string>& out) {
    DIR* dp = opendir((root + (rel.empty() ? "" : "/" + rel)).c_str());
    if (!dp) return;
    while (dirent* de = readdir(dp)) {
        std::string n = de->d_name;
        if (n == "." || n == "..") continue;
        std::string r = rel.empty() ? n : rel + "/" + n;
        std::string full = root + "/" + r;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 6) listRes(root, r, depth + 1, out); }
        else {
            std::string l = lower(n);
            if (l.size() > 4 && l.substr(l.size() - 4) == ".res") out.push_back(r);
        }
    }
    closedir(dp);
}

// JNI NewStringUTF aborts the process (CheckJNI is on in debug builds) on invalid modified UTF-8.
// Game text contains Windows-1252 bytes, so reduce everything to printable ASCII.
std::string sanitize(const std::string& in) {
    std::string o;
    o.reserve(in.size());
    for (unsigned char ch : in) o += (ch == '\n' || (ch >= 32 && ch < 127)) ? (char)ch : '?';
    return o;
}

void appendProgress(const std::string& path, const std::string& text, bool truncate = false) {
    FILE* f = std::fopen(path.c_str(), truncate ? "wb" : "ab");
    if (!f) return;
    std::string t = sanitize(text);
    std::fwrite(t.data(), 1, t.size(), f);
    std::fflush(f);
    std::fclose(f);
}

std::string hexSample(const uint8_t* p, size_t n, size_t maxBytes = 40) {
    static const char* H = "0123456789abcdef";
    std::string o;
    for (size_t i = 0; i < n && i < maxBytes; i++) { o += H[p[i] >> 4]; o += H[p[i] & 15]; o += ' '; }
    return o;
}

}  // namespace

extern "C" {

// Scans `dir` recursively for *.res files, parses each, and returns a diagnostic report.
}  // extern "C"

namespace {

std::string scanImpl(const std::string& dir) {
    const std::string progress = dir + "/../scan_progress.txt";
    appendProgress(progress, "", true);
    std::vector<std::string> names;
    listRes(dir, "", 0, names);
    std::sort(names.begin(), names.end());
    if (names.empty()) return "No .res files found in the copied folder.";

    std::string report = std::to_string(names.size()) + " .res archive(s)\n\n";
    std::set<std::string> seenExt, seenForm;
    bool meshLoaded = false;
    int mefFails = 0;
    size_t flushed = 0;
    for (const std::string& name : names) {
        // Persist everything so far, then note which archive is about to be processed, so a crash
        // leaves a trail that the next app start can show.
        appendProgress(progress, report.substr(flushed));
        flushed = report.size();
        appendProgress(progress, "[scanning " + name + "]\n");
        igi::Bytes data = readFile(dir + "/" + name);
        if (data.empty()) { report += name + ": unreadable, empty or over 600MB\n"; continue; }
        size_t mb10 = data.size() / (1024 * 1024 / 10);
        igi::ResArchive r = igi::parseRes(std::move(data));
        report += name + " (" + std::to_string(mb10 / 10) + "." + std::to_string(mb10 % 10) + "MB): ";
        if (!r.ok()) { report += "PARSE FAILED (" + r.error + ")\n  " + igi::diagnose(r.file, 8) + "\n"; continue; }

        int strings = 0;
        std::map<std::string, int> ext, forms;
        for (auto& e : r.entries) {
            if (e.isString) { strings++; continue; }
            size_t dot = e.name.rfind('.');
            ext[dot == std::string::npos ? "(none)" : lower(e.name.substr(dot + 1))]++;
            std::string ft = igi::formType(&r.file[e.offset], e.size);
            if (!ft.empty()) forms[ft]++;
        }
        report += std::to_string(r.entries.size()) + " entries";
        if (strings) report += " (" + std::to_string(strings) + " strings)";
        report += "\n";
        if (!ext.empty()) {
            report += "  ext:";
            for (auto& kv : ext) report += " " + kv.first + "=" + std::to_string(kv.second);
            report += "\n";
        }
        if (!forms.empty()) {
            report += "  forms:";
            for (auto& kv : forms) report += " " + kv.first + "=" + std::to_string(kv.second);
            report += "\n";
        }
        int shown = 0;
        for (auto& e : r.entries) {
            if (shown >= 3) break;
            if (e.isString) {
                igi::Bytes v; igi::resRead(r, e, v);
                std::string val((const char*)v.data(), v.size());
                val.resize(strnlen(val.c_str(), val.size()));
                if (val.size() > 40) val = val.substr(0, 40) + "...";
                report += "  str: " + e.name + " = " + val + "\n";
            } else report += "  e.g. " + e.name + "\n";
            shown++;
        }

        for (auto& e : r.entries) {
            if (e.isString) continue;
            const uint8_t* p = &r.file[e.offset];
            size_t dot = e.name.rfind('.');
            std::string x = dot == std::string::npos ? "(none)" : lower(e.name.substr(dot + 1));
            std::string ft = igi::formType(p, e.size);
            // One header sample per file extension / ILFF form type across all archives.
            if (ft.empty() && seenExt.insert(x).second) {
                report += "  SAMPLE ." + x + " " + e.name + " size=" + std::to_string(e.size) + "\n    " + hexSample(p, e.size) + "\n";
                if (x == "tex") {
                    igi::Bytes blob; igi::resRead(r, e, blob);
                    igi::Image im = igi::decodeTex(blob);
                    report += im.ok() ? "    TEX decode OK " + std::to_string(im.w) + "x" + std::to_string(im.h) + "\n"
                                      : "    TEX decode failed: " + im.error + "\n";
                }
            } else if (!ft.empty() && ft != "OCEM" && seenForm.insert(ft).second) {
                igi::Bytes blob; igi::resRead(r, e, blob);
                report += "  SAMPLE form " + ft + " " + e.name + "\n  " + igi::diagnose(blob, 10) + "\n";
            }
            if (meshLoaded) continue;
            std::string ln = lower(e.name);
            bool isMef = (ln.size() > 4 && ln.substr(ln.size() - 4) == ".mef") || ft == "OCEM";
            if (!isMef) continue;
            igi::Bytes blob;
            if (!igi::resRead(r, e, blob)) continue;
            igi::Mesh m = igi::parseMef(blob);
            if (m.ok() && !m.indices.empty()) {
                std::lock_guard<std::mutex> lk(g_mu);
                g_mesh = std::move(m); g_meshDirty = true; meshLoaded = true;
                report += "  LOADED MESH " + e.name + ": " + std::to_string(g_mesh.verts.size() / 8) + " verts, " +
                          std::to_string(g_mesh.indices.size() / 3) + " tris\n";
            } else if (mefFails < 3) {
                mefFails++;
                report += "  MEF parse failed (" + m.error + "): " + e.name + "\n  " + igi::diagnose(blob, 12) + "\n";
            }
        }
    }
    appendProgress(progress, report.substr(flushed));
    return report;
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL Java_com_igi_port_Native_scan(JNIEnv* env, jclass, jstring jdir) {
    const char* c = env->GetStringUTFChars(jdir, nullptr);
    std::string dir = c; env->ReleaseStringUTFChars(jdir, c);
    std::string report;
    try { report = scanImpl(dir); }
    catch (const std::exception& e) { report = std::string("Scan aborted by exception: ") + e.what(); }
    report = sanitize(report);
    jstring js = env->NewStringUTF(report.c_str());
    appendProgress(dir + "/../scan_progress.txt", "\n[DONE]\n");
    return js;
}

JNIEXPORT void JNICALL Java_com_igi_port_Native_onSurfaceCreated(JNIEnv*, jclass) {
    const char* vs = "#version 300 es\nlayout(location=0) in vec3 p; layout(location=1) in vec3 n; uniform mat4 mvp; out vec3 vn;"
                     "void main(){ vn=n; gl_Position=mvp*vec4(p,1.0); }";
    const char* fs = "#version 300 es\nprecision mediump float; in vec3 vn; out vec4 o;"
                     "void main(){ vec3 n=normalize(vn+vec3(0.001)); float l=0.4+0.6*max(dot(n,normalize(vec3(0.4,0.8,0.5))),0.0); o=vec4(vec3(0.7,0.75,0.65)*l,1.0); }";
    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(g_prog);
    g_uMvp = glGetUniformLocation(g_prog, "mvp");
    glEnable(GL_DEPTH_TEST);
    g_vbo = g_ibo = 0; g_indexCount = 0;
    // Test triangle until a real mesh is loaded.
    igi::Mesh tri;
    tri.verts = {-1, -1, 0, 0, 0, 1, 0, 0, 1, -1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0};
    tri.indices = {0, 1, 2};
    uploadMesh(tri);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_mesh.ok()) g_meshDirty = true;
}

JNIEXPORT void JNICALL Java_com_igi_port_Native_onSurfaceChanged(JNIEnv*, jclass, jint w, jint h) {
    g_w = w > 0 ? w : 1; g_h = h > 0 ? h : 1;
    glViewport(0, 0, g_w, g_h);
}

JNIEXPORT void JNICALL Java_com_igi_port_Native_onDraw(JNIEnv*, jclass) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_meshDirty) {
            uploadMesh(g_mesh); g_meshDirty = false;
            float mx = 1e-3f;
            for (size_t i = 0; i + 2 < g_mesh.verts.size(); i += 8)
                mx = std::fmax(mx, std::fmax(std::fabs(g_mesh.verts[i]), std::fmax(std::fabs(g_mesh.verts[i + 1]), std::fabs(g_mesh.verts[i + 2]))));
            g_dist = mx * 3.0f;
        }
    }
    glClearColor(0.08f, 0.09f, 0.11f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!g_indexCount) return;
    float proj[16], view[16], mvp[16];
    perspective(1.0f, (float)g_w / g_h, g_dist * 0.01f, g_dist * 20.0f, proj);
    lookAtOrbit(g_yaw, g_pitch, g_dist, view);
    mul(proj, view, mvp);
    glUseProgram(g_prog);
    glUniformMatrix4fv(g_uMvp, 1, GL_FALSE, mvp);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void*)12);
    glDisable(GL_CULL_FACE);
    glDrawElements(GL_TRIANGLES, g_indexCount, GL_UNSIGNED_SHORT, nullptr);
}

JNIEXPORT void JNICALL Java_com_igi_port_Native_onDrag(JNIEnv*, jclass, jfloat dx, jfloat dy) {
    g_yaw -= dx * 0.01f;
    g_pitch += dy * 0.01f;
    if (g_pitch > 1.5f) g_pitch = 1.5f;
    if (g_pitch < -1.5f) g_pitch = -1.5f;
}

JNIEXPORT void JNICALL Java_com_igi_port_Native_onZoom(JNIEnv*, jclass, jfloat scale) {
    if (scale > 0.1f && scale < 10.0f) g_dist /= scale;
}

}  // extern "C"
