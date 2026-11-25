#include "SegMesh.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace GenericSeg {

namespace {
// Positions are welded on a grid of 1e-5 of the (unit) bounding radius.
constexpr float kWeldGrid = 1e-5f;

uint64_t gridKey(const Vec3& p)
{
    auto q = [](float v) { return uint64_t(int64_t(std::llround(v / kWeldGrid)) & 0x1FFFFF); };
    return (q(p.x) << 42) | (q(p.y) << 21) | q(p.z);
}
} // namespace

SegMesh SegMesh::build(const float* pos, int vertexCount, const uint32_t* idx, int indexCount)
{
    SegMesh m;
    const int faceCount = indexCount / 3;
    if (!pos || !idx || vertexCount <= 0 || faceCount <= 0)
        return m;
    for (int i = 0; i < faceCount * 3; ++i)
        if (int(idx[i]) >= vertexCount)
            return m;

    // Normalise: bbox centre to origin, farthest vertex at radius 1.
    Vec3 lo(pos[0], pos[1], pos[2]), hi = lo;
    for (int v = 1; v < vertexCount; ++v) {
        lo = {std::min(lo.x, pos[3 * v]), std::min(lo.y, pos[3 * v + 1]), std::min(lo.z, pos[3 * v + 2])};
        hi = {std::max(hi.x, pos[3 * v]), std::max(hi.y, pos[3 * v + 1]), std::max(hi.z, pos[3 * v + 2])};
    }
    m.center = (lo + hi) * 0.5f;
    float radius = 0.0f;
    m.positions.resize(vertexCount);
    for (int v = 0; v < vertexCount; ++v) {
        m.positions[v] = Vec3(pos[3 * v], pos[3 * v + 1], pos[3 * v + 2]) - m.center;
        radius = std::max(radius, length(m.positions[v]));
    }
    if (!(radius > 0.0f))
        return SegMesh();
    m.scale = radius;
    for (Vec3& p : m.positions) p = p * (1.0f / radius);
    m.indices.assign(idx, idx + faceCount * 3);

    // Weld.
    std::unordered_map<uint64_t, uint32_t> ids;
    std::vector<uint32_t> weldOf(vertexCount);
    for (int v = 0; v < vertexCount; ++v) {
        auto it = ids.emplace(gridKey(m.positions[v]), uint32_t(ids.size())).first;
        weldOf[v] = it->second;
    }
    m.weldedCount = int(ids.size());
    m.weldedIndices.resize(m.indices.size());
    for (size_t i = 0; i < m.indices.size(); ++i)
        m.weldedIndices[i] = weldOf[m.indices[i]];

    // Face normals/areas + smooth welded normals.
    m.faceNormals.resize(faceCount);
    m.faceAreas.resize(faceCount);
    m.weldedNormals.assign(m.weldedCount, Vec3());
    for (int f = 0; f < faceCount; ++f) {
        const Vec3& a = m.positions[m.indices[3 * f]];
        const Vec3& b = m.positions[m.indices[3 * f + 1]];
        const Vec3& c = m.positions[m.indices[3 * f + 2]];
        const Vec3 n = cross(b - a, c - a);   // length = 2 * area
        m.faceAreas[f] = 0.5f * length(n);
        m.faceNormals[f] = normalized(n);
        for (int k = 0; k < 3; ++k) {
            Vec3& acc = m.weldedNormals[m.weldedIndices[3 * f + k]];
            acc = acc + n;
        }
    }
    for (Vec3& n : m.weldedNormals) n = normalized(n);

    // Edge adjacency on the welded mesh.
    std::unordered_map<uint64_t, std::vector<int>> edges;
    edges.reserve(size_t(faceCount) * 2);
    for (int f = 0; f < faceCount; ++f) {
        for (int k = 0; k < 3; ++k) {
            uint32_t a = m.weldedIndices[3 * f + k], b = m.weldedIndices[3 * f + (k + 1) % 3];
            if (a == b) continue;
            if (a > b) std::swap(a, b);
            edges[(uint64_t(a) << 32) | b].push_back(f);
        }
    }
    m.faceNeighbors.assign(faceCount, {});
    for (const auto& kv : edges) {
        const std::vector<int>& fs = kv.second;
        for (size_t i = 0; i < fs.size(); ++i)
            for (size_t j = i + 1; j < fs.size(); ++j)
                if (fs[i] != fs[j]) {
                    m.faceNeighbors[fs[i]].push_back(fs[j]);
                    m.faceNeighbors[fs[j]].push_back(fs[i]);
                }
    }
    for (auto& nb : m.faceNeighbors) {
        std::sort(nb.begin(), nb.end());
        nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
    }
    return m;
}

} // namespace GenericSeg
