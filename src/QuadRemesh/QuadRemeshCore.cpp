#include "QuadRemeshCore.h"

#include "QuadriFlowRunner.h"
#include "ImageTo3D/MarchingCubes.h"
#include "ImageTo3D/MeshGenCleanup.h"
#include "ImageTo3D/Trellis2Bake.h"

#include <xatlas.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <thread>
#include <unordered_map>

namespace QuadRemesh {

namespace {

inline uint64_t edgeKey(uint32_t a, uint32_t b)
{
    return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a;
}

struct V3 {
    float x = 0, y = 0, z = 0;
};
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float len(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 normalized(V3 a, V3 fallback = {0, 0, 1})
{
    const float l = len(a);
    return l > 1e-20f ? a * (1.0f / l) : fallback;
}
inline V3 at(const std::vector<float>& p, size_t i) { return {p[3 * i], p[3 * i + 1], p[3 * i + 2]}; }

void bounds(const std::vector<float>& p, float mn[3], float mx[3])
{
    for (int k = 0; k < 3; ++k) {
        mn[k] = std::numeric_limits<float>::max();
        mx[k] = -std::numeric_limits<float>::max();
    }
    for (size_t i = 0; i + 2 < p.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], p[i + k]);
            mx[k] = std::max(mx[k], p[i + k]);
        }
}

// Smooth area-weighted vertex normals of a polygon mesh (fan triangulation).
std::vector<float> smoothNormals(const std::vector<float>& pos,
                                 const std::vector<std::vector<uint32_t>>& faces)
{
    const size_t nv = pos.size() / 3;
    std::vector<V3> acc(nv);
    for (const auto& f : faces)
        for (size_t i = 2; i < f.size(); ++i) {
            const V3 a = at(pos, f[0]), b = at(pos, f[i - 1]), c = at(pos, f[i]);
            const V3 n = cross(b - a, c - a);   // length = 2 × area
            acc[f[0]] = acc[f[0]] + n;
            acc[f[i - 1]] = acc[f[i - 1]] + n;
            acc[f[i]] = acc[f[i]] + n;
        }
    std::vector<float> out(nv * 3);
    for (size_t i = 0; i < nv; ++i) {
        const V3 n = normalized(acc[i], {0, 1, 0});
        out[3 * i] = n.x;
        out[3 * i + 1] = n.y;
        out[3 * i + 2] = n.z;
    }
    return out;
}

double signedVolume(const std::vector<float>& p, const std::vector<uint32_t>& idx)
{
    double v = 0;
    for (size_t t = 0; t + 2 < idx.size(); t += 3)
        v += dot(at(p, idx[t]), cross(at(p, idx[t + 1]), at(p, idx[t + 2])));
    return v / 6.0;
}

// Bilinear, wrapping sample of `img` at UV (Ogre convention: v down).
void sampleBilinear(const QImage& img, float u, float v, float out[4])
{
    const int w = img.width(), h = img.height();
    u -= std::floor(u);
    v -= std::floor(v);
    const float fx = u * w - 0.5f, fy = v * h - 0.5f;
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float tx = fx - x0, ty = fy - y0;
    auto px = [&](int x, int y, float c[4]) {
        x = ((x % w) + w) % w;
        y = ((y % h) + h) % h;
        const QRgb q = reinterpret_cast<const QRgb*>(img.constScanLine(y))[x];
        c[0] = qRed(q) / 255.0f;
        c[1] = qGreen(q) / 255.0f;
        c[2] = qBlue(q) / 255.0f;
        c[3] = qAlpha(q) / 255.0f;
    };
    float a[4], b[4], c[4], d[4];
    px(x0, y0, a);
    px(x0 + 1, y0, b);
    px(x0, y0 + 1, c);
    px(x0 + 1, y0 + 1, d);
    for (int k = 0; k < 4; ++k)
        out[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
}

// Per-triangle tangent (dP/du) and bitangent (dP/dv) from positions + UVs.
void triangleTangents(V3 p0, V3 p1, V3 p2, float u0, float v0, float u1, float v1, float u2,
                      float v2, V3& t, V3& b)
{
    const V3 e1 = p1 - p0, e2 = p2 - p0;
    const float du1 = u1 - u0, dv1 = v1 - v0, du2 = u2 - u0, dv2 = v2 - v0;
    const float det = du1 * dv2 - du2 * dv1;
    if (std::fabs(det) < 1e-20f) {
        t = normalized(e1, {1, 0, 0});
        b = normalized(e2, {0, 1, 0});
        return;
    }
    const float r = 1.0f / det;
    t = (e1 * dv2 - e2 * dv1) * r;
    b = (e2 * du1 - e1 * du2) * r;
}

// Orthonormal tangent frame around `n` from a raw (t, b) pair.
void frameAround(V3 n, V3 t, V3 b, V3& T, V3& B)
{
    T = normalized(t - n * dot(n, t), normalized(cross(n, {0.577f, 0.577f, 0.577f}), {1, 0, 0}));
    const V3 bb = cross(n, T);
    B = dot(bb, b) < 0 ? bb * -1.0f : bb;
}

QImage::Format bakeFormat() { return QImage::Format_ARGB32; }

const QImage* mapOf(const SourceMaterial* m, const QString& slot)
{
    if (!m) return nullptr;
    auto it = m->maps.constFind(slot);
    return (it == m->maps.constEnd() || it->isNull()) ? nullptr : &it.value();
}

} // namespace

int defaultTargetFaces(int inputTriangles)
{
    return std::clamp(int(std::lround(inputTriangles * 0.6)), 1000, 20000);
}

// ---- Topology -----------------------------------------------------------------

Topology analyzeTopology(const std::vector<uint32_t>& indices, int vertexCount)
{
    Topology t;
    struct E { int count = 0; int forward = 0; };
    std::unordered_map<uint64_t, E> edges;
    edges.reserve(indices.size());
    for (size_t f = 0; f + 2 < indices.size(); f += 3)
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = indices[f + k], b = indices[f + (k + 1) % 3];
            E& e = edges[edgeKey(a, b)];
            ++e.count;
            if (a < b) ++e.forward;
        }
    for (const auto& kv : edges) {
        if (kv.second.count == 1) ++t.boundaryEdges;
        else if (kv.second.count > 2) ++t.nonManifoldEdges;
        else if (kv.second.forward != 1) ++t.inconsistentEdges;
    }

