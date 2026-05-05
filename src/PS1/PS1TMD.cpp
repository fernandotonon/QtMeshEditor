/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "PS1/PS1TMD.h"

#include <OgreHardwareBufferManager.h>
#include <OgreLogManager.h>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgrePass.h>
#include <OgreRoot.h>
#include <OgreSubMesh.h>
#include <OgreSubEntity.h>
#include <OgreTechnique.h>
#include <OgreTextureManager.h>
#include <OgreVertexIndexData.h>

#include <QByteArray>
#include <QFile>
#include <QDir>
#include <QFileInfo>

#include "PS1/PS1TIM.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kTmdId = 0x41u;
constexpr size_t kTmdHeaderSize = 12u;
constexpr size_t kObjHeaderSize = 28u;

inline uint32_t readU32le(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint16_t readU16le(const uint8_t* p)
{
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

inline int16_t readI16le(const uint8_t* p)
{
    return static_cast<int16_t>(readU16le(p));
}

inline void writeU32le(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v & 0xFF);
    p[1] = uint8_t((v >> 8) & 0xFF);
    p[2] = uint8_t((v >> 16) & 0xFF);
    p[3] = uint8_t((v >> 24) & 0xFF);
}

inline void writeU16le(uint8_t* p, uint16_t v)
{
    p[0] = uint8_t(v & 0xFF);
    p[1] = uint8_t((v >> 8) & 0xFF);
}

inline int16_t clampI16(int v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return static_cast<int16_t>(v);
}

/**
 * PS1 8-bit U/V are texel indices in the active 256×256 texture page.
 * Map to 0..1 with texel-center bias. Like most PC APIs here, increasing V moves down the image (same as PSX
 * VRAM Y), so no V flip — matches typical PNG/JPEG row order with Ogre’s 2D textures.
 * Texture page / CLUT (cba/tsb) are not baked into UVs; use a bitmap cropped to the same page.
 */
static float decodePs1TexU(uint8_t uByte)
{
    return (float(uByte) + 0.5f) / 256.0f;
}

static float decodePs1TexV(uint8_t vByte)
{
    return (float(vByte) + 0.5f) / 256.0f;
}

/** After fixed-point step: scale for editor, then 180° about +Z (right-handed: x,y → −x, −y). */
static void applyTmdImportWorldTransform(Ogre::Vector3& p)
{
    p *= PS1TMD::kTmdEditorUniformScale;
    p.x = -p.x;
    p.y = -p.y;
}

/** Same as vertex rotation: 180° about +Z (fixed-point vector from file, then unitized). */
static void applyTmdImportWorldTransformNormal(Ogre::Vector3& n)
{
    if (n.isZeroLength())
        return;
    n.normalise();
    n.x = -n.x;
    n.y = -n.y;
}

/** Offsets in object headers are relative to byte 12 (post file header). */
inline size_t absFromStored(uint32_t stored, size_t fileSize)
{
    const size_t a = size_t(12u) + size_t(stored);
    return a < fileSize ? a : SIZE_MAX;
}

struct TriSoup {
    std::vector<Ogre::Vector3> pos;
    std::vector<Ogre::Vector3> nrm;
    std::vector<Ogre::Vector2> uv;
    std::vector<Ogre::ColourValue> col;
    bool hasUv{false};
    bool hasCol{false};
};

/** Unit normal from triangle positions (no-light TMD prims have no normal indices). */
static Ogre::Vector3 flatNormalFromTri(const Ogre::Vector3& p0, const Ogre::Vector3& p1, const Ogre::Vector3& p2)
{
    const Ogre::Vector3 e1 = p1 - p0;
    const Ogre::Vector3 e2 = p2 - p0;
    Ogre::Vector3 n = e1.crossProduct(e2);
    if (n.squaredLength() < 1e-24f)
        return Ogre::Vector3::UNIT_Y;
    n.normalise();
    return n;
}

static bool triMatchesRefNormal(const Ogre::Vector3& p0, const Ogre::Vector3& p1, const Ogre::Vector3& p2,
                                const Ogre::Vector3& ref)
{
    Ogre::Vector3 n = (p1 - p0).crossProduct(p2 - p0);
    // If degenerate or ref is zero, don't try to flip.
    if (n.squaredLength() < 1e-24f || ref.isZeroLength())
        return true;
    return n.dotProduct(ref) >= 0.0f;
}

static void appendTri(TriSoup& out, const Ogre::Vector3& p0, const Ogre::Vector3& p1, const Ogre::Vector3& p2,
                      const Ogre::Vector3& n0, const Ogre::Vector3& n1, const Ogre::Vector3& n2,
                      const Ogre::Vector2& uv0, const Ogre::Vector2& uv1, const Ogre::Vector2& uv2, bool withUv,
                      const Ogre::ColourValue& c0 = Ogre::ColourValue::White,
                      const Ogre::ColourValue& c1 = Ogre::ColourValue::White,
                      const Ogre::ColourValue& c2 = Ogre::ColourValue::White,
                      bool withCol = false)
{
    out.pos.push_back(p0);
    out.pos.push_back(p1);
    out.pos.push_back(p2);
    out.nrm.push_back(n0);
    out.nrm.push_back(n1);
    out.nrm.push_back(n2);
    if (withUv) {
        out.uv.push_back(uv0);
        out.uv.push_back(uv1);
        out.uv.push_back(uv2);
        out.hasUv = true;
    }
    if (withCol) {
        out.col.push_back(c0);
        out.col.push_back(c1);
        out.col.push_back(c2);
        out.hasCol = true;
    }
}

static void appendTriMatchWinding(TriSoup& out,
                                  const Ogre::Vector3& p0, const Ogre::Vector3& p1, const Ogre::Vector3& p2,
                                  const Ogre::Vector3& n0, const Ogre::Vector3& n1, const Ogre::Vector3& n2,
                                  const Ogre::Vector2& uv0, const Ogre::Vector2& uv1, const Ogre::Vector2& uv2, bool withUv,
                                  const Ogre::ColourValue& c0, const Ogre::ColourValue& c1, const Ogre::ColourValue& c2, bool withCol,
                                  const Ogre::Vector3& refNormal)
{
    if (triMatchesRefNormal(p0, p1, p2, refNormal)) {
        appendTri(out, p0, p1, p2, n0, n1, n2, uv0, uv1, uv2, withUv, c0, c1, c2, withCol);
    } else {
        // Flip winding + associated per-vertex data for v1/v2.
        appendTri(out, p0, p2, p1, n0, n2, n1, uv0, uv2, uv1, withUv, c0, c2, c1, withCol);
    }
}

static bool parseTmdObject(const uint8_t* data, size_t fileSize, uint32_t storedVertOff, uint32_t nVert,
                           uint32_t storedNormOff, uint32_t nNorm, uint32_t storedPrimOff, uint32_t nPrim,
                           float step, TriSoup& out, Ogre::LogManager& log)
{
    const size_t vBase = absFromStored(storedVertOff, fileSize);
    const size_t nBase = absFromStored(storedNormOff, fileSize);
    const size_t pBase = absFromStored(storedPrimOff, fileSize);
    if (vBase == SIZE_MAX || nBase == SIZE_MAX || pBase == SIZE_MAX)
        return false;
    if (vBase + size_t(nVert) * 8u > fileSize || nBase + size_t(nNorm) * 8u > fileSize)
        return false;

    std::vector<Ogre::Vector3> verts(nVert);
    std::vector<Ogre::Vector3> norms(nNorm);
    for (uint32_t i = 0; i < nVert; ++i) {
        const uint8_t* v = data + vBase + i * 8u;
        const int16_t x = readI16le(v);
        const int16_t y = readI16le(v + 2);
        const int16_t z = readI16le(v + 4);
        verts[i] = Ogre::Vector3(float(x) * step, float(y) * step, float(z) * step);
        applyTmdImportWorldTransform(verts[i]);
    }
    for (uint32_t i = 0; i < nNorm; ++i) {
        const uint8_t* v = data + nBase + i * 8u;
        const int16_t x = readI16le(v);
        const int16_t y = readI16le(v + 2);
        const int16_t z = readI16le(v + 4);
        Ogre::Vector3 n{float(x), float(y), float(z)};
        if (!n.isZeroLength())
            n.normalise();
        applyTmdImportWorldTransformNormal(n);
        norms[i] = n;
    }

    size_t p = pBase;
    uint32_t consumed = 0;
    while (consumed < nPrim && p + 4 <= fileSize) {
        const uint8_t olen = data[p];
        const uint8_t ilen = data[p + 1];
        const uint8_t flag = data[p + 2];
        const uint8_t mode = data[p + 3];
        const size_t payload = size_t(ilen) * 4u;
        if (p + 4 + payload > fileSize) {
            log.logMessage("PS1TMD: truncated primitive stream", Ogre::LML_WARNING);
            break;
        }
        const uint8_t* d = data + p + 4;
        p += 4 + payload;
        ++consumed;
        (void)olen;

        if (mode == 0x20 && flag == 0 && ilen == 3) {
            const Ogre::ColourValue c0(float(d[0]) / 255.0f, float(d[1]) / 255.0f, float(d[2]) / 255.0f, 1.0f);
            const uint16_t ni = readU16le(d + 4);
            const uint16_t i0 = readU16le(d + 6);
            const uint16_t i1 = readU16le(d + 8);
            const uint16_t i2 = readU16le(d + 10);
            if (i0 < nVert && i1 < nVert && i2 < nVert && ni < nNorm) {
                const Ogre::Vector3& np = norms[ni];
                // Swap v1/v2 so front-face winding matches Ogre (CCW) vs PSX packet order.
                appendTri(out, verts[i0], verts[i2], verts[i1], np, np, np, Ogre::Vector2::ZERO, Ogre::Vector2::ZERO,
                          Ogre::Vector2::ZERO, false, c0, c0, c0, true);
            }
            continue;
        }
        if (mode == 0x30 && flag == 0 && ilen == 4) {
            const Ogre::ColourValue c0(float(d[0]) / 255.0f, float(d[1]) / 255.0f, float(d[2]) / 255.0f, 1.0f);
            const uint16_t n0 = readU16le(d + 4);
            const uint16_t v0 = readU16le(d + 6);
            const uint16_t n1 = readU16le(d + 8);
            const uint16_t v1 = readU16le(d + 10);
            const uint16_t n2 = readU16le(d + 12);
            const uint16_t v2 = readU16le(d + 14);
            if (v0 < nVert && v1 < nVert && v2 < nVert && n0 < nNorm && n1 < nNorm && n2 < nNorm)
                appendTri(out, verts[v0], verts[v2], verts[v1], norms[n0], norms[n2], norms[n1], Ogre::Vector2::ZERO,
                          Ogre::Vector2::ZERO, Ogre::Vector2::ZERO, false, c0, c0, c0, true);
            continue;
        }
        if (mode == 0x24 && flag == 0 && ilen == 5) {
            const uint16_t ni = readU16le(d + 12);
            const uint16_t i0 = readU16le(d + 14);
            const uint16_t i1 = readU16le(d + 16);
            const uint16_t i2 = readU16le(d + 18);
            if (i0 < nVert && i1 < nVert && i2 < nVert && ni < nNorm) {
                const Ogre::Vector3& np = norms[ni];
                const float u0 = decodePs1TexU(d[0]);
                const float v0 = decodePs1TexV(d[1]);
                const float u1 = decodePs1TexU(d[4]);
                const float v1 = decodePs1TexV(d[5]);
                const float u2 = decodePs1TexU(d[8]);
                const float v2 = decodePs1TexV(d[9]);
                appendTri(out, verts[i0], verts[i2], verts[i1], np, np, np, Ogre::Vector2(u0, v0), Ogre::Vector2(u2, v2),
                          Ogre::Vector2(u1, v1), true);
            }
            continue;
        }
        if (mode == 0x34 && flag == 0 && ilen == 6) {
            const uint16_t n0 = readU16le(d + 12);
            const uint16_t v0 = readU16le(d + 14);
            const uint16_t n1 = readU16le(d + 16);
            const uint16_t v1 = readU16le(d + 18);
            const uint16_t n2 = readU16le(d + 20);
            const uint16_t v2 = readU16le(d + 22);
            if (v0 < nVert && v1 < nVert && v2 < nVert && n0 < nNorm && n1 < nNorm && n2 < nNorm) {
                const float u0 = decodePs1TexU(d[0]);
                const float v0 = decodePs1TexV(d[1]);
                const float u1 = decodePs1TexU(d[4]);
                const float v1 = decodePs1TexV(d[5]);
                const float u2 = decodePs1TexU(d[8]);
                const float v2 = decodePs1TexV(d[9]);
                appendTri(out, verts[v0], verts[v2], verts[v1], norms[n0], norms[n2], norms[n1], Ogre::Vector2(u0, v0),
                          Ogre::Vector2(u2, v2), Ogre::Vector2(u1, v1), true);
            }
            continue;
        }
        if (mode == 0x28 && flag == 0 && ilen == 4) {
            const Ogre::ColourValue c0(float(d[0]) / 255.0f, float(d[1]) / 255.0f, float(d[2]) / 255.0f, 1.0f);
            const uint16_t ni = readU16le(d + 4);
            const uint16_t i0 = readU16le(d + 6);
            const uint16_t i1 = readU16le(d + 8);
            const uint16_t i2 = readU16le(d + 10);
            const uint16_t i3 = readU16le(d + 12);
            if (i0 < nVert && i1 < nVert && i2 < nVert && i3 < nVert && ni < nNorm) {
                const Ogre::Vector3& np = norms[ni];
                appendTriMatchWinding(out, verts[i0], verts[i2], verts[i1], np, np, np,
                                      Ogre::Vector2::ZERO, Ogre::Vector2::ZERO, Ogre::Vector2::ZERO, false,
                                      c0, c0, c0, true, np);
                appendTriMatchWinding(out, verts[i0], verts[i3], verts[i2], np, np, np,
                                      Ogre::Vector2::ZERO, Ogre::Vector2::ZERO, Ogre::Vector2::ZERO, false,
                                      c0, c0, c0, true, np);
            }
            continue;
        }
        // Net Yaroze / tmd.h: Mode 0x28 && Flag 4 — Gradated quad (per-vertex RGB), ilen 7.
        // Use per-vertex RGB (one normal).
        if (mode == 0x28 && flag == 4 && ilen == 7) {
            const Ogre::ColourValue c0(float(d[0]) / 255.0f, float(d[1]) / 255.0f, float(d[2]) / 255.0f, 1.0f);
            const Ogre::ColourValue c1(float(d[4]) / 255.0f, float(d[5]) / 255.0f, float(d[6]) / 255.0f, 1.0f);
            const Ogre::ColourValue c2(float(d[8]) / 255.0f, float(d[9]) / 255.0f, float(d[10]) / 255.0f, 1.0f);
            const Ogre::ColourValue c3(float(d[12]) / 255.0f, float(d[13]) / 255.0f, float(d[14]) / 255.0f, 1.0f);
            const uint16_t ni = readU16le(d + 16);
            const uint16_t i0 = readU16le(d + 18);
            const uint16_t i1 = readU16le(d + 20);
            const uint16_t i2 = readU16le(d + 22);
            const uint16_t i3 = readU16le(d + 24);
            if (i0 < nVert && i1 < nVert && i2 < nVert && i3 < nVert && ni < nNorm) {
                const Ogre::Vector3& np = norms[ni];
                // GRID.TMD uses this primitive to represent a checkerboard. Gouraud interpolation makes
                // the triangulation diagonal very visible, so for checker-like quads (c0==c2,c1==c3)
                // we treat the quad as a *flat-colored* face using c0.
                const bool isCheckerLike = (c0 == c2) && (c1 == c3);
                if (isCheckerLike) {
                    appendTriMatchWinding(out, verts[i0], verts[i2], verts[i1], np, np, np,
                                          Ogre::Vector2::ZERO, Ogre::Vector2::ZERO, Ogre::Vector2::ZERO, false,
                                          c0, c0, c0, true, np);
                    appendTriMatchWinding(out, verts[i0], verts[i3], verts[i2], np, np, np,
                                          Ogre::Vector2::ZERO, Ogre::Vector2::ZERO, Ogre::Vector2::ZERO, false,
                                          c0, c0, c0, true, np);
                } else {
                    // Default: keep per-vertex colors.
                    appendTriMatchWinding(out, verts[i0], verts[i2], verts[i1], np, np, np,
                                          Ogre::Vector2::ZERO, Ogre::Vector2::ZERO, Ogre::Vector2::ZERO, false,
                                          c0, c2, c1, true, np);
                    appendTriMatchWinding(out, verts[i0], verts[i3], verts[i2], np, np, np,
                                          Ogre::Vector2::ZERO, Ogre::Vector2::ZERO, Ogre::Vector2::ZERO, false,
                                          c0, c3, c2, true, np);
                }
            }
            continue;
        }
        // Net Yaroze: Mode 0x25 && Flag 1 — Textured triangle, no light (RGB @ 12–14, verts @ 16–20).
        if (mode == 0x25 && flag == 1 && ilen == 6) {
            const float u0 = decodePs1TexU(d[0]);
            const float v0 = decodePs1TexV(d[1]);
            const float u1 = decodePs1TexU(d[4]);
            const float v1 = decodePs1TexV(d[5]);
            const float u2 = decodePs1TexU(d[8]);
            const float v2 = decodePs1TexV(d[9]);
            const uint16_t i0 = readU16le(d + 16);
            const uint16_t i1 = readU16le(d + 18);
            const uint16_t i2 = readU16le(d + 20);
            if (i0 < nVert && i1 < nVert && i2 < nVert) {
                const Ogre::Vector3& p0 = verts[i0];
                const Ogre::Vector3& p1 = verts[i1];
                const Ogre::Vector3& p2 = verts[i2];
                const Ogre::Vector3 np = flatNormalFromTri(p0, p2, p1);
                appendTri(out, p0, p2, p1, np, np, np, Ogre::Vector2(u0, v0), Ogre::Vector2(u2, v2),
                          Ogre::Vector2(u1, v1), true);
            }
            continue;
        }
        // Net Yaroze: Mode 0x35 && Flag 1 — Gouraud textured triangle, no light (per-vert RGB 12–23, verts @ 24–28).
        if (mode == 0x35 && flag == 1 && ilen == 8) {
            const float u0 = decodePs1TexU(d[0]);
            const float v0 = decodePs1TexV(d[1]);
            const float u1 = decodePs1TexU(d[4]);
            const float v1 = decodePs1TexV(d[5]);
            const float u2 = decodePs1TexU(d[8]);
            const float v2 = decodePs1TexV(d[9]);
            const uint16_t i0 = readU16le(d + 24);
            const uint16_t i1 = readU16le(d + 26);
            const uint16_t i2 = readU16le(d + 28);
            if (i0 < nVert && i1 < nVert && i2 < nVert) {
                const Ogre::Vector3& p0 = verts[i0];
                const Ogre::Vector3& p1 = verts[i1];
                const Ogre::Vector3& p2 = verts[i2];
                const Ogre::Vector3 np = flatNormalFromTri(p0, p2, p1);
                appendTri(out, p0, p2, p1, np, np, np, Ogre::Vector2(u0, v0), Ogre::Vector2(u2, v2),
                          Ogre::Vector2(u1, v1), true);
            }
            continue;
        }
        // Sony tmd.h TMD_F_4T — lit textured quad (one normal), ilen 7.
        if (mode == 0x2c && flag == 0 && ilen == 7) {
            const float u0 = decodePs1TexU(d[0]);
            const float v0 = decodePs1TexV(d[1]);
            const float u1 = decodePs1TexU(d[4]);
            const float v1 = decodePs1TexV(d[5]);
            const float u2 = decodePs1TexU(d[8]);
            const float v2 = decodePs1TexV(d[9]);
            const float u3 = decodePs1TexU(d[12]);
            const float v3 = decodePs1TexV(d[13]);
            const uint16_t ni = readU16le(d + 16);
            const uint16_t i0 = readU16le(d + 18);
            const uint16_t i1 = readU16le(d + 20);
            const uint16_t i2 = readU16le(d + 22);
            const uint16_t i3 = readU16le(d + 24);
            if (i0 < nVert && i1 < nVert && i2 < nVert && i3 < nVert && ni < nNorm) {
                const Ogre::Vector3& np = norms[ni];
                appendTri(out, verts[i0], verts[i2], verts[i1], np, np, np, Ogre::Vector2(u0, v0),
                          Ogre::Vector2(u2, v2), Ogre::Vector2(u1, v1), true);
                appendTri(out, verts[i0], verts[i3], verts[i2], np, np, np, Ogre::Vector2(u0, v0),
                          Ogre::Vector2(u3, v3), Ogre::Vector2(u2, v2), true);
            }
            continue;
        }
        // TMD_F_4T_NL — textured quad, no light (RGB @ 16–18), verts @ 20–26.
        if (mode == 0x2d && flag == 1 && ilen == 7) {
            const float u0 = decodePs1TexU(d[0]);
            const float v0 = decodePs1TexV(d[1]);
            const float u1 = decodePs1TexU(d[4]);
            const float v1 = decodePs1TexV(d[5]);
            const float u2 = decodePs1TexU(d[8]);
            const float v2 = decodePs1TexV(d[9]);
            const float u3 = decodePs1TexU(d[12]);
            const float v3 = decodePs1TexV(d[13]);
            const uint16_t i0 = readU16le(d + 20);
            const uint16_t i1 = readU16le(d + 22);
            const uint16_t i2 = readU16le(d + 24);
            const uint16_t i3 = readU16le(d + 26);
            if (i0 < nVert && i1 < nVert && i2 < nVert && i3 < nVert) {
                const Ogre::Vector3& p0 = verts[i0];
                const Ogre::Vector3& p1 = verts[i1];
                const Ogre::Vector3& p2 = verts[i2];
                const Ogre::Vector3& p3 = verts[i3];
                Ogre::Vector3 np = flatNormalFromTri(p0, p2, p1);
                appendTri(out, p0, p2, p1, np, np, np, Ogre::Vector2(u0, v0), Ogre::Vector2(u2, v2),
                          Ogre::Vector2(u1, v1), true);
                np = flatNormalFromTri(p0, p3, p2);
                appendTri(out, p0, p3, p2, np, np, np, Ogre::Vector2(u0, v0), Ogre::Vector2(u3, v3),
                          Ogre::Vector2(u2, v2), true);
            }
            continue;
        }
        // TMD_G_4T — Gouraud textured quad, ilen 8.
        if (mode == 0x3c && flag == 0 && ilen == 8) {
            const float u0 = decodePs1TexU(d[0]);
            const float v0 = decodePs1TexV(d[1]);
            const float u1 = decodePs1TexU(d[4]);
            const float v1 = decodePs1TexV(d[5]);
            const float u2 = decodePs1TexU(d[8]);
            const float v2 = decodePs1TexV(d[9]);
            const float u3 = decodePs1TexU(d[12]);
            const float v3 = decodePs1TexV(d[13]);
            const uint16_t n0 = readU16le(d + 16);
            const uint16_t v0i = readU16le(d + 18);
            const uint16_t n1 = readU16le(d + 20);
            const uint16_t v1i = readU16le(d + 22);
            const uint16_t n2 = readU16le(d + 24);
            const uint16_t v2i = readU16le(d + 26);
            const uint16_t n3 = readU16le(d + 28);
            const uint16_t v3i = readU16le(d + 30);
            if (v0i < nVert && v1i < nVert && v2i < nVert && v3i < nVert && n0 < nNorm && n1 < nNorm
                && n2 < nNorm && n3 < nNorm) {
                appendTri(out, verts[v0i], verts[v2i], verts[v1i], norms[n0], norms[n2], norms[n1],
                          Ogre::Vector2(u0, v0), Ogre::Vector2(u2, v2), Ogre::Vector2(u1, v1), true);
                appendTri(out, verts[v0i], verts[v3i], verts[v2i], norms[n0], norms[n3], norms[n2],
                          Ogre::Vector2(u0, v0), Ogre::Vector2(u3, v3), Ogre::Vector2(u2, v2), true);
            }
            continue;
        }
        // TMD_G_4T_NL — Gouraud textured quad, no light, ilen 10.
        if (mode == 0x3d && flag == 1 && ilen == 10) {
            const float u0 = decodePs1TexU(d[0]);
            const float v0 = decodePs1TexV(d[1]);
            const float u1 = decodePs1TexU(d[4]);
            const float v1 = decodePs1TexV(d[5]);
            const float u2 = decodePs1TexU(d[8]);
            const float v2 = decodePs1TexV(d[9]);
            const float u3 = decodePs1TexU(d[12]);
            const float v3 = decodePs1TexV(d[13]);
            const uint16_t i0 = readU16le(d + 32);
            const uint16_t i1 = readU16le(d + 34);
            const uint16_t i2 = readU16le(d + 36);
            const uint16_t i3 = readU16le(d + 38);
            if (i0 < nVert && i1 < nVert && i2 < nVert && i3 < nVert) {
                const Ogre::Vector3& p0 = verts[i0];
                const Ogre::Vector3& p1 = verts[i1];
                const Ogre::Vector3& p2 = verts[i2];
                const Ogre::Vector3& p3 = verts[i3];
                Ogre::Vector3 np = flatNormalFromTri(p0, p2, p1);
                appendTri(out, p0, p2, p1, np, np, np, Ogre::Vector2(u0, v0), Ogre::Vector2(u2, v2),
                          Ogre::Vector2(u1, v1), true);
                np = flatNormalFromTri(p0, p3, p2);
                appendTri(out, p0, p3, p2, np, np, np, Ogre::Vector2(u0, v0), Ogre::Vector2(u3, v3),
                          Ogre::Vector2(u2, v2), true);
            }
            continue;
        }
    }
    return !out.pos.empty();
}

static Ogre::MeshPtr buildMeshFromSoup(const std::string& meshName, const TriSoup& soup)
{
    if (soup.pos.empty() || soup.pos.size() % 3u != 0)
        return {};

    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual(
        meshName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::SubMesh* sm = mesh->createSubMesh();
    // Use a unique per-import material so texture assignment doesn't mutate shared defaults.
    const std::string tmdMatName = std::string("TMD/") + meshName;
    try {
        if (!Ogre::MaterialManager::getSingleton().getByName(tmdMatName)) {
            // Default to a simple "empty" single-pass base. Other code may add texture units later.
            if (auto base = Ogre::MaterialManager::getSingleton().getByName("BaseMaterial")) {
                base->clone(tmdMatName);
            }
        }
    } catch (...) {
        // If materials aren't available yet, fall back to BaseMaterial.
    }
    sm->setMaterialName(Ogre::MaterialManager::getSingleton().getByName(tmdMatName) ? tmdMatName : "BaseMaterial");
    sm->useSharedVertices = false;

    const size_t nVert = soup.pos.size();
    sm->vertexData = new Ogre::VertexData();
    sm->vertexData->vertexCount = static_cast<uint32_t>(nVert);
    auto* decl = sm->vertexData->vertexDeclaration;
    auto* bind = sm->vertexData->vertexBufferBinding;
    size_t off = 0;
    decl->addElement(0, off, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    off += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, off, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    off += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    const bool hasUv = soup.hasUv && soup.uv.size() == nVert;
    if (hasUv) {
        decl->addElement(0, off, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);
        off += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT2);
    }
    const bool hasCol = soup.hasCol && soup.col.size() == nVert;
    if (hasCol) {
        decl->addElement(0, off, Ogre::VET_COLOUR_ARGB, Ogre::VES_DIFFUSE);
        off += Ogre::VertexElement::getTypeSize(Ogre::VET_COLOUR_ARGB);
    }
    const size_t vsize = decl->getVertexSize(0);
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        vsize, nVert, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint8_t* dst = static_cast<uint8_t*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
    for (size_t i = 0; i < nVert; ++i) {
        uint8_t* row = dst + i * vsize;
        float* p = nullptr;
        decl->findElementBySemantic(Ogre::VES_POSITION)->baseVertexPointerToElement(row, &p);
        p[0] = soup.pos[i].x;
        p[1] = soup.pos[i].y;
        p[2] = soup.pos[i].z;
        decl->findElementBySemantic(Ogre::VES_NORMAL)->baseVertexPointerToElement(row, &p);
        p[0] = soup.nrm[i].x;
        p[1] = soup.nrm[i].y;
        p[2] = soup.nrm[i].z;
        if (hasUv) {
            decl->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES)->baseVertexPointerToElement(row, &p);
            p[0] = soup.uv[i].x;
            p[1] = soup.uv[i].y;
        }
        if (hasCol) {
            Ogre::RGBA* c = nullptr;
            decl->findElementBySemantic(Ogre::VES_DIFFUSE)->baseVertexPointerToElement(row, &c);
            Ogre::Root::getSingleton().convertColourValue(soup.col[i], c);
        }
    }
    vbuf->unlock();
    bind->setBinding(0, vbuf);

    // If we imported vertex colors, make sure the cloned material uses them.
    try {
        auto mat = Ogre::MaterialManager::getSingleton().getByName(tmdMatName);
        if (mat) {
            if (!mat->isLoaded())
                mat->load();
            if (mat->getNumTechniques() > 0 && mat->getTechnique(0)->getNumPasses() > 0) {
                Ogre::Pass* p0 = mat->getTechnique(0)->getPass(0);
                if (p0) {
                    // Keep imported TMD materials "blank" by default (TMD primitives don't carry real material params).
                    // If the mesh has vertex colors, we track them to diffuse; otherwise track nothing.
                    p0->setAmbient(0.0f, 0.0f, 0.0f);
                    // Default diffuse should remain white so non-vertex-colored meshes aren't forced black.
                    p0->setDiffuse(1.0f, 1.0f, 1.0f, 1.0f);
                    p0->setEmissive(0.0f, 0.0f, 0.0f);
                    p0->setVertexColourTracking(hasCol ? Ogre::TVC_DIFFUSE : Ogre::TVC_NONE);
                }
            }
        }
    } catch (...) {
    }

    if (hasCol) {
        try {
            auto mat = Ogre::MaterialManager::getSingleton().getByName(tmdMatName);
            if (mat) {
                if (!mat->isLoaded())
                    mat->load();
                if (mat->getNumTechniques() > 0 && mat->getTechnique(0)->getNumPasses() > 0) {
                    Ogre::Pass* p0 = mat->getTechnique(0)->getPass(0);
                    if (p0) {
                        // Vertex-color-only meshes (like GRID.TMD) should render unlit by default to avoid
                        // triangle shading artifacts. Textured meshes will keep lighting settings.
                        if (!hasUv) {
                            // Keep the material "blank": no baked ambient/diffuse/emissive contribution.
                            // The visible color comes from vertex color tracking below.
                            p0->setAmbient(0.0f, 0.0f, 0.0f);
                            p0->setDiffuse(0.0f, 0.0f, 0.0f, 1.0f);
                            p0->setEmissive(0.0f, 0.0f, 0.0f);
                        }
                        p0->setVertexColourTracking(Ogre::TVC_DIFFUSE);
                    }
                }
            }
        } catch (...) {
        }
    }

    const size_t nTri = nVert / 3;
    const bool use32 = nVert > 65535;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        use32 ? Ogre::HardwareIndexBuffer::IT_32BIT : Ogre::HardwareIndexBuffer::IT_16BIT, nTri * 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    if (use32) {
        auto* ip = static_cast<uint32_t*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for (size_t i = 0; i < nVert; ++i)
            ip[i] = static_cast<uint32_t>(i);
        ibuf->unlock();
    } else {
        auto* ip = static_cast<uint16_t*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for (size_t i = 0; i < nVert; ++i)
            ip[i] = static_cast<uint16_t>(i);
        ibuf->unlock();
    }
    sm->indexData->indexBuffer = ibuf;
    sm->indexData->indexCount = static_cast<uint32_t>(nTri * 3);
    sm->indexData->indexStart = 0;

    Ogre::AxisAlignedBox bounds;
    for (const auto& v : soup.pos)
        bounds.merge(v);
    mesh->_setBounds(bounds);
    mesh->_setBoundingSphereRadius(bounds.getHalfSize().length());
    mesh->load();
    return mesh;
}

static bool submeshHasDiffuseTexture(const Ogre::MaterialPtr& mat)
{
    if (!mat)
        return false;
    try {
        if (!mat->isLoaded())
            mat->load();
    } catch (...) {
        return false;
    }
    if (mat->getNumTechniques() == 0)
        return false;
    Ogre::Technique* tech = mat->getTechnique(0);
    if (!tech || tech->getNumPasses() == 0)
        return false;
    Ogre::Pass* pass = tech->getPass(0);
    if (!pass)
        return false;
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        if (tus->getContentType() != Ogre::TextureUnitState::CONTENT_NAMED)
            continue;
        const std::string& nm = tus->getName();
        if (nm == "normal_map" || nm == "NormalMap")
            continue;
        if (!tus->getTextureName().empty())
            return true;
    }
    return false;
}

