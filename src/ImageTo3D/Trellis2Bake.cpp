#include "Trellis2Bake.h"
#include "MeshRefine.h"
#include "MarchingCubes.h"
#include <array>

#include "MeshSegmenter.h"   // connectedComponents (pure-data union-find)

#include <meshoptimizer.h>
#include <xatlas.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
#include <queue>
#include <set>
#include <unordered_map>
#include <iterator>

namespace Trellis2Bake {

namespace {

// ---- tiny vec3 helpers ------------------------------------------------------
inline void sub3(const float* a, const float* b, float* o)
{ o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
inline float dot3(const float* a, const float* b)
{ return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void cross3(const float* a, const float* b, float* o)
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
inline float len3(const float* a) { return std::sqrt(dot3(a, a)); }
inline void normalize3(float* a)
{
    const float l = len3(a);
    if (l > 1e-20f) { a[0] /= l; a[1] /= l; a[2] /= l; }
}
inline uint8_t toByte(float v)
{ return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

inline uint64_t packKey(int64_t x, int64_t y, int64_t z)
{
    // 21 bits per axis, offset so slightly-negative cells stay valid.
    const uint64_t bias = 1u << 20;
    return ((static_cast<uint64_t>(x + bias) & 0x1FFFFF) << 42)
         | ((static_cast<uint64_t>(y + bias) & 0x1FFFFF) << 21)
         |  (static_cast<uint64_t>(z + bias) & 0x1FFFFF);
}

// The sign the VIEWPORT applies to the bitangent relative to the textbook
// glTF/MikkTSpace convention (B = cross(N, T) * w) that this bake's per-vertex
// `wsign` follows. Ogre's TangentSpaceCalc::calculateParity says it itself:
// "this parity is the reverse of what you'd expect — because the 'V' texture
// coordinate is actually left handed", and the RTSS normal-map stage builds the
// bitangent from that parity. Baking with the textbook sign inverts the green
// channel against what the shader reconstructs, so every chart is lit as if
// its relief were flipped along V: invisible on flat areas, but dark smears
// and planar facets wherever the source tilts away from the coarse target (jaw,
// cheeks, shoulders, folds). Verified in the real viewport on a generated
// character: this sign gives coherent folds, the textbook one gives blotches.
constexpr float kViewportBitangentSign = -1.0f;

// Area-weighted smooth vertex normals.
std::vector<float> smoothNormals(const std::vector<float>& positions,
                                 const std::vector<uint32_t>& indices)
{
    std::vector<float> n(positions.size(), 0.0f);
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const float* p0 = &positions[indices[t + 0] * 3];
        const float* p1 = &positions[indices[t + 1] * 3];
        const float* p2 = &positions[indices[t + 2] * 3];
        float e1[3], e2[3], fn[3];
        sub3(p1, p0, e1);
        sub3(p2, p0, e2);
        cross3(e1, e2, fn);   // length ∝ 2×area — the weighting
        for (int k = 0; k < 3; ++k) {
            n[indices[t + k] * 3 + 0] += fn[0];
            n[indices[t + k] * 3 + 1] += fn[1];
            n[indices[t + k] * 3 + 2] += fn[2];
        }
    }
    for (size_t v = 0; v + 2 < n.size(); v += 3) {
        float* nv = &n[v];
        const float l = len3(nv);
        if (l > 1e-20f) { nv[0] /= l; nv[1] /= l; nv[2] /= l; }
        else { nv[0] = 0.0f; nv[1] = 1.0f; nv[2] = 0.0f; }
    }
    return n;
}

// Smooth vertex normals with POSITION WELDING: vertices at bit-identical
// positions (e.g. xatlas chart-seam splits, marching-cubes duplicates) share
// one accumulated normal, so seams don't read as hard edges in a normal bake.
std::vector<float> smoothNormalsWelded(const std::vector<float>& positions,
                                       const std::vector<uint32_t>& indices)
{
    struct PosKey {
        uint32_t a, b, c;
        bool operator==(const PosKey& o) const
        { return a == o.a && b == o.b && c == o.c; }
    };
    struct PosKeyHash {
        size_t operator()(const PosKey& k) const
        {
            uint64_t h = k.a;
            h = h * 0x9E3779B97F4A7C15ull + k.b;
            h = h * 0x9E3779B97F4A7C15ull + k.c;
            return static_cast<size_t>(h ^ (h >> 32));
        }
    };
    const size_t nv = positions.size() / 3;
    std::unordered_map<PosKey, uint32_t, PosKeyHash> canonOf;
    canonOf.reserve(nv * 2);
    std::vector<uint32_t> canon(nv);
    for (size_t v = 0; v < nv; ++v) {
        PosKey k;
        std::memcpy(&k.a, &positions[v * 3 + 0], 4);
        std::memcpy(&k.b, &positions[v * 3 + 1], 4);
        std::memcpy(&k.c, &positions[v * 3 + 2], 4);
        canon[v] = canonOf.emplace(k, static_cast<uint32_t>(v)).first->second;
    }
    std::vector<float> acc(positions.size(), 0.0f);
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const float* p0 = &positions[indices[t + 0] * 3];
        const float* p1 = &positions[indices[t + 1] * 3];
        const float* p2 = &positions[indices[t + 2] * 3];
        float e1[3], e2[3], fn[3];
        sub3(p1, p0, e1);
        sub3(p2, p0, e2);
        cross3(e1, e2, fn);
        for (int k = 0; k < 3; ++k) {
            const uint32_t cv = canon[indices[t + k]];
            acc[cv * 3 + 0] += fn[0];
            acc[cv * 3 + 1] += fn[1];
            acc[cv * 3 + 2] += fn[2];
        }
    }
    std::vector<float> n(positions.size());
    for (size_t v = 0; v < nv; ++v) {
        float nv3[3] = {acc[canon[v] * 3 + 0], acc[canon[v] * 3 + 1],
                        acc[canon[v] * 3 + 2]};
        const float l = len3(nv3);
        if (l > 1e-20f) { nv3[0] /= l; nv3[1] /= l; nv3[2] /= l; }
        else { nv3[0] = 0.0f; nv3[1] = 1.0f; nv3[2] = 0.0f; }
        std::memcpy(&n[v * 3], nv3, sizeof(nv3));
    }
    return n;
}

// Laplacian-smooth a per-vertex normal FIELD over the position-welded vertex
// adjacency (renormalizing each pass). Raw dual-grid surfaces carry
// voxel-scale normal noise; baked as a detail normal it reads as glittery
// specular speckle. A few averaging passes flatten the noise while the
// underlying geometry (and therefore real relief) is untouched.
std::vector<float> smoothNormalField(std::vector<float> normals,
                                     const std::vector<float>& positions,
                                     const std::vector<uint32_t>& indices,
                                     int iterations)
{
    if (iterations <= 0 || normals.size() != positions.size())
        return normals;
    const size_t nv = positions.size() / 3;
    // Weld by bit-identical position so seam-split vertices smooth together.
    struct PosKey {
        uint32_t a, b, c;
        bool operator==(const PosKey& o) const
        { return a == o.a && b == o.b && c == o.c; }
    };
    struct PosKeyHash {
        size_t operator()(const PosKey& k) const
        {
            uint64_t h = k.a;
            h = h * 0x9E3779B97F4A7C15ull + k.b;
            h = h * 0x9E3779B97F4A7C15ull + k.c;
            return static_cast<size_t>(h ^ (h >> 32));
        }
    };
    std::unordered_map<PosKey, uint32_t, PosKeyHash> canonOf;
    canonOf.reserve(nv * 2);
    std::vector<uint32_t> canon(nv);
    for (size_t v = 0; v < nv; ++v) {
        PosKey k;
        std::memcpy(&k.a, &positions[v * 3 + 0], 4);
        std::memcpy(&k.b, &positions[v * 3 + 1], 4);
        std::memcpy(&k.c, &positions[v * 3 + 2], 4);
        canon[v] = canonOf.emplace(k, static_cast<uint32_t>(v)).first->second;
    }
    // Canonical edge list (deduped implicitly by symmetric accumulation).
    std::vector<float> acc;
    for (int it = 0; it < iterations; ++it) {
        acc.assign(positions.size(), 0.0f);
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            for (int k = 0; k < 3; ++k) {
                const uint32_t a = canon[indices[t + k]];
                const uint32_t b = canon[indices[t + (k + 1) % 3]];
                if (a == b)
                    continue;
                for (int c = 0; c < 3; ++c) {
                    acc[a * 3 + c] += normals[b * 3 + c];
                    acc[b * 3 + c] += normals[a * 3 + c];
                }
            }
        }
        // Two-phase update: resolve every canonical slot from the
        // PRE-iteration values into a scratch buffer first, then broadcast.
        // Writing normals[] in place while later weld-group members still
        // read their canonical slot would hand them the already-normalized
        // post-update value (canon[v] is the smallest index of the group,
        // so it is always updated first) — split vertices would diverge
        // from their canonical vertex, compounding per iteration.
        std::vector<float> next(positions.size());
        for (size_t v = 0; v < nv; ++v) {
            if (canon[v] != v)
                continue;   // canonical slots only
            float nn[3] = {normals[v * 3 + 0] + acc[v * 3 + 0],
                           normals[v * 3 + 1] + acc[v * 3 + 1],
                           normals[v * 3 + 2] + acc[v * 3 + 2]};
            const float l = len3(nn);
            if (l > 1e-20f) {
                nn[0] /= l; nn[1] /= l; nn[2] /= l;
            } else {
                nn[0] = normals[v * 3 + 0];
                nn[1] = normals[v * 3 + 1];
                nn[2] = normals[v * 3 + 2];
            }
            std::memcpy(&next[v * 3], nn, sizeof(nn));
        }
        for (size_t v = 0; v < nv; ++v)
            std::memcpy(&normals[v * 3], &next[canon[v] * 3],
                        sizeof(float) * 3);
    }
    return normals;
}

// ---- sparse uniform grid over source triangles for closest-point queries ---
class TriangleGrid {
public:
    void build(const std::vector<float>& positions,
               const std::vector<uint32_t>& indices)
    {
        m_positions = &positions;
        m_indices = &indices;
        m_triCount = static_cast<int>(indices.size() / 3);
        float mn[3] = {std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max()};
        float mx[3] = {-mn[0], -mn[1], -mn[2]};
        const size_t nv = positions.size() / 3;
        for (size_t v = 0; v < nv; ++v) {
            for (int k = 0; k < 3; ++k) {
                mn[k] = std::min(mn[k], positions[v * 3 + k]);
                mx[k] = std::max(mx[k], positions[v * 3 + k]);
            }
        }
        float diag[3] = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};
        const float d = std::max(1e-6f, len3(diag));
        // Cell size tracks triangle density: a fixed coarse grid puts
        // hundreds of triangles in every cell on multi-million-triangle
        // TRELLIS sources and each texel query then tests them all — the
        // bake went from seconds to tens of minutes. ~2 cells per average
        // triangle edge keeps candidate sets small at any density.
        const int gridN = std::clamp(
            static_cast<int>(2.0 * std::cbrt(static_cast<double>(
                std::max(1, m_triCount)))), 64, 256);
        m_cell = d / static_cast<float>(gridN);
        for (int k = 0; k < 3; ++k) m_min[k] = mn[k];
        for (int k = 0; k < 3; ++k) {
            m_lo[k] = cellIndex(mn[k], k);
            m_hi[k] = cellIndex(mx[k], k);
        }
        m_cells.reserve(static_cast<size_t>(m_triCount) * 2);
        for (int t = 0; t < m_triCount; ++t) {
            const float* p[3] = {&positions[indices[t * 3 + 0] * 3],
                                 &positions[indices[t * 3 + 1] * 3],
                                 &positions[indices[t * 3 + 2] * 3]};
            int lo[3], hi[3];
            for (int k = 0; k < 3; ++k) {
                const float a = std::min({p[0][k], p[1][k], p[2][k]});
                const float b = std::max({p[0][k], p[1][k], p[2][k]});
                lo[k] = cellIndex(a, k);
                hi[k] = cellIndex(b, k);
            }
            for (int x = lo[0]; x <= hi[0]; ++x)
                for (int y = lo[1]; y <= hi[1]; ++y)
                    for (int z = lo[2]; z <= hi[2]; ++z)
                        m_cells[packKey(x, y, z)].push_back(t);
        }
    }

    // Closest point on the whole surface. Returns triangle index (or -1 for
    // an empty mesh) and fills the closest point + barycentrics.
    int closest(const float p[3], float outPoint[3], float outBary[3],
                const float* towardNormal = nullptr) const
    {
        // towardNormal (optional): reject candidate triangles whose face
        // normal opposes it (dot < -0.2). The bake queries the DENSE source
        // from a SIMPLIFIED surface point; on detailed regions the plain
        // nearest triangle can belong to a different nearby surface (the
        // back of an ear, the inside of a mouth) whose color then lands as
        // a dark chip exactly where the model is most detailed. Filtering
        // by facing keeps the sample on "our" side; the caller falls back
        // to the unfiltered query when nothing on-side is found.
        if (m_triCount == 0)
            return -1;
        const int cx = cellIndex(p[0], 0);
        const int cy = cellIndex(p[1], 1);
        const int cz = cellIndex(p[2], 2);
        float bestD2 = std::numeric_limits<float>::max();
        int bestTri = -1;
        // NB deliberately NO per-query visited-triangle dedup: the old
        // mutable stamp array made closest() single-threaded, and with the
        // density-scaled grid a triangle spans so few cells that the odd
        // duplicate distance test is cheaper than any synchronisation.
        // closest() is const + thread-safe — the parallel bake depends on it.
        auto visitCell = [&](int x, int y, int z) {
            const auto it = m_cells.find(packKey(x, y, z));
            if (it == m_cells.end())
                return;
            for (int t : it->second) {
                const uint32_t* idx = &(*m_indices)[t * 3];
                const float* a = &(*m_positions)[idx[0] * 3];
                const float* b = &(*m_positions)[idx[1] * 3];
                const float* c = &(*m_positions)[idx[2] * 3];
                if (towardNormal) {
                    float e1[3], e2[3], fn[3];
                    sub3(b, a, e1);
                    sub3(c, a, e2);
                    cross3(e1, e2, fn);
                    const float l = len3(fn);
                    if (l > 1e-20f
                        && dot3(fn, towardNormal) < -0.2f * l)
                        continue;
                }
                float cp[3], bc[3];
                closestPointOnTriangle(a, b, c, p, cp, bc);
                float dvec[3];
                sub3(cp, p, dvec);
                const float d2 = dot3(dvec, dvec);
                if (d2 < bestD2) {
                    bestD2 = d2;
                    bestTri = t;
                    std::memcpy(outPoint, cp, sizeof(cp));
                    std::memcpy(outBary, bc, sizeof(bc));
                }
            }
        };
        // Rings beyond the occupied grid bounds contain nothing — cap there.
        const int maxRing = std::max({std::abs(cx - m_lo[0]), std::abs(m_hi[0] - cx),
                                      std::abs(cy - m_lo[1]), std::abs(m_hi[1] - cy),
                                      std::abs(cz - m_lo[2]), std::abs(m_hi[2] - cz)})
                            + 1;
        for (int r = 0; r <= maxRing; ++r) {
            // Once we have a hit, stop when the ring's nearest possible
            // distance already exceeds the best.
            if (bestTri >= 0) {
                const float ringMin = (r - 1) * m_cell;
                if (ringMin > 0.0f && ringMin * ringMin > bestD2)
                    break;
            }
            // True O(r^2) shell iteration — the 6 faces of the ring cube.
            // (An earlier full-cube scan with a shell test was O(r^3) per
            // ring and cratered on texels far from the source surface.)
            if (r == 0) {
                visitCell(cx, cy, cz);
                continue;
            }
            for (int dx = -r; dx <= r; ++dx) {
                for (int dy = -r; dy <= r; ++dy) {
                    visitCell(cx + dx, cy + dy, cz - r);
                    visitCell(cx + dx, cy + dy, cz + r);
                }
                for (int dz = -r + 1; dz <= r - 1; ++dz) {
                    visitCell(cx + dx, cy - r, cz + dz);
                    visitCell(cx + dx, cy + r, cz + dz);
                }
            }
            for (int dy = -r + 1; dy <= r - 1; ++dy) {
                for (int dz = -r + 1; dz <= r - 1; ++dz) {
                    visitCell(cx - r, cy + dy, cz + dz);
                    visitCell(cx + r, cy + dy, cz + dz);
                }
            }
        }
        if (bestTri < 0) {
            // Grid rings exhausted without a candidate (degenerate layout) —
            // brute-force so the bake never silently fails.
            for (int t = 0; t < m_triCount; ++t) {
                const uint32_t* idx = &(*m_indices)[t * 3];
                float cp[3], bc[3];
                closestPointOnTriangle(&(*m_positions)[idx[0] * 3],
                                       &(*m_positions)[idx[1] * 3],
                                       &(*m_positions)[idx[2] * 3], p, cp, bc);
                float dvec[3];
                sub3(cp, p, dvec);
                const float d2 = dot3(dvec, dvec);
                if (d2 < bestD2) {
                    bestD2 = d2;
                    bestTri = t;
                    std::memcpy(outPoint, cp, sizeof(cp));
                    std::memcpy(outBary, bc, sizeof(bc));
                }
            }
        }
        return bestTri;
    }

private:
    int cellIndex(float v, int axis) const
    {
        return static_cast<int>(std::floor((v - m_min[axis]) / m_cell));
    }