    // Vertex fans: the faces around a vertex must form ONE edge-connected fan.
    std::vector<std::vector<uint32_t>> vf(std::max(0, vertexCount));
    for (size_t f = 0; f + 2 < indices.size(); f += 3)
        for (int k = 0; k < 3; ++k)
            if (indices[f + k] < vf.size())
                vf[indices[f + k]].push_back(uint32_t(f / 3));
    std::vector<int> parent;
    for (int v = 0; v < vertexCount; ++v) {
        const auto& fs = vf[v];
        if (fs.size() < 2)
            continue;
        parent.resize(fs.size());
        std::iota(parent.begin(), parent.end(), 0);
        std::function<int(int)> find = [&](int x) {
            while (parent[x] != x) x = parent[x] = parent[parent[x]];
            return x;
        };
        std::unordered_map<uint32_t, int> firstFaceByNeighbour;
        for (int i = 0; i < int(fs.size()); ++i) {
            const uint32_t f = fs[i];
            for (int k = 0; k < 3; ++k) {
                const uint32_t u = indices[3 * f + k];
                if (u == uint32_t(v)) continue;
                auto it = firstFaceByNeighbour.find(u);
                if (it == firstFaceByNeighbour.end())
                    firstFaceByNeighbour.emplace(u, i);
                else
                    parent[find(i)] = find(it->second);
            }
        }
        int roots = 0;
        for (int i = 0; i < int(fs.size()); ++i)
            if (find(i) == i) ++roots;
        if (roots > 1) ++t.nonManifoldVertices;
    }
    return t;
}

int weldByPosition(std::vector<float>& positions, std::vector<uint32_t>& indices, float relEps)
{
    const size_t nv = positions.size() / 3;
    if (nv == 0)
        return 0;
    float mn[3], mx[3];
    bounds(positions, mn, mx);
    const float diag = std::sqrt((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1])
                                 + (mx[2] - mn[2]) * (mx[2] - mn[2]));
    const double q = std::max(1e-12, double(diag) * relEps);
    struct K {
        int64_t x, y, z;
        bool operator==(const K& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct KH {
        size_t operator()(const K& k) const
        {
            return size_t(k.x * 73856093LL ^ k.y * 19349663LL ^ k.z * 83492791LL);
        }
    };
    std::unordered_map<K, uint32_t, KH> map;
    map.reserve(nv);
    std::vector<uint32_t> remap(nv);
    std::vector<float> out;
    out.reserve(positions.size());
    for (size_t i = 0; i < nv; ++i) {
        const K k{int64_t(std::llround(positions[3 * i] / q)), int64_t(std::llround(positions[3 * i + 1] / q)),
                  int64_t(std::llround(positions[3 * i + 2] / q))};
        auto it = map.find(k);
        if (it != map.end()) {
            remap[i] = it->second;
            continue;
        }
        const uint32_t id = uint32_t(out.size() / 3);
        map.emplace(k, id);
        remap[i] = id;
        out.insert(out.end(), {positions[3 * i], positions[3 * i + 1], positions[3 * i + 2]});
    }
    for (uint32_t& i : indices)
        i = remap[i];
    const int merged = int(nv - out.size() / 3);
    positions.swap(out);
    // Collapsed / duplicate triangles + orphan compaction.
    MeshGenCleanup::removeDegenerates(positions, indices);
    return merged;
}

// ---- Voxel pre-pass -------------------------------------------------------------

bool voxelRemesh(const std::vector<float>& positions, const std::vector<uint32_t>& indices,
                 int resolution, std::vector<float>& outPositions,
                 std::vector<uint32_t>& outIndices, QString* error,
                 const std::function<bool()>& keepGoing, bool* cancelled)
{
    if (cancelled) *cancelled = false;
    // Polled every few thousand units of work so Cancel/Stop land promptly
    // (the GUI's cancel flag, the MCP event pump) on a large voxel grid.
    auto stop = [&]() {
        if (!keepGoing || keepGoing()) return false;
        if (cancelled) *cancelled = true;
        if (error) *error = QStringLiteral("cancelled");
        outPositions.clear();
        outIndices.clear();
        return true;
    };
    outPositions.clear();
    outIndices.clear();
    if (positions.size() < 9 || indices.size() < 3) {
        if (error) *error = QStringLiteral("voxel remesh: empty input");
        return false;
    }
    resolution = std::clamp(resolution, 16, 512);
    float mn[3], mx[3];
    bounds(positions, mn, mx);
    const float longest = std::max({mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]});
    if (!(longest > 0)) {
        if (error) *error = QStringLiteral("voxel remesh: degenerate bounds");
        return false;
    }
    const float h = longest / resolution;
    const int pad = 3;
    int n[3];
    float origin[3];
    for (int k = 0; k < 3; ++k) {
        n[k] = int(std::ceil((mx[k] - mn[k]) / h)) + 2 + 2 * pad;
        // An irrational sub-cell offset keeps samples off axis-aligned input
        // faces: a sample sitting exactly ON the surface (distance 0) makes
        // marching cubes emit coincident vertices from several edges.
        origin[k] = mn[k] - (pad + 0.3819660f) * h;
    }
    const size_t total = size_t(n[0]) * n[1] * n[2];
    auto id = [&](int x, int y, int z) { return (size_t(z) * n[1] + y) * n[0] + x; };

    // Narrow-band unsigned distance to the surface.
    const float band = 1.5f * h;
    std::vector<float> dist(total, std::numeric_limits<float>::max());
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        if ((t / 3) % 2048 == 0 && stop()) return false;
        const float* a = &positions[3 * size_t(indices[t])];
        const float* b = &positions[3 * size_t(indices[t + 1])];
        const float* c = &positions[3 * size_t(indices[t + 2])];
        int lo[3], hi[3];
        for (int k = 0; k < 3; ++k) {
            const float tmn = std::min({a[k], b[k], c[k]}) - band;
            const float tmx = std::max({a[k], b[k], c[k]}) + band;
            lo[k] = std::max(0, int(std::floor((tmn - origin[k]) / h)));
            hi[k] = std::min(n[k] - 1, int(std::ceil((tmx - origin[k]) / h)));
        }
        for (int z = lo[2]; z <= hi[2]; ++z)
            for (int y = lo[1]; y <= hi[1]; ++y)
                for (int x = lo[0]; x <= hi[0]; ++x) {
                    const float p[3] = {origin[0] + x * h, origin[1] + y * h, origin[2] + z * h};
                    float cp[3], bary[3];
                    Trellis2Bake::closestPointOnTriangle(a, b, c, p, cp, bary);
                    const float d = std::sqrt((p[0] - cp[0]) * (p[0] - cp[0]) + (p[1] - cp[1]) * (p[1] - cp[1])
                                              + (p[2] - cp[2]) * (p[2] - cp[2]));
                    float& slot = dist[id(x, y, z)];
                    if (d < slot) slot = d;
                }
    }

    // A sample is a wall when the surface passes within 0.75h: any surface
    // crossing an axis-aligned sample segment (length h) blocks at least one
    // of its ends, so the 6-connected exterior flood cannot leak through.
    const float wall = 0.75f * h;
    std::vector<uint8_t> exterior(total, 0);
    std::deque<size_t> q;
    auto seed = [&](int x, int y, int z) {
        const size_t i = id(x, y, z);
        if (!exterior[i] && dist[i] > wall) {
            exterior[i] = 1;
            q.push_back(i);
        }
    };
    for (int z = 0; z < n[2]; ++z)
        for (int y = 0; y < n[1]; ++y)
            for (int x = 0; x < n[0]; ++x)
                if (x == 0 || y == 0 || z == 0 || x == n[0] - 1 || y == n[1] - 1 || z == n[2] - 1)
                    seed(x, y, z);
    size_t popped = 0;
    while (!q.empty()) {
        if (++popped % 262144 == 0 && stop()) return false;
        const size_t i = q.front();
        q.pop_front();
        const int x = int(i % n[0]), y = int((i / n[0]) % n[1]), z = int(i / (size_t(n[0]) * n[1]));
        if (x > 0) seed(x - 1, y, z);
        if (x + 1 < n[0]) seed(x + 1, y, z);
        if (y > 0) seed(x, y - 1, z);
        if (y + 1 < n[1]) seed(x, y + 1, z);
        if (z > 0) seed(x, y, z - 1);
        if (z + 1 < n[2]) seed(x, y, z + 1);
    }

    // Inside-positive field. Samples off the band carry a capped distance.
    std::vector<float> field(total);
    for (size_t i = 0; i < total; ++i) {
        const float d = std::clamp(dist[i], 1e-3f * h, 2.0f * h);
        field[i] = exterior[i] ? -d : d;
    }
    const std::array<float, 3> gmin{origin[0], origin[1], origin[2]};
    const std::array<float, 3> gmax{origin[0] + (n[0] - 1) * h, origin[1] + (n[1] - 1) * h,
                                    origin[2] + (n[2] - 1) * h};
    if (stop()) return false;
    MarchingCubes::Mesh m = MarchingCubes::extract(field.data(), n[0], n[1], n[2], 0.0f, gmin, gmax,
                                                   field.size());
    if (stop()) return false;
    if (m.indices.empty()) {
        if (error) *error = QStringLiteral("voxel remesh produced no surface");
        return false;
    }
    outPositions = std::move(m.positions);
    outIndices = std::move(m.indices);
    // NOT welded again: extract() already shares vertices per cube edge, and
    // a positional weld would pinch nearly-coincident vertices from different
    // edges into non-manifold edges.
    if (signedVolume(outPositions, outIndices) < 0)
        for (size_t t = 0; t + 2 < outIndices.size(); t += 3)
            std::swap(outIndices[t + 1], outIndices[t + 2]);

    // Drop specks (< 0.5% of the triangles) — keep the largest piece always.
    {
        const size_t nt = outIndices.size() / 3;
        std::vector<int> parent(outPositions.size() / 3);
        std::iota(parent.begin(), parent.end(), 0);
        auto find = [&](int x) {
            while (parent[x] != x) x = parent[x] = parent[parent[x]];
            return x;
        };
        for (size_t t = 0; t < nt; ++t) {
            const int a = find(int(outIndices[3 * t])), b = find(int(outIndices[3 * t + 1])),
                      c = find(int(outIndices[3 * t + 2]));
            parent[b] = a;
            parent[find(c)] = find(a);
        }
        std::unordered_map<int, size_t> count;
        for (size_t t = 0; t < nt; ++t)
            ++count[find(int(outIndices[3 * t]))];
        size_t largest = 0;
        for (const auto& kv : count) largest = std::max(largest, kv.second);
        const size_t minKeep = std::max<size_t>(1, nt / 200);
        std::vector<uint32_t> kept;
        kept.reserve(outIndices.size());
        for (size_t t = 0; t < nt; ++t) {
            const size_t c = count[find(int(outIndices[3 * t]))];
            if (c == largest || c >= minKeep)
                kept.insert(kept.end(), {outIndices[3 * t], outIndices[3 * t + 1], outIndices[3 * t + 2]});
        }
        outIndices.swap(kept);
        // Compact orphaned vertices (zero-area slivers stay: dropping one
        // would open a crack in an otherwise closed surface).
        std::vector<uint32_t> remap(outPositions.size() / 3, UINT32_MAX);
        std::vector<float> compact;
        for (uint32_t& i : outIndices) {
            if (remap[i] == UINT32_MAX) {
                remap[i] = uint32_t(compact.size() / 3);
                compact.insert(compact.end(), {outPositions[3 * i], outPositions[3 * i + 1], outPositions[3 * i + 2]});
            }
            i = remap[i];
        }
        outPositions.swap(compact);
    }
    return !outIndices.empty();
}

