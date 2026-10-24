// Parsers for Project I.G.I. 1 file formats, written from the public research at
// https://project-igi-research.vercel.app (see ROADMAP.md).
//
// IMPORTANT: layouts here follow those notes, which are partly unverified against
// retail files. Every parser is defensive (bounds-checked, never throws) and the
// diagnostics helpers exist so real files can be inspected and the layouts corrected.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>

namespace igi {

using Bytes = std::vector<uint8_t>;

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline float rdf(const uint8_t* p) { uint32_t v = rd32(p); float f; std::memcpy(&f, &v, 4); return f; }

inline std::string fourcc(const uint8_t* p) {
    std::string s;
    for (int i = 0; i < 4; i++) s += (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.';
    return s;
}

// ---------------------------------------------------------------- ILFF ----
constexpr size_t kIlffHeader = 20;
constexpr size_t kChunkHeader = 16;

struct Chunk {
    std::string id;       // FourCC as stored on disk (IGI stores several reversed, e.g. XTRV, ECFC)
    uint32_t size = 0;    // payload size
    size_t offset = 0;    // payload offset in file
    uint32_t w2 = 0, w3 = 0;  // the two remaining header words (meaning not yet established)
};

inline bool isIlff(const Bytes& d) { return d.size() >= kIlffHeader && std::memcmp(d.data(), "ILFF", 4) == 0; }

// Chunk alignment declared by the ILFF header word at offset 8: 4 in most archives, 32 in common.res.
inline size_t ilffAlign(const Bytes& d) {
    if (d.size() < kIlffHeader) return 4;
    uint32_t a = rd32(&d[8]);
    return (a >= 4 && a <= 4096 && (a & (a - 1)) == 0) ? a : 4;
}
inline size_t alignUp(size_t v, size_t a) { return (v + a - 1) / a * a; }

// Walk 16-byte chunk headers: [fourcc][size][alignment][link]. The first chunk sits at the end of the
// 20-byte header rounded up to the alignment. `link` (observed: distance from this header to the next
// one) is used when it points forward inside the file; otherwise the next header follows the payload
// rounded up to the alignment.
inline std::vector<Chunk> walkChunks(const Bytes& d, size_t start = 0, size_t maxChunks = 100000) {
    std::vector<Chunk> out;
    const size_t al = ilffAlign(d);
    size_t pos = start ? start : alignUp(kIlffHeader, al);
    while (pos + kChunkHeader <= d.size() && out.size() < maxChunks) {
        Chunk c;
        c.id = fourcc(&d[pos]);
        c.size = rd32(&d[pos + 4]);
        c.w2 = rd32(&d[pos + 8]);
        c.w3 = rd32(&d[pos + 12]);
        c.offset = pos + kChunkHeader;
        if (c.offset > d.size() || c.size > d.size() - c.offset) break;
        out.push_back(c);
        size_t next = alignUp(c.offset + c.size, al);
        if (c.w3 >= kChunkHeader && c.w3 <= d.size() - pos) next = pos + c.w3;
        if (next <= pos) break;
        pos = next;
    }
    return out;
}

// Human-readable dump used by the on-device diagnostic screen.
inline std::string diagnose(const Bytes& d, size_t maxChunks = 24) {
    std::ostringstream o;
    o << "size=" << d.size();
    if (d.size() < 4) return o.str() + " (too small)";
    o << " magic=" << fourcc(d.data());
    if (isIlff(d)) {
        o << " align=" << ilffAlign(d) << " hdr:";
        for (size_t i = 4; i < kIlffHeader; i += 4) o << " " << std::hex << rd32(&d[i]) << std::dec;
        auto ch = walkChunks(d, 0, maxChunks);
        for (auto& c : ch) o << "\n  [" << c.id << "] size=" << c.size << " @" << c.offset << " w2=" << std::hex << c.w2 << " w3=" << c.w3 << std::dec;
    }
    return o.str();
}

inline bool chunkIs(const Chunk& c, const char* a) {
    std::string rev(a); return c.id == rev || std::string(c.id.rbegin(), c.id.rend()) == rev;
}

// ----------------------------------------------------------------- RES ----
struct ResEntry {
    std::string name;     // path string from the NAME chunk (trailing NULs stripped)
    size_t offset = 0;    // BODY/CSTR payload offset in the archive file
    uint32_t size = 0;
    bool isString = false;  // true for CSTR entries (localisation/text tables), false for BODY (files)
};

struct ResArchive {
    Bytes file;
    std::vector<ResEntry> entries;
    std::string error;
    bool ok() const { return error.empty(); }
};

// Layout observed in retail archives: 20-byte ILFF header with form type "IRES" at offset 16, then a
// flat sequence of 16-byte-header chunks: NAME (entry path or string key) followed by BODY (file data)
// or CSTR (text value; used by english.res, messages.res, ...), payloads padded to the alignment. This REPLACES the DIR/DATA layout in the research
// notes, which does not match retail files.
inline ResArchive parseRes(Bytes file) {
    ResArchive r;
    r.file = std::move(file);
    const Bytes& d = r.file;
    if (!isIlff(d)) { r.error = "not an ILFF file"; return r; }
    std::string pending;
    bool have = false;
    for (const Chunk& c : walkChunks(d)) {
        if (chunkIs(c, "NAME")) {
            pending.assign((const char*)&d[c.offset], c.size);
            size_t z = pending.find('\0');
            if (z != std::string::npos) pending.resize(z);
            have = true;
        } else if (chunkIs(c, "BODY") || chunkIs(c, "CSTR")) {
            ResEntry e;
            if (have) e.name = pending;
            e.offset = c.offset; e.size = c.size;
            e.isString = chunkIs(c, "CSTR");
            r.entries.push_back(std::move(e));
            have = false;
        }
    }
    if (r.entries.empty()) r.error = "no NAME/BODY/CSTR chunks found";
    return r;
}

inline bool resRead(const ResArchive& r, const ResEntry& e, Bytes& out) {
    if (e.offset > r.file.size() || e.size > r.file.size() - e.offset) return false;
    out.assign(r.file.begin() + e.offset, r.file.begin() + e.offset + e.size);
    return true;
}

// Form type (bytes 16..19) of an ILFF blob, or "" if it is not ILFF.
inline std::string formType(const uint8_t* p, size_t n) {
    if (n < kIlffHeader || std::memcmp(p, "ILFF", 4) != 0) return "";
    return fourcc(p + 16);
}

// ----------------------------------------------------------------- TEX ----
struct Image { int w = 0, h = 0; std::vector<uint8_t> rgba; std::string error; bool ok() const { return error.empty() && w > 0; } };

// 16-byte header: version, format(1=565,2=1555,3=4444,4=8-bit paletted), w, h, mips, flags, reserved.
inline Image decodeTex(const Bytes& d) {
    Image im;
    if (d.size() < 16) { im.error = "too small"; return im; }
    uint16_t ver = rd16(&d[0]), fmt = rd16(&d[2]), w = rd16(&d[4]), h = rd16(&d[6]);
    if ((ver != 2 && ver != 7) || w == 0 || h == 0 || w > 4096 || h > 4096) { im.error = "unrecognised TEX header"; return im; }
    size_t pos = 16;
    std::vector<uint32_t> pal;
    if (fmt == 4) {
        if (d.size() < pos + 1024) { im.error = "palette truncated"; return im; }
        for (int i = 0; i < 256; i++) pal.push_back(rd32(&d[pos + i * 4]));
        pos += 1024;
    }
    size_t bpp = (fmt == 4) ? 1 : 2;
    if (fmt < 1 || fmt > 4) { im.error = "unknown pixel format"; return im; }
    if (d.size() < pos + (size_t)w * h * bpp) { im.error = "pixel data truncated"; return im; }
    im.w = w; im.h = h; im.rgba.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t* o = &im.rgba[i * 4];
        if (fmt == 4) {
            uint32_t c = pal[d[pos + i]];
            o[0] = c & 0xFF; o[1] = (c >> 8) & 0xFF; o[2] = (c >> 16) & 0xFF; o[3] = 255;
        } else {
            uint16_t v = rd16(&d[pos + i * 2]);
            if (fmt == 1) { o[0] = ((v >> 11) & 31) * 255 / 31; o[1] = ((v >> 5) & 63) * 255 / 63; o[2] = (v & 31) * 255 / 31; o[3] = 255; }
            else if (fmt == 2) { o[3] = (v & 0x8000) ? 255 : 0; o[0] = ((v >> 10) & 31) * 255 / 31; o[1] = ((v >> 5) & 31) * 255 / 31; o[2] = (v & 31) * 255 / 31; }
            else { o[3] = ((v >> 12) & 15) * 17; o[0] = ((v >> 8) & 15) * 17; o[1] = ((v >> 4) & 15) * 17; o[2] = (v & 15) * 17; }
        }
    }
    return im;
}

// ----------------------------------------------------------------- MEF ----
struct Mesh {
    std::vector<float> verts;       // interleaved: pos(3) normal(3) uv(2)
    std::vector<uint16_t> indices;  // triangle list
    std::string error;
    bool ok() const { return error.empty() && !verts.empty(); }
};


// Reads XTRV (vertices; 32-byte stride for type 0, 40 for types 1/3) and ECFC (8-byte face records,
// first three uint16 = triangle indices). Stride is inferred from the payload size and the face indices.
inline Mesh parseMef(const Bytes& d) {
    Mesh m;
    if (!isIlff(d)) { m.error = "not ILFF"; return m; }
    auto chunks = walkChunks(d);
    const Chunk *vx = nullptr, *fc = nullptr;
    for (auto& c : chunks) { if (chunkIs(c, "XTRV")) vx = &c; else if (chunkIs(c, "ECFC")) fc = &c; }
    if (!vx) { m.error = "no XTRV chunk"; return m; }
    size_t faceCount = fc ? fc->size / 8 : 0;
    uint32_t maxIdx = 0;
    for (size_t i = 0; i < faceCount; i++)
        for (int k = 0; k < 3; k++) maxIdx = std::max<uint32_t>(maxIdx, rd16(&d[fc->offset + i * 8 + k * 2]));
    size_t stride = 32;
    if (vx->size % 32 != 0 && vx->size % 40 == 0) stride = 40;
    else if (vx->size % 32 == 0 && vx->size % 40 == 0) stride = (maxIdx < vx->size / 40) ? 40 : 32;
    if (vx->size % stride != 0) { m.error = "XTRV size not a multiple of a known stride"; return m; }
    size_t n = vx->size / stride;
    for (size_t i = 0; i < n; i++) {
        const uint8_t* p = &d[vx->offset + i * stride];
        for (int k = 0; k < 8; k++) m.verts.push_back(rdf(p + k * 4));
    }
    for (size_t i = 0; i < faceCount; i++)
        for (int k = 0; k < 3; k++) {
            uint16_t ix = rd16(&d[fc->offset + i * 8 + k * 2]);
            if (ix >= n) { m.error = "face index out of range"; m.indices.clear(); return m; }
            m.indices.push_back(ix);
        }
    return m;
}

}  // namespace igi