    const std::vector<float>* m_positions = nullptr;
    const std::vector<uint32_t>* m_indices = nullptr;
    int m_triCount = 0;
    float m_min[3] = {0, 0, 0};
    int m_lo[3] = {0, 0, 0};
    int m_hi[3] = {0, 0, 0};
    float m_cell = 1.0f;
    std::unordered_map<uint64_t, std::vector<int32_t>> m_cells;
};

} // namespace

// ---- closest point on triangle (Ericson, RTCD §5.1.5) -----------------------
void closestPointOnTriangle(const float a[3], const float b[3],
                            const float c[3], const float p[3],
                            float outClosest[3], float outBary[3])
{
    float ab[3], ac[3], ap[3];
    sub3(b, a, ab);
    sub3(c, a, ac);
    sub3(p, a, ap);
    const float d1 = dot3(ab, ap);
    const float d2 = dot3(ac, ap);
    auto emitBary = [&](float u, float v, float w) {
        outBary[0] = u; outBary[1] = v; outBary[2] = w;
        for (int k = 0; k < 3; ++k)
            outClosest[k] = u * a[k] + v * b[k] + w * c[k];
    };
    if (d1 <= 0.0f && d2 <= 0.0f) { emitBary(1, 0, 0); return; }

    float bp[3];
    sub3(p, b, bp);
    const float d3 = dot3(ab, bp);
    const float d4 = dot3(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) { emitBary(0, 1, 0); return; }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        emitBary(1.0f - v, v, 0.0f);
        return;
    }

    float cp[3];
    sub3(p, c, cp);
    const float d5 = dot3(ab, cp);
    const float d6 = dot3(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) { emitBary(0, 0, 1); return; }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        emitBary(1.0f - w, 0.0f, w);
        return;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        emitBary(0.0f, 1.0f - w, w);
        return;
    }

    const float denom = 1.0f / (va + vb + vc);
    const float v = vb * denom;
    const float w = vc * denom;
    emitBary(1.0f - v - w, v, w);
}

// ---- SparseVolumeSampler -----------------------------------------------------
void SparseVolumeSampler::build(const uint32_t* coords, const uint8_t* attrs,
                                int count, float voxelSize,
                                const float origin[3])
{
    m_map.clear();
    m_attrs = attrs;
    m_count = count;
    m_voxelSize = voxelSize > 0.0f ? voxelSize : 1.0f;
    for (int k = 0; k < 3; ++k)
        m_origin[k] = origin[k];
    m_map.reserve(static_cast<size_t>(count) * 2);
    for (int i = 0; i < count; ++i)
        m_map.emplace(packKey(coords[i * 3 + 0], coords[i * 3 + 1],
                              coords[i * 3 + 2]), i);
}

bool SparseVolumeSampler::sample(const float p[3], float out[6]) const
{
    // Neutral defaults (mid gray, dielectric, matte, opaque).
    out[0] = out[1] = out[2] = 0.5f;
    out[3] = 0.0f;
    out[4] = 0.8f;
    out[5] = 1.0f;
    if (m_count <= 0)
        return false;

    // Continuous voxel coordinate with voxel CENTRE at ijk + 0.5.
    float g[3];
    for (int k = 0; k < 3; ++k)
        g[k] = (p[k] - m_origin[k]) / m_voxelSize - 0.5f;
    const int64_t bx = static_cast<int64_t>(std::floor(g[0]));
    const int64_t by = static_cast<int64_t>(std::floor(g[1]));
    const int64_t bz = static_cast<int64_t>(std::floor(g[2]));
    const float fx = g[0] - bx, fy = g[1] - by, fz = g[2] - bz;

    // ALPHA-WEIGHTED trilinear for the colour/material channels: TRELLIS
    // wraps the surface in a two-layer band — an inner layer carrying the
    // real attributes (alpha≈1) and an OUTER transparent-black "air" layer
    // (alpha≈0). Bake texels sit between the layers, so a plain trilinear
    // average mixes the surface colour ~50/50 with transparent black and the
    // result renders as dark mottling (measured on a real generation: 45% of
    // occupied voxels are the alpha≈0 skin). Weighting by voxel alpha is the
    // standard premultiplied fix; the alpha CHANNEL itself stays plainly
    // interpolated so genuine transparency still comes through.
    float acc[6] = {0, 0, 0, 0, 0, 0};
    float wsum = 0.0f;   // plain trilinear weight (alpha channel)
    float wa = 0.0f;     // alpha-weighted (colour/material channels)
    for (int dx = 0; dx <= 1; ++dx) {
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dz = 0; dz <= 1; ++dz) {
                const auto it = m_map.find(packKey(bx + dx, by + dy, bz + dz));
                if (it == m_map.end())
                    continue;
                const float w = (dx ? fx : 1.0f - fx)
                              * (dy ? fy : 1.0f - fy)
                              * (dz ? fz : 1.0f - fz);
                if (w <= 0.0f)
                    continue;
                const uint8_t* row = m_attrs + static_cast<size_t>(it->second) * 6;
                const float va = row[5] / 255.0f;
                for (int c = 0; c < 5; ++c)
                    acc[c] += w * va * (row[c] / 255.0f);
                acc[5] += w * va;
                wsum += w;
                wa += w * va;
            }
        }
    }
    // Accept the trilinear result only when the neighbourhood carries REAL
    // coverage. The transparent skin is not alpha == 0 but alpha ≈ 2/255, so
    // a pure-skin neighbourhood still produces wa ≈ 0.007·wsum — a tiny
    // absolute threshold happily "alpha-weighted" black skin against nothing
    // (measured: ~1/3 of bake texels). Coverage below 25% falls through to
    // the nearest-OPAQUE search instead.
    if (wsum > 1e-6f && wa > 0.25f * wsum) {
        for (int c = 0; c < 5; ++c)
            out[c] = acc[c] / wa;
        out[5] = acc[5] / wsum;   // true coverage
        return true;
    }

    // Nearest occupied voxel in growing Chebyshev shells (surface points can
    // land just outside the occupied band after simplification).
    const int64_t rx = static_cast<int64_t>(std::llround(g[0]));
    const int64_t ry = static_cast<int64_t>(std::llround(g[1]));
    const int64_t rz = static_cast<int64_t>(std::llround(g[2]));
    // Nearest OPAQUE voxel only. TRELLIS can mark whole surface patches
    // transparent with the real colour several voxels deeper (measured skin
    // thickness up to ~3-5 voxels on real generations) — a transparent hit
    // carries no usable colour, so the skin is excluded outright and the
    // search reaches deep enough to cross it.
    for (int r = 1; r <= 8; ++r) {
        int best = -1;
        float bestD2 = std::numeric_limits<float>::max();
        for (int64_t x = rx - r; x <= rx + r; ++x) {
            for (int64_t y = ry - r; y <= ry + r; ++y) {
                for (int64_t z = rz - r; z <= rz + r; ++z) {
                    if (std::max({std::llabs(x - rx), std::llabs(y - ry),
                                  std::llabs(z - rz)}) != r)
                        continue;
                    const auto it = m_map.find(packKey(x, y, z));
                    if (it == m_map.end())
                        continue;
                    const float va = m_attrs[static_cast<size_t>(it->second) * 6 + 5]
                                     / 255.0f;
                    if (va < 0.25f)
                        continue;   // skin — no usable colour
                    const float d2 = float(x - g[0]) * float(x - g[0])
                                   + float(y - g[1]) * float(y - g[1])
                                   + float(z - g[2]) * float(z - g[2]);
                    if (d2 < bestD2) {
                        bestD2 = d2;
                        best = it->second;
                    }
                }
            }
        }
        if (best >= 0) {
            const uint8_t* row = m_attrs + static_cast<size_t>(best) * 6;
            for (int c = 0; c < 6; ++c)
                out[c] = row[c] / 255.0f;
            return true;
        }
    }
    return false;
}

// ---- unifyWinding --------------------------------------------------------------
int unifyWinding(const std::vector<float>& positions,
                 std::vector<uint32_t>& indices)
{
    const size_t faceCount = indices.size() / 3;
    if (faceCount == 0)
        return 0;
    // Undirected edge -> up to two (face, direction) uses. Edges used by 3+
    // faces are non-manifold and excluded from propagation.
    struct EdgeUse { int32_t face[2]; uint8_t dir[2]; uint8_t n; };
    std::unordered_map<uint64_t, EdgeUse> edges;
    edges.reserve(indices.size());
    auto edgeKey = [](uint32_t a, uint32_t b) {
        const uint32_t lo = std::min(a, b), hi = std::max(a, b);
        return (static_cast<uint64_t>(lo) << 32) | hi;
    };
    for (size_t f = 0; f < faceCount; ++f) {
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = indices[f * 3 + k];
            const uint32_t b = indices[f * 3 + (k + 1) % 3];
            if (a == b)
                continue;
            EdgeUse& e = edges[edgeKey(a, b)];
            if (e.n < 2) {
                e.face[e.n] = static_cast<int32_t>(f);
                e.dir[e.n] = a < b ? 0 : 1;
            }
            if (e.n < 255)
                ++e.n;
        }
    }

    // BFS: consistent orientation means the two faces traverse the shared
    // edge in OPPOSITE directions (after accounting for flips applied so far).
    std::vector<uint8_t> state(faceCount, 0);   // 0 unvisited, 1 keep, 2 flip
    std::vector<uint32_t> queue;
    std::vector<uint32_t> component;
    std::vector<int32_t> compOf(faceCount, -1);
    std::vector<std::vector<uint32_t>> comps;
    int flipped = 0;
    for (size_t seed = 0; seed < faceCount; ++seed) {
        if (state[seed])
            continue;
        state[seed] = 1;
        queue.clear();
        component.clear();
        queue.push_back(static_cast<uint32_t>(seed));
        component.push_back(static_cast<uint32_t>(seed));
        while (!queue.empty()) {
            const uint32_t f = queue.back();
            queue.pop_back();
            const bool fFlip = state[f] == 2;
            for (int k = 0; k < 3; ++k) {
                const uint32_t a = indices[f * 3 + k];
                const uint32_t b = indices[f * 3 + (k + 1) % 3];
                if (a == b)
                    continue;
                const auto it = edges.find(edgeKey(a, b));
                if (it == edges.end() || it->second.n != 2)
                    continue;   // border or non-manifold — no propagation
                const EdgeUse& e = it->second;
                const int slot = e.face[0] == static_cast<int32_t>(f) ? 0 : 1;
                const int32_t g = e.face[1 - slot];
                if (g < 0 || state[g])
                    continue;
                // Effective directions after the flips chosen so far: a flip
                // reverses every edge direction of the face.
                const bool dirF = (e.dir[slot] != 0) != fFlip;
                const bool dirG = (e.dir[1 - slot] != 0);
                // Consistent when directions differ; if they'd match, flip g.
                const bool gFlip = (dirG == dirF);
                state[g] = gFlip ? 2 : 1;
                queue.push_back(static_cast<uint32_t>(g));
                component.push_back(static_cast<uint32_t>(g));
            }
        }
        // Orient the whole component OUTWARD. Signed volume works for closed
        // shells but is meaningless for the open sheets and small islands a
        // non-manifold source shatters into (BFS can't cross non-manifold
        // edges, so complex organic decodes yield MANY islands — misoriented
        // small ones rendered as holes on real generations). The centroid
        // heuristic — do face normals point away from the island's own
        // centroid relative to the global centroid? — behaves like signed
        // volume on closed shells and stays meaningful on patches: score =
        // Σ area·dot(n̂, faceCentroid − globalCentroid).
        double cx = 0, cy = 0, cz = 0;
        {
            size_t n = 0;
            for (uint32_t f : component) {
                for (int k = 0; k < 3; ++k) {
                    const float* pv = &positions[indices[f * 3 + k] * 3];
                    cx += pv[0]; cy += pv[1]; cz += pv[2];
                }
                n += 3;
            }
            if (n) { cx /= n; cy /= n; cz /= n; }
        }
        double score = 0.0;
        for (uint32_t f : component) {
            const float* p0 = &positions[indices[f * 3 + 0] * 3];
            const float* p1 = &positions[indices[f * 3 + 1] * 3];
            const float* p2 = &positions[indices[f * 3 + 2] * 3];
            float e1[3], e2[3], fn[3];
            sub3(p1, p0, e1);
            sub3(p2, p0, e2);
            cross3(e1, e2, fn);   // length ∝ area
            const double gx = (p0[0] + p1[0] + p2[0]) / 3.0 - cx;
            const double gy = (p0[1] + p1[1] + p2[1]) / 3.0 - cy;
            const double gz = (p0[2] + p1[2] + p2[2]) / 3.0 - cz;
            double v = fn[0] * gx + fn[1] * gy + fn[2] * gz;
            if (state[f] == 2)
                v = -v;
            score += v;
        }
        const bool flipComponent = score < 0.0;
        for (uint32_t f : component) {
            const bool doFlip = (state[f] == 2) != flipComponent;
            if (doFlip) {
                std::swap(indices[f * 3 + 1], indices[f * 3 + 2]);
                ++flipped;
            }
            compOf[f] = static_cast<int32_t>(comps.size());
        }
        comps.push_back(component);
    }

    // ---- Phase 2: re-orient SMALL islands against the dominant surface ------
    // A fuzzy voxel decode (fur, hair, foliage) shatters into hundreds of
    // 1-3-triangle wisp islands attached to the body only through
    // non-manifold edges or shared vertices — BFS can't reach them, and for
    // a near-flat wisp the centroid score above is ~0, a coin flip (measured
    // on a real generation: 34% of small-island faces flipped → rendered as
    // dark pepper speckle under backface culling). Re-orient each small
    // island to agree with the LARGEST island's smooth normal field at the
    // vertices they share; islands sharing nothing keep the centroid choice.
    if (comps.size() > 1) {
        size_t largest = 0;
        for (size_t c = 1; c < comps.size(); ++c)
            if (comps[c].size() > comps[largest].size())
                largest = c;
        const size_t smallLimit = std::max<size_t>(100, faceCount / 100);
        if (comps[largest].size() > smallLimit) {
            // Area-weighted vertex normals of the dominant island (current,
            // post-phase-1 winding).
            std::unordered_map<uint32_t, std::array<float, 3>> bigN;
            for (uint32_t f : comps[largest]) {
                const float* p0 = &positions[indices[f * 3 + 0] * 3];
                const float* p1 = &positions[indices[f * 3 + 1] * 3];
                const float* p2 = &positions[indices[f * 3 + 2] * 3];
                float e1[3], e2[3], fn[3];
                sub3(p1, p0, e1);
                sub3(p2, p0, e2);
                cross3(e1, e2, fn);
                for (int k = 0; k < 3; ++k) {
                    auto& n = bigN[indices[f * 3 + k]];
                    n[0] += fn[0]; n[1] += fn[1]; n[2] += fn[2];
                }
            }
            for (size_t c = 0; c < comps.size(); ++c) {
                if (c == largest || comps[c].size() > smallLimit)
                    continue;
                double agree = 0.0;
                for (uint32_t f : comps[c]) {
                    const float* p0 = &positions[indices[f * 3 + 0] * 3];
                    const float* p1 = &positions[indices[f * 3 + 1] * 3];
                    const float* p2 = &positions[indices[f * 3 + 2] * 3];
                    float e1[3], e2[3], fn[3];
                    sub3(p1, p0, e1);
                    sub3(p2, p0, e2);
                    cross3(e1, e2, fn);
                    for (int k = 0; k < 3; ++k) {
                        const auto it = bigN.find(indices[f * 3 + k]);
                        if (it == bigN.end())
                            continue;
                        agree += fn[0] * it->second[0]
                               + fn[1] * it->second[1]
                               + fn[2] * it->second[2];
                    }
                }
                if (agree < 0.0) {
                    for (uint32_t f : comps[c]) {
                        std::swap(indices[f * 3 + 1], indices[f * 3 + 2]);
                        ++flipped;
                    }
                }
            }
        }
    }
    return flipped;
}

