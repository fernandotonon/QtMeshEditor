/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — cavity / curvature / AO derived-map generation
(Paint v2 Slice G, issue #550)

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include "DerivedMapGenerator.h"

#include "HalfEdgeMesh.h"

#include <OgreVector2.h>
#include <OgreVector3.h>

#include <algorithm>
#include <cmath>

namespace {

// Edge-function half-space rasteriser over the UV triangle, matching
// VertexColorBaker::rasterizeTriangle's conventions exactly: UV -> pixel is
// (u*W, v*H) with a TOP-LEFT origin, and the winding is handled by flipping
// the edge weights when the signed area is negative (so the caller need not
// pre-sort). Kept local (rather than reusing VertexColorBaker) because that
// one is typed on RGBA8 ColourValue; a scalar map wants float precision and
// 1/4 the memory, and replicating a scalar into RGB then reading back .r
// would quantise to 8 bits — visible banding on a smooth AO gradient.
int rasteriseScalarTriangle(DerivedMap& map,
                            const Ogre::Vector2& uv0, const Ogre::Vector2& uv1,
                            const Ogre::Vector2& uv2,
                            float s0, float s1, float s2)
{
    const int W = map.width;
    const int H = map.height;
    if (W <= 0 || H <= 0) return 0;

    const float x0 = uv0.x * W, y0 = uv0.y * H;
    const float x1 = uv1.x * W, y1 = uv1.y * H;
    const float x2 = uv2.x * W, y2 = uv2.y * H;

    int minX = static_cast<int>(std::floor(std::min({x0, x1, x2})));
    int maxX = static_cast<int>(std::ceil (std::max({x0, x1, x2})));
    int minY = static_cast<int>(std::floor(std::min({y0, y1, y2})));
    int maxY = static_cast<int>(std::ceil (std::max({y0, y1, y2})));
    minX = std::max(0, minX); minY = std::max(0, minY);
    maxX = std::min(W, maxX); maxY = std::min(H, maxY);
    if (minX >= maxX || minY >= maxY) return 0;

    const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (std::abs(area) < 1e-12f) return 0;      // degenerate in UV space
    const float invArea = 1.0f / area;

    int written = 0;
    for (int py = minY; py < maxY; ++py) {
        for (int px = minX; px < maxX; ++px) {
            const float cx = px + 0.5f, cy = py + 0.5f;
            float w0 = ((x1 - cx) * (y2 - cy) - (x2 - cx) * (y1 - cy)) * invArea;
            float w1 = ((x2 - cx) * (y0 - cy) - (x0 - cx) * (y2 - cy)) * invArea;
            float w2 = 1.0f - w0 - w1;
            const float eps = -1e-5f;
            if (w0 < eps || w1 < eps || w2 < eps) continue;
            const size_t idx = static_cast<size_t>(py) * W + px;
            map.values[idx] = w0 * s0 + w1 * s1 + w2 * s2;
            map.coverage[idx] = 1;
            ++written;
        }
    }
    return written;
}

// Smear covered texels outward by `iterations` pixels. Mirrors
// VertexColorBaker::dilate: double-buffered so a pass cannot cascade within
// itself, first-covered-neighbour wins, early-exit when a pass changes
// nothing.
int dilateScalar(DerivedMap& map, int iterations)
{
    const int W = map.width, H = map.height;
    if (W <= 0 || H <= 0 || iterations <= 0) return 0;

    int totalFlipped = 0;
    for (int it = 0; it < iterations; ++it) {
        std::vector<float> nextVals = map.values;
        std::vector<uint8_t> nextCov = map.coverage;
        int flipped = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const size_t idx = static_cast<size_t>(y) * W + x;
                if (map.coverage[idx]) continue;
                bool found = false;
                for (int dy = -1; dy <= 1 && !found; ++dy) {
                    for (int dx = -1; dx <= 1 && !found; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                        const size_t nidx = static_cast<size_t>(ny) * W + nx;
                        if (!map.coverage[nidx]) continue;
                        nextVals[idx] = map.values[nidx];
                        nextCov[idx] = 1;
                        found = true;
                        ++flipped;
                    }
                }
            }
        }
        if (flipped == 0) break;
        map.values.swap(nextVals);
        map.coverage.swap(nextCov);
        totalFlipped += flipped;
    }
    return totalFlipped;
}

} // namespace