static void writeVertex8(int16_t x, int16_t y, int16_t z, uint8_t* out8)
{
    writeU16le(out8 + 0, static_cast<uint16_t>(x));
    writeU16le(out8 + 2, static_cast<uint16_t>(y));
    writeU16le(out8 + 4, static_cast<uint16_t>(z));
    writeU16le(out8 + 6, 0);
}

static void appendG3(std::vector<uint8_t>& pb, uint16_t i0, uint16_t i1, uint16_t i2, uint16_t n0, uint16_t n1, uint16_t n2)
{
    const size_t start = pb.size();
    pb.resize(start + 4 + 16);
    uint8_t* pkt = pb.data() + start;
    pkt[0] = 6;
    pkt[1] = 4;
    pkt[2] = 0;
    pkt[3] = 0x30;
    pkt[4] = 200;
    pkt[5] = 200;
    pkt[6] = 200;
    pkt[7] = 0x30;
    writeU16le(pkt + 8, n0);
    writeU16le(pkt + 10, i0);
    writeU16le(pkt + 12, n1);
    writeU16le(pkt + 14, i1);
    writeU16le(pkt + 16, n2);
    writeU16le(pkt + 18, i2);
}

static void appendFt3(std::vector<uint8_t>& pb, uint16_t i0, uint16_t i1, uint16_t i2, uint16_t ni, const Ogre::Vector2& t0,
                      const Ogre::Vector2& t1, const Ogre::Vector2& t2)
{
    const size_t start = pb.size();
    pb.resize(start + 4 + 20);
    uint8_t* pkt = pb.data() + start;
    pkt[0] = 7;
    pkt[1] = 5;
    pkt[2] = 0;
    pkt[3] = 0x24;
    auto encU = [](float u) -> uint8_t {
        return static_cast<uint8_t>(std::clamp(int(std::lround(u * 256.0f - 0.5f)), 0, 255));
    };
    pkt[4] = encU(t0.x);
    pkt[5] = encU(t0.y);
    writeU16le(pkt + 6, 0);
    pkt[8] = encU(t1.x);
    pkt[9] = encU(t1.y);
    writeU16le(pkt + 10, 0);
    pkt[12] = encU(t2.x);
    pkt[13] = encU(t2.y);
    writeU16le(pkt + 14, 0);
    writeU16le(pkt + 16, ni);
    writeU16le(pkt + 18, i0);
    writeU16le(pkt + 20, i1);
    writeU16le(pkt + 22, i2);
}

