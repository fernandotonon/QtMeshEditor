/*
-----------------------------------------------------------------------------------
This source file is part of QtMeshEditor.

Paint v2 Slice E — topology-aware symmetry mirror (issue #548).

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include "SymmetryMirrorMap.h"

#include "EditableMesh.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {
// Reflect a mesh-local point across a single axis bit about `pivot`.
Ogre::Vector3 reflect(const Ogre::Vector3& p, int axisBit, const Ogre::Vector3& pivot)
{
    Ogre::Vector3 r = p;
    if (axisBit == 1) r.x = 2.0f * pivot.x - p.x;
    else if (axisBit == 2) r.y = 2.0f * pivot.y - p.y;
    else if (axisBit == 4) r.z = 2.0f * pivot.z - p.z;
    return r;
}

// Quantise a coordinate to a spatial-hash cell of size `cell`.
inline long long cellCoord(float v, float cell) {
    return static_cast<long long>(std::floor(v / cell));
}
inline uint64_t cellKey(long long x, long long y, long long z) {
    // Mix three cell coords into one key (cheap, collision-tolerant — buckets
    // just hold a few candidates each).
    const uint64_t ux = static_cast<uint64_t>(x) * 0x9E3779B97F4A7C15ull;
    const uint64_t uy = static_cast<uint64_t>(y) * 0xC2B2AE3D27D4EB4Full;
    const uint64_t uz = static_cast<uint64_t>(z) * 0x165667B19E3779F9ull;
    return ux ^ (uy + 0x9E3779B9ull + (ux << 6) + (ux >> 2)) ^ uz;
}
} // namespace

uint64_t SymmetryMirrorMap::triKey(int a, int b, int c)
{
    // Sort the three ids ascending, pack into 64 bits (21 bits each → up to
    // ~2M verts per triple slot, ample for a paint mesh).
    if (a > b) std::swap(a, b);
    if (b > c) std::swap(b, c);
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) & 0x1FFFFF)
         | ((static_cast<uint64_t>(b) & 0x1FFFFF) << 21)
         | ((static_cast<uint64_t>(c) & 0x1FFFFF) << 42);
}

bool SymmetryMirrorMap::build(const EditableMesh& mesh, int axisBit,
                              const Ogre::Vector3& pivot, float weldTol)
{
    m_valid = false;
    m_coverage = 0.0f;
    m_vertMirror.clear();
    m_faceByVerts.clear();
    m_submeshBase.clear();

    const auto& subs = mesh.subMeshes();
    if (subs.empty()) return false;
    if (weldTol <= 0.0f) weldTol = 1e-4f;

    // Flatten (submesh,index) → global id.
    m_submeshBase.resize(subs.size());
    int total = 0;
    for (size_t s = 0; s < subs.size(); ++s) {
        m_submeshBase[s] = total;
        total += static_cast<int>(subs[s].vertices.size());
    }
    if (total == 0) return false;

    // Spatial hash of every vertex position (cell = weldTol).
    struct Cand { int submesh; int index; Ogre::Vector3 pos; };
    std::unordered_map<uint64_t, std::vector<Cand>> grid;
    grid.reserve(static_cast<size_t>(total));
    for (size_t s = 0; s < subs.size(); ++s) {
        const auto& verts = subs[s].vertices;
        for (size_t i = 0; i < verts.size(); ++i) {
            const Ogre::Vector3& p = verts[i].position;
            grid[cellKey(cellCoord(p.x, weldTol), cellCoord(p.y, weldTol),
                         cellCoord(p.z, weldTol))]
                .push_back({static_cast<int>(s), static_cast<int>(i), p});
        }
    }

    auto nearestTo = [&](const Ogre::Vector3& target) -> VertRef {
        VertRef best;
        float bestD2 = weldTol * weldTol;
        const long long cx = cellCoord(target.x, weldTol);
        const long long cy = cellCoord(target.y, weldTol);
        const long long cz = cellCoord(target.z, weldTol);
        for (long long dx = -1; dx <= 1; ++dx)
        for (long long dy = -1; dy <= 1; ++dy)
        for (long long dz = -1; dz <= 1; ++dz) {
            auto it = grid.find(cellKey(cx + dx, cy + dy, cz + dz));
            if (it == grid.end()) continue;
            for (const Cand& c : it->second) {
                const float d2 = (c.pos - target).squaredLength();
                if (d2 <= bestD2) { bestD2 = d2; best = {c.submesh, c.index}; }
            }
        }
        return best;
    };

    // Per-vertex mirror partner.
    m_vertMirror.resize(subs.size());
    int matched = 0;
    for (size_t s = 0; s < subs.size(); ++s) {
        const auto& verts = subs[s].vertices;
        m_vertMirror[s].assign(verts.size(), VertRef{});
        for (size_t i = 0; i < verts.size(); ++i) {
            const Ogre::Vector3 target = reflect(verts[i].position, axisBit, pivot);
            VertRef m = nearestTo(target);
            if (m.valid()) { m_vertMirror[s][i] = m; ++matched; }
        }
    }
    m_coverage = static_cast<float>(matched) / static_cast<float>(total);

    // Build a per-vertex 1-ring adjacency (from the triangle list) so we can
    // verify the correspondence topologically: the mirror of a neighbour must
    // be a neighbour of the mirror vertex. This rejects spurious position
    // collisions on dense meshes.
    std::vector<std::unordered_set<int>> adj(static_cast<size_t>(total));
    for (size_t s = 0; s < subs.size(); ++s) {
        const auto& sub = subs[s];
        for (const auto& tri : sub.triangles) {
            const int g0 = globalId(static_cast<int>(s), static_cast<int>(tri.indices[0]));
            const int g1 = globalId(static_cast<int>(s), static_cast<int>(tri.indices[1]));
            const int g2 = globalId(static_cast<int>(s), static_cast<int>(tri.indices[2]));
            adj[g0].insert(g1); adj[g0].insert(g2);
            adj[g1].insert(g0); adj[g1].insert(g2);
            adj[g2].insert(g0); adj[g2].insert(g1);
        }
    }

    // Verify on a sample: fraction of correspondences whose neighbours' mirrors
    // land on the mirror vertex's neighbours. Cheap and order-independent.
    long long checks = 0, good = 0;
    for (size_t s = 0; s < subs.size(); ++s) {
        for (size_t i = 0; i < m_vertMirror[s].size(); ++i) {
            const VertRef m = m_vertMirror[s][i];
            if (!m.valid()) continue;
            const int gi = globalId(static_cast<int>(s), static_cast<int>(i));
            const int gm = globalId(m.submesh, m.index);
            for (int nb : adj[gi]) {
                // Find nb's mirror (search its submesh/index back out of global).
                // Global → (submesh,index): linear-free via m_submeshBase.
                int ns = 0;
                while (ns + 1 < static_cast<int>(m_submeshBase.size())
                       && m_submeshBase[ns + 1] <= nb) ++ns;
                const int nidx = nb - m_submeshBase[ns];
                const VertRef nbm = m_vertMirror[ns][nidx];
                if (!nbm.valid()) continue;
                const int gnbm = globalId(nbm.submesh, nbm.index);
                ++checks;
                if (gnbm == gm || adj[gm].count(gnbm)) ++good;
            }
        }
    }
    const float verifyRatio = checks > 0
        ? static_cast<float>(good) / static_cast<float>(checks) : 0.0f;

    // Face lookup keyed by the sorted GLOBAL vertex triple, plus per-face stored
    // corner ids for the barycentric permutation in mirrorDab.
    m_faceCorners.resize(subs.size());
    for (size_t s = 0; s < subs.size(); ++s) {
        const auto& sub = subs[s];
        m_faceCorners[s].resize(sub.triangles.size());
        for (size_t t = 0; t < sub.triangles.size(); ++t) {
            const auto& tri = sub.triangles[t];
            const int g0 = globalId(static_cast<int>(s), static_cast<int>(tri.indices[0]));
            const int g1 = globalId(static_cast<int>(s), static_cast<int>(tri.indices[1]));
            const int g2 = globalId(static_cast<int>(s), static_cast<int>(tri.indices[2]));
            m_faceCorners[s][t] = {g0, g1, g2};
            m_faceByVerts.emplace(triKey(g0, g1, g2),
                                  std::make_pair(static_cast<int>(s), static_cast<int>(t)));
        }
    }

    // Accept only when coverage AND topological verification are strong.
    m_valid = (m_coverage >= 0.6f) && (verifyRatio >= 0.9f);
    return m_valid;
}

bool SymmetryMirrorMap::mirrorDab(int submesh, const int corner[3],
                                  const float bary[3],
                                  int& outSubmesh, int& outTriangle,
                                  float outBary[3]) const
{
    if (!m_valid) return false;
    if (submesh < 0 || submesh >= static_cast<int>(m_vertMirror.size())) return false;

    // Map each primary corner to its mirror vertex (global id).
    int mg[3];
    for (int k = 0; k < 3; ++k) {
        const int idx = corner[k];
        if (idx < 0 || idx >= static_cast<int>(m_vertMirror[submesh].size()))
            return false;
        const VertRef m = m_vertMirror[submesh][idx];
        if (!m.valid()) return false;
        mg[k] = globalId(m.submesh, m.index);
    }

    // Find the mirror triangle by its three mirror-vertex global ids.
    auto it = m_faceByVerts.find(triKey(mg[0], mg[1], mg[2]));
    if (it == m_faceByVerts.end()) return false;
    outSubmesh = it->second.first;
    outTriangle = it->second.second;

    // Permute the barycentric weights to the mirror triangle's STORED corner
    // order: for each stored corner of the mirror face, find which primary
    // corner maps to it and copy that weight. (Reflection reverses winding, so
    // the stored order generally differs from mg[0..2].)
    const std::array<int, 3>& stored =
        m_faceCorners[static_cast<size_t>(outSubmesh)][static_cast<size_t>(outTriangle)];
    for (int j = 0; j < 3; ++j) {
        int src = -1;
        for (int k = 0; k < 3; ++k) if (mg[k] == stored[j]) { src = k; break; }
        if (src < 0) return false;         // degenerate (shared vertex) — bail
        outBary[j] = bary[src];
    }
    return true;
}