float DerivedMap::sample(float u, float v) const
{
    if (empty()) return 0.0f;
    int x = static_cast<int>(u * width);
    int y = static_cast<int>(v * height);
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    return values[static_cast<size_t>(y) * width + x];
}

const char* DerivedMapGenerator::kindName(DerivedMapKind kind)
{
    switch (kind) {
    case DerivedMapKind::Cavity:           return "cavity";
    case DerivedMapKind::Curvature:        return "curvature";
    case DerivedMapKind::AmbientOcclusion: return "ao";
    }
    return "unknown";
}

float DerivedMapGenerator::backgroundFor(DerivedMapKind kind)
{
    // "No effect" per kind: cavity/AO 0 = no dirt / no occlusion;
    // curvature 0.5 = flat (it is a signed signal centred at 0.5).
    switch (kind) {
    case DerivedMapKind::Cavity:           return 0.0f;
    case DerivedMapKind::AmbientOcclusion: return 0.0f;
    case DerivedMapKind::Curvature:        return 0.5f;
    }
    return 0.0f;
}

float DerivedMapGenerator::remapForKind(float concavity, DerivedMapKind kind,
                                        const Options& options)
{
    const float c = std::clamp(concavity * options.contrast, -1.0f, 1.0f);
    switch (kind) {
    case DerivedMapKind::Cavity:
        // Keep only the concave half: crevices matter, ridges do not.
        return std::clamp(c, 0.0f, 1.0f);
    case DerivedMapKind::Curvature: {
        // Signed, centred at 0.5. Pin near-flat to neutral so tessellation
        // noise on flat panels does not show up as speckled edge wear.
        if (std::abs(c) < options.flatTolerance) return 0.5f;
        return std::clamp(0.5f + 0.5f * c, 0.0f, 1.0f);
    }
    case DerivedMapKind::AmbientOcclusion:
        return std::clamp(c, 0.0f, 1.0f);
    }
    return 0.0f;
}

size_t DerivedMapGenerator::weldedVertexCount(const EditableMesh& mesh)
{
    HalfEdgeMesh he;
    if (!he.buildFromEditableMesh(mesh)) return 0;
    return he.vertexCount();
}

std::vector<float> DerivedMapGenerator::vertexConcavity(const EditableMesh& mesh)
{
    HalfEdgeMesh he;
    if (!he.buildFromEditableMesh(mesh)) return {};

    const size_t n = he.vertexCount();
    std::vector<float> out(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const int vi = static_cast<int>(i);
        const HEVertex& v = he.vertex(vi);
        Ogre::Vector3 nrm = v.normal;
        if (!v.hasNormal || nrm.isZeroLength()) {
            // No usable normal: leave flat rather than inventing a signal.
            out[i] = 0.0f;
            continue;
        }
        nrm.normalise();

        const std::vector<int> ring = he.verticesAroundVertex(vi);
        if (ring.empty()) { out[i] = 0.0f; continue; }

        // Average, over the 1-ring, of how far each neighbour sits along the
        // vertex normal relative to its distance. Neighbours "above" the
        // tangent plane (positive dot) mean the surface closes in around this
        // vertex => concave. Below => convex ridge.
        float acc = 0.0f;
        int used = 0;
        for (const int ni : ring) {
            const Ogre::Vector3 d = he.vertex(ni).position - v.position;
            const float len = d.length();
            if (len < 1e-9f) continue;          // coincident vertex
            acc += nrm.dotProduct(d / len);
            ++used;
        }
        out[i] = (used > 0) ? (acc / static_cast<float>(used)) : 0.0f;
    }
    return out;
}