static uint32_t countPrimPackets(const std::vector<uint8_t>& prims)
{
    uint32_t n = 0;
    for (size_t i = 0; i < prims.size();) {
        if (i + 4 > prims.size())
            break;
        const uint8_t ilen = prims[i + 1];
        const size_t step = 4 + size_t(ilen) * 4u;
        if (i + step > prims.size())
            break;
        i += step;
        ++n;
    }
    return n;
}

} // namespace

namespace PS1TMD {

Ogre::MeshPtr importTmd(const QString& filePath, const std::string& meshName, float ogreUnitsPerTmdStep)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray raw = f.readAll();
    f.close();
    const uint8_t* data = reinterpret_cast<const uint8_t*>(raw.constData());
    const size_t fileSize = static_cast<size_t>(raw.size());
    auto& log = Ogre::LogManager::getSingleton();

    if (fileSize < kTmdHeaderSize)
        return {};
    if (readU32le(data) != kTmdId) {
        log.logMessage("PS1TMD: bad ID (expected 0x41)", Ogre::LML_WARNING);
        return {};
    }
    const uint32_t numObj = readU32le(data + 8);
    if (numObj == 0 || numObj > 4096u)
        return {};
    if (fileSize < kTmdHeaderSize + numObj * kObjHeaderSize)
        return {};

