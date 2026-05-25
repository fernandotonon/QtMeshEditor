#include "PsxTmdRamScanner.h"

#include "CapturedModelMesh.h"
#include "EmuHooks.h"
#include "MeshReconstructor.h"
#include "PsxModelRamScanCommon.h"
#include "ReconstructedMesh.h"

#include <QHash>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace {

using PsxModelRamScan::fnv1a64;
using PsxModelRamScan::readI16le;
using PsxModelRamScan::readU16le;
using PsxModelRamScan::readU32le;

constexpr uint32_t kTmdMagic = 0x00000041u;
constexpr size_t kTmdHeaderSize = 12u;
constexpr size_t kObjHeaderSize = 28u;
constexpr int kMaxTmdsPerFrame = 16;
constexpr uint32_t kMaxObjects = 256u;
constexpr uint32_t kMaxVerts = 8192u;
constexpr uint32_t kMaxPrims = 8192u;
constexpr size_t kScanStrideBytes = 4u;

/** Scale factor: PSX 12.4 fixed (1/4096) × editor uniform scale (10.0). Mirrors
 *  `PS1TMD::kDefaultOgreUnitsPerTmdStep * PS1TMD::kTmdEditorUniformScale`. We use a local
 *  constant to keep this scanner free of the Ogre-heavy PS1TMD.h dependency. */
constexpr float kPosScale = 10.0f / 4096.0f;

struct ObjectHeader {
    uint32_t vertOff = 0;
    uint32_t nVert = 0;
    uint32_t normOff = 0;
    uint32_t nNorm = 0;
    uint32_t primOff = 0;
    uint32_t nPrim = 0;
};

struct ResolvedRange {
    size_t base = 0;
    bool ok = false;
};

/** Resolve a TMD object-header offset to an absolute RAM offset.
 *  - flags & 1 == 0 (FIX_P off): offsets are relative to the TMD start + 12 (file form).
 *  - flags & 1 == 1 (FIX_P on):  offsets are KSEG0 RAM pointers; mask 0x001FFFFF. */
ResolvedRange resolveOffset(uint32_t storedOff, uint32_t flags, size_t tmdStart, size_t ramSize,
                            size_t payloadBytes)
{
    ResolvedRange out;
    size_t base;
    if ((flags & 1u) == 0u) {
        base = tmdStart + kTmdHeaderSize + storedOff;
    } else {
        const uint32_t masked = storedOff & 0x001FFFFFu;
        base = masked;
    }
    if (base >= ramSize)
        return out;
    if (payloadBytes > 0 && base + payloadBytes > ramSize)
        return out;
    out.base = base;
    out.ok = true;
    return out;
}

bool readObjectHeaders(const uint8_t *ram, size_t ramSize, size_t tmdStart, uint32_t numObj,
                      QVector<ObjectHeader> &out)
{
    out.clear();
    out.reserve(static_cast<int>(numObj));
    const size_t tableStart = tmdStart + kTmdHeaderSize;
    if (tableStart + size_t(numObj) * kObjHeaderSize > ramSize)
        return false;
    for (uint32_t i = 0; i < numObj; ++i) {
        const uint8_t *oh = ram + tableStart + size_t(i) * kObjHeaderSize;
        ObjectHeader h;
        h.vertOff = readU32le(oh + 0);
        h.nVert = readU32le(oh + 4);
        h.normOff = readU32le(oh + 8);
        h.nNorm = readU32le(oh + 12);
        h.primOff = readU32le(oh + 16);
        h.nPrim = readU32le(oh + 20);
        // Sanity-check counts. A real TMD object always has >= 1 vertex and >= 1 primitive;
        // normals can be zero on lit-but-flat-shaded variants but never absurdly large.
        if (h.nVert == 0 || h.nVert > kMaxVerts || h.nPrim == 0 || h.nPrim > kMaxPrims
            || h.nNorm > kMaxVerts)
            return false;
        out.append(h);
    }
    return true;
}

struct ParsedVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

