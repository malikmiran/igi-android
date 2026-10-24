// Host-side tests: build synthetic files following the documented layouts and parse them.
#include <cstdio>
#include "../app/src/main/cpp/igi_formats.h"
using namespace igi;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void p32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((v >> (8 * i)) & 0xFF); }
static void p16(Bytes& b, uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
static void pf(Bytes& b, float f) { uint32_t v; std::memcpy(&v, &f, 4); p32(b, v); }
static void pstr(Bytes& b, const char* s) { for (int i = 0; i < 4; i++) b.push_back(s[i]); }

static Bytes ilffHeader(const char* form, uint32_t align = 4) {
    // Observed retail header: "ILFF", file size, alignment (4, or 32 in common.res), 0, form type.
    Bytes b; pstr(b, "ILFF"); p32(b, 0); p32(b, align); p32(b, 0); pstr(b, form); return b;
}

// Adds a chunk like retail files do: header at an aligned offset, link = distance to the next header.
static void addChunk(Bytes& b, const char* id, const Bytes& payload, uint32_t align = 4) {
    while (b.size() % align) b.push_back(0);
    size_t padded = (16 + payload.size() + align - 1) / align * align;
    pstr(b, id); p32(b, (uint32_t)payload.size()); p32(b, align); p32(b, (uint32_t)padded);
    b.insert(b.end(), payload.begin(), payload.end());
    while (b.size() % align) b.push_back(0);
}
static Bytes str(const char* s, bool nul = true) { Bytes v(s, s + std::strlen(s)); if (nul) v.push_back(0); return v; }

static void testRes() {
    Bytes b = ilffHeader("IRES");
    CHECK(b.size() == 20);
    addChunk(b, "NAME", str("sounds/a.wav"));
    addChunk(b, "BODY", str("hello", false));        // odd length -> padded
    addChunk(b, "NAME", str("models/gun.mef"));
    addChunk(b, "BODY", str("abcdefgh", false));
    addChunk(b, "BODY", str("orphan", false));       // BODY without a NAME
    ResArchive r = parseRes(b);
    CHECK(r.ok());
    CHECK(r.entries.size() == 3);
    CHECK(r.entries[0].name == "sounds/a.wav");
    CHECK(r.entries[1].name == "models/gun.mef");
    CHECK(r.entries[2].name.empty());
    Bytes out;
    CHECK(resRead(r, r.entries[0], out));
    CHECK(out.size() == 5 && std::memcmp(out.data(), "hello", 5) == 0);
    CHECK(resRead(r, r.entries[1], out) && out.size() == 8);
    CHECK(formType(b.data(), b.size()) == "IRES");
    Bytes bad = b; bad[0] = 'X';
    CHECK(!parseRes(bad).ok());
    Bytes tiny(3, 0);
    CHECK(!parseRes(tiny).ok());
    Bytes truncated(b.begin(), b.begin() + 50);      // cut mid-chunk: must not crash
    (void)parseRes(truncated);
    Bytes empty = ilffHeader("IRES");
    CHECK(!parseRes(empty).ok());
}

static void testResVariants() {
    // 32-byte aligned archive (common.res): first chunk at offset 32, not 20.
    Bytes a = ilffHeader("IRES", 32);
    addChunk(a, "NAME", str("LOCAL:common/x.mef"), 32);
    addChunk(a, "BODY", str("0123456789", false), 32);
    addChunk(a, "NAME", str("LOCAL:common/y.mef"), 32);
    addChunk(a, "BODY", str("zz", false), 32);
    ResArchive ra = parseRes(a);
    CHECK(ra.ok() && ra.entries.size() == 2);
    CHECK(ra.entries[0].name == "LOCAL:common/x.mef" && ra.entries[0].size == 10);
    CHECK(ra.entries[1].name == "LOCAL:common/y.mef" && ra.entries[1].size == 2);
    // Same archive with the link word zeroed: falls back to alignment arithmetic.
    Bytes z = a;
    for (size_t p = 32; p + 16 <= z.size();) {
        uint32_t sz = rd32(&z[p + 4]), link = rd32(&z[p + 12]);
        for (int i = 0; i < 4; i++) z[p + 12 + i] = 0;
        if (!link) break;
        p += link; (void)sz;
    }
    CHECK(parseRes(z).entries.size() == 2);
    // String table: NAME + CSTR.
    Bytes s = ilffHeader("IRES");
    addChunk(s, "NAME", str("greeting"));
    addChunk(s, "CSTR", str("Hello"));
    addChunk(s, "NAME", str("bye"));
    addChunk(s, "CSTR", str("Goodbye"));
    ResArchive rs = parseRes(s);
    CHECK(rs.ok() && rs.entries.size() == 2);
    CHECK(rs.entries[0].isString && rs.entries[0].name == "greeting");
    Bytes v; CHECK(resRead(rs, rs.entries[1], v) && std::memcmp(v.data(), "Goodbye", 7) == 0);
    // Hostile link words must not hang or crash.
    Bytes h = s; for (int i = 0; i < 4; i++) { h[20 + 12 + i] = 0xFF; }
    (void)parseRes(h);
    Bytes h2 = s; h2[20 + 12] = 0; h2[20 + 13] = 0; h2[20 + 14] = 0; h2[20 + 15] = 0;
    (void)parseRes(h2);
}

static void testTex() {
    // 2x2 RGB565
    Bytes b; p16(b, 7); p16(b, 1); p16(b, 2); p16(b, 2); p16(b, 1); p16(b, 0); p32(b, 0);
    p16(b, 0xF800); p16(b, 0x07E0); p16(b, 0x001F); p16(b, 0xFFFF);
    Image im = decodeTex(b);
    CHECK(im.ok() && im.w == 2 && im.h == 2);
    CHECK(im.rgba[0] == 255 && im.rgba[1] == 0 && im.rgba[2] == 0);        // red
    CHECK(im.rgba[4] == 0 && im.rgba[5] == 255 && im.rgba[6] == 0);        // green
    CHECK(im.rgba[8] == 0 && im.rgba[10] == 255);                          // blue
    CHECK(im.rgba[12] == 255 && im.rgba[13] == 255 && im.rgba[14] == 255); // white
    // 2x1 paletted
    Bytes p; p16(p, 2); p16(p, 4); p16(p, 2); p16(p, 1); p16(p, 1); p16(p, 0); p32(p, 0);
    for (int i = 0; i < 256; i++) p32(p, i == 1 ? 0x000000FF : 0x0000FF00);   // idx1 = R:255
    p.push_back(1); p.push_back(0);
    Image ip = decodeTex(p);
    CHECK(ip.ok() && ip.rgba[0] == 255 && ip.rgba[4] == 0 && ip.rgba[5] == 255);
    Bytes junk(16, 0);
    CHECK(!decodeTex(junk).ok());
}

static void testMef() {
    Bytes b = ilffHeader("OCEM");
    // XTRV: 3 vertices, stride 32. Chunk header: id, size, unknown, link (0 = follow padded size)
    pstr(b, "XTRV"); p32(b, 96); p32(b, 0); p32(b, 0);
    float v[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    for (auto& pos : v) { for (float f : pos) pf(b, f); for (int i = 0; i < 3; i++) pf(b, 0); pf(b, 0); pf(b, 0); }
    pstr(b, "ECFC"); p32(b, 8); p32(b, 0); p32(b, 0);
    p16(b, 0); p16(b, 1); p16(b, 2); p16(b, 0);
    Mesh m = parseMef(b);
    CHECK(m.ok());
    CHECK(m.verts.size() == 3 * 8);
    CHECK(m.indices.size() == 3 && m.indices[2] == 2);
    CHECK(m.verts[8] == 1.0f);
    // out-of-range index rejected
    Bytes bad = b; bad[bad.size() - 6] = 9;
    CHECK(!parseMef(bad).ok());
    // reversed fourcc ("VRTX") is also accepted
    Bytes rev = b; std::memcpy(&rev[20], "VRTX", 4);
    CHECK(parseMef(rev).ok());
    CHECK(!diagnose(b).empty());
}

// Mutation fuzz: corrupt valid files at random and make sure parsers neither crash nor hang.
static void fuzz() {
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    Bytes res = ilffHeader("IRES");
    addChunk(res, "NAME", str("LOCAL:a/b.mef")); addChunk(res, "BODY", str("0123456789", false));
    addChunk(res, "NAME", str("k")); addChunk(res, "CSTR", str("text"));
    Bytes mef = ilffHeader("OCEM");
    pstr(mef, "XTRV"); p32(mef, 96); p32(mef, 0); p32(mef, 0);
    for (int i = 0; i < 24; i++) pf(mef, (float)i);
    pstr(mef, "ECFC"); p32(mef, 8); p32(mef, 0); p32(mef, 0); p16(mef, 0); p16(mef, 1); p16(mef, 2); p16(mef, 0);
    Bytes tex; p16(tex, 2); p16(tex, 4); p16(tex, 2); p16(tex, 2); p16(tex, 1); p16(tex, 0); p32(tex, 0);
    for (int i = 0; i < 256; i++) p32(tex, i);
    for (int i = 1; i <= 4; i++) tex.push_back((uint8_t)i);
    for (int iter = 0; iter < 20000; iter++) {
        for (Bytes* src : {&res, &mef, &tex}) {
            Bytes m = *src;
            int muts = 1 + rnd() % 4;
            for (int k = 0; k < muts; k++) {
                if (m.empty()) break;
                size_t at = rnd() % m.size();
                switch (rnd() % 3) {
                    case 0: m[at] = (uint8_t)rnd(); break;
                    case 1: m[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
                    default: m.resize(at); break;           // truncate
                }
            }
            ResArchive r = parseRes(m);
            for (auto& e : r.entries) { Bytes o; (void)resRead(r, e, o); (void)formType(r.file.data() + (e.offset < r.file.size() ? e.offset : 0), e.size); }
            (void)parseMef(m);
            (void)decodeTex(m);
            (void)diagnose(m);
        }
    }
}

int main() {
    testRes(); testResVariants(); testTex(); testMef(); fuzz();
    std::printf(failures ? "%d FAILURE(S)\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