    TriSoup merged;
    for (uint32_t oi = 0; oi < numObj; ++oi) {
        const uint8_t* oh = data + 12 + oi * kObjHeaderSize;
        const uint32_t vOff = readU32le(oh);
        const uint32_t nV = readU32le(oh + 4);
        const uint32_t nOff = readU32le(oh + 8);
        const uint32_t nN = readU32le(oh + 12);
        const uint32_t pOff = readU32le(oh + 16);
        const uint32_t nP = readU32le(oh + 20);
        TriSoup part;
        if (!parseTmdObject(data, fileSize, vOff, nV, nOff, nN, pOff, nP, ogreUnitsPerTmdStep, part, log))
            continue;
        merged.pos.insert(merged.pos.end(), part.pos.begin(), part.pos.end());
        merged.nrm.insert(merged.nrm.end(), part.nrm.begin(), part.nrm.end());
        if (part.hasUv) {
            merged.hasUv = true;
            merged.uv.insert(merged.uv.end(), part.uv.begin(), part.uv.end());
        } else if (merged.hasUv) {
            for (size_t k = 0; k < part.pos.size(); ++k)
                merged.uv.push_back(Ogre::Vector2::ZERO);
        }
        if (part.hasCol) {
            merged.hasCol = true;
            merged.col.insert(merged.col.end(), part.col.begin(), part.col.end());
        } else if (merged.hasCol) {
            for (size_t k = 0; k < part.pos.size(); ++k)
                merged.col.push_back(Ogre::ColourValue::White);
        }
    }
    if (merged.pos.empty())
        return {};
    if (merged.hasUv && merged.uv.size() != merged.pos.size()) {
        merged.uv.clear();
        merged.hasUv = false;
    }
    if (merged.hasCol && merged.col.size() != merged.pos.size()) {
        merged.col.clear();
        merged.hasCol = false;
    }