DerivedMap DerivedMapGenerator::rasterise(const EditableMesh& mesh,
                                          const std::vector<float>& perVertex,
                                          const Options& options,
                                          Report* report)
{
    DerivedMap map;
    Report rep;

    const int res = std::max(1, options.resolution);
    HalfEdgeMesh he;
    if (!he.buildFromEditableMesh(mesh)) {
        rep.error = QStringLiteral("could not build half-edge mesh");
        if (report) *report = rep;
        return map;
    }
    if (perVertex.size() != he.vertexCount()) {
        rep.error = QStringLiteral("per-vertex array size %1 != welded vertex count %2")
                        .arg(perVertex.size()).arg(he.vertexCount());
        if (report) *report = rep;
        return map;
    }

    map.width = res;
    map.height = res;
    map.values.assign(static_cast<size_t>(res) * res, options.background);
    map.coverage.assign(static_cast<size_t>(res) * res, 0u);

    const size_t faceCount = he.faceCount();
    for (size_t f = 0; f < faceCount; ++f) {
        const std::vector<int> fv = he.faceVertices(static_cast<int>(f));
        if (fv.size() != 3) continue;
        const HEVertex& a = he.vertex(fv[0]);
        const HEVertex& b = he.vertex(fv[1]);
        const HEVertex& c = he.vertex(fv[2]);
        if (!a.hasUV || !b.hasUV || !c.hasUV) { ++rep.trianglesSkippedNoUv; continue; }
        rep.texelsRasterised += rasteriseScalarTriangle(
            map, a.uv, b.uv, c.uv,
            perVertex[fv[0]], perVertex[fv[1]], perVertex[fv[2]]);
    }

    rep.texelsDilated = dilateScalar(map, options.dilationPixels);

    // Report the observed range over covered texels only — an all-background
    // range would hide a generator that produced nothing.
    bool any = false;
    for (size_t i = 0; i < map.values.size(); ++i) {
        if (!map.coverage[i]) continue;
        const float x = map.values[i];
        if (!any) { rep.minValue = rep.maxValue = x; any = true; }
        else { rep.minValue = std::min(rep.minValue, x); rep.maxValue = std::max(rep.maxValue, x); }
    }
    rep.ok = true;
    if (report) *report = rep;
    return map;
}

DerivedMap DerivedMapGenerator::generate(const EditableMesh& mesh,
                                         DerivedMapKind kind,
                                         const Options& options,
                                         Report* report)
{
    if (kind == DerivedMapKind::AmbientOcclusion) {
        Report rep;
        rep.error = QStringLiteral(
            "AmbientOcclusion needs scene-side occlusion sampling; "
            "use fromVertexOcclusion()");
        if (report) *report = rep;
        return {};
    }

    const std::vector<float> raw = vertexConcavity(mesh);
    if (raw.empty()) {
        Report rep;
        rep.error = QStringLiteral("mesh has no vertices / could not be welded");
        if (report) *report = rep;
        return {};
    }
    std::vector<float> mapped(raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
        mapped[i] = remapForKind(raw[i], kind, options);

    Options opts = options;
    opts.background = backgroundFor(kind);
    return rasterise(mesh, mapped, opts, report);
}

DerivedMap DerivedMapGenerator::fromVertexOcclusion(const EditableMesh& mesh,
                                                    const std::vector<float>& occlusion,
                                                    const Options& options,
                                                    Report* report)
{
    std::vector<float> clamped(occlusion.size());
    for (size_t i = 0; i < occlusion.size(); ++i)
        clamped[i] = std::clamp(occlusion[i], 0.0f, 1.0f);

    Options opts = options;
    opts.background = backgroundFor(DerivedMapKind::AmbientOcclusion);
    return rasterise(mesh, clamped, opts, report);
}
