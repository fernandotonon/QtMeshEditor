/*
-----------------------------------------------------------------------------------
This source file is part of QtMeshEditor.

Paint v2 Slice F (#549) — ProjectionPainter implementation.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include "ProjectionPainter.h"

#include "ProjectionMath.h"

#include <algorithm>
#include <cmath>

using ProjectionMath::Projected;
using ProjectionMath::projectToViewportUV;
using ProjectionMath::sampleImage;

namespace {

// Occlusion / depth-limit test for one texel at world position `wp`.
// Returns: 0 = visible, 1 = occluded, 2 = beyond depth-limit.
// `occ` must be non-null. Reconciles in LINEAR camera-space distance because
// MeshDepthRenderer encodes distance via fog (not NDC-z).
int classifyDepth(const Ogre::Vector3& wp,
                  const ProjectionPainter::OcclusionMap& occ,
                  float depthLimit)
{
    const Projected p = projectToViewportUV(wp, occ.viewProj);
    if (p.behind) return 1;                       // behind the depth camera → hide
    if (p.uv.x < 0.0f || p.uv.x > 1.0f || p.uv.y < 0.0f || p.uv.y > 1.0f)
        return 1;                                 // projects off the depth image → hide
    // Depth map: grayscale g in [0..1], near=bright(1)/far=dark(0), LINEAR in
    // world distance over [depthNear, depthFar].
    const Ogre::ColourValue d = sampleImage(occ.depth, p.uv);
    const float g = d.r;                          // grayscale → any channel
    const float dMap = occ.depthNear + (1.0f - g) * (occ.depthFar - occ.depthNear);
    const float dTexel = (wp - occ.camPosition).length();
    if (dTexel > dMap + occ.biasWorld) return 1;  // something nearer occludes it
    if (depthLimit > 0.0f && dTexel > dMap + depthLimit) return 2;  // too far behind
    return 0;
}

// Border feather in source-UV space: 0 at the very edge, 1 by `softEdge` in.
float edgeFeather(const Ogre::Vector2& suv, float softEdge)
{
    if (softEdge <= 0.0f) return 1.0f;
    const float d = std::min({suv.x, 1.0f - suv.x, suv.y, 1.0f - suv.y});
    return std::clamp(d / softEdge, 0.0f, 1.0f);
}

// src-over composite of (rgb, a) onto the texel already in `buf` at (px,py).
void compositeOver(TexturePaintBuffer& buf, int px, int py,
                   const Ogre::ColourValue& src, float a)
{
    if (a <= 0.0f) return;
    a = std::clamp(a, 0.0f, 1.0f);
    const Ogre::ColourValue dst = buf.pixel(px, py);
    const float outA = a + dst.a * (1.0f - a);
    Ogre::ColourValue out;
    if (outA > 1e-6f) {
        out.r = (src.r * a + dst.r * dst.a * (1.0f - a)) / outA;
        out.g = (src.g * a + dst.g * dst.a * (1.0f - a)) / outA;
        out.b = (src.b * a + dst.b * dst.a * (1.0f - a)) / outA;
    }
    out.a = outA;
    buf.setPixel(px, py, out);
}

// Facing weight of a triangle normal vs the view direction (camera looks ALONG
// camDirection). Returns <0 sentinel when it should be culled.
float facingWeight(const Ogre::Vector3& nrm, const Ogre::Vector3& camDir,
                   const ProjectionPainter::Options& opts)
{
    float facing = -nrm.dotProduct(camDir);
    if (opts.backfaceCull && facing < opts.minFacing) return -1.0f;
    if (facing <= 0.0f) return 0.0f;   // grazing but not culled (cull off)
    return std::pow(facing, std::max(0.01f, opts.facingPower));
}

} // namespace

ProjectionPainter::Report ProjectionPainter::project(
    const std::vector<Triangle>& tris, const View& view, const QImage& source,
    TexturePaintBuffer& out, const Options& opts, const OcclusionMap* occ)
{
    Report rep;
    if (tris.empty()) { rep.error = "no triangles"; return rep; }
    if (source.isNull()) { rep.error = "empty source image"; return rep; }

    const int res = opts.resolution > 0 ? opts.resolution
                                        : std::max(out.width(), out.height());
    if (res <= 0) { rep.error = "invalid resolution"; return rep; }
    out.resize(res, res);
    out.clear(Ogre::ColourValue(0, 0, 0, 0));   // new-layer buffer starts transparent

    for (const Triangle& t : tris) {
        Ogre::Vector3 nrm = t.normal;
        if (nrm.isZeroLength())
            nrm = (t.p[1] - t.p[0]).crossProduct(t.p[2] - t.p[0]);
        if (nrm.isZeroLength()) continue;
        nrm.normalise();

        const float facing = facingWeight(nrm, view.camDirection, opts);
        if (facing < 0.0f) { ++rep.texelsBackface; continue; }

        const Projected pa = projectToViewportUV(t.p[0], view.viewProj);
        const Projected pb = projectToViewportUV(t.p[1], view.viewProj);
        const Projected pc = projectToViewportUV(t.p[2], view.viewProj);
        if (pa.behind || pb.behind || pc.behind) continue;

        // UV0 -> pixel space (top-left origin).
        auto toPix = [&](const Ogre::Vector2& uv) {
            return Ogre::Vector2(uv.x * res, uv.y * res);
        };
        const Ogre::Vector2 A = toPix(t.uv[0]), B = toPix(t.uv[1]), C = toPix(t.uv[2]);
        int x0 = std::max(0, static_cast<int>(std::floor(std::min({A.x, B.x, C.x}))));
        int x1 = std::min(res - 1, static_cast<int>(std::ceil(std::max({A.x, B.x, C.x}))));
        int y0 = std::max(0, static_cast<int>(std::floor(std::min({A.y, B.y, C.y}))));
        int y1 = std::min(res - 1, static_cast<int>(std::ceil(std::max({A.y, B.y, C.y}))));
        const float denom = (B.y - C.y) * (A.x - C.x) + (C.x - B.x) * (A.y - C.y);
        if (std::abs(denom) < 1e-9f) continue;
        const float invDen = 1.0f / denom;

        for (int py = y0; py <= y1; ++py) {
            for (int px = x0; px <= x1; ++px) {
                const float fx = px + 0.5f, fy = py + 0.5f;
                const float l0 = ((B.y - C.y) * (fx - C.x) + (C.x - B.x) * (fy - C.y)) * invDen;
                const float l1 = ((C.y - A.y) * (fx - C.x) + (A.x - C.x) * (fy - C.y)) * invDen;
                const float l2 = 1.0f - l0 - l1;
                const float eps = -1e-4f;
                if (l0 < eps || l1 < eps || l2 < eps) continue;

                // Interpolated projected source UV — reject texels off the image.
                const Ogre::Vector2 suv = pa.uv * l0 + pb.uv * l1 + pc.uv * l2;
                if (suv.x < 0.0f || suv.x > 1.0f || suv.y < 0.0f || suv.y > 1.0f) continue;

                // Occlusion / depth-limit (F-B).
                if (occ && (opts.useOcclusion || opts.depthLimit > 0.0f)) {
                    const Ogre::Vector3 wp = t.p[0] * l0 + t.p[1] * l1 + t.p[2] * l2;
                    const int cls = classifyDepth(wp, *occ, opts.depthLimit);
                    if (cls == 1) { ++rep.texelsOccluded; continue; }
                    if (cls == 2) { ++rep.texelsDepthCulled; continue; }
                }

                Ogre::ColourValue col = sampleImage(source, suv);
                col.r *= opts.tint.r; col.g *= opts.tint.g; col.b *= opts.tint.b;
                const float a = facing * col.a * edgeFeather(suv, opts.softEdge)
                                * opts.strength * opts.tint.a;
                if (a <= 0.0f) continue;
                compositeOver(out, px, py, col, a);
                ++rep.texelsWritten;
            }
        }
    }

    // Seam dilation (opts.dilationPixels) is intentionally not applied here:
    // stencil/decal want a hard alpha mask, and VertexColorBaker::dilate needs a
    // separate coverage buffer. If a future mode needs it, track coverage and
    // call VertexColorBaker::dilate(out, coverage, opts.dilationPixels).
    rep.ok = true;
    return rep;
}

int ProjectionPainter::projectDab(
    const std::vector<Triangle>& tris, const View& view, const QImage& stencil,
    const Ogre::Vector2& brushUv, float brushRadiusUv,
    const Ogre::ColourValue& brushColor, float strength,
    TexturePaintBuffer& out, const Options& opts, const OcclusionMap* occ)
{
    if (tris.empty() || out.width() <= 0) return 0;
    const int res = std::max(out.width(), out.height());
    const bool haveStencil = !stencil.isNull();
    int touched = 0;

    // Brush footprint in pixel space (round falloff).
    const float rPix = brushRadiusUv * res;
    const float cxPix = brushUv.x * res, cyPix = brushUv.y * res;

    for (const Triangle& t : tris) {
        Ogre::Vector3 nrm = t.normal;
        if (nrm.isZeroLength())
            nrm = (t.p[1] - t.p[0]).crossProduct(t.p[2] - t.p[0]);
        if (nrm.isZeroLength()) continue;
        nrm.normalise();
        const float facing = facingWeight(nrm, view.camDirection, opts);
        if (facing < 0.0f) continue;

        const Projected pa = projectToViewportUV(t.p[0], view.viewProj);
        const Projected pb = projectToViewportUV(t.p[1], view.viewProj);
        const Projected pc = projectToViewportUV(t.p[2], view.viewProj);
        if (pa.behind || pb.behind || pc.behind) continue;

        auto toPix = [&](const Ogre::Vector2& uv) {
            return Ogre::Vector2(uv.x * res, uv.y * res);
        };
        const Ogre::Vector2 A = toPix(t.uv[0]), B = toPix(t.uv[1]), C = toPix(t.uv[2]);
        // Clamp the scanned box to the brush footprint (perf: only near the dab).
        int x0 = std::max(0, static_cast<int>(std::floor(std::max(std::min({A.x, B.x, C.x}), cxPix - rPix))));
        int x1 = std::min(res - 1, static_cast<int>(std::ceil(std::min(std::max({A.x, B.x, C.x}), cxPix + rPix))));
        int y0 = std::max(0, static_cast<int>(std::floor(std::max(std::min({A.y, B.y, C.y}), cyPix - rPix))));
        int y1 = std::min(res - 1, static_cast<int>(std::ceil(std::min(std::max({A.y, B.y, C.y}), cyPix + rPix))));
        const float denom = (B.y - C.y) * (A.x - C.x) + (C.x - B.x) * (A.y - C.y);
        if (std::abs(denom) < 1e-9f) continue;
        const float invDen = 1.0f / denom;

        for (int py = y0; py <= y1; ++py) {
            for (int px = x0; px <= x1; ++px) {
                const float fx = px + 0.5f, fy = py + 0.5f;
                // Round brush falloff.
                const float dr = std::hypot(fx - cxPix, fy - cyPix);
                if (rPix <= 0.0f || dr > rPix) continue;
                const float fall = 1.0f - (dr / rPix);

                const float l0 = ((B.y - C.y) * (fx - C.x) + (C.x - B.x) * (fy - C.y)) * invDen;
                const float l1 = ((C.y - A.y) * (fx - C.x) + (A.x - C.x) * (fy - C.y)) * invDen;
                const float l2 = 1.0f - l0 - l1;
                const float eps = -1e-4f;
                if (l0 < eps || l1 < eps || l2 < eps) continue;

                const Ogre::Vector2 suv = pa.uv * l0 + pb.uv * l1 + pc.uv * l2;
                if (suv.x < 0.0f || suv.x > 1.0f || suv.y < 0.0f || suv.y > 1.0f) continue;

                if (occ && (opts.useOcclusion || opts.depthLimit > 0.0f)) {
                    const Ogre::Vector3 wp = t.p[0] * l0 + t.p[1] * l1 + t.p[2] * l2;
                    const int cls = classifyDepth(wp, *occ, opts.depthLimit);
                    if (cls != 0) continue;
                }

                const float stencilA = haveStencil ? sampleImage(stencil, suv).a : 1.0f;
                const float a = facing * stencilA * fall * strength;
                if (a <= 0.0f) continue;
                compositeOver(out, px, py, brushColor, a);
                ++touched;
            }
        }
    }
    return touched;
}