// ---- remeshManifold ------------------------------------------------------------
bool remeshManifold(const std::vector<float>& positions,
                    const std::vector<uint32_t>& indices, int resolution,
                    std::vector<float>& outPositions,
                    std::vector<uint32_t>& outIndices)
{
    const size_t nv = positions.size() / 3;
    if (nv == 0 || indices.size() < 3)
        return false;
    resolution = std::clamp(resolution, 32, 2048);
    float mn[3] = {positions[0], positions[1], positions[2]};
    float mx[3] = {mn[0], mn[1], mn[2]};
    for (size_t v = 0; v < nv; ++v)
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], positions[v * 3 + k]);
            mx[k] = std::max(mx[k], positions[v * 3 + k]);
        }
    const float ext = std::max({mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]});
    if (!(ext > 0.0f))
        return false;
    constexpr int kPad = 3;   // empty border so the exterior is connected
    // The grid is cropped to the bounding box, so a tall thin figure at 1536
    // is ~870M cells but a roughly cubic object (a building) would be ~3.6G.
    // Cap the cell count (5 bytes each while building, so ~4.5 GB at the
    // default) by lowering the resolution rather than failing or thrashing.
    // QTMESH_TRELLIS2_REMESH_MAX_CELLS (millions) overrides the cap.
    double kMaxCells = 900e6;
    {
        const int envMax = qEnvironmentVariableIntValue("QTMESH_TRELLIS2_REMESH_MAX_CELLS");
        if (envMax > 0)
            kMaxCells = double(envMax) * 1e6;
        // cell indices are uint32 (flood-fill stack, marching cubes)
        kMaxCells = std::min(kMaxCells, 4.0e9);
    }
    float cell = 0.0f;
    int dim[3];
    for (;;) {
        cell = ext / float(resolution);
        for (int k = 0; k < 3; ++k)
            dim[k] = int(std::ceil((mx[k] - mn[k]) / cell)) + 2 * kPad + 1;
        if (double(dim[0]) * dim[1] * dim[2] <= kMaxCells || resolution <= 64)
            break;
        resolution = std::max(64, int(resolution * 0.9));
    }
    const size_t total = size_t(dim[0]) * dim[1] * dim[2];
    auto at = [&](int x, int y, int z) {
        return (size_t(z) * dim[1] + y) * dim[0] + x;
    };
    // grid origin: cell (0,0,0) lower corner
    const float org[3] = {mn[0] - kPad * cell, mn[1] - kPad * cell,
                          mn[2] - kPad * cell};

    // 1. signed distance to the source surface, exact within one cell of it.
    //    The old binary "touched voxel" mask + 3x3x3 blur dilated every
    //    surface by ~1 cell, which fused fingers and rounded off eyes/ears;
    //    a distance field lets marching cubes place the surface sub-voxel.
    //    d holds a SIGNED distance (inside positive, sign from the nearest
    //    triangle's normal); untouched cells keep +kFar and are classified by
    //    the flood fill below. Worker threads own disjoint z slabs, so no
    //    atomics are needed.
    constexpr float kFar = 1.0e9f;
    constexpr float kBand = 1.0f;   // cells; exact distance wanted this close
    std::vector<float> d(total, kFar);
    {
        const unsigned nThreads = std::max(1u, std::min(16u,
                                  std::thread::hardware_concurrency()));
        const int slab = (dim[2] + int(nThreads) - 1) / int(nThreads);
        auto work = [&](int z0, int z1) {
            for (size_t t = 0; t + 2 < indices.size(); t += 3) {
                const float* p0 = &positions[indices[t] * 3];
                const float* p1 = &positions[indices[t + 1] * 3];
                const float* p2 = &positions[indices[t + 2] * 3];
                int lo[3], hi[3];
                for (int k = 0; k < 3; ++k) {
                    const float a = std::min({p0[k], p1[k], p2[k]});
                    const float b = std::max({p0[k], p1[k], p2[k]});
                    lo[k] = std::max(0, int(std::floor((a - org[k]) / cell - kBand)));
                    hi[k] = std::min(dim[k] - 1,
                                     int(std::floor((b - org[k]) / cell + kBand)) + 1);
                }
                lo[2] = std::max(lo[2], z0);
                hi[2] = std::min(hi[2], z1 - 1);
                if (lo[2] > hi[2])
                    continue;
                float e1[3], e2[3], n[3];
                sub3(p1, p0, e1);
                sub3(p2, p0, e2);
                cross3(e1, e2, n);
                const float nl = std::sqrt(dot3(n, n));
                if (!(nl > 0.0f))
                    continue;   // degenerate triangle
                for (int k = 0; k < 3; ++k)
                    n[k] /= nl;
                const float reach = kBand * cell;
                for (int z = lo[2]; z <= hi[2]; ++z)
                    for (int y = lo[1]; y <= hi[1]; ++y)
                        for (int x = lo[0]; x <= hi[0]; ++x) {
                            const float c[3] = {org[0] + (x + 0.5f) * cell,
                                                org[1] + (y + 0.5f) * cell,
                                                org[2] + (z + 0.5f) * cell};
                            float cp0[3];
                            sub3(c, p0, cp0);
                            // plane distance is a lower bound on the true one
                            if (std::fabs(dot3(cp0, n)) > reach)
                                continue;
                            float cp[3], bc[3];
                            closestPointOnTriangle(p0, p1, p2, c, cp, bc);
                            float off[3];
                            sub3(c, cp, off);
                            const float dist = std::sqrt(dot3(off, off));
                            float& cur = d[at(x, y, z)];
                            if (dist >= std::fabs(cur))
                                continue;
                            // outside = on the normal's side of the surface
                            cur = dot3(off, n) > 0.0f ? -dist : dist;
                        }
            }
        };
        std::vector<std::thread> pool;
        for (unsigned i = 0; i < nThreads; ++i) {
            const int z0 = int(i) * slab;
            const int z1 = std::min(dim[2], z0 + slab);
            if (z0 < z1)
                pool.emplace_back(work, z0, z1);
        }
        for (auto& th : pool)
            th.join();
    }

    // 2. flood-fill the exterior from the padded corner (6-connectivity),
    //    never entering a cell closer than kBlock cells to the surface. A
    //    surface crossing the face between two 6-neighbours puts one of them
    //    within 0.5 cell of it, so 0.75 cannot leak.
    constexpr float kBlock = 0.75f;
    std::vector<uint8_t> outside(total, 0);
    std::vector<uint32_t> stack;
    stack.push_back(0);
    outside[0] = 1;
    while (!stack.empty()) {
        const uint32_t i = stack.back();
        stack.pop_back();
        const int x = int(i % dim[0]);
        const int y = int((i / dim[0]) % dim[1]);
        const int z = int(i / (size_t(dim[0]) * dim[1]));
        const int nb[6][3] = {{x - 1, y, z}, {x + 1, y, z}, {x, y - 1, z},
                              {x, y + 1, z}, {x, y, z - 1}, {x, y, z + 1}};
        for (const auto& q : nb) {
            if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[0] >= dim[0]
                || q[1] >= dim[1] || q[2] >= dim[2])
                continue;
            const size_t qi = at(q[0], q[1], q[2]);
            if (outside[qi] || std::fabs(d[qi]) * (1.0f / cell) < kBlock)
                continue;
            outside[qi] = 1;
            stack.push_back(static_cast<uint32_t>(qi));
        }
    }

    // 3. inside-positive field, zero at kInflate cells OUTSIDE the surface.
    //    The inflation is what keeps a zero-thickness sheet (leaf, cloth, a
    //    pinched double wall) alive: it becomes a slab 2*kInflate thick, and
    //    >= 1 cell thick means a sample always lands inside it. Everything
    //    else is only inflated by that same small amount (it used to be
    //    ~1 cell plus a blur).
    constexpr float kInflate = 0.55f;
    constexpr float kClamp = 1.5f;
    // In place: d becomes the field (one float per cell, no second array), so
    // the peak is 5 bytes per cell — it is what lets the grid go to 1024.
    std::vector<float>& field = d;
    for (size_t i = 0; i < total; ++i) {
        const float di = d[i];
        const float ad = std::min(std::fabs(di) / cell, kClamp);
        float g;
        if (outside[i])
            g = kInflate - ad;                       // definitely outside
        else if (std::fabs(di) >= kFar * 0.5f)
            g = kClamp;                              // enclosed, far from any surface
        else
            g = kInflate + (di > 0.0f ? ad : -ad);   // near surface: normal's sign
        field[i] = g;
    }
    outside.clear(); outside.shrink_to_fit();

    // 4. marching cubes. Sample corner (i,j,k) sits at the CENTRE of cell
    //    (i,j,k): world = org + (i+0.5)*cell.
    const std::array<float, 3> gmin = {org[0] + 0.5f * cell, org[1] + 0.5f * cell,
                                       org[2] + 0.5f * cell};
    const std::array<float, 3> gmax = {
        org[0] + (dim[0] - 0.5f) * cell, org[1] + (dim[1] - 0.5f) * cell,
        org[2] + (dim[2] - 0.5f) * cell};
    MarchingCubes::Mesh m = MarchingCubes::extract(
        field.data(), dim[0], dim[1], dim[2], 0.0f, gmin, gmax, total);
    if (m.triangleCount <= 0)
        return false;
    // extract() already emits outward-facing winding for an inside-positive
    // field (it flips v1/v2 internally).
    outPositions = std::move(m.positions);
    outIndices = std::move(m.indices);
    return true;
}

// ---- simplifyManifold ----------------------------------------------------------
// Garland-Heckbert quadric edge collapse with the LINK CONDITION enforced, so a
// closed manifold input stays a closed manifold (meshopt_simplify does not
// check it: on thin sheets it pinches edges shared by 3+ faces).
namespace {
struct Quadric {
    double a[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};   // xx xy xz xw yy yz yw zz zw ww
    void add(const Quadric& o) { for (int i = 0; i < 10; ++i) a[i] += o.a[i]; }
    void addPlane(double nx, double ny, double nz, double d, double w) {
        a[0] += w * nx * nx; a[1] += w * nx * ny; a[2] += w * nx * nz; a[3] += w * nx * d;
        a[4] += w * ny * ny; a[5] += w * ny * nz; a[6] += w * ny * d;
        a[7] += w * nz * nz; a[8] += w * nz * d;  a[9] += w * d * d;
    }
    double eval(const double v[3]) const {
        return a[0] * v[0] * v[0] + 2 * a[1] * v[0] * v[1] + 2 * a[2] * v[0] * v[2]
             + 2 * a[3] * v[0] + a[4] * v[1] * v[1] + 2 * a[5] * v[1] * v[2]
             + 2 * a[6] * v[1] + a[7] * v[2] * v[2] + 2 * a[8] * v[2] + a[9];
    }
};
} // namespace

