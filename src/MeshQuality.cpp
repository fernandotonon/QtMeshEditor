#include "MeshQuality.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace MeshQuality {

namespace {

constexpr uint32_t kDropped = ~0u;

bool indicesValid(const std::vector<uint32_t>& indices, size_t vertexCount)
{
    if (indices.size() % 3 != 0)
        return false;
    return std::all_of(indices.begin(), indices.end(),
                       [vertexCount](uint32_t i) { return i < vertexCount; });
}

bool positionsValid(const Geometry& g)
{
    return g.positions.size() >= g.vertexCount * 3;
}

// The bytes two vertices must share to be merged: the caller's key, or the
// position when there is none.
const unsigned char* keyData(const Geometry& g, size_t* stride)
{
    if (!g.keys.empty() && g.keyStride > 0 && g.keys.size() >= g.vertexCount * g.keyStride) {
        *stride = g.keyStride;
        return g.keys.data();
    }
    *stride = sizeof(float) * 3;
    return reinterpret_cast<const unsigned char*>(g.positions.data());
}

size_t countReferenced(const std::vector<uint32_t>& indices, size_t vertexCount)
{
    std::vector<char> seen(vertexCount, 0);
    size_t n = 0;
    for (uint32_t i : indices) {
        if (!seen[i]) {
            seen[i] = 1;
            ++n;
        }
    }
    return n;
}

double acmrOf(const std::vector<uint32_t>& idx, size_t vertexCount, unsigned int cacheSize)
{
    if (idx.empty())
        return 0.0;
    return meshopt_analyzeVertexCache(idx.data(), idx.size(), vertexCount, cacheSize, 0, 0).acmr;
}

double overdrawOf(const std::vector<uint32_t>& idx, const std::vector<float>& pos, size_t vertexCount)
{
    if (idx.empty())
        return -1.0;
    return meshopt_analyzeOverdraw(idx.data(), idx.size(), pos.data(), vertexCount,
                                   sizeof(float) * 3).overdraw;
}

double overfetchOf(const std::vector<uint32_t>& idx, size_t vertexCount, size_t vertexBytes)
{
    if (idx.empty() || vertexCount == 0)
        return 0.0;
    return meshopt_analyzeVertexFetch(idx.data(), idx.size(), vertexCount, vertexBytes).overfetch;
}

std::vector<uint32_t> withoutDegenerates(const Geometry& g, const std::vector<uint32_t>& idx,
                                         const Limits& limits, size_t* removed)
{
    std::vector<uint32_t> out;
    out.reserve(idx.size());
    size_t dropped = 0;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        if (isDegenerate(g, idx[t], idx[t + 1], idx[t + 2], limits)) {
            ++dropped;
            continue;
        }
        out.insert(out.end(), {idx[t], idx[t + 1], idx[t + 2]});
    }
    if (removed)
        *removed = dropped;
    return out;
}

} // namespace

bool isDegenerate(const Geometry& g, uint32_t a, uint32_t b, uint32_t c, const Limits& limits)
{
    if (a == b || b == c || a == c)
        return true;
    if (!positionsValid(g))
        return false;
    const float* p0 = &g.positions[size_t(a) * 3];
    const float* p1 = &g.positions[size_t(b) * 3];
    const float* p2 = &g.positions[size_t(c) * 3];
    const double e0[3] = {double(p1[0]) - p0[0], double(p1[1]) - p0[1], double(p1[2]) - p0[2]};
    const double e1[3] = {double(p2[0]) - p0[0], double(p2[1]) - p0[1], double(p2[2]) - p0[2]};
    const double e2[3] = {double(p2[0]) - p1[0], double(p2[1]) - p1[1], double(p2[2]) - p1[2]};
    const double cx = e0[1] * e1[2] - e0[2] * e1[1];
    const double cy = e0[2] * e1[0] - e0[0] * e1[2];
    const double cz = e0[0] * e1[1] - e0[1] * e1[0];
    const double twiceArea = std::sqrt(cx * cx + cy * cy + cz * cz);
    const double longestSq = std::max({e0[0] * e0[0] + e0[1] * e0[1] + e0[2] * e0[2],
                                       e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2],
                                       e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2]});
    if (!(longestSq > 0.0) || !(twiceArea > 0.0))
        return true;   // coincident corners (or NaN)
    // Height over the longest edge = twiceArea / longest; compared to
    // eps * longest without the square root.
    return twiceArea < limits.degenerateRelativeHeight * longestSq;
}

