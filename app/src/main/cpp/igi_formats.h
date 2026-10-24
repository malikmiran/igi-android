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
};

inline bool isIlff(const Bytes& d) { return d.size() >= kIlffHeader && std::memcmp(d.data(), "ILFF", 4) == 0; }

// Generic walk over 16-byte chunk headers: [fourcc][size][?][link].
// If the link field points forward inside the file it is used, otherwise the next
// header is assumed to follow the payload padded to 4 bytes.
inline std::vector<Chunk> walkChunks(const Bytes& d, size_t start = kIlffHeader, size_t maxChunks = 100000) {
    std::vector<Chunk> out;
    size_t pos = start;
    while (pos + kChunkHeader <= d.size() && out.size() < maxChunks) {
        Chunk c;
        c.id = fourcc(&d[pos]);
        c.size = rd32(&d[pos + 4]);
        c.offset = pos + kChunkHeader;
        if (c.offset > d.size() || c.size > d.size() - c.offset) break;
        out.push_back(c);
        uint32_t link = rd32(&d[pos + 12]);
        size_t next = c.offset + ((c.size + 3u) & ~3u);
        if (link != 0 && pos + (size_t)link >= c.offset && pos + (size_t)link <= d.size()) next = pos + link;
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
        o << " hdr:";
        for (size_t i = 4; i < kIlffHeader; i += 4) o << " " << std::hex << rd32(&d[i]) << std::dec;
        auto ch = walkChunks(d, kIlffHeader, maxChunks);
        for (auto& c : ch) o << "\n  [" << c.id << "] size=" << c.size << " @" << c.offset;
    }
    return o.str();
}

// ----------------------------------------------------------------- RES ----
struct ResEntry {
    uint32_t nameHash = 0, dataOffset = 0, unpackedSize = 0, packedSize = 0, flags = 0, nameOffset = 0;
};

struct ResArchive {
    Bytes file;
    std::vector<ResEntry> entries;
    size_t dataBase = 0;   // file offset of the DATA chunk payload
    std::string error;
    bool ok() const { return error.empty(); }
};

// Layout per research notes: 20-byte ILFF header, then "DIR " chunk header
// (fourcc, size, entry_count) followed by 24-byte entries, then a "DATA" chunk.
inline ResArchive parseRes(Bytes file) {
    ResArchive r;
    r.file = std::move(file);
    const Bytes& d = r.file;
    if (!isIlff(d)) { r.error = "not an ILFF file"; return r; }
    size_t pos = kIlffHeader;
    if (pos + 12 > d.size() || std::memcmp(&d[pos], "DIR ", 4) != 0) {
        r.error = "no DIR chunk at 0x14 (layout differs from notes) - see diagnose()";
        return r;
    }
    uint32_t dirSize = rd32(&d[pos + 4]);
    uint32_t count = rd32(&d[pos + 8]);
    size_t entriesAt = pos + 12;
    if ((uint64_t)count * 24 + entriesAt > d.size()) { r.error = "entry count exceeds file size"; return r; }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* p = &d[entriesAt + (size_t)i * 24];
        ResEntry e;
        e.nameHash = rd32(p); e.dataOffset = rd32(p + 4); e.unpackedSize = rd32(p + 8);
        e.packedSize = rd32(p + 12); e.flags = rd32(p + 16); e.nameOffset = rd32(p + 20);
        r.entries.push_back(e);
    }
    size_t after = pos + 8 + dirSize;                 // size assumed to cover count + entries
    if (after + 8 <= d.size() && std::memcmp(&d[after], "DATA", 4) == 0) r.dataBase = after + 8;
    else {
        size_t guess = entriesAt + (size_t)count * 24;
        for (size_t p = guess; p + 8 <= d.size() && p < guess + 4096; p++)
            if (std::memcmp(&d[p], "DATA", 4) == 0) { r.dataBase = p + 8; break; }
    }
    if (!r.dataBase) r.error = "DATA chunk not found";
    return r;
}

// Returns raw (uncompressed-flag only) bytes of an entry; compressed entries unsupported yet.
inline bool resRead(const ResArchive& r, const ResEntry& e, Bytes& out) {
    if (e.flags != 0) return false;
    size_t off = r.dataBase + e.dataOffset;
    if (off > r.file.size() || e.packedSize > r.file.size() - off) return false;
    out.assign(r.file.begin() + off, r.file.begin() + off + e.packedSize);
    return true;
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

inline bool chunkIs(const Chunk& c, const char* a) {
    std::string rev(a); return c.id == rev || std::string(c.id.rbegin(), c.id.rend()) == rev;
}

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
