#include "MeshRefine.h"

#include <algorithm>
#include <cmath>

namespace MeshRefine {

namespace {

// Uniform-weight vertex Laplacian pass: out = pos + factor * (avgNbr - pos).
// Adjacency comes in as flattened neighbour lists (CSR-style offsets).
void laplacianPass(std::vector<float>& positions,
                   const std::vector<uint32_t>& nbrOffsets,
                   const std::vector<uint32_t>& nbrList,
                   std::vector<float>& scratch,
                   float factor)
{
    const size_t nv = positions.size() / 3;
    scratch = positions;
    for (size_t v = 0; v < nv; ++v) {
        const uint32_t begin = nbrOffsets[v];
        const uint32_t end   = nbrOffsets[v + 1];
        if (begin == end)
            continue;   // isolated vertex — leave in place
        float ax = 0, ay = 0, az = 0;
        for (uint32_t k = begin; k < end; ++k) {
            const size_t n3 = static_cast<size_t>(nbrList[k]) * 3;
            ax += scratch[n3 + 0];
            ay += scratch[n3 + 1];
            az += scratch[n3 + 2];
        }
        const float inv = 1.0f / float(end - begin);
        positions[v * 3 + 0] = scratch[v * 3 + 0]
            + factor * (ax * inv - scratch[v * 3 + 0]);
        positions[v * 3 + 1] = scratch[v * 3 + 1]
            + factor * (ay * inv - scratch[v * 3 + 1]);
        positions[v * 3 + 2] = scratch[v * 3 + 2]
            + factor * (az * inv - scratch[v * 3 + 2]);
    }
}

} // namespace

void taubinSmooth(std::vector<float>& positions,
                  const std::vector<uint32_t>& indices,
                  int iterations, float lambda, float mu)
{
    const size_t nv = positions.size() / 3;
    if (nv == 0 || indices.size() < 3 || iterations <= 0)
        return;
    for (uint32_t i : indices)
        if (i >= nv)
            return;   // corrupt index data — refuse to touch the mesh

    // Build unique undirected adjacency: collect both directions of every
    // triangle edge, sort, dedupe, then pack CSR offsets + neighbour list.
    std::vector<uint64_t> edges;
    edges.reserve(indices.size() * 2);
    const size_t triCount = indices.size() / 3;
    for (size_t t = 0; t < triCount; ++t) {
        const uint32_t a = indices[t * 3 + 0];
        const uint32_t b = indices[t * 3 + 1];
        const uint32_t c = indices[t * 3 + 2];
        auto push = [&edges](uint32_t u, uint32_t v) {
            edges.push_back((static_cast<uint64_t>(u) << 32) | v);
        };
        push(a, b); push(b, a);
        push(b, c); push(c, b);
        push(c, a); push(a, c);
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

    std::vector<uint32_t> nbrOffsets(nv + 1, 0);
    for (uint64_t e : edges)
        ++nbrOffsets[static_cast<size_t>(e >> 32) + 1];
    for (size_t v = 1; v <= nv; ++v)
        nbrOffsets[v] += nbrOffsets[v - 1];
    std::vector<uint32_t> nbrList(edges.size());
    {
        std::vector<uint32_t> cursor(nbrOffsets.begin(), nbrOffsets.end() - 1);
        for (uint64_t e : edges)
            nbrList[cursor[static_cast<size_t>(e >> 32)]++] =
                static_cast<uint32_t>(e & 0xffffffffu);
    }

    std::vector<float> scratch;
    for (int it = 0; it < iterations; ++it) {
        laplacianPass(positions, nbrOffsets, nbrList, scratch, lambda);
        laplacianPass(positions, nbrOffsets, nbrList, scratch, mu);
    }
}

void isoProjectStep(std::vector<float>& positions,
                    const std::vector<float>& f,
                    const std::vector<float>& grad,
                    float maxStep)
{
    const size_t nv = positions.size() / 3;
    if (nv == 0 || f.size() != nv || grad.size() != nv * 3 || maxStep <= 0)
        return;
    for (size_t v = 0; v < nv; ++v) {
        const float gx = grad[v * 3 + 0];
        const float gy = grad[v * 3 + 1];
        const float gz = grad[v * 3 + 2];
        const float g2 = gx * gx + gy * gy + gz * gz;
        if (g2 < 1e-12f)
            continue;
        // Newton step toward the zero level set: Δ = -f·g/|g|².
        const float s = -f[v] / g2;
        float dx = s * gx, dy = s * gy, dz = s * gz;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len > maxStep) {
            const float scale = maxStep / len;
            dx *= scale; dy *= scale; dz *= scale;
        }
        positions[v * 3 + 0] += dx;
        positions[v * 3 + 1] += dy;
        positions[v * 3 + 2] += dz;
    }
}

} // namespace MeshRefine
