#include "GeodesicVoxelBind.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <queue>
#include <tuple>
#include <vector>

namespace {

// ─── Small vector helpers ───────────────────────────────────────────────────

struct V3 {
    double x = 0, y = 0, z = 0;
};

inline V3 sub(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 cross(const V3& a, const V3& b)
{
    return { a.y * b.z - a.z * b.y,
             a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x };
}
inline double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// ─── Triangle/box overlap (Akenine-Möller SAT) ──────────────────────────────
//
// Classic 13-axis separating-axis test: 3 box face normals (reduces
// to an AABB check of the triangle), the triangle normal, and 9
// cross products of box axes × triangle edges. Implemented from the
// paper ("Fast 3D Triangle-Box Overlap Testing", 2001) — no
// dependency.

inline void axisMinMax(double a, double b, double c, double& mn, double& mx)
{
    mn = std::min(a, std::min(b, c));
    mx = std::max(a, std::max(b, c));
}

// Tests one cross-product axis: projects the triangle verts (p0, p1,
// p2 pre-projected onto the axis) against the box projection radius.
inline bool axisSeparates(double p0, double p1, double p2, double rad)
{
    double mn, mx;
    axisMinMax(p0, p1, p2, mn, mx);
    return mn > rad || mx < -rad;
}

bool triBoxOverlap(const V3& boxCenter, const V3& boxHalf,
                   const V3& tv0, const V3& tv1, const V3& tv2)
{
    // Move the triangle into the box's local frame.
    const V3 v0 = sub(tv0, boxCenter);
    const V3 v1 = sub(tv1, boxCenter);
    const V3 v2 = sub(tv2, boxCenter);

    const V3 e0 = sub(v1, v0);
    const V3 e1 = sub(v2, v1);
    const V3 e2 = sub(v0, v2);

    // 1) The three box face normals — AABB test.
    double mn, mx;
    axisMinMax(v0.x, v1.x, v2.x, mn, mx);
    if (mn > boxHalf.x || mx < -boxHalf.x) return false;
    axisMinMax(v0.y, v1.y, v2.y, mn, mx);
    if (mn > boxHalf.y || mx < -boxHalf.y) return false;
    axisMinMax(v0.z, v1.z, v2.z, mn, mx);
    if (mn > boxHalf.z || mx < -boxHalf.z) return false;

    // 2) The nine cross-product axes a_ij = box axis i × edge j.
    // Written out per the paper (projections simplify because box
    // axes are the unit basis).
    auto testEdge = [&](const V3& e) -> bool {
        const double fex = std::abs(e.x);
        const double fey = std::abs(e.y);
        const double fez = std::abs(e.z);
        // axis = X × e = (0, -e.z, e.y)
        {
            const double p0 = -e.z * v0.y + e.y * v0.z;
            const double p1 = -e.z * v1.y + e.y * v1.z;
            const double p2 = -e.z * v2.y + e.y * v2.z;
            const double rad = fez * boxHalf.y + fey * boxHalf.z;
            if (axisSeparates(p0, p1, p2, rad)) return true;
        }
        // axis = Y × e = (e.z, 0, -e.x)
        {
            const double p0 = e.z * v0.x - e.x * v0.z;
            const double p1 = e.z * v1.x - e.x * v1.z;
            const double p2 = e.z * v2.x - e.x * v2.z;
            const double rad = fez * boxHalf.x + fex * boxHalf.z;
            if (axisSeparates(p0, p1, p2, rad)) return true;
        }
        // axis = Z × e = (-e.y, e.x, 0)
        {
            const double p0 = -e.y * v0.x + e.x * v0.y;
            const double p1 = -e.y * v1.x + e.x * v1.y;
            const double p2 = -e.y * v2.x + e.x * v2.y;
            const double rad = fey * boxHalf.x + fex * boxHalf.y;
            if (axisSeparates(p0, p1, p2, rad)) return true;
        }
        return false;
    };
    if (testEdge(e0) || testEdge(e1) || testEdge(e2)) return false;

    // 3) The triangle's own plane vs the box.
    const V3 n = cross(e0, e1);
    const double d = -dot(n, v0);
    const double r = boxHalf.x * std::abs(n.x)
                   + boxHalf.y * std::abs(n.y)
                   + boxHalf.z * std::abs(n.z);
    return std::abs(d) <= r;
}

// ─── Top-K insert into VertexWeights (sorted descending) ────────────────────

void pushTopK(SkinWeights::VertexWeights& vw, int maxK, int boneIdx, double weight)
{
    if (vw.count < maxK) {
        int i = vw.count;
        while (i > 0 && vw.weights[i - 1] < weight) {
            vw.weights[i]     = vw.weights[i - 1];
            vw.boneIndices[i] = vw.boneIndices[i - 1];
            --i;
        }
        vw.weights[i]     = weight;
        vw.boneIndices[i] = boneIdx;
        ++vw.count;
        return;
    }
    if (weight <= vw.weights[maxK - 1]) return;
    int i = maxK - 1;
    while (i > 0 && vw.weights[i - 1] < weight) {
        vw.weights[i]     = vw.weights[i - 1];
        vw.boneIndices[i] = vw.boneIndices[i - 1];
        --i;
    }
    vw.weights[i]     = weight;
    vw.boneIndices[i] = boneIdx;
}

// ─── The voxel grid ─────────────────────────────────────────────────────────

struct Grid {
    int nx = 0, ny = 0, nz = 0;
    double voxel = 1.0;               // cubic voxel edge length (world units)
    double ox = 0, oy = 0, oz = 0;    // world position of voxel (0,0,0) corner

    std::size_t count() const
    {
        return std::size_t(nx) * std::size_t(ny) * std::size_t(nz);
    }
    std::size_t idx(int x, int y, int z) const
    {
        return (std::size_t(z) * ny + y) * nx + x;
    }
    bool inside(int x, int y, int z) const
    {
        return x >= 0 && x < nx && y >= 0 && y < ny && z >= 0 && z < nz;
    }
    void toCell(double wx, double wy, double wz, int& x, int& y, int& z) const
    {
        x = int(std::floor((wx - ox) / voxel));
        y = int(std::floor((wy - oy) / voxel));
        z = int(std::floor((wz - oz) / voxel));
    }
    V3 center(int x, int y, int z) const
    {
        return { ox + (x + 0.5) * voxel,
                 oy + (y + 0.5) * voxel,
                 oz + (z + 0.5) * voxel };
    }
};

// Amanatides-Woo 3D-DDA: every voxel the segment [a, b] passes
// through. Coordinates are clamped into the grid — the caller
// guarantees the grid's padded AABB contains the mesh, but a bone
// can legitimately stick out of it (wide clothing rigs).
void ddaSegment(const Grid& g, const V3& a, const V3& b,
                std::vector<std::size_t>& outCells)
{
    int x, y, z, xe, ye, ze;
    g.toCell(a.x, a.y, a.z, x, y, z);
    g.toCell(b.x, b.y, b.z, xe, ye, ze);

    const V3 d = sub(b, a);
    const double len = std::sqrt(dot(d, d));
    if (len < 1e-12) {
        if (g.inside(x, y, z)) outCells.push_back(g.idx(x, y, z));
        return;
    }

    const int stepX = d.x > 0 ? 1 : (d.x < 0 ? -1 : 0);
    const int stepY = d.y > 0 ? 1 : (d.y < 0 ? -1 : 0);
    const int stepZ = d.z > 0 ? 1 : (d.z < 0 ? -1 : 0);

    auto tToBoundary = [&](double origin, double gridO, int cell, int step,
                           double dir) -> double {
        if (step == 0) return std::numeric_limits<double>::infinity();
        const double boundary = gridO + (cell + (step > 0 ? 1 : 0)) * g.voxel;
        return (boundary - origin) / dir;
    };

    double tMaxX = tToBoundary(a.x, g.ox, x, stepX, d.x);
    double tMaxY = tToBoundary(a.y, g.oy, y, stepY, d.y);
    double tMaxZ = tToBoundary(a.z, g.oz, z, stepZ, d.z);
    const double tDeltaX = stepX ? g.voxel / std::abs(d.x)
                                 : std::numeric_limits<double>::infinity();
    const double tDeltaY = stepY ? g.voxel / std::abs(d.y)
                                 : std::numeric_limits<double>::infinity();
    const double tDeltaZ = stepZ ? g.voxel / std::abs(d.z)
                                 : std::numeric_limits<double>::infinity();

    // Hard iteration cap: a segment can cross at most nx+ny+nz cells
    // (+ a small safety margin for FP edge cases).
    const int maxSteps = g.nx + g.ny + g.nz + 8;
    for (int i = 0; i < maxSteps; ++i) {
        if (g.inside(x, y, z)) outCells.push_back(g.idx(x, y, z));
        if (x == xe && y == ye && z == ze) break;
        if (tMaxX <= tMaxY && tMaxX <= tMaxZ) {
            if (tMaxX > 1.0) break;
            x += stepX; tMaxX += tDeltaX;
        } else if (tMaxY <= tMaxZ) {
            if (tMaxY > 1.0) break;
            y += stepY; tMaxY += tDeltaY;
        } else {
            if (tMaxZ > 1.0) break;
            z += stepZ; tMaxZ += tDeltaZ;
        }
    }
}

} // namespace

// ─── Public API ─────────────────────────────────────────────────────────────

GeodesicVoxelBind::Result GeodesicVoxelBind::compute(
    const float* vertexPositions,
    int vertexCount,
    const std::uint32_t* indices,
    std::size_t indexCount,
    const std::vector<SkinWeights::BoneSegment>& bones,
    const SkinWeightsOptions& opts,
    std::vector<SkinWeights::VertexWeights>& outWeights,
    std::vector<std::vector<int>>* outAllowedBones)
{
    Result res;
    if (!vertexPositions || vertexCount < 3 || bones.empty()) {
        res.error = QStringLiteral("degenerate input (no vertices or bones)");
        return res;
    }
    if (!indices || indexCount < 3) {
        res.error = QStringLiteral("no triangle indices — geodesic voxel "
                                   "binding needs a surface");
        return res;
    }

    // ── AABB + grid setup ──────────────────────────────────────────
    double mnx =  std::numeric_limits<double>::max(), mny = mnx, mnz = mnx;
    double mxx = -mnx, mxy = -mnx, mxz = -mnx;
    for (int i = 0; i < vertexCount; ++i) {
        const float* p = vertexPositions + 3 * i;
        mnx = std::min<double>(mnx, p[0]); mxx = std::max<double>(mxx, p[0]);
        mny = std::min<double>(mny, p[1]); mxy = std::max<double>(mxy, p[1]);
        mnz = std::min<double>(mnz, p[2]); mxz = std::max<double>(mxz, p[2]);
    }
    const double ex = mxx - mnx, ey = mxy - mny, ez = mxz - mnz;
    const double maxExtent = std::max(ex, std::max(ey, ez));
    if (maxExtent < 1e-12) {
        res.error = QStringLiteral("mesh has zero extent");
        return res;
    }
    const double diag = std::sqrt(ex * ex + ey * ey + ez * ez);
    const double maxGeoDist = (opts.maxInfluenceDistance > 0)
        ? opts.maxInfluenceDistance * diag
        : std::numeric_limits<double>::infinity();

    Grid g;
    const int resolution = std::clamp(opts.voxelResolution, 8, 256);
    g.voxel = maxExtent / resolution;
    // +2 voxels of padding on every axis so the exterior flood fill
    // always has a guaranteed-empty boundary shell to start from.
    g.nx = int(std::ceil(ex / g.voxel)) + 2;
    g.ny = int(std::ceil(ey / g.voxel)) + 2;
    g.nz = int(std::ceil(ez / g.voxel)) + 2;
    g.ox = mnx - g.voxel;
    g.oy = mny - g.voxel;
    g.oz = mnz - g.voxel;
    res.gridX = g.nx; res.gridY = g.ny; res.gridZ = g.nz;

    const std::size_t numVox = g.count();

    // ── 1) Voxelize the surface ────────────────────────────────────
    std::vector<std::uint8_t> surface(numVox, 0);
    const V3 half { g.voxel * 0.5, g.voxel * 0.5, g.voxel * 0.5 };
    const std::size_t triCount = indexCount / 3;
    for (std::size_t t = 0; t < triCount; ++t) {
        const std::uint32_t i0 = indices[3 * t + 0];
        const std::uint32_t i1 = indices[3 * t + 1];
        const std::uint32_t i2 = indices[3 * t + 2];
        if (i0 >= std::uint32_t(vertexCount) || i1 >= std::uint32_t(vertexCount)
            || i2 >= std::uint32_t(vertexCount))
            continue;
        const float* p0 = vertexPositions + 3 * i0;
        const float* p1 = vertexPositions + 3 * i1;
        const float* p2 = vertexPositions + 3 * i2;
        const V3 v0 { p0[0], p0[1], p0[2] };
        const V3 v1 { p1[0], p1[1], p1[2] };
        const V3 v2 { p2[0], p2[1], p2[2] };

        // Candidate cells = the triangle's AABB in voxel coords.
        int cx0, cy0, cz0, cx1, cy1, cz1;
        g.toCell(std::min(v0.x, std::min(v1.x, v2.x)),
                 std::min(v0.y, std::min(v1.y, v2.y)),
                 std::min(v0.z, std::min(v1.z, v2.z)), cx0, cy0, cz0);
        g.toCell(std::max(v0.x, std::max(v1.x, v2.x)),
                 std::max(v0.y, std::max(v1.y, v2.y)),
                 std::max(v0.z, std::max(v1.z, v2.z)), cx1, cy1, cz1);
        cx0 = std::clamp(cx0, 0, g.nx - 1); cx1 = std::clamp(cx1, 0, g.nx - 1);
        cy0 = std::clamp(cy0, 0, g.ny - 1); cy1 = std::clamp(cy1, 0, g.ny - 1);
        cz0 = std::clamp(cz0, 0, g.nz - 1); cz1 = std::clamp(cz1, 0, g.nz - 1);

        for (int z = cz0; z <= cz1; ++z)
            for (int y = cy0; y <= cy1; ++y)
                for (int x = cx0; x <= cx1; ++x) {
                    const std::size_t vi = g.idx(x, y, z);
                    if (surface[vi]) continue;
                    if (triBoxOverlap(g.center(x, y, z), half, v0, v1, v2))
                        surface[vi] = 1;
                }
    }
    res.surfaceVoxels = int(std::count(surface.begin(), surface.end(),
                                       std::uint8_t(1)));
    if (res.surfaceVoxels == 0) {
        res.error = QStringLiteral("voxelization produced no surface voxels");
        return res;
    }

    // ── 2) Classify: flood-fill EXTERIOR from the boundary shell ───
    std::vector<std::uint8_t> exterior(numVox, 0);
    {
        std::vector<std::size_t> stack;
        auto seed = [&](int x, int y, int z) {
            const std::size_t vi = g.idx(x, y, z);
            if (!surface[vi] && !exterior[vi]) {
                exterior[vi] = 1;
                stack.push_back(vi);
            }
        };
        for (int z = 0; z < g.nz; ++z)
            for (int y = 0; y < g.ny; ++y) {
                seed(0, y, z); seed(g.nx - 1, y, z);
            }
        for (int z = 0; z < g.nz; ++z)
            for (int x = 0; x < g.nx; ++x) {
                seed(x, 0, z); seed(x, g.ny - 1, z);
            }
        for (int y = 0; y < g.ny; ++y)
            for (int x = 0; x < g.nx; ++x) {
                seed(x, y, 0); seed(x, y, g.nz - 1);
            }
        // 6-connected fill: the exterior must not leak through a
        // voxel-thick surface diagonally.
        while (!stack.empty()) {
            const std::size_t vi = stack.back();
            stack.pop_back();
            const int x = int(vi % g.nx);
            const int y = int((vi / g.nx) % g.ny);
            const int z = int(vi / (std::size_t(g.nx) * g.ny));
            const int nb[6][3] = { {x-1,y,z}, {x+1,y,z}, {x,y-1,z},
                                   {x,y+1,z}, {x,y,z-1}, {x,y,z+1} };
            for (const auto& c : nb) {
                if (!g.inside(c[0], c[1], c[2])) continue;
                const std::size_t ni = g.idx(c[0], c[1], c[2]);
                if (surface[ni] || exterior[ni]) continue;
                exterior[ni] = 1;
                stack.push_back(ni);
            }
        }
    }
    // solid = surface ∪ interior; interior = not surface, not exterior.
    std::vector<std::uint8_t> solid(numVox, 0);
    int interiorCount = 0;
    for (std::size_t i = 0; i < numVox; ++i) {
        if (surface[i]) { solid[i] = 1; continue; }
        if (!exterior[i]) { solid[i] = 1; ++interiorCount; }
    }
    res.interiorVoxels = interiorCount;
    if (std::getenv("QTMESH_GVB_DEBUG")) {
        std::fprintf(stderr,
            "[gvb] verts=%d tris=%zu aabb=(%.4f %.4f %.4f)-(%.4f %.4f %.4f) "
            "grid=%dx%dx%d voxel=%.5f surface=%d interior=%d bones=%zu\n",
            vertexCount, triCount, mnx, mny, mnz, mxx, mxy, mxz,
            g.nx, g.ny, g.nz, g.voxel, res.surfaceVoxels, interiorCount,
            bones.size());
        if (!bones.empty())
            std::fprintf(stderr,
                "[gvb] bone[0] head=(%.4f %.4f %.4f) tail=(%.4f %.4f %.4f)\n",
                bones[0].headX, bones[0].headY, bones[0].headZ,
                bones[0].tailX, bones[0].tailY, bones[0].tailZ);
    }
    if (interiorCount == 0) {
        // Planes / cloth / billboards enclose no volume — geodesic
        // distances through the interior are meaningless. Caller
        // falls back to the inverse-distance heuristic.
        res.error = QStringLiteral("mesh encloses no volume at voxel "
                                   "resolution (plane / cloth / billboard)");
        return res;
    }

    // Helper: nearest solid voxel to a cell, expanding shell search.
    auto nearestSolid = [&](int x, int y, int z, int maxRadius) -> std::ptrdiff_t {
        x = std::clamp(x, 0, g.nx - 1);
        y = std::clamp(y, 0, g.ny - 1);
        z = std::clamp(z, 0, g.nz - 1);
        if (solid[g.idx(x, y, z)]) return std::ptrdiff_t(g.idx(x, y, z));
        for (int r = 1; r <= maxRadius; ++r) {
            std::ptrdiff_t best = -1;
            int bestD2 = std::numeric_limits<int>::max();
            for (int dz = -r; dz <= r; ++dz)
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) {
                        // Only the shell of the cube — inner cells
                        // were covered by smaller radii.
                        if (std::max(std::abs(dx),
                                     std::max(std::abs(dy), std::abs(dz))) != r)
                            continue;
                        const int cx = x + dx, cy = y + dy, cz = z + dz;
                        if (!g.inside(cx, cy, cz)) continue;
                        const std::size_t ci = g.idx(cx, cy, cz);
                        if (!solid[ci]) continue;
                        const int d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 < bestD2) { bestD2 = d2; best = std::ptrdiff_t(ci); }
                    }
            if (best >= 0) return best;
        }
        return -1;
    };

    // ── 3) Rasterize bones → seed voxels ───────────────────────────
    // Per-voxel best-K (bone, distance) pairs, flat arrays.
    const int K = kPairsPerVoxel;
    std::vector<std::int32_t> pairBone(numVox * K, -1);
    std::vector<float>        pairDist(numVox * K,
                                       std::numeric_limits<float>::infinity());

    // insert() returns true when the (bone, dist) pair improved the
    // voxel's list (new bone, or shorter path for a known bone).
    auto insertPair = [&](std::size_t vox, int bone, float dist) -> bool {
        const std::size_t base = vox * K;
        int emptySlot = -1, maxSlot = 0;
        float maxDist = -1.0f;
        for (int k = 0; k < K; ++k) {
            const std::int32_t b = pairBone[base + k];
            if (b == bone) {
                if (dist < pairDist[base + k] - 1e-9f) {
                    pairDist[base + k] = dist;
                    return true;
                }
                return false;
            }
            if (b < 0 && emptySlot < 0) emptySlot = k;
            if (b >= 0 && pairDist[base + k] > maxDist) {
                maxDist = pairDist[base + k];
                maxSlot = k;
            }
        }
        if (emptySlot >= 0) {
            pairBone[base + emptySlot] = bone;
            pairDist[base + emptySlot] = dist;
            return true;
        }
        if (dist < maxDist - 1e-9f) {
            pairBone[base + maxSlot] = bone;
            pairDist[base + maxSlot] = dist;
            return true;
        }
        return false;
    };

    // Min-heap of (distance, voxel, bone).
    using QE = std::tuple<float, std::size_t, std::int32_t>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> queue;

    std::vector<std::size_t> cells;
    for (std::size_t b = 0; b < bones.size(); ++b) {
        const auto& seg = bones[b];
        cells.clear();
        ddaSegment(g, { seg.headX, seg.headY, seg.headZ },
                      { seg.tailX, seg.tailY, seg.tailZ }, cells);
        bool seeded = false;
        for (const std::size_t c : cells) {
            if (!solid[c]) continue;
            if (insertPair(c, int(b), 0.0f)) queue.emplace(0.0f, c, int(b));
            seeded = true;
        }
        if (!seeded) {
            // Bone lies outside the solid (wide clothing, offset
            // helper bones): snap to the nearest solid voxel within
            // a small radius of the segment midpoint. The world-
            // distance check matters — nearestSolid clamps into the
            // grid, so without it a bone 50 units away would
            // silently snap onto the mesh edge.
            const V3 mid { (seg.headX + seg.tailX) * 0.5,
                           (seg.headY + seg.tailY) * 0.5,
                           (seg.headZ + seg.tailZ) * 0.5 };
            int mx, my, mz;
            g.toCell(mid.x, mid.y, mid.z, mx, my, mz);
            const std::ptrdiff_t snap = nearestSolid(mx, my, mz, 4);
            bool accepted = false;
            if (snap >= 0) {
                const std::size_t si = std::size_t(snap);
                const int sx = int(si % g.nx);
                const int sy = int((si / g.nx) % g.ny);
                const int sz = int(si / (std::size_t(g.nx) * g.ny));
                const V3 c = g.center(sx, sy, sz);
                const V3 d = sub(c, mid);
                if (std::sqrt(dot(d, d)) <= 4.5 * g.voxel) {
                    if (insertPair(si, int(b), 0.0f))
                        queue.emplace(0.0f, si, int(b));
                    accepted = true;
                }
            }
            if (!accepted) res.bonesWithoutSeeds.push_back(int(b));
        }
    }

    if (queue.empty()) {
        res.error = QStringLiteral("no bone produced a seed voxel");
        return res;
    }

    // ── 4) Multi-source Dijkstra over solid voxels (26-conn) ───────
    // Precompute the 26 neighbour offsets with their world-space
    // step costs.
    struct Step { int dx, dy, dz; float cost; };
    std::vector<Step> steps;
    steps.reserve(26);
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy && !dz) continue;
                steps.push_back({ dx, dy, dz,
                    float(g.voxel * std::sqrt(double(dx*dx + dy*dy + dz*dz))) });
            }

    while (!queue.empty()) {
        const auto [d, vox, bone] = queue.top();
        queue.pop();
        // Stale-entry check: is this (bone, d) still the voxel's
        // current best for that bone?
        {
            const std::size_t base = vox * K;
            bool current = false;
            for (int k = 0; k < K; ++k) {
                if (pairBone[base + k] == bone
                    && pairDist[base + k] >= d - 1e-9f
                    && pairDist[base + k] <= d + 1e-9f) {
                    current = true;
                    break;
                }
            }
            if (!current) continue;
        }
        const int x = int(vox % g.nx);
        const int y = int((vox / g.nx) % g.ny);
        const int z = int(vox / (std::size_t(g.nx) * g.ny));
        for (const Step& s : steps) {
            const int cx = x + s.dx, cy = y + s.dy, cz = z + s.dz;
            if (!g.inside(cx, cy, cz)) continue;
            const std::size_t ni = g.idx(cx, cy, cz);
            if (!solid[ni]) continue;
            const float nd = d + s.cost;
            if (nd > maxGeoDist) continue;   // distance cap prunes the walk
            if (insertPair(ni, bone, nd)) queue.emplace(nd, ni, bone);
        }
    }

    // ── 5) Per-vertex weights from the containing voxel ────────────
    outWeights.assign(vertexCount, {});
    if (outAllowedBones) outAllowedBones->assign(vertexCount, {});
    const int maxK = std::clamp(opts.maxInfluencesPerVertex, 1, 8);
    // Weight epsilon: quarter of a voxel keeps seed-voxel weights
    // finite and comparable (a vertex sitting on a bone gets d≈0).
    const double eps = g.voxel * 0.25;
    const double falloff = std::max(0.5, opts.falloff);
    const int searchRadius = std::max(4, std::max(g.nx, std::max(g.ny, g.nz)));

    for (int v = 0; v < vertexCount; ++v) {
        const float* p = vertexPositions + 3 * v;
        int x, y, z;
        g.toCell(p[0], p[1], p[2], x, y, z);
        std::ptrdiff_t vox = -1;
        if (g.inside(x, y, z) && solid[g.idx(x, y, z)])
            vox = std::ptrdiff_t(g.idx(x, y, z));
        else
            vox = nearestSolid(x, y, z, searchRadius);

        SkinWeights::VertexWeights& vw = outWeights[v];
        if (vox < 0) { ++res.verticesWithoutGeodesicWeights; continue; }

        const std::size_t base = std::size_t(vox) * K;
        for (int k = 0; k < K; ++k) {
            const std::int32_t b = pairBone[base + k];
            if (b < 0) continue;
            const double dist = double(pairDist[base + k]);
            if (dist > maxGeoDist) continue;
            if (outAllowedBones) (*outAllowedBones)[v].push_back(int(b));
            const double w = 1.0 / std::pow(dist + eps, falloff);
            pushTopK(vw, maxK, int(b), w);
        }
        double sum = 0.0;
        for (int i = 0; i < vw.count; ++i) sum += vw.weights[i];
        if (sum > 0.0) {
            for (int i = 0; i < vw.count; ++i) vw.weights[i] /= sum;
        } else {
            vw.count = 0;
            ++res.verticesWithoutGeodesicWeights;
        }
    }

    res.ok = true;
    return res;
}