bool simplifyManifold(std::vector<float>& positions,
                      std::vector<uint32_t>& indices, size_t targetTriangles,
                      double maxCost)
{
    const size_t nv = positions.size() / 3;
    const size_t nf = indices.size() / 3;
    if (nv == 0 || nf == 0 || nf <= targetTriangles)
        return true;
    std::vector<double> P(nv * 3);
    for (size_t i = 0; i < nv * 3; ++i)
        P[i] = positions[i];
    std::vector<uint32_t> F(indices);
    std::vector<char> faceAlive(nf, 1), vertAlive(nv, 1);
    std::vector<std::vector<uint32_t>> vf(nv);
    for (size_t f = 0; f < nf; ++f)
        for (int k = 0; k < 3; ++k)
            vf[F[f * 3 + k]].push_back(static_cast<uint32_t>(f));
    std::vector<Quadric> Q(nv);
    auto faceNormal = [&](uint32_t f, double n[3], const double* ov = nullptr,
                          uint32_t replaceV = UINT32_MAX) {
        double p[3][3];
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = F[f * 3 + k];
            const double* s = (v == replaceV && ov) ? ov : &P[v * 3];
            p[k][0] = s[0]; p[k][1] = s[1]; p[k][2] = s[2];
        }
        const double e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
        const double e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
        n[0] = e1[1] * e2[2] - e1[2] * e2[1];
        n[1] = e1[2] * e2[0] - e1[0] * e2[2];
        n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    };
    for (size_t f = 0; f < nf; ++f) {
        double n[3];
        faceNormal(static_cast<uint32_t>(f), n);
        const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < 1e-30)
            continue;
        const double area = 0.5 * len;
        const double ux = n[0] / len, uy = n[1] / len, uz = n[2] / len;
        const double* p0 = &P[F[f * 3] * 3];
        const double d = -(ux * p0[0] + uy * p0[1] + uz * p0[2]);
        for (int k = 0; k < 3; ++k)
            Q[F[f * 3 + k]].addPlane(ux, uy, uz, d, area);
    }

    std::vector<uint32_t> version(nv, 0);
    struct Cand { double cost; uint32_t a, b, va, vb; double pos[3]; };
    struct Cmp { bool operator()(const Cand& x, const Cand& y) const { return x.cost > y.cost; } };
    std::priority_queue<Cand, std::vector<Cand>, Cmp> heap;

    auto evalEdge = [&](uint32_t a, uint32_t b, Cand& out) {
        Quadric q = Q[a];
        q.add(Q[b]);
        const double* pa = &P[a * 3];
        const double* pb = &P[b * 3];
        const double mid[3] = {(pa[0] + pb[0]) * 0.5, (pa[1] + pb[1]) * 0.5,
                               (pa[2] + pb[2]) * 0.5};
        const double elen2 = (pa[0] - pb[0]) * (pa[0] - pb[0])
                           + (pa[1] - pb[1]) * (pa[1] - pb[1])
                           + (pa[2] - pb[2]) * (pa[2] - pb[2]);
        double best[3] = {mid[0], mid[1], mid[2]};
        double bestCost = q.eval(mid);
        for (const double* cand : {pa, pb}) {
            const double cc = q.eval(cand);
            if (cc < bestCost) { bestCost = cc; best[0] = cand[0]; best[1] = cand[1]; best[2] = cand[2]; }
        }
        // optimal position: solve A x = -b, accept only near the edge
        const double A00 = q.a[0], A01 = q.a[1], A02 = q.a[2], A11 = q.a[4],
                     A12 = q.a[5], A22 = q.a[7];
        const double det = A00 * (A11 * A22 - A12 * A12) - A01 * (A01 * A22 - A12 * A02)
                         + A02 * (A01 * A12 - A11 * A02);
        if (std::fabs(det) > 1e-18) {
            const double b0 = -q.a[3], b1 = -q.a[6], b2 = -q.a[8];
            const double x = (b0 * (A11 * A22 - A12 * A12) - A01 * (b1 * A22 - A12 * b2)
                              + A02 * (b1 * A12 - A11 * b2)) / det;
            const double y = (A00 * (b1 * A22 - A12 * b2) - b0 * (A01 * A22 - A12 * A02)
                              + A02 * (A01 * b2 - b1 * A02)) / det;
            const double z = (A00 * (A11 * b2 - b1 * A12) - A01 * (A01 * b2 - b1 * A02)
                              + b0 * (A01 * A12 - A11 * A02)) / det;
            const double opt[3] = {x, y, z};
            const double dx = x - mid[0], dy = y - mid[1], dz = z - mid[2];
            if (dx * dx + dy * dy + dz * dz < elen2) {
                const double oc = q.eval(opt);
                if (oc <= bestCost) { bestCost = oc; best[0] = x; best[1] = y; best[2] = z; }
            }
        }
        out.cost = std::max(0.0, bestCost);
        out.a = a; out.b = b; out.va = version[a]; out.vb = version[b];
        out.pos[0] = best[0]; out.pos[1] = best[1]; out.pos[2] = best[2];
    };
    auto fillHeap = [&]() {
        heap = decltype(heap)();
        for (size_t f = 0; f < nf; ++f) {
            if (!faceAlive[f])
                continue;
            for (int k = 0; k < 3; ++k) {
                const uint32_t a = F[f * 3 + k], b = F[f * 3 + (k + 1) % 3];
                // each undirected edge of an oriented manifold appears once
                // as a<b (stale duplicates are skipped by the version check)
                if (a < b) {
                    Cand cd; evalEdge(a, b, cd); heap.push(cd);
                }
            }
        }
    };
    fillHeap();

    auto neighbours = [&](uint32_t v, std::vector<uint32_t>& out) {
        out.clear();
        for (uint32_t f : vf[v]) {
            // vf[] of the far vertices keeps faces killed by earlier collapses
            // (they still name the dead vertex); counting them would inflate
            // the common-neighbour count and reject legal collapses.
            if (!faceAlive[f])
                continue;
            for (int k = 0; k < 3; ++k) {
                const uint32_t w = F[f * 3 + k];
                if (w != v) out.push_back(w);
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    };
    size_t alive = nf;
    std::vector<uint32_t> na, nb, common, shared;
    // Never collapse below a tetrahedron: for a closed 4-face component every
    // edge passes the link test, yet the collapse leaves two coincident faces.
    const size_t floorTris = std::max<size_t>(targetTriangles, 4);
    // A rejected candidate is dropped from the heap for good, but a later
    // collapse can make it legal; on a dense input (1M+ marching-cubes
    // triangles) the heap ran dry at ~2.5x the target. So when it empties
    // short of the target, rebuild it from the surviving edges and go again.
    // The flip test stays strict: loosening it lets collapses fold the
    // surface into spikes (a cone grew out of a head).
    bool capped = false;
    for (int sweep = 0; sweep < 4 && alive > floorTris && !capped; ++sweep) {
    if (sweep > 0)
        fillHeap();
    const size_t aliveBefore = alive;
    constexpr double minFlipCos = 0.1;
    while (alive > floorTris && !heap.empty()) {
        Cand cd = heap.top();
        heap.pop();
        const uint32_t a = cd.a, b = cd.b;
        if (!vertAlive[a] || !vertAlive[b] || version[a] != cd.va || version[b] != cd.vb)
            continue;
        if (cd.cost > maxCost) {   // everything left is at least this costly
            capped = true;
            break;
        }
        // faces containing both a and b
        shared.clear();
        for (uint32_t f : vf[a])
            if (faceAlive[f] && (F[f * 3] == b || F[f * 3 + 1] == b || F[f * 3 + 2] == b))
                shared.push_back(f);
        if (shared.size() != 2) continue;   // boundary / non-manifold edge: never collapse
        neighbours(a, na);
        neighbours(b, nb);
        common.clear();
        std::set_intersection(na.begin(), na.end(), nb.begin(), nb.end(),
                              std::back_inserter(common));
        if (common.size() != 2) continue;   // LINK CONDITION violated: collapse would pinch
        // flip / degeneracy check on the surviving faces
        bool ok = true;
        for (int side = 0; side < 2 && ok; ++side) {
            const uint32_t v = side == 0 ? a : b;
            const uint32_t other = side == 0 ? b : a;
            for (uint32_t f : vf[v]) {
                if (!faceAlive[f])
                    continue;
                if (F[f * 3] == other || F[f * 3 + 1] == other || F[f * 3 + 2] == other)
                    continue;
                double n0[3], n1[3];
                faceNormal(f, n0);
                faceNormal(f, n1, cd.pos, v);
                const double l0 = std::sqrt(n0[0] * n0[0] + n0[1] * n0[1] + n0[2] * n0[2]);
                const double l1 = std::sqrt(n1[0] * n1[0] + n1[1] * n1[1] + n1[2] * n1[2]);
                if (l1 < 1e-24 || l0 < 1e-24
                    || (n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2]) / (l0 * l1) < minFlipCos) {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok)
            continue;
        // collapse a -> b at cd.pos
        for (uint32_t f : shared) {
            faceAlive[f] = 0;
            --alive;
        }
        P[b * 3] = cd.pos[0]; P[b * 3 + 1] = cd.pos[1]; P[b * 3 + 2] = cd.pos[2];
        std::vector<uint32_t> nvf;
        nvf.reserve(vf[a].size() + vf[b].size());
        for (uint32_t f : vf[b])
            if (faceAlive[f]) nvf.push_back(f);
        for (uint32_t f : vf[a]) {
            if (!faceAlive[f]) continue;
            for (int k = 0; k < 3; ++k)
                if (F[f * 3 + k] == a) F[f * 3 + k] = b;
            nvf.push_back(f);
        }
        vf[b].swap(nvf);
        vf[a].clear();
        vertAlive[a] = 0;
        Q[b].add(Q[a]);
        ++version[b];
        neighbours(b, nb);
        for (uint32_t w : nb) {
            Cand nc;
            if (w < b) evalEdge(w, b, nc); else evalEdge(b, w, nc);
            heap.push(nc);
        }
    }
    if (alive == aliveBefore)
        break;   // a full sweep found nothing legal to collapse
    }

    // write back, compacting
    std::vector<uint32_t> remap(nv, UINT32_MAX);
    std::vector<float> outP;
    std::vector<uint32_t> outI;
    outI.reserve(alive * 3);
    for (size_t f = 0; f < nf; ++f) {
        if (!faceAlive[f]) continue;
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = F[f * 3 + k];
            if (remap[v] == UINT32_MAX) {
                remap[v] = static_cast<uint32_t>(outP.size() / 3);
                outP.push_back(static_cast<float>(P[v * 3]));
                outP.push_back(static_cast<float>(P[v * 3 + 1]));
                outP.push_back(static_cast<float>(P[v * 3 + 2]));
            }
            outI.push_back(remap[v]);
        }
    }
    positions.swap(outP);
    indices.swap(outI);
    return true;
}

// ---- topologyStats -------------------------------------------------------------
int splitNonManifold(std::vector<float>& positions,
                     std::vector<uint32_t>& indices)
{
    const size_t nf = indices.size() / 3;
    if (nf == 0)
        return 0;
    struct EdgeFace { uint32_t face; uint8_t corner; };   // edge: corner -> corner+1
    std::unordered_map<uint64_t, std::vector<EdgeFace>> edges;
    edges.reserve(nf * 2);
    for (size_t f = 0; f < nf; ++f)
        for (uint8_t k = 0; k < 3; ++k) {
            const uint32_t a = indices[f * 3 + k], b = indices[f * 3 + (k + 1) % 3];
            const uint64_t key = (uint64_t(std::min(a, b)) << 32) | std::max(a, b);
            edges[key].push_back({static_cast<uint32_t>(f), k});
        }

    // union-find over face CORNERS; a corner class is one output vertex
    std::vector<uint32_t> parent(nf * 3);
    for (size_t i = 0; i < parent.size(); ++i)
        parent[i] = static_cast<uint32_t>(i);
    auto find = [&](uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    auto unite = [&](uint32_t x, uint32_t y) {
        x = find(x); y = find(y);
        if (x != y) parent[std::max(x, y)] = std::min(x, y);
    };
    auto cornerOf = [&](uint32_t face, uint32_t vertex) -> uint32_t {
        for (uint32_t k = 0; k < 3; ++k)
            if (indices[face * 3 + k] == vertex)
                return face * 3 + k;
        return UINT32_MAX;
    };
    auto link = [&](const EdgeFace& x, const EdgeFace& y, uint32_t a, uint32_t b) {
        for (uint32_t v : {a, b}) {
            const uint32_t cx = cornerOf(x.face, v), cy = cornerOf(y.face, v);
            if (cx != UINT32_MAX && cy != UINT32_MAX)
                unite(cx, cy);
        }
    };
    auto pos3 = [&](uint32_t v) { return &positions[size_t(v) * 3]; };

    for (auto& kv : edges) {
        const auto& L = kv.second;
        const uint32_t a = static_cast<uint32_t>(kv.first >> 32);
        const uint32_t b = static_cast<uint32_t>(kv.first & 0xffffffffu);
        if (L.size() == 2) {
            link(L[0], L[1], a, b);
            continue;
        }
        if (L.size() < 4 || (L.size() & 1))
            continue;   // a lone/odd face cannot be paired: leave it
        // Order the faces by angle around the edge, then pair the two faces
        // that bound the same SOLID wedge (a face's outward normal says which
        // side of it is empty). Pairing across solid keeps touching parts as
        // separate bodies.
        float ax[3];
        sub3(pos3(b), pos3(a), ax);
        const float al = len3(ax);
        if (!(al > 0.0f))
            continue;
        for (float& c : ax) c /= al;
        struct Ent { float angle; float side; size_t i; };
        std::vector<Ent> ents;
        float u[3] = {0, 0, 0}, w[3] = {0, 0, 0};
        bool haveBasis = false;
        for (size_t i = 0; i < L.size(); ++i) {
            const uint32_t f = L[i].face;
            const uint32_t c = indices[f * 3 + (L[i].corner + 2) % 3];
            float d[3];
            sub3(pos3(c), pos3(a), d);
            const float along = dot3(d, ax);
            float perp[3] = {d[0] - ax[0] * along, d[1] - ax[1] * along,
                             d[2] - ax[2] * along};
            if (!haveBasis) {
                const float pl = len3(perp);
                if (!(pl > 0.0f)) continue;
                for (int k = 0; k < 3; ++k) u[k] = perp[k] / pl;
                cross3(ax, u, w);
                haveBasis = true;
            }
            float n[3], e1[3], e2[3];
            sub3(pos3(indices[f * 3 + 1]), pos3(indices[f * 3]), e1);
            sub3(pos3(indices[f * 3 + 2]), pos3(indices[f * 3]), e2);
            cross3(e1, e2, n);
            ents.push_back({std::atan2(dot3(perp, w), dot3(perp, u)),
                            dot3(n, w), i});
        }
        if (ents.size() != L.size())
            continue;
        std::sort(ents.begin(), ents.end(),
                  [](const Ent& x, const Ent& y) { return x.angle < y.angle; });
        // Which way does the face's outward normal lean around the edge? The
        // face's own half-plane is spanned by `perp` (angle), so its normal is
        // roughly +-(axis x perp): compare it with the tangent direction.
        const size_t k = ents.size();
        std::vector<char> used(k, 0);
        for (size_t i = 0; i < k; ++i) {
            const size_t j = (i + 1) % k;
            // normal.w is measured against the FIRST face's perpendicular
            // frame; use the face's own tangent instead
            auto tangentSide = [&](const Ent& e) {
                const float c = std::cos(e.angle), s = std::sin(e.angle);
                // tangent (increasing angle) = -sin*u + cos*w
                const float t[3] = {-s * u[0] + c * w[0], -s * u[1] + c * w[1],
                                    -s * u[2] + c * w[2]};
                const uint32_t f = L[e.i].face;
                float n[3], e1[3], e2[3];
                sub3(pos3(indices[f * 3 + 1]), pos3(indices[f * 3]), e1);
                sub3(pos3(indices[f * 3 + 2]), pos3(indices[f * 3]), e2);
                cross3(e1, e2, n);
                return dot3(n, t);
            };
            if (!used[i] && !used[j] && tangentSide(ents[i]) < 0.0f
                && tangentSide(ents[j]) > 0.0f) {
                link(L[ents[i].i], L[ents[j].i], a, b);
                used[i] = used[j] = 1;
            }
        }
        // Nearly coplanar touching sheets leave the wedge test ambiguous. Pair
        // what is left by edge DIRECTION instead (a consistently wound surface
        // runs a->b on one face and b->a on its neighbour), nearest in angle
        // first, so an even group never leaves a hole behind.
        auto fwd = [&](const Ent& e) {
            return indices[L[e.i].face * 3 + L[e.i].corner] == a;
        };
        for (size_t i = 0; i < k; ++i) {
            if (used[i])
                continue;
            for (size_t step = 1; step < k; ++step) {
                const size_t j = (i + step) % k;
                if (!used[j] && fwd(ents[i]) != fwd(ents[j])) {
                    link(L[ents[i].i], L[ents[j].i], a, b);
                    used[i] = used[j] = 1;
                    break;
                }
            }
        }
    }

    // one output vertex per (original vertex, corner class)
    std::unordered_map<uint64_t, uint32_t> remap;
    remap.reserve(nf * 3);
    std::vector<uint8_t> seen(positions.size() / 3, 0);
    int splits = 0;
    std::vector<uint32_t> newIdx(indices.size());
    for (size_t c = 0; c < indices.size(); ++c) {
        const uint32_t v = indices[c];
        const uint64_t key = (uint64_t(v) << 32) | find(static_cast<uint32_t>(c));
        auto it = remap.find(key);
        if (it == remap.end()) {
            uint32_t nv = v;
            if (seen[v]) {
                nv = static_cast<uint32_t>(positions.size() / 3);
                const float p[3] = {positions[size_t(v) * 3],
                                    positions[size_t(v) * 3 + 1],
                                    positions[size_t(v) * 3 + 2]};
                positions.insert(positions.end(), p, p + 3);
                ++splits;
            }
            seen[v] = 1;
            it = remap.emplace(key, nv).first;
        }
        newIdx[c] = it->second;
    }
    indices.swap(newIdx);
    return splits;
}

// Last-resort cleanup after splitNonManifold: edges still shared by 3+ faces
// (two sheets that are ALSO joined elsewhere, so no vertex split can part
// them) lose their smallest faces until two remain, then the small holes that
// leaves — and any that already existed — are closed with a centroid fan.
// Returns the number of faces dropped.
using TriKey = std::array<uint32_t, 3>;   // sorted vertex triple
static TriKey triKey(uint32_t a, uint32_t b, uint32_t c)
{
    TriKey k{a, b, c};
    std::sort(k.begin(), k.end());
    return k;
}

static int dropResidualNonManifold(const std::vector<float>& pos,
                            std::vector<uint32_t>& idx,
                            std::set<TriKey>& droppedOut)
{
    const size_t nf = idx.size() / 3;
    auto area = [&](size_t f) {
        float e1[3], e2[3], n[3];
        sub3(&pos[size_t(idx[f * 3 + 1]) * 3], &pos[size_t(idx[f * 3]) * 3], e1);
        sub3(&pos[size_t(idx[f * 3 + 2]) * 3], &pos[size_t(idx[f * 3]) * 3], e2);
        cross3(e1, e2, n);
        return len3(n);
    };
    std::unordered_map<uint64_t, std::vector<uint32_t>> edges;
    edges.reserve(nf * 2);
    for (size_t f = 0; f < nf; ++f)
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = idx[f * 3 + k], b = idx[f * 3 + (k + 1) % 3];
            edges[(uint64_t(std::min(a, b)) << 32) | std::max(a, b)]
                .push_back(static_cast<uint32_t>(f));
        }
    std::vector<char> dead(nf, 0);
    for (auto& kv : edges) {
        std::vector<uint32_t> live;
        for (uint32_t f : kv.second)
            if (!dead[f]) live.push_back(f);
        if (live.size() <= 2)
            continue;
        std::sort(live.begin(), live.end(), [&](uint32_t x, uint32_t y) {
            return area(x) > area(y);
        });
        for (size_t i = 2; i < live.size(); ++i)
            dead[live[i]] = 1;
    }
    int dropped = 0;
    std::vector<uint32_t> out;
    out.reserve(idx.size());
    for (size_t f = 0; f < nf; ++f) {
        if (dead[f]) {
            ++dropped;
            droppedOut.insert(triKey(idx[f * 3], idx[f * 3 + 1], idx[f * 3 + 2]));
            continue;
        }
        out.insert(out.end(), idx.begin() + f * 3, idx.begin() + f * 3 + 3);
    }
    idx.swap(out);
    return dropped;
}

// Close small boundary loops with a centroid fan (loops that revisit a vertex
// or run longer than maxLoop are left alone). Returns the loops closed.
// Drop small components that still have open (single-face) edges: a floating
// scrap with a hole in it cannot be closed sensibly and is not part of the
// subject. Components under `fraction` of the triangles only; returns the
// triangles removed.
static int dropSmallOpenComponents(const std::vector<float>& pos,
                                   std::vector<uint32_t>& idx, double fraction)
{
    const size_t nv = pos.size() / 3, nf = idx.size() / 3;
    if (nf == 0)
        return 0;
    std::vector<uint32_t> parent(nv);
    for (size_t i = 0; i < nv; ++i) parent[i] = static_cast<uint32_t>(i);
    auto find = [&](uint32_t x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    for (size_t f = 0; f < nf; ++f)
        for (int k = 1; k < 3; ++k) {
            const uint32_t x = find(idx[f * 3]), y = find(idx[f * 3 + k]);
            if (x != y) parent[std::max(x, y)] = std::min(x, y);
        }
    std::unordered_map<uint64_t, int> directed;
    for (size_t t = 0; t < nf; ++t)
        for (int k = 0; k < 3; ++k)
            directed[(uint64_t(idx[t * 3 + k]) << 32) | idx[t * 3 + (k + 1) % 3]]++;
    std::unordered_map<uint32_t, int> tris;
    std::unordered_map<uint32_t, bool> open;
    for (size_t t = 0; t < nf; ++t) {
        const uint32_t root = find(idx[t * 3]);
        ++tris[root];
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = idx[t * 3 + k], b = idx[t * 3 + (k + 1) % 3];
            if (!directed.count((uint64_t(b) << 32) | a))
                open[root] = true;
        }
    }
    std::vector<uint32_t> out;
    out.reserve(idx.size());
    int removed = 0;
    for (size_t t = 0; t < nf; ++t) {
        const uint32_t root = find(idx[t * 3]);
        if (open.count(root) && tris[root] < fraction * double(nf)) {
            ++removed;
            continue;
        }
        out.insert(out.end(), idx.begin() + t * 3, idx.begin() + t * 3 + 3);
    }
    idx.swap(out);
    return removed;
}

// A one-triangle hole can only be closed by the very triangle that was dropped
// (a fin hanging off an over-shared edge), so grow every remaining hole by the
// faces that touch it; the larger loop is then closed with a fan. Returns the
// faces removed.
static int growHoles(std::vector<uint32_t>& idx)
{
    std::unordered_map<uint64_t, int> directed;
    directed.reserve(idx.size());
    for (size_t t = 0; t + 2 < idx.size(); t += 3)
        for (int k = 0; k < 3; ++k)
            directed[(uint64_t(idx[t + k]) << 32) | idx[t + (k + 1) % 3]]++;
    std::unordered_map<uint32_t, char> onHole;
    for (auto& kv : directed) {
        const uint32_t u = static_cast<uint32_t>(kv.first >> 32);
        const uint32_t v = static_cast<uint32_t>(kv.first & 0xffffffffu);
        if (!directed.count((uint64_t(v) << 32) | u)) {
            onHole[u] = 1;
            onHole[v] = 1;
        }
    }
    if (onHole.empty())
        return 0;
    std::vector<uint32_t> out;
    out.reserve(idx.size());
    int removed = 0;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        if (onHole.count(idx[t]) || onHole.count(idx[t + 1]) || onHole.count(idx[t + 2])) {
            ++removed;
            continue;
        }
        out.insert(out.end(), idx.begin() + t, idx.begin() + t + 3);
    }
    idx.swap(out);
    return removed;
}