    Ogre::MeshPtr mesh = buildMeshFromSoup(meshName, merged);
    if (!mesh)
        return {};

    // Auto-apply a sibling .TIM texture when:
    // - the TMD actually has UVs (textured primitives), and
    // - there is a TIM file next to the TMD with the same basename (e.g., CAR.TMD -> CAR.TIM).
    if (merged.hasUv) {
        const QFileInfo tmdFi(filePath);
        const QString base = tmdFi.completeBaseName();
        const QString dir = tmdFi.absolutePath();
        const QString timUpper = QDir(dir).filePath(base + ".TIM");
        const QString timLower = QDir(dir).filePath(base + ".tim");
        const QString timPath = QFileInfo::exists(timUpper) ? timUpper : (QFileInfo::exists(timLower) ? timLower : QString());

        if (!timPath.isEmpty()) {
            const std::string matName = std::string("TMD/") + meshName;
            try {
                auto mat = Ogre::MaterialManager::getSingleton().getByName(matName);
                if (mat) {
                    if (!mat->isLoaded())
                        mat->load();
                    if (mat->getNumTechniques() > 0 && mat->getTechnique(0)->getNumPasses() > 0) {
                        Ogre::Pass* pass0 = mat->getTechnique(0)->getPass(0);
                        if (pass0) {
                            Ogre::TextureUnitState* tus = nullptr;
                            if (pass0->getNumTextureUnitStates() > 0) {
                                tus = pass0->getTextureUnitState(0);
                            } else {
                                tus = pass0->createTextureUnitState();
                            }

                            if (tus) {
                                Ogre::Image img;
                                QString err;
                                if (PS1TIM::loadTimToOgreImage(timPath, img, &err)) {
                                    const QString timFileName = QFileInfo(timPath).fileName();
                                    const std::string texName = timFileName.toStdString();
                                    const std::string group = Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME;
                                    Ogre::TextureManager::getSingleton().loadImage(texName, group, img);
                                    tus->setTextureName(texName);

                                    // Simplify: single pass (remove outline/wireframe) on these per-import materials.
                                    Ogre::Technique* tech0 = mat->getTechnique(0);
                                    while (tech0 && tech0->getNumPasses() > 1) {
                                        tech0->removePass(1);
                                    }
                                } else {
                                    log.logMessage(QString("PS1TMD: TIM decode failed (%1): %2").arg(timPath, err).toStdString(),
                                                   Ogre::LML_WARNING);
                                }
                            }
                        }
                    }
                }
            } catch (...) {
                // Best-effort: if material/texture setup fails, keep the mesh usable.
            }
        }
    }