// ---- SurfaceLocator --------------------------------------------------------------

void SurfaceLocator::build(const std::vector<float>& positions, const std::vector<uint32_t>& indices)
{
    m_pos = &positions;
    m_idx = &indices;
    m_tris.clear();
    m_cellStart.clear();
    const size_t nt = indices.size() / 3;
    if (nt == 0 || positions.size() < 9)
        return;
    float mn[3], mx[3];
    bounds(positions, mn, mx);
    double area = 0;
    for (size_t t = 0; t < nt; ++t)
        area += 0.5 * len(cross(at(positions, indices[3 * t + 1]) - at(positions, indices[3 * t]),
                                at(positions, indices[3 * t + 2]) - at(positions, indices[3 * t])));
    float cell = float(std::sqrt(std::max(1e-20, area / double(nt))) * 2.0);
    const float ext = std::max({mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2], 1e-12f});
    cell = std::max(cell, ext / 256.0f);   // bounds the cell count
    for (int k = 0; k < 3; ++k) {
        m_min[k] = mn[k];
        m_n[k] = std::max(1, int(std::ceil((mx[k] - mn[k]) / cell)) + 1);
    }
    m_cell = cell;
    const size_t cells = size_t(m_n[0]) * m_n[1] * m_n[2];
    std::vector<int> counts(cells + 1, 0);
    auto range = [&](size_t t, int lo[3], int hi[3]) {
        for (int k = 0; k < 3; ++k) {
            float a = positions[3 * indices[3 * t] + k], b = positions[3 * indices[3 * t + 1] + k],
                  c = positions[3 * indices[3 * t + 2] + k];
            lo[k] = std::clamp(int((std::min({a, b, c}) - m_min[k]) / cell), 0, m_n[k] - 1);
            hi[k] = std::clamp(int((std::max({a, b, c}) - m_min[k]) / cell), 0, m_n[k] - 1);
        }
    };
    auto cid = [&](int x, int y, int z) { return (size_t(z) * m_n[1] + y) * m_n[0] + x; };
    int lo[3], hi[3];
    for (size_t t = 0; t < nt; ++t) {
        range(t, lo, hi);
        for (int z = lo[2]; z <= hi[2]; ++z)
            for (int y = lo[1]; y <= hi[1]; ++y)
                for (int x = lo[0]; x <= hi[0]; ++x)
                    ++counts[cid(x, y, z) + 1];
    }
    for (size_t i = 1; i <= cells; ++i)
        counts[i] += counts[i - 1];
    m_cellStart = counts;
    m_tris.assign(size_t(counts[cells]), 0);
    std::vector<int> fill(counts.begin(), counts.end() - 1);
    for (size_t t = 0; t < nt; ++t) {
        range(t, lo, hi);
        for (int z = lo[2]; z <= hi[2]; ++z)
            for (int y = lo[1]; y <= hi[1]; ++y)
                for (int x = lo[0]; x <= hi[0]; ++x)
                    m_tris[size_t(fill[cid(x, y, z)]++)] = int(t);
    }
}