bool readVertices(const uint8_t *ram, size_t ramSize, uint32_t flags, size_t tmdStart,
                  const ObjectHeader &obj, QVector<ParsedVertex> &out)
{
    const size_t payload = size_t(obj.nVert) * 8u;
    const ResolvedRange r = resolveOffset(obj.vertOff, flags, tmdStart, ramSize, payload);
    if (!r.ok)
        return false;
    out.resize(static_cast<int>(obj.nVert));
    for (uint32_t i = 0; i < obj.nVert; ++i) {
        const uint8_t *v = ram + r.base + size_t(i) * 8u;
        const int16_t x = readI16le(v + 0);
        const int16_t y = readI16le(v + 2);
        const int16_t z = readI16le(v + 4);
        // Apply same editor transform as PS1TMD::applyTmdImportWorldTransform:
        // uniform-scale + 180° Z rotation (x,y → −x,−y).
        out[static_cast<int>(i)] = ParsedVertex{
            -static_cast<float>(x) * kPosScale,
            -static_cast<float>(y) * kPosScale,
             static_cast<float>(z) * kPosScale,
        };
    }
    return true;
}

uint32_t packAbgr(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint32_t(0xFFu) << 24) | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
}

/** UV / tpage decoding follows the PSX TMD convention used by `PS1TMD.cpp`:
 *  texel coords are 0..255 inside the 256×256 logical texture page, with a half-texel bias. */
float decodePs1U(uint8_t u) { return (static_cast<float>(u) + 0.5f) / 256.0f; }
float decodePs1V(uint8_t v) { return (static_cast<float>(v) + 0.5f) / 256.0f; }

struct PrimContext {
    const QVector<ParsedVertex> *verts;
    ReconstructedSubMesh *flatColor;
    QHash<quint64, int> *textureSubMeshIndex;
    QVector<ReconstructedSubMesh> *textureSubMeshes;
};

ReconstructedSubMesh &ensureTextureSubMesh(PrimContext &ctx, uint16_t tpage, uint16_t clut)
{
    const quint64 key = MeshReconstructor::textureGroupKey(tpage, clut, 0u, 0u);
    const auto it = ctx.textureSubMeshIndex->constFind(key);
    if (it != ctx.textureSubMeshIndex->constEnd())
        return (*ctx.textureSubMeshes)[it.value()];
    const int idx = ctx.textureSubMeshes->size();
    ctx.textureSubMeshIndex->insert(key, idx);
    ReconstructedSubMesh sub;
    sub.materialName = MeshReconstructor::textureMaterialName(tpage, clut, 0u, 0u);
    ctx.textureSubMeshes->append(sub);
    return (*ctx.textureSubMeshes)[idx];
}

/** Push a triangle into the sub-mesh, swapping v1/v2 to match Ogre CCW front-face winding
 *  (same swap `PS1TMD::importTmd` applies — see appendTri in PS1TMD.cpp). */
void emitTri(ReconstructedSubMesh &sub, const ParsedVertex &p0, const ParsedVertex &p1,
             const ParsedVertex &p2, uint32_t c0, uint32_t c1, uint32_t c2,
             float u0, float v0, float u1, float v1, float u2, float v2, bool textured)
{
    const uint32_t base = static_cast<uint32_t>(sub.vertices.size());
    auto push = [&](const ParsedVertex &p, uint32_t c, float u, float v) {
        ReconstructedVertex rv;
        rv.px = p.x;
        rv.py = p.y;
        rv.pz = p.z;
        rv.diffuseArgb = c;
        rv.u = textured ? u : 0.0f;
        rv.v = textured ? v : 0.0f;
        sub.vertices.append(rv);
    };
    push(p0, c0, u0, v0);
    // Swap idx 1 and 2 to match Ogre CCW front (PSX packets are CW).
    push(p2, c2, u2, v2);
    push(p1, c1, u1, v1);
    sub.indices.append(base);
    sub.indices.append(base + 1);
    sub.indices.append(base + 2);
}

/** Walk one object's primitive packet stream and emit triangles. Returns false on a packet
 *  parse error so the candidate is rejected entirely (we don't want partial dumps that
 *  mis-bind to a different TMD overlapping in RAM). */