static int fillSmallHoles(std::vector<float>& pos, std::vector<uint32_t>& idx,
                   size_t maxLoop, const std::set<TriKey>& forbidden,
                   size_t maxTriangles)
{
    std::unordered_map<uint64_t, int> directed;   // (u<<32|v) present
    directed.reserve(idx.size());
    for (size_t t = 0; t + 2 < idx.size(); t += 3)
        for (int k = 0; k < 3; ++k)
            directed[(uint64_t(idx[t + k]) << 32) | idx[t + (k + 1) % 3]]++;
    // boundary half-edges: u->v whose reverse is absent
    std::unordered_map<uint32_t, std::vector<uint32_t>> out;   // u -> [v]
    for (auto& kv : directed) {
        const uint32_t u = static_cast<uint32_t>(kv.first >> 32);
        const uint32_t v = static_cast<uint32_t>(kv.first & 0xffffffffu);
        if (!directed.count((uint64_t(v) << 32) | u))
            out[u].push_back(v);
    }
    int closed = 0;
    std::unordered_map<uint64_t, char> done;
    for (auto& kv : out) {
        for (uint32_t v0 : kv.second) {
            const uint32_t u0 = kv.first;
            if (done.count((uint64_t(u0) << 32) | v0))
                continue;
            std::vector<uint32_t> loop{u0};
            uint32_t cur = v0;
            bool ok = true;
            while (cur != u0) {
                if (loop.size() > maxLoop) { ok = false; break; }
                for (uint32_t w : loop)
                    if (w == cur) { ok = false; break; }
                if (!ok) break;
                loop.push_back(cur);
                auto it = out.find(cur);
                if (it == out.end()) { ok = false; break; }
                // clustered holes share vertices: take the way that closes the
                // loop, else any vertex not yet on it
                uint32_t next = UINT32_MAX;
                for (uint32_t cand : it->second)
                    if (cand == u0) { next = cand; break; }
                if (next == UINT32_MAX)
                    for (uint32_t cand : it->second) {
                        bool seen = false;
                        for (uint32_t w : loop) if (w == cand) { seen = true; break; }
                        if (!seen && !done.count((uint64_t(cur) << 32) | cand)) {
                            next = cand;
                            break;
                        }
                    }
                if (next == UINT32_MAX) { ok = false; break; }
                cur = next;
            }
            if (!ok || loop.size() < 3)
                continue;
            for (size_t i = 0; i < loop.size(); ++i)
                done[(uint64_t(loop[i]) << 32) | loop[(i + 1) % loop.size()]] = 1;
            // a hole bounded by a face we just dropped must stay open (filling
            // it would put the over-shared face straight back)
            if (loop.size() == 3
                && forbidden.count(triKey(loop[0], loop[1], loop[2])))
                continue;
            if (idx.size() / 3 + (loop.size() == 3 ? 1 : loop.size()) > maxTriangles)
                continue;
            if (loop.size() == 3) {
                idx.insert(idx.end(), {loop[0], loop[2], loop[1]});
            } else {
                float c[3] = {0, 0, 0};
                for (uint32_t v : loop)
                    for (int k = 0; k < 3; ++k)
                        c[k] += pos[size_t(v) * 3 + k] / float(loop.size());
                const uint32_t ci = static_cast<uint32_t>(pos.size() / 3);
                pos.insert(pos.end(), c, c + 3);
                for (size_t i = 0; i < loop.size(); ++i)
                    idx.insert(idx.end(), {loop[(i + 1) % loop.size()], loop[i], ci});
            }
            ++closed;
        }
    }
    return closed;
}

TopologyStats topologyStats(size_t vertexCount,
                            const std::vector<uint32_t>& indices)
{
    TopologyStats s;
    s.triangles = static_cast<int>(indices.size() / 3);
    // lo<hi key -> {use count, mask of directions seen (1 = lo->hi, 2 = hi->lo)}
    struct Use { uint32_t n = 0; uint32_t same = 0; uint8_t dirs[2] = {0, 0}; };
    std::unordered_map<uint64_t, Use> uses;
    uses.reserve(indices.size());
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = indices[t + k];
            const uint32_t b = indices[t + (k + 1) % 3];
            if (a == b)
                continue;
            const uint32_t lo = std::min(a, b), hi = std::max(a, b);
            Use& u = uses[(static_cast<uint64_t>(lo) << 32) | hi];
            if (u.n < 2)
                u.dirs[u.n] = a < b ? 1 : 2;
            ++u.n;
        }
    }
    for (const auto& kv : uses) {
        const Use& u = kv.second;
        if (u.n == 1) ++s.boundaryEdges;
        else if (u.n > 2) ++s.nonManifoldEdges;
        else if (u.dirs[0] == u.dirs[1]) ++s.flippedEdges;
    }
    std::vector<int> islandId;
    s.components = MeshSegmenter::connectedComponents(
        static_cast<int>(vertexCount), indices.data(),
        static_cast<int>(indices.size()), islandId);
    std::vector<int> per(static_cast<size_t>(std::max(s.components, 0)), 0);
    for (size_t t = 0; t + 2 < indices.size(); t += 3)
        ++per[islandId[indices[t]]];
    const int limit = std::max(1, s.triangles / 100);
    for (int n : per)
        if (n < limit) ++s.smallComponents;
    return s;
}

// ---- makeGameReady -----------------------------------------------------------
GameReadyResult makeGameReady(const std::vector<float>& positions,
                              const std::vector<uint32_t>& indices,
                              const GameReadyOptions& opts)
{
    GameReadyResult r;
    const size_t nv = positions.size() / 3;
    if (nv == 0 || indices.size() < 3 || indices.size() % 3 != 0
        || positions.size() % 3 != 0) {
        r.error = QStringLiteral("makeGameReady: empty/degenerate input mesh.");
        return r;
    }
    for (uint32_t i : indices) {
        if (i >= nv) {
            r.error = QStringLiteral("makeGameReady: index out of range.");
            return r;
        }
    }
    r.inputTriangles = static_cast<int>(indices.size() / 3);

    // ---- 1. weld near-duplicate vertices (quantized remap) -----------------
    float mn[3] = {positions[0], positions[1], positions[2]};
    float mx[3] = {positions[0], positions[1], positions[2]};
    for (size_t v = 0; v < nv; ++v) {
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], positions[v * 3 + k]);
            mx[k] = std::max(mx[k], positions[v * 3 + k]);
        }
    }
    float diagv[3] = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};
    const float diag = std::max(1e-9f, len3(diagv));
    const float eps = opts.weldEpsilonAbsolute > 0.0f
        ? opts.weldEpsilonAbsolute
        : (opts.weldEpsilonFraction > 0.0f ? diag * opts.weldEpsilonFraction
                                           : 0.0f);

    std::vector<int32_t> keys(nv * 3);
    for (size_t v = 0; v < nv * 3; ++v) {
        keys[v] = eps > 0.0f
            ? static_cast<int32_t>(std::llround(positions[v] / eps))
            : 0;
    }
    std::vector<unsigned int> remap(nv);
    size_t weldedCount;
    std::vector<uint32_t> idx;
    std::vector<float> pos;
    if (eps > 0.0f) {
        weldedCount = meshopt_generateVertexRemap(
            remap.data(), indices.data(), indices.size(), keys.data(), nv,
            sizeof(int32_t) * 3);
    } else {
        weldedCount = meshopt_generateVertexRemap(
            remap.data(), indices.data(), indices.size(), positions.data(), nv,
            sizeof(float) * 3);
    }
    pos.resize(weldedCount * 3);
    meshopt_remapVertexBuffer(pos.data(), positions.data(), nv,
                              sizeof(float) * 3, remap.data());
    idx.resize(indices.size());
    meshopt_remapIndexBuffer(idx.data(), indices.data(), indices.size(),
                             remap.data());
    r.weldedVertices = static_cast<int>(nv - weldedCount);

    // ---- 2. drop degenerate triangles ---------------------------------------
    size_t w = 0;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        if (idx[t] == idx[t + 1] || idx[t + 1] == idx[t + 2]
            || idx[t] == idx[t + 2])
            continue;
        idx[w++] = idx[t];
        idx[w++] = idx[t + 1];
        idx[w++] = idx[t + 2];
    }
    r.removedTriangles += static_cast<int>((idx.size() - w) / 3);
    idx.resize(w);
    if (idx.empty()) {
        r.error = QStringLiteral("makeGameReady: mesh degenerated to nothing.");
        return r;
    }

    // ---- 3. drop tiny disconnected components -------------------------------
    // (also re-run after simplification — see 5b)
    auto cullDebris = [&](std::vector<uint32_t>& indicesIo, size_t vertexCount) {
        std::vector<int> islandId;
        const int islands = MeshSegmenter::connectedComponents(
            static_cast<int>(vertexCount), indicesIo.data(),
            static_cast<int>(indicesIo.size()), islandId);
        if (islands <= 1)
            return;
        std::vector<int> triPerIsland(islands, 0);
        for (size_t t = 0; t + 2 < indicesIo.size(); t += 3)
            ++triPerIsland[islandId[indicesIo[t]]];
        const int largest = static_cast<int>(
            std::max_element(triPerIsland.begin(), triPerIsland.end())
            - triPerIsland.begin());
        const int totalTris = static_cast<int>(indicesIo.size() / 3);
        const int threshold = std::max(
            opts.minComponentTriangles,
            static_cast<int>(opts.minComponentFraction * totalTris));
        std::vector<char> keep(islands, 0);
        for (int i = 0; i < islands; ++i)
            keep[i] = (i == largest || triPerIsland[i] >= threshold) ? 1 : 0;
        size_t w2 = 0;
        for (size_t t = 0; t + 2 < indicesIo.size(); t += 3) {
            if (!keep[islandId[indicesIo[t]]])
                continue;
            indicesIo[w2++] = indicesIo[t];
            indicesIo[w2++] = indicesIo[t + 1];
            indicesIo[w2++] = indicesIo[t + 2];
        }
        for (int i = 0; i < islands; ++i)
            if (!keep[i]) ++r.removedComponents;
        r.removedTriangles += static_cast<int>((indicesIo.size() - w2) / 3);
        indicesIo.resize(w2);
    };
    cullDebris(idx, weldedCount);

    // ---- 3b. unify winding ----------------------------------------------------
    // Must run before normals/culling ever see the mesh — raw dual-grid
    // output ships large flipped patches (they render as holes under
    // backface culling and poison the smooth normals the bake relies on).
    unifyWinding(pos, idx);

    // ---- 4. compact unreferenced vertices -----------------------------------
    auto compact = [](std::vector<float>& positionsIo,
                      std::vector<uint32_t>& indicesIo) {
        const size_t count = positionsIo.size() / 3;
        std::vector<uint32_t> map(count, UINT32_MAX);
        uint32_t next = 0;
        for (uint32_t& i : indicesIo) {
            if (map[i] == UINT32_MAX)
                map[i] = next++;
            i = map[i];
        }
        std::vector<float> outp(static_cast<size_t>(next) * 3);
        for (size_t v = 0; v < count; ++v) {
            if (map[v] == UINT32_MAX)
                continue;
            std::memcpy(&outp[map[v] * 3], &positionsIo[v * 3],
                        sizeof(float) * 3);
        }
        positionsIo.swap(outp);
    };
    compact(pos, idx);
    if (opts.exportWeldedSource) {
        r.weldedPositions = pos;
        r.weldedIndices = idx;
    }

    // ---- 4a. optional voxel remesh -> clean, consistently oriented surface ---
    // Marching cubes on a signed distance field of the welded source: fixes the
    // flipped patches and pinched double walls of the raw dual-grid output and
    // keeps detail to ~1 cell. It is deliberately NOT simplified here — the
    // link-condition simplifier (simplifyManifold) cannot reach a game budget
    // on a dense mesh without destroying thin features, and every coarser grid
    // fuses fingers and flattens faces. The shape-preserving simplifier in
    // step 5 does the reduction, and step 5c repairs the few pinches it leaves.
    if (opts.remeshResolution > 0) {
        std::vector<float> rp;
        std::vector<uint32_t> ri;
        if (remeshManifold(pos, idx, opts.remeshResolution, rp, ri)) {
            const TopologyStats ts = topologyStats(rp.size() / 3, ri);
            if (ts.boundaryEdges == 0 && ts.nonManifoldEdges == 0) {
                r.remeshedFromTriangles = static_cast<int>(idx.size() / 3);
                pos.swap(rp);
                idx.swap(ri);
                unifyWinding(pos, idx);
                r.remeshed = true;
                cullDebris(idx, pos.size() / 3);
                compact(pos, idx);
                // The remeshed surface is the better dense source for the bake:
                // marching cubes on an accurate distance field has no voxel
                // terracing (the raw dual-grid surface bakes into the detail
                // normal map as horizontal banding on every flat panel), it is
                // consistently wound, and it is a fraction of the size.
                if (opts.exportWeldedSource) {
                    r.weldedPositions = pos;
                    r.weldedIndices = idx;
                }
            }
        }
    }

    // ---- 4b. optional Taubin pre-smooth --------------------------------------
    // Voxel decodes of fuzzy subjects (fur, hair, foliage) carry sub-voxel
    // micro-pits and wisps that read as dark "pepper" speckle in renders and
    // confuse QEM into collapsing thin double-walled features into flipped
    // soup. A few volume-preserving Taubin passes flatten noise below the
    // voxel scale while leaving real shape (runs on the WELDED mesh, so the
    // Laplacian sees true adjacency, and before simplification so QEM ranks
    // clean geometry). Skipped for a remeshed surface: it is already smooth
    // (marching cubes on a distance field) and the pass rounds off detail.
    if (opts.taubinIterations > 0 && !r.remeshed)
        MeshRefine::taubinSmooth(pos, idx, opts.taubinIterations);

    // Steps 5-5c run on a COPY-able starting point: splitting pinches discards
    // the junk meshopt spends triangles on (hidden slivers, flaps hanging off
    // pinch points — measured: half of a 25k result), so when a remeshed result
    // lands well under budget the reduction is repeated once with a
    // proportionally higher target.
    const std::vector<float> startPos = r.remeshed ? pos : std::vector<float>();
    const std::vector<uint32_t> startIdx = r.remeshed ? idx : std::vector<uint32_t>();
    int tgt = opts.targetTriangles;
    std::vector<float> firstPos;
    std::vector<uint32_t> firstIdx;
    int firstSplit = 0;
    bool haveFirst = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt > 0) {
            pos = startPos;
            idx = startIdx;
            r.splitVertices = 0;
        }
        // ---- 5. simplify toward the target --------------------------------------
        if (tgt > 0
            && static_cast<int>(idx.size() / 3) > tgt) {
            std::vector<uint32_t> simplified(idx.size());
            float resultError = 0.0f;
            const size_t targetIndexCount =
                static_cast<size_t>(tgt) * 3;
            size_t newCount = meshopt_simplify(
                simplified.data(), idx.data(), idx.size(), pos.data(),
                pos.size() / 3, sizeof(float) * 3, targetIndexCount,
                opts.simplifyTargetError, /*options=*/0, &resultError);
            // Game-ready budgets treat the COUNT as the contract (the lost
            // detail comes back via the normal-map bake): on dense organic
            // sources (a 4.8M-tri TRELLIS decode) the relative error cap stops
            // collapsing millions of triangles short of the budget — and the
            // downstream xatlas unwrap of that "simplified" mesh then takes tens
            // of minutes (its chart compute is superlinear). If the capped pass
            // landed far off target, redo it uncapped.
            if (newCount > targetIndexCount * 2) {
                newCount = meshopt_simplify(
                    simplified.data(), idx.data(), idx.size(), pos.data(),
                    pos.size() / 3, sizeof(float) * 3, targetIndexCount,
                    std::numeric_limits<float>::max(), /*options=*/0,
                    &resultError);
            }
            // Topology-preserving QEM can still be STUCK far above the budget on
            // non-manifold sources (raw TRELLIS dual-grid output: ~4.9M tris
            // refused to go below ~2.7M even uncapped). Game presets promise a
            // usable budget — fall back to the topology-free sloppy simplifier,
            // which always reaches it; the detail returns via the normal bake.
            if (newCount > targetIndexCount * 2) {
                newCount = meshopt_simplifySloppy(
                    simplified.data(), idx.data(), idx.size(), pos.data(),
                    pos.size() / 3, sizeof(float) * 3, targetIndexCount,
                    std::numeric_limits<float>::max(), &resultError);
            }
            // Platform presets (Roblox 4k / 20k) promise a CEILING, not a
            // budget: a count anywhere above the target is an upload the
            // platform rejects, so the 2x tolerance above does not apply. The
            // sloppy simplifier does not lock border edges and reaches any
            // target; the detail lost comes back through the normal bake.
            if (opts.strictTriangleBudget && newCount > targetIndexCount) {
                std::vector<uint32_t> strictIdx(idx.size());
                float strictError = 0.0f;
                const size_t strictCount = meshopt_simplifySloppy(
                    strictIdx.data(), idx.data(), idx.size(), pos.data(),
                    pos.size() / 3, sizeof(float) * 3, targetIndexCount,
                    std::numeric_limits<float>::max(), &strictError);
                // The sloppy simplifier is a vertex-clustering grid whose count
                // is not monotonic in grid size: on a mesh made of separate
                // pieces it can jump from "over budget" straight to nothing.
                // Neither outcome honours the ceiling — an empty mesh is no
                // asset, and the over-budget QEM mesh is one the platform will
                // reject — so a strict pass that cannot land under the target
                // FAILS rather than returning ok with a count the preset
                // promised not to exceed (review finding on #1075).
                if (strictCount == 0 || strictCount > targetIndexCount) {
                    r.error = QStringLiteral(
                        "strict triangle budget of %1 is unreachable: the "
                        "topology-preserving simplifier stalled at %2 triangles "
                        "and the clustering fallback collapsed the mesh entirely "
                        "(disconnected pieces?) — choose a higher budget, or a "
                        "non-strict tier and decimate the parts separately.")
                        .arg(tgt).arg(newCount / 3);
                    return r;
                }
                simplified.swap(strictIdx);
                newCount = strictCount;
                resultError = strictError;
            }
            simplified.resize(newCount);
            idx.swap(simplified);
            r.simplifyError = resultError;
            compact(pos, idx);

            // ---- 5b. repair what simplification broke ----------------------------
            // Measured on real TRELLIS.2 generations (every raw source and every
            // pre-simplify mesh is CLOSED — 0 boundary edges — so this is where
            // the artefacts come from): the topology-free/non-manifold-tolerant
            // collapses (a) FLIP triangles (hundreds of edges whose two faces
            // disagree on winding → holes under backface culling, bad smooth
            // normals) and (b) pinch thin double-walled parts off into small
            // floating fragments (1 → 13 components, 11 of them < 1% of the
            // mesh). Steps 3/3b only ran BEFORE simplification, so neither was
            // ever cleaned afterwards.
            cullDebris(idx, pos.size() / 3);
            compact(pos, idx);
            unifyWinding(pos, idx);
        }

        // ---- 5c. split the pinches simplification leaves --------------------------
        // The simplifier fuses touching sheets into edges shared by 4+ faces. Each
        // such edge is split so every sheet gets its own vertices: no geometry
        // moves and no hole opens, and parts that merely touch (fingers pressed
        // together) end up separate instead of welded.
        if (r.remeshed) {
            // Splitting detaches the flaps that hung on a pinch as pieces of their
            // own; drop those like any other debris, and go again since removing
            // them can expose the next pinch.
            for (int pass = 0; pass < 3; ++pass) {
                const int added = splitNonManifold(pos, idx);
                r.splitVertices += added;
                if (added == 0)
                    break;
                unifyWinding(pos, idx);
                cullDebris(idx, pos.size() / 3);
                compact(pos, idx);
            }
        }

        // ---- 5d. residual pinches + holes ------------------------------------
        if (r.remeshed) {
            // A hard ceiling (strict budget) must survive the repairs: a centroid
            // fan adds a triangle per loop edge, so filling is capped at it.
            const size_t fillCap = opts.strictTriangleBudget && tgt > 0
                ? static_cast<size_t>(tgt) : std::numeric_limits<size_t>::max();
            // No compaction between passes: `droppedTris` is keyed by vertex index
            // and must keep naming the same triangles until the passes are done.
            std::set<TriKey> droppedTris;
            for (int pass = 0; pass < 8; ++pass) {
                const int dropped = dropResidualNonManifold(pos, idx, droppedTris);
                const int filled = fillSmallHoles(pos, idx, 256, droppedTris, fillCap);
                if (dropped == 0 && filled == 0)
                    break;
            }
            // fins left holes the fan cannot close without re-adding them: grow
            // each by the faces touching it and close the larger loop. A grown
            // hole that still cannot be closed (revisited vertex, too long, no
            // room under a ceiling) would be strictly worse than the original,
            // so a round that does not reduce the open edges is undone.
            for (int round = 0; round < 2; ++round) {
                const int openBefore = topologyStats(pos.size() / 3, idx).boundaryEdges;
                if (openBefore == 0)
                    break;
                const std::vector<float> posSnap = pos;
                const std::vector<uint32_t> idxSnap = idx;
                if (growHoles(idx) == 0)
                    break;
                fillSmallHoles(pos, idx, 256, std::set<TriKey>(), fillCap);
                if (topologyStats(pos.size() / 3, idx).boundaryEdges >= openBefore) {
                    pos = posSnap;
                    idx = idxSnap;
                    break;
                }
            }
            dropSmallOpenComponents(pos, idx, 0.02);
            compact(pos, idx);
            unifyWinding(pos, idx);
        }

        if (attempt == 0 && r.remeshed && !opts.strictTriangleBudget
            && opts.targetTriangles > 0) {
            const int got = static_cast<int>(idx.size() / 3);
            if (got > 0 && got < static_cast<int>(opts.targetTriangles * 0.8)) {
                // keep this result: the retry is only taken if it lands inside
                // the usual 2x tolerance
                firstPos = pos;
                firstIdx = idx;
                firstSplit = r.splitVertices;
                haveFirst = true;
                // scale in double and cap BEFORE the int conversion (a tiny
                // `got` would otherwise overflow it)
                const double want = double(opts.targetTriangles)
                                  * double(opts.targetTriangles) / double(got);
                tgt = static_cast<int>(std::min(want, 3.0 * opts.targetTriangles));
                continue;
            }
        }
        if (attempt == 1 && haveFirst && opts.targetTriangles > 0
            && static_cast<int>(idx.size() / 3) > opts.targetTriangles * 2) {
            pos = firstPos;
            idx = firstIdx;
            r.splitVertices = firstSplit;
        }
        break;
    }

    // ---- 6. cache-friendly ordering ------------------------------------------
    meshopt_optimizeVertexCache(idx.data(), idx.data(), idx.size(),
                                pos.size() / 3);

    r.positions = std::move(pos);
    r.indices = std::move(idx);
    r.outputTriangles = static_cast<int>(r.indices.size() / 3);
    r.ok = true;
    return r;
}