bool SurfaceLocator::closest(const float p[3], int& triangle, float bary[3], float closestPt[3]) const
{
    if (m_tris.empty())
        return false;
    int c[3];
    for (int k = 0; k < 3; ++k)
        c[k] = std::clamp(int((p[k] - m_min[k]) / m_cell), 0, m_n[k] - 1);
    float best = std::numeric_limits<float>::max();
    triangle = -1;
    const int maxR = std::max({m_n[0], m_n[1], m_n[2]});
    // A triangle spans several cells; a small local list of tested ids is
    // cheaper than a shared stamp array and keeps queries thread-safe.
    std::vector<int> tested;
    tested.reserve(64);
    for (int r = 0; r <= maxR; ++r) {
        if (triangle >= 0 && float(std::max(0, r - 1)) * m_cell > std::sqrt(best))
            break;
        for (int z = c[2] - r; z <= c[2] + r; ++z) {
            if (z < 0 || z >= m_n[2]) continue;
            for (int y = c[1] - r; y <= c[1] + r; ++y) {
                if (y < 0 || y >= m_n[1]) continue;
                for (int x = c[0] - r; x <= c[0] + r; ++x) {
                    if (x < 0 || x >= m_n[0]) continue;
                    if (std::max({std::abs(x - c[0]), std::abs(y - c[1]), std::abs(z - c[2])}) != r)
                        continue;   // shell only
                    const size_t cell = (size_t(z) * m_n[1] + y) * m_n[0] + x;
                    for (int i = m_cellStart[cell]; i < m_cellStart[cell + 1]; ++i) {
                        const int t = m_tris[size_t(i)];
                        if (std::find(tested.begin(), tested.end(), t) != tested.end())
                            continue;
                        if (tested.size() < 4096) tested.push_back(t);
                        const auto& P = *m_pos;
                        const auto& I = *m_idx;
                        float cp[3], b[3];
                        Trellis2Bake::closestPointOnTriangle(&P[3 * size_t(I[3 * t])], &P[3 * size_t(I[3 * t + 1])],
                                                             &P[3 * size_t(I[3 * t + 2])], p, cp, b);
                        const float d = (cp[0] - p[0]) * (cp[0] - p[0]) + (cp[1] - p[1]) * (cp[1] - p[1])
                                        + (cp[2] - p[2]) * (cp[2] - p[2]);
                        if (d < best) {
                            best = d;
                            triangle = t;
                            std::memcpy(bary, b, sizeof(float) * 3);
                            std::memcpy(closestPt, cp, sizeof(float) * 3);
                        }
                    }
                }
            }
        }
    }
    return triangle >= 0;
}

std::vector<BoneWeight> blendBones(const std::vector<BoneWeight>* corners[3], const float bary[3])
{
    std::map<uint16_t, float> acc;
    for (int k = 0; k < 3; ++k) {
        if (!corners[k] || bary[k] <= 0) continue;
        for (const BoneWeight& bw : *corners[k])
            acc[bw.bone] += bw.weight * bary[k];
    }
    std::vector<BoneWeight> out;
    for (const auto& kv : acc)
        if (kv.second > 1e-6f) out.push_back({kv.first, kv.second});
    std::sort(out.begin(), out.end(), [](const BoneWeight& a, const BoneWeight& b) {
        return a.weight != b.weight ? a.weight > b.weight : a.bone < b.bone;
    });
    if (out.size() > 4) out.resize(4);
    float sum = 0;
    for (const BoneWeight& bw : out) sum += bw.weight;
    if (sum > 0)
        for (BoneWeight& bw : out) bw.weight /= sum;
    return out;
}

void fitToSurface(std::vector<float>& positions, const std::vector<std::vector<uint32_t>>& faces,
                  const SurfaceLocator& source, int iterations, float maxSnap)
{
    const size_t nv = positions.size() / 3;
    if (nv == 0 || !source.valid()) return;
    // Vertex 1-rings from the polygon edges.
    std::vector<std::vector<uint32_t>> ring(nv);
    for (const auto& f : faces)
        for (size_t i = 0; i < f.size(); ++i) {
            const uint32_t a = f[i], b = f[(i + 1) % f.size()];
            if (a >= nv || b >= nv) continue;
            if (std::find(ring[a].begin(), ring[a].end(), b) == ring[a].end()) ring[a].push_back(b);
            if (std::find(ring[b].begin(), ring[b].end(), a) == ring[b].end()) ring[b].push_back(a);
        }
    const float maxSnap2 = maxSnap * maxSnap;
    auto snap = [&]() {
        for (size_t v = 0; v < nv; ++v) {
            int t;
            float b[3], cp[3];
            if (!source.closest(&positions[3 * v], t, b, cp)) continue;
            const float dx = cp[0] - positions[3 * v], dy = cp[1] - positions[3 * v + 1],
                        dz = cp[2] - positions[3 * v + 2];
            if (dx * dx + dy * dy + dz * dz > maxSnap2) continue;
            positions[3 * v] = cp[0];
            positions[3 * v + 1] = cp[1];
            positions[3 * v + 2] = cp[2];
        }
    };
    for (int it = 0; it < iterations; ++it) {
        // Tangential relaxation: move halfway toward the 1-ring centroid,
        // with the normal component removed so the shape is not shrunk.
        const std::vector<float> n = smoothNormals(positions, faces);
        std::vector<float> next = positions;
        for (size_t v = 0; v < nv; ++v) {
            if (ring[v].size() < 3) continue;   // boundary/degenerate: keep
            V3 c{0, 0, 0};
            for (uint32_t u : ring[v]) c = c + at(positions, u);
            c = c * (1.0f / float(ring[v].size()));
            V3 d = c - at(positions, v);
            const V3 nn = at(n, v);
            d = d - nn * dot(d, nn);
            next[3 * v] += 0.5f * d.x;
            next[3 * v + 1] += 0.5f * d.y;
            next[3 * v + 2] += 0.5f * d.z;
        }
        positions.swap(next);
        snap();
    }
    if (iterations <= 0) snap();
}

void surfaceDeviation(const std::vector<float>& samples, const std::vector<float>& positions,
                      const std::vector<uint32_t>& indices, float& mean, float& max)
{
    mean = max = 0.0f;
    SurfaceLocator loc;
    loc.build(positions, indices);
    if (!loc.valid() || samples.empty()) return;
    const size_t n = samples.size() / 3;
    const size_t step = std::max<size_t>(1, n / 20000);
    double sum = 0;
    size_t count = 0;
    for (size_t i = 0; i < n; i += step) {
        int t;
        float b[3], cp[3];
        if (!loc.closest(&samples[3 * i], t, b, cp)) continue;
        const float d = len(V3{cp[0] - samples[3 * i], cp[1] - samples[3 * i + 1], cp[2] - samples[3 * i + 2]});
        sum += d;
        max = std::max(max, d);
        ++count;
    }
    mean = count ? float(sum / count) : 0.0f;
}

// ---- Attribute transfer + bake --------------------------------------------------