Metrics analyze(const Geometry& g, const Limits& limits)
{
    Metrics m;
    m.triangles = g.indices.size() / 3;
    m.vertices = g.vertexCount;
    if (g.indices.empty() || g.vertexCount == 0 || !indicesValid(g.indices, g.vertexCount)
        || !positionsValid(g))
        return m;

    for (size_t t = 0; t + 2 < g.indices.size(); t += 3)
        if (isDegenerate(g, g.indices[t], g.indices[t + 1], g.indices[t + 2], limits))
            ++m.degenerateTriangles;

    const size_t referenced = countReferenced(g.indices, g.vertexCount);
    m.unreferencedVertices = g.vertexCount - referenced;
    {
        size_t stride = 0;
        const unsigned char* keys = keyData(g, &stride);
        std::vector<unsigned int> remap(g.vertexCount);
        const size_t unique = meshopt_generateVertexRemap(remap.data(), g.indices.data(),
                                                          g.indices.size(), keys,
                                                          g.vertexCount, stride);
        m.redundantVertices = referenced > unique ? referenced - unique : 0;
    }

    const size_t vertexBytes = g.vertexBytes ? g.vertexBytes : sizeof(float) * 3;
    m.acmr = acmrOf(g.indices, g.vertexCount, limits.cacheSize);
    m.overfetch = overfetchOf(g.indices, g.vertexCount, vertexBytes);

    std::vector<uint32_t> opt(g.indices.size());
    meshopt_optimizeVertexCache(opt.data(), g.indices.data(), g.indices.size(), g.vertexCount);
    m.acmrOptimized = acmrOf(opt, g.vertexCount, limits.cacheSize);

    if (m.triangles <= limits.maxOverdrawTriangles) {
        m.overdraw = overdrawOf(g.indices, g.positions, g.vertexCount);
        std::vector<uint32_t> od(opt.size());
        meshopt_optimizeOverdraw(od.data(), opt.data(), opt.size(), g.positions.data(),
                                 g.vertexCount, sizeof(float) * 3, limits.overdrawThreshold);
        m.overdrawOptimized = overdrawOf(od, g.positions, g.vertexCount);
        opt.swap(od);
    }

    // The fetch fix runs last on the reordered triangles, so measure it there.
    std::vector<unsigned int> fetchRemap(g.vertexCount);
    const size_t fetched = meshopt_optimizeVertexFetchRemap(fetchRemap.data(), opt.data(),
                                                            opt.size(), g.vertexCount);
    meshopt_remapIndexBuffer(opt.data(), opt.data(), opt.size(), fetchRemap.data());
    m.overfetchOptimized = overfetchOf(opt, fetched, vertexBytes);
    return m;
}