namespace {
// Unit normal of source triangle `tri`; false when it is degenerate.
bool sourceFaceNormal(const std::vector<float>& positions,
                      const std::vector<uint32_t>& indices, int tri, float n[3])
{
    const uint32_t* idx = &indices[static_cast<size_t>(tri) * 3];
    float e1[3], e2[3];
    sub3(&positions[idx[1] * 3], &positions[idx[0] * 3], e1);
    sub3(&positions[idx[2] * 3], &positions[idx[0] * 3], e2);
    cross3(e1, e2, n);
    if (!(len3(n) > 1e-20f))
        return false;
    normalize3(n);
    return true;
}
} // namespace

void resolveDetailNormal(const float nt[3], const float* sourceFaceN,
                         float ns[3])
{
    // The target normal is already the dense model's low-frequency field, so
    // what is left is relief. Measured on real generations: past ~45 degrees
    // the texels are almost all speckle (micro-pits of the voxel decode, the
    // opposite wall of a thin part), not modelled detail — flattening every
    // texel tilted more than that removed the chips and kept seams/abs intact.
    // Trust fades from 45 to ~78 degrees instead of a hard cut.
    constexpr float kFull = 0.7f;   // cos 45
    constexpr float kNone = 0.2f;   // cos ~78
    const float c = dot3(ns, nt);
    if (c >= kFull)
        return;
    if (sourceFaceN && dot3(sourceFaceN, nt) >= kFull) {
        std::memcpy(ns, sourceFaceN, 3 * sizeof(float));
        return;
    }
    const float t = std::clamp((c - kNone) / (kFull - kNone), 0.0f, 1.0f);
    for (int k = 0; k < 3; ++k)
        ns[k] = nt[k] * (1.0f - t) + ns[k] * t;
    if (len3(ns) < 1e-12f) {
        std::memcpy(ns, nt, 3 * sizeof(float));
        return;
    }
    normalize3(ns);
}

std::vector<float> creaseAwareVertexNormals(
    const std::vector<float>& targetPositions,
    const std::vector<uint32_t>& targetIndices,
    const std::vector<uint32_t>& xref,
    const std::vector<uint32_t>& outIndices, int vertexCount)
{
    std::vector<float> tNormals(static_cast<size_t>(vertexCount) * 3);
    {
        // Crease-aware: an original vertex shared by faces that point opposite
        // ways (the rim of a thin slab, two sheets a cell apart after a remesh
        // + simplify — hair against a neck, a collar against a jaw) would
        // average them into a sideways or cancelled normal and shade whole
        // regions wrongly. Each output vertex therefore smooths across charts
        // as before but only over faces within ~100 degrees of the faces it
        // actually belongs to (its chart-local fan).
        const size_t onv = targetPositions.size() / 3;
        const size_t otc = targetIndices.size() / 3;
        std::vector<float> faceN(otc * 3);   // area-weighted (unnormalised)
        for (size_t f = 0; f < otc; ++f) {
            const float* p0 = &targetPositions[targetIndices[f * 3] * 3];
            const float* p1 = &targetPositions[targetIndices[f * 3 + 1] * 3];
            const float* p2 = &targetPositions[targetIndices[f * 3 + 2] * 3];
            float e1[3], e2[3];
            sub3(p1, p0, e1);
            sub3(p2, p0, e2);
            cross3(e1, e2, &faceN[f * 3]);
        }
        std::vector<uint32_t> start(onv + 1, 0);
        for (uint32_t i : targetIndices) ++start[i + 1];
        for (size_t v = 0; v < onv; ++v) start[v + 1] += start[v];
        std::vector<uint32_t> fan(targetIndices.size());
        std::vector<uint32_t> fill(start.begin(), start.end() - 1);
        for (size_t f = 0; f < otc; ++f)
            for (int k = 0; k < 3; ++k)
                fan[fill[targetIndices[f * 3 + k]]++] = static_cast<uint32_t>(f);
        // chart-local accumulation on the xatlas mesh
        std::vector<float> local(static_cast<size_t>(vertexCount) * 3, 0.0f);
        for (size_t t = 0; t + 2 < outIndices.size(); t += 3) {
            const float* p0 = &targetPositions[xref[outIndices[t]] * 3];
            const float* p1 = &targetPositions[xref[outIndices[t + 1]] * 3];
            const float* p2 = &targetPositions[xref[outIndices[t + 2]] * 3];
            float e1[3], e2[3], fn[3];
            sub3(p1, p0, e1);
            sub3(p2, p0, e2);
            cross3(e1, e2, fn);
            for (int k = 0; k < 3; ++k)
                for (int c = 0; c < 3; ++c)
                    local[static_cast<size_t>(outIndices[t + k]) * 3 + c] += fn[c];
        }
        for (int v = 0; v < vertexCount; ++v) {
            const float* ref = &local[static_cast<size_t>(v) * 3];
            const uint32_t o = xref[v];
            float acc[3] = {0, 0, 0};
            for (uint32_t i = start[o]; i < start[o + 1]; ++i) {
                const float* fn = &faceN[fan[i] * 3];
                // normalised: past ~100 degrees (cos < -0.17) is the other sheet;
            // right-angle neighbours (box edges) still smooth as before
            if (dot3(fn, ref) > -0.17f * len3(fn) * len3(ref)) {
                    acc[0] += fn[0]; acc[1] += fn[1]; acc[2] += fn[2];
                }
            }
            float* out = &tNormals[static_cast<size_t>(v) * 3];
            float l = len3(acc);
            if (!(l > 1e-20f)) {       // no agreeing face: use the local fan
                std::memcpy(acc, ref, sizeof(acc));
                l = len3(acc);
            }
            if (l > 1e-20f) {
                out[0] = acc[0] / l; out[1] = acc[1] / l; out[2] = acc[2] / l;
            } else {
                out[0] = 0.0f; out[1] = 1.0f; out[2] = 0.0f;
            }
        }
    }
    return tNormals;
}