bool walkPrimitives(const uint8_t *ram, size_t ramSize, uint32_t flags, size_t tmdStart,
                    const ObjectHeader &obj, PrimContext &ctx)
{
    const ResolvedRange r = resolveOffset(obj.primOff, flags, tmdStart, ramSize, 4u);
    if (!r.ok)
        return false;
    const uint32_t nVert = obj.nVert;
    size_t p = r.base;
    uint32_t consumed = 0;
    while (consumed < obj.nPrim) {
        if (p + 4 > ramSize)
            return false;
        const uint8_t olen = ram[p + 0];
        const uint8_t ilen = ram[p + 1];
        const uint8_t flag = ram[p + 2];
        const uint8_t mode = ram[p + 3];
        const size_t payload = size_t(ilen) * 4u;
        if (ilen == 0 || ilen > 32)
            return false;
        if (p + 4 + payload > ramSize)
            return false;
        const uint8_t *d = ram + p + 4;
        p += 4 + payload;
        ++consumed;
        (void)olen;

        auto getVert = [&](uint16_t idx) -> const ParsedVertex * {
            if (idx >= nVert)
                return nullptr;
            return &(*ctx.verts)[static_cast<int>(idx)];
        };

        // Bread-and-butter primitive set. Anything else is silently skipped (consumed,
        // not failed) so an unknown packet flavor doesn't sink the whole TMD object.

        // 0x20 flag=0 ilen=3 — flat-shaded triangle (lit, mono color)
        if (mode == 0x20 && flag == 0 && ilen == 3) {
            const uint32_t c = packAbgr(d[0], d[1], d[2]);
            const uint16_t i0 = readU16le(d + 6);
            const uint16_t i1 = readU16le(d + 8);
            const uint16_t i2 = readU16le(d + 10);
            const ParsedVertex *p0 = getVert(i0);
            const ParsedVertex *p1 = getVert(i1);
            const ParsedVertex *p2 = getVert(i2);
            if (p0 && p1 && p2)
                emitTri(*ctx.flatColor, *p0, *p1, *p2, c, c, c, 0, 0, 0, 0, 0, 0, false);
            continue;
        }
        // 0x30 flag=0 ilen=4 — flat-shaded tri (lit, mono color, full-normal-per-vert variant)
        if (mode == 0x30 && flag == 0 && ilen == 4) {
            const uint32_t c = packAbgr(d[0], d[1], d[2]);
            const uint16_t v0 = readU16le(d + 6);
            const uint16_t v1 = readU16le(d + 10);
            const uint16_t v2 = readU16le(d + 14);
            const ParsedVertex *p0 = getVert(v0);
            const ParsedVertex *p1 = getVert(v1);
            const ParsedVertex *p2 = getVert(v2);
            if (p0 && p1 && p2)
                emitTri(*ctx.flatColor, *p0, *p1, *p2, c, c, c, 0, 0, 0, 0, 0, 0, false);
            continue;
        }
        // 0x30 flag=0 ilen=6 — Gouraud tri (per-vertex RGB, lit)
        if (mode == 0x30 && flag == 0 && ilen == 6) {
            const uint32_t c0 = packAbgr(d[0], d[1], d[2]);
            const uint32_t c1 = packAbgr(d[4], d[5], d[6]);
            const uint32_t c2 = packAbgr(d[8], d[9], d[10]);
            const uint16_t v0 = readU16le(d + 14);
            const uint16_t v1 = readU16le(d + 18);
            const uint16_t v2 = readU16le(d + 22);
            const ParsedVertex *p0 = getVert(v0);
            const ParsedVertex *p1 = getVert(v1);
            const ParsedVertex *p2 = getVert(v2);
            if (p0 && p1 && p2)
                emitTri(*ctx.flatColor, *p0, *p1, *p2, c0, c1, c2, 0, 0, 0, 0, 0, 0, false);
            continue;
        }
        // 0x24 flag=0 ilen=5 — textured tri (lit)
        if (mode == 0x24 && flag == 0 && ilen == 5) {
            const float u0 = decodePs1U(d[0]);
            const float v0 = decodePs1V(d[1]);
            const uint16_t clut = readU16le(d + 2);
            const float u1 = decodePs1U(d[4]);
            const float v1 = decodePs1V(d[5]);
            const uint16_t tpage = readU16le(d + 6);
            const float u2 = decodePs1U(d[8]);
            const float v2 = decodePs1V(d[9]);
            const uint16_t i0 = readU16le(d + 14);
            const uint16_t i1 = readU16le(d + 16);
            const uint16_t i2 = readU16le(d + 18);
            const ParsedVertex *pp0 = getVert(i0);
            const ParsedVertex *pp1 = getVert(i1);
            const ParsedVertex *pp2 = getVert(i2);
            if (pp0 && pp1 && pp2) {
                ReconstructedSubMesh &sub = ensureTextureSubMesh(ctx, tpage, clut);
                const uint32_t c = packAbgr(0x80u, 0x80u, 0x80u);
                emitTri(sub, *pp0, *pp1, *pp2, c, c, c, u0, v0, u1, v1, u2, v2, true);
            }
            continue;
        }
        // 0x34 flag=0 ilen=6 — Gouraud textured tri (lit). Per PS1TMD.cpp: bytes 0..11 are
        // UV0/CLUT/UV1/TPAGE/UV2/pad, bytes 12..23 are (n_idx, v_idx) × 3. The texture
        // provides the visible color so we bind neutral grey for the FFP modulate.
        if (mode == 0x34 && flag == 0 && ilen == 6) {
            const float u0 = decodePs1U(d[0]);
            const float v0 = decodePs1V(d[1]);
            const uint16_t clut = readU16le(d + 2);
            const float u1 = decodePs1U(d[4]);
            const float v1 = decodePs1V(d[5]);
            const uint16_t tpage = readU16le(d + 6);
            const float u2 = decodePs1U(d[8]);
            const float v2 = decodePs1V(d[9]);
            const uint16_t i0 = readU16le(d + 14);
            const uint16_t i1 = readU16le(d + 18);
            const uint16_t i2 = readU16le(d + 22);
            const ParsedVertex *pp0 = getVert(i0);
            const ParsedVertex *pp1 = getVert(i1);
            const ParsedVertex *pp2 = getVert(i2);
            if (pp0 && pp1 && pp2) {
                ReconstructedSubMesh &sub = ensureTextureSubMesh(ctx, tpage, clut);
                const uint32_t c = packAbgr(0x80u, 0x80u, 0x80u);
                emitTri(sub, *pp0, *pp1, *pp2, c, c, c, u0, v0, u1, v1, u2, v2, true);
            }
            continue;
        }
        // 0x28 flag=0 ilen=4 — mono-quad (lit)
        if (mode == 0x28 && flag == 0 && ilen == 4) {
            const uint32_t c = packAbgr(d[0], d[1], d[2]);
            const uint16_t i0 = readU16le(d + 6);
            const uint16_t i1 = readU16le(d + 8);
            const uint16_t i2 = readU16le(d + 10);
            const uint16_t i3 = readU16le(d + 12);
            const ParsedVertex *p0 = getVert(i0);
            const ParsedVertex *p1 = getVert(i1);
            const ParsedVertex *p2 = getVert(i2);
            const ParsedVertex *p3 = getVert(i3);
            if (p0 && p1 && p2 && p3) {
                emitTri(*ctx.flatColor, *p0, *p1, *p2, c, c, c, 0, 0, 0, 0, 0, 0, false);
                emitTri(*ctx.flatColor, *p1, *p3, *p2, c, c, c, 0, 0, 0, 0, 0, 0, false);
            }
            continue;
        }
        // 0x28 flag=4 ilen=7 — Gouraud-quad (lit, per-vertex RGB)
        if (mode == 0x28 && flag == 4 && ilen == 7) {
            const uint32_t c0 = packAbgr(d[0], d[1], d[2]);
            const uint32_t c1 = packAbgr(d[4], d[5], d[6]);
            const uint32_t c2 = packAbgr(d[8], d[9], d[10]);
            const uint32_t c3 = packAbgr(d[12], d[13], d[14]);
            const uint16_t i0 = readU16le(d + 18);
            const uint16_t i1 = readU16le(d + 20);
            const uint16_t i2 = readU16le(d + 22);
            const uint16_t i3 = readU16le(d + 24);
            const ParsedVertex *p0 = getVert(i0);
            const ParsedVertex *p1 = getVert(i1);
            const ParsedVertex *p2 = getVert(i2);
            const ParsedVertex *p3 = getVert(i3);
            if (p0 && p1 && p2 && p3) {
                emitTri(*ctx.flatColor, *p0, *p1, *p2, c0, c1, c2, 0, 0, 0, 0, 0, 0, false);
                emitTri(*ctx.flatColor, *p1, *p3, *p2, c1, c3, c2, 0, 0, 0, 0, 0, 0, false);
            }
            continue;
        }
        // 0x25 flag=1 ilen=6 — textured tri (no-light, per-vertex RGB at 12..14 ignored)
        if (mode == 0x25 && flag == 1 && ilen == 6) {
            const float u0 = decodePs1U(d[0]);
            const float v0 = decodePs1V(d[1]);
            const uint16_t clut = readU16le(d + 2);
            const float u1 = decodePs1U(d[4]);
            const float v1 = decodePs1V(d[5]);
            const uint16_t tpage = readU16le(d + 6);
            const float u2 = decodePs1U(d[8]);
            const float v2 = decodePs1V(d[9]);
            const uint16_t i0 = readU16le(d + 16);
            const uint16_t i1 = readU16le(d + 18);
            const uint16_t i2 = readU16le(d + 20);
            const ParsedVertex *pp0 = getVert(i0);
            const ParsedVertex *pp1 = getVert(i1);
            const ParsedVertex *pp2 = getVert(i2);
            if (pp0 && pp1 && pp2) {
                ReconstructedSubMesh &sub = ensureTextureSubMesh(ctx, tpage, clut);
                const uint32_t c = packAbgr(0x80u, 0x80u, 0x80u);
                emitTri(sub, *pp0, *pp1, *pp2, c, c, c, u0, v0, u1, v1, u2, v2, true);
            }
            continue;
        }
        // 0x35 flag=1 ilen=8 — Gouraud textured tri (no-light)
        if (mode == 0x35 && flag == 1 && ilen == 8) {
            const float u0 = decodePs1U(d[0]);
            const float v0 = decodePs1V(d[1]);
            const uint16_t clut = readU16le(d + 2);
            const float u1 = decodePs1U(d[4]);
            const float v1 = decodePs1V(d[5]);
            const uint16_t tpage = readU16le(d + 6);
            const float u2 = decodePs1U(d[8]);
            const float v2 = decodePs1V(d[9]);
            const uint16_t i0 = readU16le(d + 24);
            const uint16_t i1 = readU16le(d + 26);
            const uint16_t i2 = readU16le(d + 28);
            const ParsedVertex *pp0 = getVert(i0);
            const ParsedVertex *pp1 = getVert(i1);
            const ParsedVertex *pp2 = getVert(i2);
            if (pp0 && pp1 && pp2) {
                ReconstructedSubMesh &sub = ensureTextureSubMesh(ctx, tpage, clut);
                const uint32_t c = packAbgr(0x80u, 0x80u, 0x80u);
                emitTri(sub, *pp0, *pp1, *pp2, c, c, c, u0, v0, u1, v1, u2, v2, true);
            }
            continue;
        }

        // Unknown / unsupported primitive flavor — skip the payload, keep walking. (We
        // already advanced `p` and `consumed` above.) Don't return false: a typical TMD
        // mixes flavors and missing one shouldn't sink the whole object.
        (void)flag;
        (void)mode;
    }
    return true;
}