namespace {

struct Unwrapped {
    bool ok = false;
    QString error;
    std::vector<uint32_t> xref;                    // output vertex → input vertex
    std::vector<float> uvs;                        // Nx2 in [0,1]
    std::vector<std::vector<uint32_t>> faces;      // over output vertices
    int atlasW = 0, atlasH = 0;
};

Unwrapped unwrap(const std::vector<float>& pos, const std::vector<std::vector<uint32_t>>& faces, int padding)
{
    // Triangle mode. (The pinned xatlas's polygon input does not keep a
    // polygon inside one chart, and its polygon output indexes by input
    // VERTEX rather than by polygon corner, so it cannot be used.) A face
    // stays whole when every corner maps to a single output vertex across its
    // triangles — i.e. no chart seam runs through it; otherwise its triangles
    // are emitted on their own.
    Unwrapped u;
    std::vector<uint32_t> idx;
    for (size_t fi = 0; fi < faces.size(); ++fi)
        for (size_t i = 2; i < faces[fi].size(); ++i)
            idx.insert(idx.end(), {faces[fi][0], faces[fi][i - 1], faces[fi][i]});

    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::MeshDecl decl;
    decl.vertexCount = uint32_t(pos.size() / 3);
    decl.vertexPositionData = pos.data();
    decl.vertexPositionStride = sizeof(float) * 3;
    decl.indexCount = uint32_t(idx.size());
    decl.indexData = idx.data();
    decl.indexFormat = xatlas::IndexFormat::UInt32;
    const auto err = xatlas::AddMesh(atlas, decl);
    if (err != xatlas::AddMeshError::Success) {
        u.error = QStringLiteral("xatlas::AddMesh failed: %1").arg(QString::fromLatin1(xatlas::StringForEnum(err)));
        xatlas::Destroy(atlas);
        return u;
    }
    xatlas::PackOptions pack;
    pack.padding = uint32_t(std::max(1, padding));
    pack.bilinear = true;
    // resolution = texelsPerUnit = 0: one atlas of roughly 1024², whatever
    // its exact size — UVs are normalised by it, the bake picks its own size.
    xatlas::Generate(atlas, {}, pack);
    if (atlas->meshCount != 1 || atlas->width == 0 || atlas->height == 0
        || atlas->meshes[0].indexCount != idx.size()) {
        u.error = QStringLiteral("xatlas produced no atlas");
        xatlas::Destroy(atlas);
        return u;
    }
    const xatlas::Mesh& m = atlas->meshes[0];
    u.atlasW = int(atlas->width);
    u.atlasH = int(atlas->height);
    u.xref.resize(m.vertexCount);
    u.uvs.resize(size_t(m.vertexCount) * 2);
    for (uint32_t v = 0; v < m.vertexCount; ++v) {
        u.xref[v] = m.vertexArray[v].xref;
        u.uvs[2 * v] = m.vertexArray[v].uv[0] / float(atlas->width);
        u.uvs[2 * v + 1] = m.vertexArray[v].uv[1] / float(atlas->height);
    }
    size_t t = 0;
    for (size_t fi = 0; fi < faces.size(); ++fi) {
        const size_t nt = faces[fi].size() >= 3 ? faces[fi].size() - 2 : 0;
        std::unordered_map<uint32_t, uint32_t> corner;
        bool whole = true;
        for (size_t k = 0; k < nt * 3; ++k) {
            const uint32_t o = m.indexArray[3 * t + k];
            auto [it, ins] = corner.emplace(u.xref[o], o);
            if (!ins && it->second != o) whole = false;
        }
        if (whole) {
            std::vector<uint32_t> f;
            f.reserve(faces[fi].size());
            for (uint32_t v : faces[fi]) f.push_back(corner[v]);
            u.faces.push_back(std::move(f));
        } else {
            for (size_t k = 0; k < nt; ++k)
                u.faces.push_back({m.indexArray[3 * (t + k)], m.indexArray[3 * (t + k) + 1],
                                   m.indexArray[3 * (t + k) + 2]});
        }
        t += nt;
    }
    xatlas::Destroy(atlas);
    u.ok = true;
    return u;
}

void dilate(QImage& img, std::vector<uint8_t>& covered, int passes)
{
    const int w = img.width(), h = img.height();
    for (int pass = 0; pass < passes; ++pass) {
        std::vector<uint8_t> next = covered;
        QImage src = img.copy();
        for (int y = 0; y < h; ++y) {
            QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                if (covered[size_t(y) * w + x]) continue;
                int r = 0, g = 0, b = 0, a = 0, n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        if (!covered[size_t(yy) * w + xx]) continue;
                        const QRgb q = reinterpret_cast<const QRgb*>(src.constScanLine(yy))[xx];
                        r += qRed(q); g += qGreen(q); b += qBlue(q); a += qAlpha(q); ++n;
                    }
                if (n) {
                    row[x] = qRgba(r / n, g / n, b / n, a / n);
                    next[size_t(y) * w + x] = 1;
                }
            }
        }
        covered.swap(next);
    }
}

} // namespace