// ---- bake ---------------------------------------------------------------------
BakeResult bake(const std::vector<float>& targetPositions,
                const std::vector<uint32_t>& targetIndices,
                const std::vector<float>& sourcePositions,
                const std::vector<uint32_t>& sourceIndices,
                const SparseVolumeSampler& volume,
                const BakeOptions& opts)
{
    BakeResult r;
    const size_t nv = targetPositions.size() / 3;
    if (nv == 0 || targetIndices.size() < 3 || targetIndices.size() % 3 != 0) {
        r.error = QStringLiteral("bake: empty/degenerate target mesh.");
        return r;
    }
    for (uint32_t i : targetIndices) {
        if (i >= nv) {
            r.error = QStringLiteral("bake: target index out of range.");
            return r;
        }
    }
    const size_t snv = sourcePositions.size() / 3;
    if (snv == 0 || sourceIndices.size() < 3 || sourceIndices.size() % 3 != 0) {
        r.error = QStringLiteral("bake: empty/degenerate source mesh.");
        return r;
    }
    for (uint32_t i : sourceIndices) {
        if (i >= snv) {
            r.error = QStringLiteral("bake: source index out of range.");
            return r;
        }
    }
    const int texSize = std::clamp(opts.textureSize, 64, 8192);
    const int ss = std::clamp(opts.supersample, 1, 2);

    // ---- 1. xatlas unwrap of the target -------------------------------------
    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::MeshDecl decl;
    decl.vertexCount          = static_cast<uint32_t>(nv);
    decl.vertexPositionData   = targetPositions.data();
    decl.vertexPositionStride = sizeof(float) * 3;
    decl.indexCount           = static_cast<uint32_t>(targetIndices.size());
    decl.indexData            = targetIndices.data();
    decl.indexFormat          = xatlas::IndexFormat::UInt32;
    const auto err = xatlas::AddMesh(atlas, decl);
    if (err != xatlas::AddMeshError::Success) {
        r.error = QStringLiteral("bake: xatlas::AddMesh failed: %1")
                      .arg(QString::fromLatin1(xatlas::StringForEnum(err)));
        xatlas::Destroy(atlas);
        return r;
    }
    // Cancellation during the unwrap: ComputeCharts can run for minutes on
    // dense targets and used to be un-cancellable (the Cancel button only
    // reached the sampling phase). xatlas's progress callback returns false
    // to abort; throttled so the callback overhead stays negligible.
    struct UnwrapCancelCtx {
        const std::function<bool(int, int)>* progress = nullptr;
        int counter = 0;
        bool cancelled = false;
    } ucc;
    if (opts.progress) {
        ucc.progress = &opts.progress;
        xatlas::SetProgressCallback(
            atlas,
            [](xatlas::ProgressCategory, int, void* user) -> bool {
                auto* c = static_cast<UnwrapCancelCtx*>(user);
                if ((++c->counter & 63) != 0)
                    return true;
                if (!(*c->progress)(0, 1)) {
                    c->cancelled = true;
                    return false;
                }
                return true;
            },
            &ucc);
    }
    xatlas::PackOptions pack;
    pack.resolution = static_cast<uint32_t>(texSize);
    pack.padding    = std::max(1, opts.dilatePx);
    pack.bilinear   = true;
    xatlas::Generate(atlas, /*chartOptions=*/{}, pack);
    if (ucc.cancelled) {
        xatlas::Destroy(atlas);
        r = BakeResult{};
        r.cancelled = true;
        r.error = QStringLiteral("cancelled");
        return r;
    }
    if (atlas->meshCount != 1 || atlas->width == 0 || atlas->height == 0) {
        r.error = QStringLiteral("bake: xatlas produced no atlas.");
        xatlas::Destroy(atlas);
        return r;
    }
    const xatlas::Mesh& xm = atlas->meshes[0];
    const int W = static_cast<int>(atlas->width);
    const int H = static_cast<int>(atlas->height);

    r.positions.resize(static_cast<size_t>(xm.vertexCount) * 3);
    r.uvs.resize(static_cast<size_t>(xm.vertexCount) * 2);
    std::vector<uint32_t> xref(xm.vertexCount);
    for (uint32_t v = 0; v < xm.vertexCount; ++v) {
        const xatlas::Vertex& xv = xm.vertexArray[v];
        xref[v] = xv.xref;
        std::memcpy(&r.positions[v * 3],
                    &targetPositions[static_cast<size_t>(xv.xref) * 3],
                    sizeof(float) * 3);
        r.uvs[v * 2 + 0] = xv.uv[0] / float(W);
        r.uvs[v * 2 + 1] = xv.uv[1] / float(H);
    }
    r.indices.assign(xm.indexArray, xm.indexArray + xm.indexCount);
    r.vertexCount   = static_cast<int>(xm.vertexCount);
    r.triangleCount = static_cast<int>(xm.indexCount / 3);

    // ---- 2. target normals + tangents (Lengyel), source normals -------------
    // Target normals come from the ORIGINAL (pre-split) mesh and are carried
    // through the xatlas re-index via xref — computing them on the split mesh
    // would flatten every chart seam into a hard edge, and an identity bake
    // (source == target) would stop being the flat (128,128,255) map.
    std::vector<float> tNormals = creaseAwareVertexNormals(
        targetPositions, targetIndices, xref, r.indices, r.vertexCount);
    const std::vector<float> sNormals = smoothNormalField(
        smoothNormals(sourcePositions, sourceIndices),
        sourcePositions, sourceIndices, opts.sourceNormalSmoothIterations);
    // The closest-point accelerator is needed here (shading-normal transfer) as
    // well as by the texel loop below.
    TriangleGrid grid;
    grid.build(sourcePositions, sourceIndices);
    if (opts.sourceNormalLowIterations > opts.sourceNormalSmoothIterations) {
        // continue from the mid-frequency field: only the extra passes are new
        const std::vector<float> sLow = smoothNormalField(
            sNormals, sourcePositions, sourceIndices,
            opts.sourceNormalLowIterations - opts.sourceNormalSmoothIterations);
        float mn3[3] = {targetPositions[0], targetPositions[1], targetPositions[2]};
        float mx3[3] = {mn3[0], mn3[1], mn3[2]};
        for (size_t v = 0; v < targetPositions.size() / 3; ++v)
            for (int k = 0; k < 3; ++k) {
                mn3[k] = std::min(mn3[k], targetPositions[v * 3 + k]);
                mx3[k] = std::max(mx3[k], targetPositions[v * 3 + k]);
            }
        float diag3[3] = {mx3[0] - mn3[0], mx3[1] - mn3[1], mx3[2] - mn3[2]};
        // a sampled normal only counts when the source surface is right there
        const float maxDist = 0.01f * len3(diag3);
        for (int v = 0; v < r.vertexCount; ++v) {
            const float* P = &r.positions[static_cast<size_t>(v) * 3];
            float* Nt = &tNormals[static_cast<size_t>(v) * 3];
            float S[3], sb[3];
            const int sTri = grid.closest(P, S, sb, Nt);
            if (sTri < 0)
                continue;
            float d3[3];
            sub3(S, P, d3);
            if (len3(d3) > maxDist)
                continue;
            const uint32_t* si = &sourceIndices[static_cast<size_t>(sTri) * 3];
            float Ns[3];
            for (int k = 0; k < 3; ++k)
                Ns[k] = sb[0] * sLow[si[0] * 3 + k] + sb[1] * sLow[si[1] * 3 + k]
                      + sb[2] * sLow[si[2] * 3 + k];
            if (!(len3(Ns) > 1e-12f))
                continue;
            normalize3(Ns);
            if (dot3(Ns, Nt) > 0.35f)   // roughly the same way as the target
                std::memcpy(Nt, Ns, sizeof(Ns));
        }
    }
    std::vector<float> tTangent;    // xyzw per vertex (w = handedness)
    if (opts.bakeNormalMap) {
        std::vector<float> tan1(r.positions.size(), 0.0f);
        std::vector<float> tan2(r.positions.size(), 0.0f);
        for (size_t t = 0; t + 2 < r.indices.size(); t += 3) {
            const uint32_t i0 = r.indices[t], i1 = r.indices[t + 1],
                           i2 = r.indices[t + 2];
            const float* p0 = &r.positions[i0 * 3];
            const float* p1 = &r.positions[i1 * 3];
            const float* p2 = &r.positions[i2 * 3];
            const float* u0 = &r.uvs[i0 * 2];
            const float* u1 = &r.uvs[i1 * 2];
            const float* u2 = &r.uvs[i2 * 2];
            float e1[3], e2[3];
            sub3(p1, p0, e1);
            sub3(p2, p0, e2);
            const float du1 = u1[0] - u0[0], dv1 = u1[1] - u0[1];
            const float du2 = u2[0] - u0[0], dv2 = u2[1] - u0[1];
            const float det = du1 * dv2 - du2 * dv1;
            if (std::fabs(det) < 1e-20f)
                continue;
            const float rd = 1.0f / det;
            const float sdir[3] = {(e1[0] * dv2 - e2[0] * dv1) * rd,
                                   (e1[1] * dv2 - e2[1] * dv1) * rd,
                                   (e1[2] * dv2 - e2[2] * dv1) * rd};
            const float tdir[3] = {(e2[0] * du1 - e1[0] * du2) * rd,
                                   (e2[1] * du1 - e1[1] * du2) * rd,
                                   (e2[2] * du1 - e1[2] * du2) * rd};
            for (uint32_t i : {i0, i1, i2}) {
                for (int k = 0; k < 3; ++k) {
                    tan1[i * 3 + k] += sdir[k];
                    tan2[i * 3 + k] += tdir[k];
                }
            }
        }
        tTangent.resize(static_cast<size_t>(r.vertexCount) * 4);
        for (int v = 0; v < r.vertexCount; ++v) {
            const float* n = &tNormals[static_cast<size_t>(v) * 3];
            const float* t1 = &tan1[static_cast<size_t>(v) * 3];
            float t[3] = {t1[0] - n[0] * dot3(n, t1),
                          t1[1] - n[1] * dot3(n, t1),
                          t1[2] - n[2] * dot3(n, t1)};
            if (len3(t) < 1e-12f) {
                // Degenerate UV — pick any tangent orthogonal to n.
                const float up[3] = {0.0f,
                                     std::fabs(n[1]) < 0.9f ? 1.0f : 0.0f,
                                     std::fabs(n[1]) < 0.9f ? 0.0f : 1.0f};
                cross3(up, n, t);
            }
            normalize3(t);
            float bc[3];
            cross3(n, t, bc);
            const float wsign =
                dot3(bc, &tan2[static_cast<size_t>(v) * 3]) < 0.0f ? -1.0f
                                                                   : 1.0f;
            tTangent[static_cast<size_t>(v) * 4 + 0] = t[0];
            tTangent[static_cast<size_t>(v) * 4 + 1] = t[1];
            tTangent[static_cast<size_t>(v) * 4 + 2] = t[2];
            tTangent[static_cast<size_t>(v) * 4 + 3] = wsign;
        }
    }

    // ---- 3. source closest-point accelerator (built above) -------------------

    // ---- 4. rasterize + sample -------------------------------------------------
    QImage baseColor(W, H, QImage::Format_RGBA8888);
    baseColor.fill(QColor(110, 110, 110, 255));
    QImage rough(W, H, QImage::Format_Grayscale8);
    rough.fill(204);
    QImage metal(W, H, QImage::Format_Grayscale8);
    metal.fill(0);
    QImage normal;
    if (opts.bakeNormalMap) {
        normal = QImage(W, H, QImage::Format_RGB888);
        normal.fill(QColor(128, 128, 255));
    }
    std::vector<uint8_t> covered(static_cast<size_t>(W) * H, 0);

    // All atlas (xm) references end here — the r.* copies carry everything
    // the sampling needs, so the atlas is freed before the heavy phase.
    xatlas::Destroy(atlas);

    // ---- Phase 1: serial UV rasterization → texel job list -------------------
    // Cheap (pure 2D coverage). Each covered texel becomes one independent
    // job; first triangle wins a texel (xatlas charts don't overlap).
    struct TexelJob { uint32_t lin; uint32_t tri; };
    std::vector<TexelJob> jobs;
    jobs.reserve(static_cast<size_t>(W) * H / 2);
    for (uint32_t t = 0; t + 2 < static_cast<uint32_t>(r.indices.size()); t += 3) {
        const uint32_t i0 = r.indices[t], i1 = r.indices[t + 1],
                       i2 = r.indices[t + 2];
        const float uv0[2] = {r.uvs[i0 * 2] * W, r.uvs[i0 * 2 + 1] * H};
        const float uv1[2] = {r.uvs[i1 * 2] * W, r.uvs[i1 * 2 + 1] * H};
        const float uv2[2] = {r.uvs[i2 * 2] * W, r.uvs[i2 * 2 + 1] * H};
        const int minX = std::max(0, static_cast<int>(std::floor(
            std::min({uv0[0], uv1[0], uv2[0]}))));
        const int maxX = std::min(W - 1, static_cast<int>(std::ceil(
            std::max({uv0[0], uv1[0], uv2[0]}))));
        const int minY = std::max(0, static_cast<int>(std::floor(
            std::min({uv0[1], uv1[1], uv2[1]}))));
        const int maxY = std::min(H - 1, static_cast<int>(std::ceil(
            std::max({uv0[1], uv1[1], uv2[1]}))));
        const float denom = (uv1[1] - uv2[1]) * (uv0[0] - uv2[0])
                          + (uv2[0] - uv1[0]) * (uv0[1] - uv2[1]);
        if (std::fabs(denom) < 1e-12f)
            continue;
        const float inv = 1.0f / denom;
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const size_t lin = static_cast<size_t>(y) * W + x;
                if (covered[lin])
                    continue;
                const float px = x + 0.5f, py = y + 0.5f;
                const float w0 = ((uv1[1] - uv2[1]) * (px - uv2[0])
                                + (uv2[0] - uv1[0]) * (py - uv2[1])) * inv;
                const float w1 = ((uv2[1] - uv0[1]) * (px - uv2[0])
                                + (uv0[0] - uv2[0]) * (py - uv2[1])) * inv;
                const float w2 = 1.0f - w0 - w1;
                const float eps = -0.001f;
                if (w0 < eps || w1 < eps || w2 < eps)
                    continue;
                covered[lin] = 1;
                jobs.push_back({static_cast<uint32_t>(lin), t});
            }
        }
    }
    if (jobs.empty()) {
        r = BakeResult{};
        r.error = QStringLiteral("bake: no texels covered (unwrap failed?).");
        return r;
    }

    // ---- Phase 2: parallel sampling -------------------------------------------
    // Each job owns a unique texel: workers write disjoint pixels through raw
    // bits() pointers (detached up front), the grid/volume queries are const,
    // and the main thread pumps progress/cancellation off an atomic counter.
    // This is what keeps multi-million-triangle TRELLIS sources at seconds-
    // to-minutes instead of tens of minutes single-threaded.
    uchar* bcBits = baseColor.bits();
    const qsizetype bcBpl = baseColor.bytesPerLine();
    uchar* roBits = rough.bits();
    const qsizetype roBpl = rough.bytesPerLine();
    uchar* meBits = metal.bits();
    const qsizetype meBpl = metal.bytesPerLine();
    uchar* noBits = opts.bakeNormalMap ? normal.bits() : nullptr;
    const qsizetype noBpl = opts.bakeNormalMap ? normal.bytesPerLine() : 0;

    std::atomic<size_t> nextJob{0};
    std::atomic<size_t> doneJobs{0};
    std::atomic<bool>   abortBake{false};
    const unsigned nThreads = std::min(
        16u, std::max(1u, std::thread::hardware_concurrency()));
    auto workerFn = [&]() {
        constexpr size_t kChunk = 1024;
        for (;;) {
            const size_t start = nextJob.fetch_add(kChunk);
            if (start >= jobs.size() || abortBake.load(std::memory_order_relaxed))
                return;
            const size_t endJ = std::min(jobs.size(), start + kChunk);
            for (size_t j = start; j < endJ; ++j) {
                const uint32_t lin = jobs[j].lin;
                const uint32_t t = jobs[j].tri;
                const int x = static_cast<int>(lin % W);
                const int y = static_cast<int>(lin / W);
                const uint32_t i0 = r.indices[t], i1 = r.indices[t + 1],
                               i2 = r.indices[t + 2];
                const float uv0[2] = {r.uvs[i0 * 2] * W, r.uvs[i0 * 2 + 1] * H};
                const float uv1[2] = {r.uvs[i1 * 2] * W, r.uvs[i1 * 2 + 1] * H};
                const float uv2[2] = {r.uvs[i2 * 2] * W, r.uvs[i2 * 2 + 1] * H};
                const float denom = (uv1[1] - uv2[1]) * (uv0[0] - uv2[0])
                                  + (uv2[0] - uv1[0]) * (uv0[1] - uv2[1]);
                if (std::fabs(denom) < 1e-12f)
                    continue;
                const float inv = 1.0f / denom;

                float accAttr[6] = {0, 0, 0, 0, 0, 0};
                float accN[3] = {0, 0, 0};
                int samples = 0;
                for (int sy = 0; sy < ss; ++sy) {
                    for (int sx = 0; sx < ss; ++sx) {
                        const float px = ss == 1 ? x + 0.5f
                            : x + (sx + 0.5f) / ss;
                        const float py = ss == 1 ? y + 0.5f
                            : y + (sy + 0.5f) / ss;
                        float w0 = ((uv1[1] - uv2[1]) * (px - uv2[0])
                                  + (uv2[0] - uv1[0]) * (py - uv2[1])) * inv;
                        float w1 = ((uv2[1] - uv0[1]) * (px - uv2[0])
                                  + (uv0[0] - uv2[0]) * (py - uv2[1])) * inv;
                        float w2 = 1.0f - w0 - w1;
                        w0 = std::max(w0, 0.0f);
                        w1 = std::max(w1, 0.0f);
                        w2 = std::max(w2, 0.0f);
                        const float wsumB = w0 + w1 + w2;
                        if (wsumB < 1e-12f)
                            continue;
                        w0 /= wsumB; w1 /= wsumB; w2 /= wsumB;

                        float P[3], Nt[3];
                        for (int k = 0; k < 3; ++k) {
                            P[k] = w0 * r.positions[i0 * 3 + k]
                                 + w1 * r.positions[i1 * 3 + k]
                                 + w2 * r.positions[i2 * 3 + k];
                            Nt[k] = w0 * tNormals[i0 * 3 + k]
                                  + w1 * tNormals[i1 * 3 + k]
                                  + w2 * tNormals[i2 * 3 + k];
                        }
                        normalize3(Nt);

                        float S[3], sb[3];
                        int sTri = grid.closest(P, S, sb, Nt);
                        if (sTri < 0)
                            sTri = grid.closest(P, S, sb);
                        float attr[6];
                        if (!volume.sample(sTri >= 0 ? S : P, attr))
                            continue;   // no opaque voxel within reach —
                                        // attr is unwritten; skip so the
                                        // texel dilates from valid
                                        // neighbours instead of averaging
                                        // garbage (rendered as dark pepper
                                        // speckle on fuzzy subjects)
                        for (int c = 0; c < 6; ++c)
                            accAttr[c] += attr[c];

                        if (opts.bakeNormalMap) {
                            float Ns[3] = {0.0f, 1.0f, 0.0f};
                            if (sTri >= 0) {
                                const uint32_t* sidx = &sourceIndices[sTri * 3];
                                for (int k = 0; k < 3; ++k)
                                    Ns[k] = sb[0] * sNormals[sidx[0] * 3 + k]
                                          + sb[1] * sNormals[sidx[1] * 3 + k]
                                          + sb[2] * sNormals[sidx[2] * 3 + k];
                                normalize3(Ns);
                                float fn[3];
                                resolveDetailNormal(
                                    Nt,
                                    sourceFaceNormal(sourcePositions,
                                                     sourceIndices, sTri, fn)
                                        ? fn : nullptr,
                                    Ns);
                            } else {
                                std::memcpy(Ns, Nt, sizeof(Ns));
                            }
                            float T[3], wsign = 1.0f;
                            for (int k = 0; k < 3; ++k)
                                T[k] = w0 * tTangent[i0 * 4 + k]
                                     + w1 * tTangent[i1 * 4 + k]
                                     + w2 * tTangent[i2 * 4 + k];
                            wsign = (w0 * tTangent[i0 * 4 + 3]
                                   + w1 * tTangent[i1 * 4 + 3]
                                   + w2 * tTangent[i2 * 4 + 3]) < 0.0f
                                ? -1.0f : 1.0f;
                            const float ndt = dot3(Nt, T);
                            for (int k = 0; k < 3; ++k)
                                T[k] -= Nt[k] * ndt;
                            if (len3(T) < 1e-12f) {
                                const float up[3] = {0, 1, 0};
                                cross3(up, Nt, T);
                            }
                            normalize3(T);
                            float B[3];
                            cross3(Nt, T, B);
                            for (int k = 0; k < 3; ++k)
                                B[k] *= wsign * kViewportBitangentSign;
                            float nts[3] = {dot3(Ns, T), dot3(Ns, B),
                                            dot3(Ns, Nt)};
                            normalize3(nts);
                            // A detail normal can never point INTO the
                            // surface: negative tangent-space z texels (a
                            // few % on dense noisy sources) render as dark
                            // glints under lighting. Clamp and renormalize.
                            if (nts[2] < 0.05f) nts[2] = 0.05f;
                            normalize3(nts);
                            for (int k = 0; k < 3; ++k)
                                accN[k] += nts[k];
                        }
                        ++samples;
                    }
                }
                if (samples == 0) {
                    // Every subsample failed: un-cover the texel so the
                    // border dilation + background fill treat it like an
                    // unrasterized one. Jobs own disjoint texels, so this
                    // byte write is race-free.
                    covered[lin] = 0;
                    continue;
                }
                const float invS = 1.0f / samples;
                uchar* bc = bcBits + static_cast<size_t>(y) * bcBpl
                          + static_cast<size_t>(x) * 4;
                bc[0] = toByte(accAttr[0] * invS);
                bc[1] = toByte(accAttr[1] * invS);
                bc[2] = toByte(accAttr[2] * invS);
                // OPAQUE by design: game-ready assets bake the surface as
                // solid (the whole sampler intentionally reads through the
                // transparent TRELLIS skin). Writing the volume's alpha here
                // let low-alpha rim texels alpha-blend at render time and
                // flash as bright seam speckle in every viewer.
                bc[3] = 255;
                roBits[static_cast<size_t>(y) * roBpl + x] =
                    toByte(accAttr[4] * invS);
                meBits[static_cast<size_t>(y) * meBpl + x] =
                    toByte(accAttr[3] * invS);
                if (noBits) {
                    float n[3] = {accN[0] * invS, accN[1] * invS,
                                  accN[2] * invS};
                    normalize3(n);
                    uchar* np = noBits + static_cast<size_t>(y) * noBpl
                              + static_cast<size_t>(x) * 3;
                    np[0] = toByte(n[0] * 0.5f + 0.5f);
                    np[1] = toByte(n[1] * 0.5f + 0.5f);
                    np[2] = toByte(n[2] * 0.5f + 0.5f);
                }
            }
            doneJobs.fetch_add(endJ - start);
        }
    };
    {
        std::vector<std::thread> workers;
        workers.reserve(nThreads);
        for (unsigned i = 0; i < nThreads; ++i)
            workers.emplace_back(workerFn);
        while (doneJobs.load() < jobs.size()
               && !abortBake.load(std::memory_order_relaxed)) {
            if (opts.progress
                && !opts.progress(static_cast<int>(doneJobs.load()),
                                  static_cast<int>(jobs.size())))
                abortBake.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        for (auto& th : workers)
            th.join();
    }
    if (abortBake.load()) {
        r = BakeResult{};
        r.cancelled = true;
        r.error = QStringLiteral("cancelled");
        return r;
    }


    size_t coveredCount = 0;
    for (uint8_t c : covered)
        coveredCount += c;
    if (coveredCount == 0) {
        r = BakeResult{};
        r.error = QStringLiteral("bake: no texels covered (unwrap failed?).");
        return r;
    }

    // ---- 5. dilate chart borders on every channel -----------------------------
    struct Channel {
        QImage* img;
        int bpp;
    };
    std::vector<Channel> channels = {{&baseColor, 4}, {&rough, 1}, {&metal, 1}};
    if (opts.bakeNormalMap)
        channels.push_back({&normal, 3});
    for (int pass = 0; pass < opts.dilatePx; ++pass) {
        std::vector<uint8_t> next = covered;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const size_t lin = static_cast<size_t>(y) * W + x;
                if (covered[lin])
                    continue;
                for (int dy = -1; dy <= 1 && !next[lin]; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int sx = x + dx, sy = y + dy;
                        if (sx < 0 || sy < 0 || sx >= W || sy >= H)
                            continue;
                        if (!covered[static_cast<size_t>(sy) * W + sx])
                            continue;
                        for (const Channel& ch : channels) {
                            std::memcpy(
                                ch.img->scanLine(y)
                                    + static_cast<size_t>(x) * ch.bpp,
                                ch.img->scanLine(sy)
                                    + static_cast<size_t>(sx) * ch.bpp,
                                static_cast<size_t>(ch.bpp));
                        }
                        next[lin] = 1;
                        break;
                    }
                }
            }
        }
        covered.swap(next);
    }

    // Mip-safe fill: after the exact gutter dilation above, fill EVERY
    // remaining uncovered texel — inter-chart background AND interior holes
    // (failed samples) — with its NEAREST covered texel's values via a
    // jump-flood pass. The previous global-average flood was the source of
    // the "dark chip" flecks on detailed models: a heavily-charted simplify
    // (thousands of tiny charts) mixes bright skin and dark leather, so the
    // average is mud, and both mip bleed at chart borders and any interior
    // fill landed muddy chips onto bright surfaces. Nearest-texel fill makes
    // every empty texel extend its closest chart instead.
    {
        constexpr int32_t kNoSeed = -1;
        std::vector<int32_t> seed(static_cast<size_t>(W) * H, kNoSeed);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t lin = static_cast<size_t>(y) * W + x;
                if (covered[lin])
                    seed[lin] = static_cast<int32_t>(lin);
            }
        auto better = [&](int32_t cand, int x, int y, int32_t cur) {
            if (cand == kNoSeed) return false;
            const int cx = cand % W, cy = cand / W;
            const long long dcx = cx - x, dcy = cy - y;
            const long long dc = dcx * dcx + dcy * dcy;
            if (cur == kNoSeed) return true;
            const int ux = cur % W, uy = cur / W;
            const long long dux = ux - x, duy = uy - y;
            return dc < dux * dux + duy * duy;
        };
        int step = 1;
        while (step < std::max(W, H)) step <<= 1;
        for (; step >= 1; step >>= 1) {
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    const size_t lin = static_cast<size_t>(y) * W + x;
                    int32_t best = seed[lin];
                    for (int dy = -1; dy <= 1; ++dy) {
                        const int ny = y + dy * step;
                        if (ny < 0 || ny >= H) continue;
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = x + dx * step;
                            if (nx < 0 || nx >= W) continue;
                            const int32_t cand =
                                seed[static_cast<size_t>(ny) * W + nx];
                            if (better(cand, x, y, best))
                                best = cand;
                        }
                    }
                    seed[lin] = best;
                }
            }
        }
        for (int y = 0; y < H; ++y) {
            uchar* bcRow = bcBits + static_cast<size_t>(y) * bcBpl;
            uchar* roRow = roBits + static_cast<size_t>(y) * roBpl;
            uchar* meRow = meBits + static_cast<size_t>(y) * meBpl;
            for (int x = 0; x < W; ++x) {
                const size_t lin = static_cast<size_t>(y) * W + x;
                if (covered[lin])
                    continue;
                const int32_t src = seed[lin];
                if (src == kNoSeed)
                    continue;   // fully empty atlas (no covered texels)
                const int sx = src % W, sy = src / W;
                const uchar* sbc = bcBits + static_cast<size_t>(sy) * bcBpl
                                 + static_cast<size_t>(sx) * 4;
                bcRow[x * 4 + 0] = sbc[0];
                bcRow[x * 4 + 1] = sbc[1];
                bcRow[x * 4 + 2] = sbc[2];
                bcRow[x * 4 + 3] = 255;
                roRow[x] = (roBits + static_cast<size_t>(sy) * roBpl)[sx];
                meRow[x] = (meBits + static_cast<size_t>(sy) * meBpl)[sx];
                if (noBits) {
                    uchar* nRow = noBits + static_cast<size_t>(y) * noBpl;
                    const uchar* snr = noBits + static_cast<size_t>(sy) * noBpl
                                     + static_cast<size_t>(sx) * 3;
                    nRow[x * 3 + 0] = snr[0];
                    nRow[x * 3 + 1] = snr[1];
                    nRow[x * 3 + 2] = snr[2];
                }
            }
        }
    }

    if (opts.progress)
        opts.progress(static_cast<int>(jobs.size()), static_cast<int>(jobs.size()));

    r.baseColor = std::move(baseColor);
    r.roughness = std::move(rough);
    r.metallic  = std::move(metal);
    if (opts.bakeNormalMap)
        r.normalMap = std::move(normal);
    r.normals = tNormals;   // welded smooth shading normals (see header)
    r.ok = true;
    return r;
}