/** Compute the byte extent covered by all of a TMD's pools and primitive streams. We use
 *  this to (a) build a content hash and (b) skip the scanner past the entire blob so we
 *  don't accidentally rescan inner bytes as a second TMD candidate. */
size_t computeTmdSpan(const uint8_t *ram, size_t ramSize, uint32_t flags, size_t tmdStart,
                      const QVector<ObjectHeader> &headers)
{
    size_t maxEnd = tmdStart + kTmdHeaderSize + size_t(headers.size()) * kObjHeaderSize;
    for (const ObjectHeader &h : headers) {
        if (h.nVert > 0) {
            const auto r = resolveOffset(h.vertOff, flags, tmdStart, ramSize, size_t(h.nVert) * 8u);
            if (r.ok)
                maxEnd = std::max(maxEnd, r.base + size_t(h.nVert) * 8u);
        }
        if (h.nNorm > 0) {
            const auto r = resolveOffset(h.normOff, flags, tmdStart, ramSize, size_t(h.nNorm) * 8u);
            if (r.ok)
                maxEnd = std::max(maxEnd, r.base + size_t(h.nNorm) * 8u);
        }
        if (h.nPrim > 0) {
            // We don't know prim-payload sizes ahead of time. Conservatively reserve 8 bytes
            // per prim (smallest packet) — used purely for dedupe range, not for parsing.
            const auto r = resolveOffset(h.primOff, flags, tmdStart, ramSize, size_t(h.nPrim) * 8u);
            if (r.ok)
                maxEnd = std::max(maxEnd, r.base + size_t(h.nPrim) * 8u);
        }
    }
    return maxEnd > ramSize ? ramSize : maxEnd;
}

} // namespace