FixResult fix(const Geometry& g, const FixOptions& opt, const Limits& limits)
{
    FixResult r;
    r.trianglesBefore = g.indices.size() / 3;
    r.indices = g.indices;
    r.newVertexCount = g.vertexCount;
    r.trianglesAfter = r.trianglesBefore;
    if (!opt.any() || g.indices.empty() || !indicesValid(g.indices, g.vertexCount)
        || !positionsValid(g))
        return r;

    std::vector<uint32_t> idx = g.indices;
    std::vector<float> pos(g.positions.begin(), g.positions.begin() + g.vertexCount * 3);
    size_t n = g.vertexCount;
    std::vector<uint32_t> remap;   // old -> current, empty = identity

    if (opt.removeDegenerates)
        idx = withoutDegenerates(g, idx, limits, &r.degeneratesRemoved);

    if (opt.weld && !idx.empty()) {
        size_t stride = 0;
        const unsigned char* keys = keyData(g, &stride);
        const size_t referenced = countReferenced(idx, n);
        std::vector<unsigned int> weld(n);
        const size_t unique = meshopt_generateVertexRemap(weld.data(), idx.data(), idx.size(),
                                                          keys, n, stride);
        r.verticesWelded = referenced - unique;
        r.verticesDropped += n - referenced;
        meshopt_remapIndexBuffer(idx.data(), idx.data(), idx.size(), weld.data());
        std::vector<float> welded(unique * 3);
        meshopt_remapVertexBuffer(welded.data(), pos.data(), n, sizeof(float) * 3, weld.data());
        pos.swap(welded);
        remap.assign(weld.begin(), weld.end());
        n = unique;
    }

    if (opt.simplifyTargetTriangles > 0 && idx.size() / 3 > opt.simplifyTargetTriangles) {
        std::vector<uint32_t> simple(idx.size());
        float achieved = 0.0f;
        const size_t count = meshopt_simplify(simple.data(), idx.data(), idx.size(), pos.data(),
                                              n, sizeof(float) * 3,
                                              opt.simplifyTargetTriangles * 3,
                                              opt.simplifyMaxError, 0, &achieved);
        if (count > 0 && count < idx.size()) {
            simple.resize(count);
            idx.swap(simple);
            r.simplified = true;
        }
    }

    if (opt.vertexCache)
        meshopt_optimizeVertexCache(idx.data(), idx.data(), idx.size(), n);
    if (opt.overdraw && idx.size() / 3 <= limits.maxOverdrawTriangles)
        meshopt_optimizeOverdraw(idx.data(), idx.data(), idx.size(), pos.data(), n,
                                 sizeof(float) * 3, limits.overdrawThreshold);

    if (opt.vertexFetch && !idx.empty()) {
        std::vector<unsigned int> fetch(n);
        const size_t fetched = meshopt_optimizeVertexFetchRemap(fetch.data(), idx.data(),
                                                                idx.size(), n);
        meshopt_remapIndexBuffer(idx.data(), idx.data(), idx.size(), fetch.data());
        if (!opt.weld)
            r.verticesDropped += n - fetched;
        if (remap.empty()) {
            remap.assign(fetch.begin(), fetch.end());
        } else {
            for (uint32_t& v : remap)
                v = (v == kDropped) ? kDropped : fetch[v];
        }
        n = fetched;
    }

    // An identity remap means the vertex buffer needs no rewrite.
    if (!remap.empty() && n == g.vertexCount) {
        bool identity = true;
        for (size_t i = 0; i < remap.size() && identity; ++i)
            identity = remap[i] == i;
        if (identity)
            remap.clear();
    }

    r.indices = std::move(idx);
    r.remap = std::move(remap);
    r.newVertexCount = n;
    r.trianglesAfter = r.indices.size() / 3;
    r.changed = !r.remap.empty() || r.indices != g.indices;
    return r;
}

std::vector<unsigned char> remapStream(const unsigned char* data, size_t vertexCount,
                                       size_t stride, const std::vector<uint32_t>& remap,
                                       size_t newVertexCount)
{
    std::vector<unsigned char> out(newVertexCount * stride, 0);
    std::vector<char> written(newVertexCount, 0);
    const size_t count = std::min(vertexCount, remap.size());
    for (size_t i = 0; i < count; ++i) {
        const uint32_t to = remap[i];
        if (to == kDropped || to >= newVertexCount || written[to])
            continue;
        std::memcpy(out.data() + size_t(to) * stride, data + i * stride, stride);
        written[to] = 1;
    }
    return out;
}

} // namespace MeshQuality