// ---- bakeDetailNormal ----------------------------------------------------------
NormalBakeResult bakeDetailNormal(const std::vector<float>& targetPositions,
                                  const std::vector<uint32_t>& targetIndices,
                                  const std::vector<float>& targetUvs,
                                  int width, int height,
                                  const std::vector<float>& sourcePositions,
                                  const std::vector<uint32_t>& sourceIndices,
                                  const BakeOptions& opts)
{
    NormalBakeResult r;
    const size_t nv = targetPositions.size() / 3;
    if (nv == 0 || targetIndices.size() < 3 || targetIndices.size() % 3 != 0
        || targetUvs.size() != nv * 2) {
        r.error = QStringLiteral("detail normal: invalid target mesh/uvs.");
        return r;
    }
    for (uint32_t i : targetIndices) {
        if (i >= nv) {
            r.error = QStringLiteral("detail normal: target index out of range.");
            return r;
        }
    }
    const size_t snv = sourcePositions.size() / 3;
    if (snv == 0 || sourceIndices.size() < 3 || sourceIndices.size() % 3 != 0) {
        r.error = QStringLiteral("detail normal: invalid source mesh.");
        return r;
    }
    for (uint32_t i : sourceIndices) {
        if (i >= snv) {
            r.error = QStringLiteral("detail normal: source index out of range.");
            return r;
        }
    }
    if (width < 8 || height < 8 || width > 16384 || height > 16384) {
        r.error = QStringLiteral("detail normal: bad atlas size.");
        return r;
    }
    const int W = width, H = height;

    // Shading normals: position-welded so chart seams stay smooth; tangents
    // accumulated per split vertex (UV seams SHOULD split the tangent basis).
    const std::vector<float> tNormals =
        smoothNormalsWelded(targetPositions, targetIndices);
    const std::vector<float> sNormals = smoothNormalField(
        smoothNormalsWelded(sourcePositions, sourceIndices),
        sourcePositions, sourceIndices, opts.sourceNormalSmoothIterations);
    std::vector<float> tan1(targetPositions.size(), 0.0f);
    std::vector<float> tan2(targetPositions.size(), 0.0f);
    for (size_t t = 0; t + 2 < targetIndices.size(); t += 3) {
        const uint32_t i0 = targetIndices[t], i1 = targetIndices[t + 1],
                       i2 = targetIndices[t + 2];
        const float* p0 = &targetPositions[i0 * 3];
        const float* p1 = &targetPositions[i1 * 3];
        const float* p2 = &targetPositions[i2 * 3];
        const float* u0 = &targetUvs[i0 * 2];
        const float* u1 = &targetUvs[i1 * 2];
        const float* u2 = &targetUvs[i2 * 2];
        float e1[3], e2[3];
        sub3(p1, p0, e1);
        sub3(p2, p0, e2);
        const float du1 = u1[0] - u0[0], dv1 = u1[1] - u0[1];
        const float du2 = u2[0] - u0[0], dv2 = u2[1] - u0[1];
        const float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-20f)
            continue;
        const float rd = 1.0f / det;
        const float sdir[3] = {(e1[0] * dv2 - e2[0] * dv1) * rd,
                               (e1[1] * dv2 - e2[1] * dv1) * rd,
                               (e1[2] * dv2 - e2[2] * dv1) * rd};
        const float tdir[3] = {(e2[0] * du1 - e1[0] * du2) * rd,
                               (e2[1] * du1 - e1[1] * du2) * rd,
                               (e2[2] * du1 - e1[2] * du2) * rd};
        for (uint32_t i : {i0, i1, i2}) {
            for (int k = 0; k < 3; ++k) {
                tan1[i * 3 + k] += sdir[k];
                tan2[i * 3 + k] += tdir[k];
            }
        }
    }
    std::vector<float> tTangent(nv * 4);
    for (size_t v = 0; v < nv; ++v) {
        const float* n = &tNormals[v * 3];
        const float* t1 = &tan1[v * 3];
        float t[3] = {t1[0] - n[0] * dot3(n, t1), t1[1] - n[1] * dot3(n, t1),
                      t1[2] - n[2] * dot3(n, t1)};
        if (len3(t) < 1e-12f) {
            const float up[3] = {0.0f, std::fabs(n[1]) < 0.9f ? 1.0f : 0.0f,
                                 std::fabs(n[1]) < 0.9f ? 0.0f : 1.0f};
            cross3(up, n, t);
        }
        normalize3(t);
        float bc[3];
        cross3(n, t, bc);
        tTangent[v * 4 + 0] = t[0];
        tTangent[v * 4 + 1] = t[1];
        tTangent[v * 4 + 2] = t[2];
        tTangent[v * 4 + 3] = dot3(bc, &tan2[v * 3]) < 0.0f ? -1.0f : 1.0f;
    }

    TriangleGrid grid;
    grid.build(sourcePositions, sourceIndices);

    QImage normal(W, H, QImage::Format_RGB888);
    normal.fill(QColor(128, 128, 255));
    std::vector<uint8_t> covered(static_cast<size_t>(W) * H, 0);
    const int progressTotal = W * H;
    int processed = 0, sinceProgress = 0;

    for (size_t t = 0; t + 2 < targetIndices.size(); t += 3) {
        const uint32_t i0 = targetIndices[t], i1 = targetIndices[t + 1],
                       i2 = targetIndices[t + 2];
        // Atlas-pixel UVs (uvs are normalized [0,1]).
        const float uv0[2] = {targetUvs[i0 * 2] * W, targetUvs[i0 * 2 + 1] * H};
        const float uv1[2] = {targetUvs[i1 * 2] * W, targetUvs[i1 * 2 + 1] * H};
        const float uv2[2] = {targetUvs[i2 * 2] * W, targetUvs[i2 * 2 + 1] * H};
        const int minX = std::max(0, static_cast<int>(std::floor(
            std::min({uv0[0], uv1[0], uv2[0]}))));
        const int maxX = std::min(W - 1, static_cast<int>(std::ceil(
            std::max({uv0[0], uv1[0], uv2[0]}))));
        const int minY = std::max(0, static_cast<int>(std::floor(
            std::min({uv0[1], uv1[1], uv2[1]}))));
        const int maxY = std::min(H - 1, static_cast<int>(std::ceil(
            std::max({uv0[1], uv1[1], uv2[1]}))));
        const float denom = (uv1[1] - uv2[1]) * (uv0[0] - uv2[0])
                          + (uv2[0] - uv1[0]) * (uv0[1] - uv2[1]);
        if (std::fabs(denom) < 1e-12f)
            continue;
        const float inv = 1.0f / denom;
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const size_t lin = static_cast<size_t>(y) * W + x;
                if (covered[lin])
                    continue;
                const float px = x + 0.5f, py = y + 0.5f;
                const float w0 = ((uv1[1] - uv2[1]) * (px - uv2[0])
                                + (uv2[0] - uv1[0]) * (py - uv2[1])) * inv;
                const float w1 = ((uv2[1] - uv0[1]) * (px - uv2[0])
                                + (uv0[0] - uv2[0]) * (py - uv2[1])) * inv;
                const float w2 = 1.0f - w0 - w1;
                const float eps = -0.001f;
                if (w0 < eps || w1 < eps || w2 < eps)
                    continue;
                covered[lin] = 1;

                float P[3], Nt[3], T[3];
                for (int k = 0; k < 3; ++k) {
                    P[k] = w0 * targetPositions[i0 * 3 + k]
                         + w1 * targetPositions[i1 * 3 + k]
                         + w2 * targetPositions[i2 * 3 + k];
                    Nt[k] = w0 * tNormals[i0 * 3 + k]
                          + w1 * tNormals[i1 * 3 + k]
                          + w2 * tNormals[i2 * 3 + k];
                    T[k] = w0 * tTangent[i0 * 4 + k]
                         + w1 * tTangent[i1 * 4 + k]
                         + w2 * tTangent[i2 * 4 + k];
                }
                normalize3(Nt);
                const float wsign = (w0 * tTangent[i0 * 4 + 3]
                                   + w1 * tTangent[i1 * 4 + 3]
                                   + w2 * tTangent[i2 * 4 + 3]) < 0.0f
                    ? -1.0f : 1.0f;
                const float ndt = dot3(Nt, T);
                for (int k = 0; k < 3; ++k)
                    T[k] -= Nt[k] * ndt;
                if (len3(T) < 1e-12f) {
                    const float up[3] = {0, 1, 0};
                    cross3(up, Nt, T);
                }
                normalize3(T);
                float B[3];
                cross3(Nt, T, B);
                for (int k = 0; k < 3; ++k)
                    B[k] *= wsign * kViewportBitangentSign;

                float S[3], sb[3];
                int sTri = grid.closest(P, S, sb, Nt);
                if (sTri < 0)
                    sTri = grid.closest(P, S, sb);
                float Ns[3];
                if (sTri >= 0) {
                    const uint32_t* sidx = &sourceIndices[sTri * 3];
                    for (int k = 0; k < 3; ++k)
                        Ns[k] = sb[0] * sNormals[sidx[0] * 3 + k]
                              + sb[1] * sNormals[sidx[1] * 3 + k]
                              + sb[2] * sNormals[sidx[2] * 3 + k];
                    normalize3(Ns);
                    float fn[3];
                    resolveDetailNormal(
                        Nt,
                        sourceFaceNormal(sourcePositions, sourceIndices, sTri, fn)
                            ? fn : nullptr,
                        Ns);
                } else {
                    std::memcpy(Ns, Nt, sizeof(Ns));
                }
                float nts[3] = {dot3(Ns, T), dot3(Ns, B), dot3(Ns, Nt)};
                normalize3(nts);
                // Detail normals never point INTO the surface — clamp the
                // few inverted-z texels dense noisy sources produce (they
                // render as dark glints) and renormalize.
                if (nts[2] < 0.05f) { nts[2] = 0.05f; normalize3(nts); }
                uchar* np = normal.scanLine(y) + static_cast<size_t>(x) * 3;
                np[0] = toByte(nts[0] * 0.5f + 0.5f);
                np[1] = toByte(nts[1] * 0.5f + 0.5f);
                np[2] = toByte(nts[2] * 0.5f + 0.5f);

                ++processed;
                if (opts.progress && ++sinceProgress >= 8192) {
                    sinceProgress = 0;
                    if (!opts.progress(std::min(processed, progressTotal - 1),
                                       progressTotal)) {
                        r = NormalBakeResult{};
                        r.cancelled = true;
                        r.error = QStringLiteral("cancelled");
                        return r;
                    }
                }
            }
        }
    }

    size_t coveredCount = 0;
    for (uint8_t c : covered)
        coveredCount += c;
    if (coveredCount == 0) {
        r.error = QStringLiteral("detail normal: no texels covered.");
        return r;
    }

    // Border dilation (same policy as bake()).
    for (int pass = 0; pass < opts.dilatePx; ++pass) {
        std::vector<uint8_t> next = covered;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const size_t lin = static_cast<size_t>(y) * W + x;
                if (covered[lin])
                    continue;
                for (int dy = -1; dy <= 1 && !next[lin]; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int sx = x + dx, sy = y + dy;
                        if (sx < 0 || sy < 0 || sx >= W || sy >= H)
                            continue;
                        if (!covered[static_cast<size_t>(sy) * W + sx])
                            continue;
                        std::memcpy(normal.scanLine(y) + static_cast<size_t>(x) * 3,
                                    normal.scanLine(sy) + static_cast<size_t>(sx) * 3,
                                    3);
                        next[lin] = 1;
                        break;
                    }
                }
            }
        }
        covered.swap(next);
    }

    if (opts.progress)
        opts.progress(progressTotal, progressTotal);
    r.normalMap = std::move(normal);
    r.ok = true;
    return r;
}

} // namespace Trellis2Bake