Result transferOnto(const SourceMesh& src, const std::vector<float>& quadPositionsIn,
                    const std::vector<std::vector<uint32_t>>& quadFaces, const Options& opts)
{
    std::vector<float> quadPositions = quadPositionsIn;
    Result r;
    r.inputTriangles = int(src.indices.size() / 3);
    if (quadPositions.size() < 9 || quadFaces.empty()) {
        r.error = QStringLiteral("empty remesh result");
        return r;
    }
    auto report = [&](const QString& s, double f) { return !opts.progress || opts.progress(s, f); };

    SurfaceLocator loc;
    loc.build(src.positions, src.indices);
    if (!loc.valid()) {
        r.error = QStringLiteral("source mesh is empty");
        return r;
    }
    float srcMin[3], srcMax[3];
    bounds(src.positions, srcMin, srcMax);
    const float srcDiag = len(V3{srcMax[0] - srcMin[0], srcMax[1] - srcMin[1], srcMax[2] - srcMin[2]});
    if (opts.reprojectIterations > 0) {
        if (!report(QStringLiteral("fit to surface"), 0.72)) { r.cancelled = true; r.error = QStringLiteral("cancelled"); return r; }
        // Snap limit: a few average quad edges — far enough to recover
        // rounded-off features, short enough not to jump onto another part.
        double edge = 0;
        size_t ne = 0;
        for (const auto& f : quadFaces)
            for (size_t i = 0; i < f.size(); ++i) {
                edge += len(at(quadPositions, f[i]) - at(quadPositions, f[(i + 1) % f.size()]));
                ++ne;
            }
        const float avgEdge = ne ? float(edge / ne) : srcDiag * 0.01f;
        fitToSurface(quadPositions, quadFaces, loc, opts.reprojectIterations, 3.0f * avgEdge);
    }
    const std::vector<float> qNormals = smoothNormals(quadPositions, quadFaces);
    const size_t qn = quadPositions.size() / 3;

    // Bone weights per remeshed vertex.
    std::vector<std::vector<BoneWeight>> qBones;
    const bool hasBones = !src.bones.empty() && src.bones.size() == src.positions.size() / 3;
    if (hasBones) {
        qBones.resize(qn);
        for (size_t v = 0; v < qn; ++v) {
            int t;
            float b[3], cp[3];
            if (!loc.closest(&quadPositions[3 * v], t, b, cp)) continue;
            const std::vector<BoneWeight>* c[3] = {&src.bones[src.indices[3 * t]],
                                                   &src.bones[src.indices[3 * t + 1]],
                                                   &src.bones[src.indices[3 * t + 2]]};
            qBones[v] = blendBones(c, b);
        }
    }

    // Which slotNames to bake. Slots whose images are the same across every
    // material (diffuse_map aliased as albedo, …) are baked once and shared.
    const bool hasUv = src.uvs.size() == src.positions.size() / 3 * 2 && !src.uvs.empty();
    QStringList slotNames;
    for (const SourceMaterial& m : src.materials)
        for (auto it = m.maps.cbegin(); it != m.maps.cend(); ++it)
            if (!it.value().isNull() && !slotNames.contains(it.key())) slotNames << it.key();
    if (!hasUv) slotNames.clear();
    // Submeshes that differ by diffuse COLOUR: bake the colours in (a
    // synthetic diffuse map when no material has one). Needs no UVs.
    bool colorsDiffer = false;
    for (size_t m = 1; m < src.materials.size(); ++m)
        for (int k = 0; k < 4; ++k)
            if (std::fabs(src.materials[m].diffuse[k] - src.materials[0].diffuse[k]) > 1e-3f)
                colorsDiffer = true;
    const bool bakeColor = colorsDiffer && opts.bakeTextures;
    auto isDiffuseSlot = [](const QString& n) {
        return n == QLatin1String("diffuse_map") || n == QLatin1String("albedo");
    };
    if (bakeColor && std::none_of(slotNames.begin(), slotNames.end(), isDiffuseSlot))
        slotNames << QStringLiteral("diffuse_map");
    const bool haveSourceNormal = slotNames.contains(QStringLiteral("normal_map"));
    const bool bakeNormal = opts.bakeTextures && (haveSourceNormal || opts.bakeDetailNormal);
    const bool doUnwrap = opts.bakeTextures && (!slotNames.isEmpty() || bakeNormal);

    if (!doUnwrap) {
        r.positions = quadPositions;
        r.normals = qNormals;
        r.faces = quadFaces;
        r.bones = std::move(qBones);
    } else {
        if (!report(QStringLiteral("unwrap"), 0.75)) { r.cancelled = true; r.error = QStringLiteral("cancelled"); return r; }
        Unwrapped u = unwrap(quadPositions, quadFaces, opts.dilatePx);
        if (!u.ok) {
            r.error = QStringLiteral("UV unwrap failed: %1").arg(u.error);
            return r;
        }
        const size_t nv = u.xref.size();
        r.positions.resize(nv * 3);
        r.normals.resize(nv * 3);
        for (size_t v = 0; v < nv; ++v)
            for (int k = 0; k < 3; ++k) {
                r.positions[3 * v + k] = quadPositions[3 * size_t(u.xref[v]) + k];
                r.normals[3 * v + k] = qNormals[3 * size_t(u.xref[v]) + k];
            }
        if (hasBones) {
            r.bones.resize(nv);
            for (size_t v = 0; v < nv; ++v) r.bones[v] = qBones[u.xref[v]];
        }
        r.uvs = std::move(u.uvs);
        r.faces = std::move(u.faces);

        // Bake size: the largest source map, at least 1024, longest side.
        int size = opts.textureSize;
        if (size <= 0) {
            size = 1024;
            for (const SourceMaterial& m : src.materials)
                for (const QImage& img : m.maps)
                    size = std::max({size, img.width(), img.height()});
        }
        size = std::clamp(size, 64, 8192);
        const int W = u.atlasW >= u.atlasH ? size : std::max(1, int(std::lround(double(size) * u.atlasW / u.atlasH)));
        const int H = u.atlasH >= u.atlasW ? size : std::max(1, int(std::lround(double(size) * u.atlasH / u.atlasW)));
        r.textureWidth = W;
        r.textureHeight = H;

        // Group slotNames that are byte-identical per material.
        struct Group { QString key; QStringList slotNames; bool normal = false; bool diffuse = false; };
        std::vector<Group> groups;
        for (const QString& s : slotNames) {
            if (s == QStringLiteral("normal_map")) continue;
            QString key;
            for (const SourceMaterial& m : src.materials)
                key += QString::number(m.maps.value(s).cacheKey()) + QLatin1Char(',');
            auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.key == key; });
            if (it == groups.end()) groups.push_back({key, {s}, false, bakeColor && isDiffuseSlot(s)});
            else it->slotNames << s;
        }
        if (bakeNormal) groups.push_back({QStringLiteral("normal"), {QStringLiteral("normal_map")}, true, false});

        std::vector<QImage> images(groups.size());
        for (QImage& img : images) {
            img = QImage(W, H, bakeFormat());
            img.fill(Qt::transparent);
        }
        std::vector<uint8_t> covered(size_t(W) * H, 0);
        // Raw pixel pointers taken up front: QImage::scanLine() may detach,
        // which must not happen concurrently on the worker threads.
        std::vector<QRgb*> pixels(images.size());
        for (size_t g = 0; g < images.size(); ++g)
            pixels[g] = reinterpret_cast<QRgb*>(images[g].bits());

        // Source per-triangle tangents (normal-map decoding).
        std::vector<V3> srcT, srcB;
        if (hasUv && bakeNormal) {
            const size_t nt = src.indices.size() / 3;
            srcT.resize(nt);
            srcB.resize(nt);
            for (size_t t = 0; t < nt; ++t) {
                const uint32_t a = src.indices[3 * t], b = src.indices[3 * t + 1], c = src.indices[3 * t + 2];
                triangleTangents(at(src.positions, a), at(src.positions, b), at(src.positions, c),
                                 src.uvs[2 * a], src.uvs[2 * a + 1], src.uvs[2 * b], src.uvs[2 * b + 1],
                                 src.uvs[2 * c], src.uvs[2 * c + 1], srcT[t], srcB[t]);
            }
        }
        const bool srcHasNormals = src.normals.size() == src.positions.size();

        // Rasterise every (fan) triangle; rows of triangles in parallel.
        struct Tri { uint32_t v[3]; };
        std::vector<Tri> tris;
        for (const auto& f : r.faces)
            for (size_t i = 2; i < f.size(); ++i) tris.push_back({{f[0], f[i - 1], f[i]}});

        std::atomic<size_t> nextTri{0};
        std::atomic<bool> cancel{false};
        std::atomic<size_t> doneTris{0};
        // Texel ownership. Triangles sharing an edge both cover texel centres
        // on it, and two workers writing one texel is a data race even when
        // the values agree (normal-map values need not: each triangle has its
        // own tangent frame). A cheap first pass gives every texel to the
        // LOWEST triangle index covering it (atomic min); the expensive second
        // pass writes only owned texels — exclusive and deterministic.
        std::unique_ptr<std::atomic<int32_t>[]> owner(new std::atomic<int32_t>[size_t(W) * H]);
        for (size_t i = 0; i < size_t(W) * H; ++i)
            owner[i].store(std::numeric_limits<int32_t>::max(), std::memory_order_relaxed);
        bool claimPass = true;
        auto worker = [&]() {
            const size_t chunk = 64;
            while (!cancel.load()) {
                const size_t start = nextTri.fetch_add(chunk);
                if (start >= tris.size()) break;
                const size_t end = std::min(tris.size(), start + chunk);
                for (size_t ti = start; ti < end; ++ti) {
                    const Tri& t = tris[ti];
                    float px[3], py[3];
                    for (int k = 0; k < 3; ++k) {
                        px[k] = r.uvs[2 * t.v[k]] * W;
                        py[k] = r.uvs[2 * t.v[k] + 1] * H;
                    }
                    const float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
                    if (std::fabs(area) < 1e-12f) continue;
                    V3 tT, tB;
                    triangleTangents(at(r.positions, t.v[0]), at(r.positions, t.v[1]), at(r.positions, t.v[2]),
                                     r.uvs[2 * t.v[0]], r.uvs[2 * t.v[0] + 1], r.uvs[2 * t.v[1]],
                                     r.uvs[2 * t.v[1] + 1], r.uvs[2 * t.v[2]], r.uvs[2 * t.v[2] + 1], tT, tB);
                    const int x0 = std::max(0, int(std::floor(std::min({px[0], px[1], px[2]}))));
                    const int x1 = std::min(W - 1, int(std::ceil(std::max({px[0], px[1], px[2]}))));
                    const int y0 = std::max(0, int(std::floor(std::min({py[0], py[1], py[2]}))));
                    const int y1 = std::min(H - 1, int(std::ceil(std::max({py[0], py[1], py[2]}))));
                    const float eps = -1e-4f * std::fabs(area);
                    for (int y = y0; y <= y1; ++y)
                        for (int x = x0; x <= x1; ++x) {
                            const float sx = x + 0.5f, sy = y + 0.5f;
                            float w0 = (px[1] - sx) * (py[2] - sy) - (px[2] - sx) * (py[1] - sy);
                            float w1 = (px[2] - sx) * (py[0] - sy) - (px[0] - sx) * (py[2] - sy);
                            float w2 = (px[0] - sx) * (py[1] - sy) - (px[1] - sx) * (py[0] - sy);
                            if (area < 0) { w0 = -w0; w1 = -w1; w2 = -w2; }
                            const float aa = std::fabs(area);
                            if (w0 < eps || w1 < eps || w2 < eps) continue;
                            w0 /= aa; w1 /= aa; w2 /= aa;
                            const size_t pix = size_t(y) * W + x;
                            const int32_t me = int32_t(ti);
                            if (claimPass) {
                                int32_t cur = owner[pix].load(std::memory_order_relaxed);
                                while (me < cur
                                       && !owner[pix].compare_exchange_weak(cur, me, std::memory_order_relaxed)) {
                                }
                                continue;
                            }
                            if (owner[pix].load(std::memory_order_relaxed) != me) continue;
                            const V3 p = at(r.positions, t.v[0]) * w0 + at(r.positions, t.v[1]) * w1
                                         + at(r.positions, t.v[2]) * w2;
                            const V3 nT = normalized(at(r.normals, t.v[0]) * w0 + at(r.normals, t.v[1]) * w1
                                                     + at(r.normals, t.v[2]) * w2);
                            const float pp[3] = {p.x, p.y, p.z};
                            int st;
                            float sb[3], cp[3];
                            if (!loc.closest(pp, st, sb, cp)) continue;
                            const uint32_t a = src.indices[3 * st], b = src.indices[3 * st + 1],
                                           c = src.indices[3 * st + 2];
                            float su = 0, sv = 0;
                            if (hasUv) {
                                su = src.uvs[2 * a] * sb[0] + src.uvs[2 * b] * sb[1] + src.uvs[2 * c] * sb[2];
                                sv = src.uvs[2 * a + 1] * sb[0] + src.uvs[2 * b + 1] * sb[1]
                                     + src.uvs[2 * c + 1] * sb[2];
                            }
                            const int mat = (size_t(st) < src.triangleMaterial.size())
                                                ? src.triangleMaterial[size_t(st)] : 0;
                            const SourceMaterial* sm = (mat >= 0 && size_t(mat) < src.materials.size())
                                                           ? &src.materials[size_t(mat)] : nullptr;
                            for (size_t g = 0; g < groups.size(); ++g) {
                                QRgb out;
                                if (!groups[g].normal) {
                                    const QImage* img = hasUv ? mapOf(sm, groups[g].slotNames.first()) : nullptr;
                                    float c4[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                                    if (img) sampleBilinear(*img, su, sv, c4);
                                    if (groups[g].diffuse && sm)
                                        for (int k = 0; k < 4; ++k) c4[k] *= sm->diffuse[k];
                                    out = qRgba(int(c4[0] * 255 + 0.5f), int(c4[1] * 255 + 0.5f),
                                                int(c4[2] * 255 + 0.5f), int(c4[3] * 255 + 0.5f));
                                } else {
                                    // Source shading normal (+ its normal map) → world → target frame.
                                    V3 nS = srcHasNormals
                                                ? normalized(at(src.normals, a) * sb[0] + at(src.normals, b) * sb[1]
                                                             + at(src.normals, c) * sb[2])
                                                : normalized(cross(at(src.positions, b) - at(src.positions, a),
                                                                   at(src.positions, c) - at(src.positions, a)));
                                    V3 world = nS;
                                    const QImage* nimg = mapOf(sm, QStringLiteral("normal_map"));
                                    if (nimg && !srcT.empty()) {
                                        float c4[4];
                                        sampleBilinear(*nimg, su, sv, c4);
                                        V3 T, B;
                                        frameAround(nS, srcT[size_t(st)], srcB[size_t(st)], T, B);
                                        world = normalized(T * (c4[0] * 2 - 1) + B * (c4[1] * 2 - 1)
                                                           + nS * (c4[2] * 2 - 1), nS);
                                    }
                                    V3 T, B;
                                    frameAround(nT, tT, tB, T, B);
                                    const V3 tn = normalized({dot(world, T), dot(world, B), dot(world, nT)});
                                    out = qRgba(int((tn.x * 0.5f + 0.5f) * 255 + 0.5f),
                                                int((tn.y * 0.5f + 0.5f) * 255 + 0.5f),
                                                int((tn.z * 0.5f + 0.5f) * 255 + 0.5f), 255);
                                }
                                pixels[g][pix] = out;   // exclusive: this triangle owns pix
                            }
                            covered[pix] = 1;
                        }
                }
                doneTris.fetch_add(end - start);
            }
        };
        const unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
        auto runPass = [&](double from, double span) {
            nextTri = 0;
            doneTris = 0;
            std::vector<std::thread> pool;
            for (unsigned i = 0; i < threads; ++i) pool.emplace_back(worker);
            // The calling thread polls progress/cancel until every chunk is
            // DONE — not merely assigned: on a large atlas the workers claim
            // all triangles at once and the per-texel work follows, and
            // stopping the poll there would make Cancel unreachable until the
            // whole bake finished. (Each chunk adds to doneTris when finished;
            // a cancel stops the loop via the flag.)
            while (doneTris.load() < tris.size() && !cancel.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (!report(QStringLiteral("bake"),
                            from + span * double(doneTris.load()) / std::max<size_t>(1, tris.size())))
                    cancel = true;
            }
            for (auto& t : pool) t.join();   // joins publish the pass's writes
        };
        runPass(0.76, 0.02);
        claimPass = false;
        if (!cancel.load()) runPass(0.78, 0.2);
        if (cancel.load()) {
            r = Result();
            r.cancelled = true;
            r.error = QStringLiteral("cancelled");
            return r;
        }
        for (size_t g = 0; g < groups.size(); ++g) {
            std::vector<uint8_t> cov = covered;
            dilate(images[g], cov, std::max(1, opts.dilatePx));
            // Uncovered gutters: neutral rather than transparent black.
            if (groups[g].normal) {
                for (int y = 0; y < H; ++y) {
                    QRgb* row = reinterpret_cast<QRgb*>(images[g].scanLine(y));
                    for (int x = 0; x < W; ++x)
                        if (!cov[size_t(y) * W + x]) row[x] = qRgba(128, 128, 255, 255);
                }
            }
            for (const QString& s : groups[g].slotNames)
                r.maps.insert(s, images[g]);
            if (groups[g].diffuse) r.diffuseColorBaked = true;
        }
    }

    // Fan triangulation for the render buffer.
    for (const auto& f : r.faces) {
        if (f.size() == 4) ++r.quads;
        else if (f.size() == 3) ++r.triangles;
        for (size_t i = 2; i < f.size(); ++i)
            r.indices.insert(r.indices.end(), {f[0], f[i - 1], f[i]});
    }
    r.vertexCount = int(r.positions.size() / 3);
    if (srcDiag > 0) {
        float mean, mx;
        surfaceDeviation(src.positions, r.positions, r.indices, mean, mx);
        r.meanDeviation = mean / srcDiag;
        r.maxDeviation = mx / srcDiag;
    }
    r.ok = true;
    return r;
}