    return mesh;
}

bool exportEntity(const Ogre::Entity* entity, const QString& filePath, float ogreUnitsPerTmdStep)
{
    if (!entity || !entity->getMesh())
        return false;
    const float invStep = 1.0f / ogreUnitsPerTmdStep;
    const float invEditorScale = 1.0f / PS1TMD::kTmdEditorUniformScale;
    Ogre::Mesh* mesh = entity->getMesh().get();
    const unsigned numSub = mesh->getNumSubMeshes();
    if (numSub == 0)
        return false;

    struct ObjBlob {
        std::vector<uint8_t> verts;
        std::vector<uint8_t> norms;
        std::vector<uint8_t> prims;
    };
    std::vector<ObjBlob> objects(numSub);

    for (unsigned si = 0; si < numSub; ++si) {
        Ogre::SubMesh* sm = mesh->getSubMesh(si);
        Ogre::VertexData* vd = sm->useSharedVertices ? mesh->sharedVertexData : sm->vertexData;
        if (!vd || !sm->indexData || sm->indexData->indexCount < 3)
            continue;

        const auto* posEl = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        const auto* nrmEl = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL);
        const auto* uvEl = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES);
        if (!posEl || !nrmEl)
            continue;

        auto* subEnt = entity->getSubEntity(si);
        const bool textured = subEnt && submeshHasDiffuseTexture(subEnt->getMaterial());
        const bool hasUv = textured && uvEl;

        auto posBuf = vd->vertexBufferBinding->getBuffer(posEl->getSource());
        auto nrmBuf = vd->vertexBufferBinding->getBuffer(nrmEl->getSource());
        Ogre::HardwareVertexBufferSharedPtr uvBuf;
        if (hasUv)
            uvBuf = vd->vertexBufferBinding->getBuffer(uvEl->getSource());
        const size_t posStride = posBuf->getVertexSize();
        const size_t nrmStride = nrmBuf->getVertexSize();
        const size_t uvStride = uvBuf ? uvBuf->getVertexSize() : 0;

        const uint32_t vCount = vd->vertexCount;
        objects[si].verts.resize(size_t(vCount) * 8u, 0);
        objects[si].norms.resize(size_t(vCount) * 8u, 0);

        const uint8_t* posBase = static_cast<const uint8_t*>(posBuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        const uint8_t* nrmBase = static_cast<const uint8_t*>(nrmBuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        const uint8_t* uvBase = uvBuf ? static_cast<const uint8_t*>(uvBuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY)) : nullptr;

        for (uint32_t vi = 0; vi < vCount; ++vi) {
            const uint8_t* prow = posBase + vi * posStride;
            const uint8_t* nrow = nrmBase + vi * nrmStride;
            Ogre::Real* pf = nullptr;
            Ogre::Real* nf = nullptr;
            posEl->baseVertexPointerToElement(const_cast<uint8_t*>(prow), &pf);
            nrmEl->baseVertexPointerToElement(const_cast<uint8_t*>(nrow), &nf);
            // Inverse of import: undo 180° Z then divide by editor scale, then TMD fixed-point.
            const float tmdX = -pf[0] * invEditorScale;
            const float tmdY = -pf[1] * invEditorScale;
            const float tmdZ = pf[2] * invEditorScale;
            const int16_t px = clampI16(static_cast<int>(std::lround(tmdX * invStep)));
            const int16_t py = clampI16(static_cast<int>(std::lround(tmdY * invStep)));
            const int16_t pz = clampI16(static_cast<int>(std::lround(tmdZ * invStep)));
            // Inverse of R_z on normals (same rotation as vertex positions in mesh space).
            const int16_t nx = clampI16(static_cast<int>(std::lround(-nf[0] * 4096.0f)));
            const int16_t ny = clampI16(static_cast<int>(std::lround(-nf[1] * 4096.0f)));
            const int16_t nz = clampI16(static_cast<int>(std::lround(nf[2] * 4096.0f)));
            writeVertex8(px, py, pz, objects[si].verts.data() + vi * 8);
            writeVertex8(nx, ny, nz, objects[si].norms.data() + vi * 8);
        }
        posBuf->unlock();
        nrmBuf->unlock();
        if (uvBuf)
            uvBuf->unlock();

        auto ibuf = sm->indexData->indexBuffer;
        const uint8_t* ib = static_cast<const uint8_t*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        const bool i32 = ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT;
        const size_t triCount = sm->indexData->indexCount / 3;
        const unsigned ist = sm->indexData->indexStart;

        for (size_t t = 0; t < triCount; ++t) {
            uint32_t i0, i1, i2;
            if (i32) {
                const auto* ip = reinterpret_cast<const uint32_t*>(ib);
                i0 = ip[ist + t * 3 + 0];
                i1 = ip[ist + t * 3 + 1];
                i2 = ip[ist + t * 3 + 2];
            } else {
                const auto* ip = reinterpret_cast<const uint16_t*>(ib);
                i0 = ip[ist + t * 3 + 0];
                i1 = ip[ist + t * 3 + 1];
                i2 = ip[ist + t * 3 + 2];
            }
            if (i0 >= vCount || i1 >= vCount || i2 >= vCount)
                continue;
            if (hasUv && uvEl && uvBase) {
                Ogre::Real* tf = nullptr;
                uvEl->baseVertexPointerToElement(const_cast<uint8_t*>(const_cast<uint8_t*>(uvBase + i0 * uvStride)), &tf);
                const Ogre::Vector2 tv0(tf[0], tf[1]);
                uvEl->baseVertexPointerToElement(const_cast<uint8_t*>(const_cast<uint8_t*>(uvBase + i1 * uvStride)), &tf);
                const Ogre::Vector2 tv1(tf[0], tf[1]);
                uvEl->baseVertexPointerToElement(const_cast<uint8_t*>(const_cast<uint8_t*>(uvBase + i2 * uvStride)), &tf);
                const Ogre::Vector2 tv2(tf[0], tf[1]);
                // Undo import winding swap so .tmd primitive order matches PSX convention on disk.
                appendFt3(objects[si].prims, static_cast<uint16_t>(i0), static_cast<uint16_t>(i2), static_cast<uint16_t>(i1),
                          static_cast<uint16_t>(i0), tv0, tv2, tv1);
            } else if (textured) {
                appendFt3(objects[si].prims, static_cast<uint16_t>(i0), static_cast<uint16_t>(i2), static_cast<uint16_t>(i1),
                          static_cast<uint16_t>(i0), Ogre::Vector2(0, 0), Ogre::Vector2(0, 0), Ogre::Vector2(0, 0));
            } else {
                appendG3(objects[si].prims, static_cast<uint16_t>(i0), static_cast<uint16_t>(i2), static_cast<uint16_t>(i1),
                         static_cast<uint16_t>(i0), static_cast<uint16_t>(i2), static_cast<uint16_t>(i1));
            }
        }
        ibuf->unlock();
    }

    bool anyGeometry = false;
    for (unsigned si = 0; si < numSub; ++si) {
        if (!objects[si].verts.empty() && !objects[si].prims.empty()) {
            anyGeometry = true;
            break;
        }
    }
    if (!anyGeometry)
        return false;

    std::vector<uint8_t> file;
    const size_t headBytes = kTmdHeaderSize + numSub * kObjHeaderSize;
    file.resize(headBytes);
    writeU32le(file.data(), kTmdId);
    writeU32le(file.data() + 4, 0);
    writeU32le(file.data() + 8, numSub);

    for (unsigned si = 0; si < numSub; ++si) {
        uint8_t* oh = file.data() + 12 + si * kObjHeaderSize;
        if (objects[si].verts.empty() || objects[si].prims.empty()) {
            std::memset(oh, 0, kObjHeaderSize);
            continue;
        }
        const uint32_t vOff = static_cast<uint32_t>(file.size() - 12u);
        file.insert(file.end(), objects[si].verts.begin(), objects[si].verts.end());
        const uint32_t nVert = static_cast<uint32_t>(objects[si].verts.size() / 8u);
        const uint32_t nOff = static_cast<uint32_t>(file.size() - 12u);
        file.insert(file.end(), objects[si].norms.begin(), objects[si].norms.end());
        const uint32_t nNorm = static_cast<uint32_t>(objects[si].norms.size() / 8u);
        const uint32_t pOff = static_cast<uint32_t>(file.size() - 12u);
        const uint32_t pktCount = countPrimPackets(objects[si].prims);
        file.insert(file.end(), objects[si].prims.begin(), objects[si].prims.end());

        writeU32le(oh, vOff);
        writeU32le(oh + 4, nVert);
        writeU32le(oh + 8, nOff);
        writeU32le(oh + 12, nNorm);
        writeU32le(oh + 16, pOff);
        writeU32le(oh + 20, pktCount);
        writeU32le(oh + 24, 0);
    }

    QFile out(filePath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<qint64>(file.size()));
    out.close();
    return true;
}

} // namespace PS1TMD