int PsxTmdRamScanner::captureFromSystemRam(const uint8_t *ram, size_t byteSize, EmuHooks *hooks)
{
    if (!ram || byteSize < kTmdHeaderSize + kObjHeaderSize || !hooks || !hooks->isCaptureEnabled())
        return 0;

    int found = 0;
    size_t offset = 0;
    while (offset + kTmdHeaderSize <= byteSize && found < kMaxTmdsPerFrame) {
        const uint32_t magic = readU32le(ram + offset);
        if (magic != kTmdMagic) {
            offset += kScanStrideBytes;
            continue;
        }
        const uint32_t flags = readU32le(ram + offset + 4);
        const uint32_t numObj = readU32le(ram + offset + 8);
        if ((flags & ~1u) != 0u || numObj == 0u || numObj > kMaxObjects) {
            offset += kScanStrideBytes;
            continue;
        }

        QVector<ObjectHeader> headers;
        if (!readObjectHeaders(ram, byteSize, offset, numObj, headers)) {
            offset += kScanStrideBytes;
            continue;
        }

        // Pre-walk pass: read all vertex/normal pools + walk primitives. If anything looks
        // bogus (OOB pointer, bad packet header, zero vertex output) we reject the whole
        // candidate. This is the false-positive filter: random RAM that happens to start
        // with 0x41 + plausible flags but doesn't form a real TMD gets thrown out here.
        ReconstructedSubMesh flatColor;
        flatColor.materialName = QStringLiteral("PS1Rip_tmd_vertcolor");
        QVector<ReconstructedSubMesh> textureSubMeshes;
        QHash<quint64, int> textureIndex;
        PrimContext ctx;
        ctx.flatColor = &flatColor;
        ctx.textureSubMeshes = &textureSubMeshes;
        ctx.textureSubMeshIndex = &textureIndex;

        bool allObjectsOk = true;
        int totalEmittedTris = 0;
        for (const ObjectHeader &h : headers) {
            QVector<ParsedVertex> verts;
            if (!readVertices(ram, byteSize, flags, offset, h, verts)) {
                allObjectsOk = false;
                break;
            }
            ctx.verts = &verts;
            const int trisBefore =
                flatColor.indices.size() / 3
                + std::accumulate(textureSubMeshes.constBegin(), textureSubMeshes.constEnd(), 0,
                                  [](int acc, const ReconstructedSubMesh &sub) {
                                      return acc + sub.indices.size() / 3;
                                  });
            if (!walkPrimitives(ram, byteSize, flags, offset, h, ctx)) {
                allObjectsOk = false;
                break;
            }
            const int trisAfter =
                flatColor.indices.size() / 3
                + std::accumulate(textureSubMeshes.constBegin(), textureSubMeshes.constEnd(), 0,
                                  [](int acc, const ReconstructedSubMesh &sub) {
                                      return acc + sub.indices.size() / 3;
                                  });
            totalEmittedTris += (trisAfter - trisBefore);
        }

        if (!allObjectsOk || totalEmittedTris == 0) {
            offset += kScanStrideBytes;
            continue;
        }

        // Span covers the bytes the TMD actually occupies; we'll skip the scan stride past
        // this so inner bytes don't get rescanned as second-level candidates.
        const size_t span = computeTmdSpan(ram, byteSize, flags, offset, headers);

        ReconstructedMesh mesh;
        mesh.meshName = QStringLiteral("ps1_tmd_at_0x%1").arg(offset, 0, 16);
        if (!flatColor.vertices.isEmpty() && flatColor.indices.size() >= 3) {
            mesh.subMeshes.append(flatColor);
            mesh.vertexCount += flatColor.vertices.size();
            mesh.triangleCount += flatColor.indices.size() / 3;
        }
        for (const ReconstructedSubMesh &sub : textureSubMeshes) {
            if (sub.vertices.isEmpty() || sub.indices.size() < 3)
                continue;
            mesh.subMeshes.append(sub);
            mesh.vertexCount += sub.vertices.size();
            mesh.triangleCount += sub.indices.size() / 3;
        }
        if (mesh.isEmpty()) {
            offset += kScanStrideBytes;
            continue;
        }

        CapturedModelMesh cap;
        cap.mesh = mesh;
        cap.sourceAddress = static_cast<uint32_t>(offset);
        cap.format = QStringLiteral("tmd");
        // Content hash over the full TMD byte span (header + pools + primitives).
        const size_t hashBytes = span > offset ? span - offset : kTmdHeaderSize;
        cap.contentHash = fnv1a64(ram + offset, hashBytes);

        if (hooks->onModelMesh(cap))
            ++found;

        // Skip past the parsed blob to avoid rescanning inner data. Align up to the next
        // 4-byte boundary so the outer stride stays well-formed.
        const size_t advance = std::max(span - offset, kScanStrideBytes);
        offset += (advance + 3u) & ~size_t(3u);
    }
    return found;
}