Result remesh(const SourceMesh& src, const Options& opts)
{
    Result fail;
    fail.inputTriangles = int(src.indices.size() / 3);
    auto report = [&](const QString& s, double f) { return !opts.progress || opts.progress(s, f); };
    if (src.positions.size() < 9 || src.indices.size() < 3) {
        fail.error = QStringLiteral("mesh has no triangles");
        return fail;
    }

    if (!report(QStringLiteral("prepare"), 0.0)) { fail.cancelled = true; fail.error = QStringLiteral("cancelled"); return fail; }
    std::vector<float> pos = src.positions;
    std::vector<uint32_t> idx = src.indices;
    const int welded = weldByPosition(pos, idx);
    Trellis2Bake::unifyWinding(pos, idx);
    const Topology topo = analyzeTopology(idx, int(pos.size() / 3));

    bool voxelized = false;
    QString voxelReason;
    auto voxelize = [&](const QString& why) -> bool {
        if (!report(QStringLiteral("voxel"), 0.1)) return false;
        int res = opts.voxelResolution;
        if (res <= 0)
            res = std::clamp(int(std::lround(2.5 * std::sqrt(double(std::max(16, opts.targetFaces))))), 64, 256);
        std::vector<float> vp;
        std::vector<uint32_t> vi;
        QString err;
        bool cancelled = false;
        // A poll (empty stage, fraction < 0): the MCP pump and the GUI
        // cancel flag both answer through the progress callback.
        if (!voxelRemesh(pos, idx, res, vp, vi, &err,
                         [&]() { return report(QString(), -1.0); }, &cancelled)) {
            if (!cancelled) fail.error = err;   // empty error = cancelled (voxelFailed)
            return false;
        }
        pos.swap(vp);
        idx.swap(vi);
        voxelized = true;
        voxelReason = why;
        return true;
    };

    auto voxelFailed = [&]() {
        if (fail.error.isEmpty()) {   // voxelize() only leaves it empty on cancel
            fail.cancelled = true;
            fail.error = QStringLiteral("cancelled");
        }
        return fail;
    };
    if (opts.voxel == VoxelMode::Always) {
        if (!voxelize(QStringLiteral("requested")))
            return voxelFailed();
    } else if (opts.voxel == VoxelMode::Auto && !topo.manifold()) {
        if (!voxelize(QStringLiteral("non-manifold input (%1 edges, %2 vertices)")
                          .arg(topo.nonManifoldEdges).arg(topo.nonManifoldVertices)))
            return voxelFailed();
    }

    auto runFlow = [&](bool sharp, bool adaptive) {
        QuadriFlowRunner::Request req;
        req.positions = pos;
        req.indices = idx;
        req.targetFaces = opts.targetFaces;
        req.preserveSharp = sharp;
        req.preserveBoundary = opts.preserveBoundary;
        req.adaptiveScale = adaptive;
        req.seed = opts.seed;
        req.timeoutMs = opts.timeoutMs;
        req.helperPath = opts.helperPath;
        req.progress = [&](int stage, const QString& name) {
            if (stage <= 0) return report(QString(), -1.0);
            return report(QStringLiteral("quadriflow: %1").arg(name), 0.2 + 0.5 * (stage - 1) / 6.0);
        };
        return QuadriFlowRunner::run(req);
    };
    QString fallbackReason;
    QuadriFlowRunner::Outcome flow = runFlow(opts.preserveSharp, opts.adaptiveScale);
    if (!flow.ok && !flow.cancelled && !flow.helperMissing && !voxelized
        && opts.voxel == VoxelMode::Auto) {
        // A "manifold" mesh can still trip QuadriFlow (self-intersections,
        // slivers); the voxel rebuild is the robust second attempt.
        const QString first = flow.error;
        if (voxelize(QStringLiteral("direct remesh failed: %1").arg(first)))
            flow = runFlow(opts.preserveSharp, opts.adaptiveScale);
        else if (fail.error.isEmpty())
            return voxelFailed();   // cancelled during the rebuild: stop, no plain retry
    }
    if (!flow.ok && !flow.cancelled && !flow.helperMissing
        && (opts.preserveSharp || opts.adaptiveScale)) {
        // Last resort: the plain field. The crease/adaptive constraints are
        // what drove QuadriFlow's valence clean-up into a non-manifold loop on
        // a real character (now bounded in the vendored source, but upstream
        // has other fragile spots); a plain remesh beats no remesh.
        fallbackReason = QStringLiteral("retried without sharp/adaptive: %1").arg(flow.error);
        flow = runFlow(false, false);
    }
    if (!flow.ok) {
        fail.cancelled = flow.cancelled;
        fail.error = flow.error;
        return fail;
    }

    Options to = opts;
    if (to.reprojectIterations < 0) to.reprojectIterations = voxelized ? 3 : 0;
    Result r = transferOnto(src, flow.positions, flow.faces, to);
    r.weldedVertices = welded;
    r.nonManifoldEdges = topo.nonManifoldEdges;
    r.boundaryEdges = topo.boundaryEdges;
    r.voxelized = voxelized;
    r.voxelReason = voxelReason;
    r.fallbackReason = fallbackReason;
    if (r.ok) report(QStringLiteral("done"), 1.0);
    return r;
}

} // namespace QuadRemesh
