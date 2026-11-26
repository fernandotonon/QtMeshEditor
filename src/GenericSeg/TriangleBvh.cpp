#include "TriangleBvh.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GenericSeg {

float length(const Vec3& v) { return std::sqrt(dot(v, v)); }

Vec3 normalized(const Vec3& v)
{
    const float l = length(v);
    return l > 0.0f ? v * (1.0f / l) : Vec3();
}

namespace {
constexpr int kLeafSize = 4;

Vec3 vmin(const Vec3& a, const Vec3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
Vec3 vmax(const Vec3& a, const Vec3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
} // namespace

void TriangleBvh::build(const float* positions, int vertexCount,
                        const uint32_t* indices, int indexCount)
{
    m_tris.clear();
    m_order.clear();
    m_nodes.clear();
    const int triCount = indexCount / 3;
    if (!positions || !indices || triCount <= 0 || vertexCount <= 0)
        return;
    auto P = [&](uint32_t i) {
        if (i >= uint32_t(vertexCount)) return Vec3();
        return Vec3(positions[3 * i], positions[3 * i + 1], positions[3 * i + 2]);
    };
    m_tris.resize(triCount);
    std::vector<Vec3> centroids(triCount);
    for (int t = 0; t < triCount; ++t) {
        m_tris[t] = {P(indices[3 * t]), P(indices[3 * t + 1]), P(indices[3 * t + 2])};
        centroids[t] = (m_tris[t].a + m_tris[t].b + m_tris[t].c) * (1.0f / 3.0f);
    }
    m_order.resize(triCount);
    for (int t = 0; t < triCount; ++t) m_order[t] = t;
    m_nodes.reserve(2 * triCount / kLeafSize + 1);
    buildNode(0, triCount, centroids);
}

int TriangleBvh::buildNode(int first, int count, std::vector<Vec3>& centroids)
{
    const int index = int(m_nodes.size());
    m_nodes.emplace_back();
    Vec3 lo(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    Vec3 hi = lo * -1.0f;
    Vec3 clo = lo, chi = hi;
    for (int i = first; i < first + count; ++i) {
        const Tri& t = m_tris[m_order[i]];
        lo = vmin(lo, vmin(t.a, vmin(t.b, t.c)));
        hi = vmax(hi, vmax(t.a, vmax(t.b, t.c)));
        clo = vmin(clo, centroids[m_order[i]]);
        chi = vmax(chi, centroids[m_order[i]]);
    }
    m_nodes[index].lo = lo;
    m_nodes[index].hi = hi;
    if (count <= kLeafSize) {
        m_nodes[index].first = first;
        m_nodes[index].count = count;
        return index;
    }
    const Vec3 ext = chi - clo;
    const int axis = (ext.x >= ext.y && ext.x >= ext.z) ? 0 : (ext.y >= ext.z ? 1 : 2);
    const int mid = first + count / 2;
    std::nth_element(m_order.begin() + first, m_order.begin() + mid, m_order.begin() + first + count,
                     [&](int a, int b) { return centroids[a][axis] < centroids[b][axis]; });
    const int left = buildNode(first, mid - first, centroids);
    const int right = buildNode(mid, first + count - mid, centroids);
    m_nodes[index].left = left;
    m_nodes[index].right = right;
    return index;
}

bool TriangleBvh::hitBox(const Node& n, const Vec3& o, const Vec3& inv, float tMin, float tMax)
{
    for (int a = 0; a < 3; ++a) {
        float t0 = (n.lo[a] - o[a]) * inv[a];
        float t1 = (n.hi[a] - o[a]) * inv[a];
        if (t0 > t1) std::swap(t0, t1);
        // NaN (0 * inf on a flat box along a parallel ray) must not reject.
        if (!(t0 <= t1)) continue;
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMax < tMin) return false;
    }
    return true;
}

bool TriangleBvh::intersect(const Vec3& o, const Vec3& d, float tMin, float tMax,
                            Hit& out, int ignoreFace) const
{
    if (m_nodes.empty())
        return false;
    const float inf = std::numeric_limits<float>::infinity();
    const Vec3 inv(d.x != 0 ? 1.0f / d.x : inf, d.y != 0 ? 1.0f / d.y : inf, d.z != 0 ? 1.0f / d.z : inf);
    bool found = false;
    float best = tMax;
    int stack[128];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Node& n = m_nodes[stack[--sp]];
        if (!hitBox(n, o, inv, tMin, best))
            continue;
        if (n.count > 0) {
            for (int i = n.first; i < n.first + n.count; ++i) {
                const int f = m_order[i];
                if (f == ignoreFace) continue;
                const Tri& tri = m_tris[f];
                // Moller-Trumbore, both sides.
                const Vec3 e1 = tri.b - tri.a, e2 = tri.c - tri.a;
                const Vec3 p = cross(d, e2);
                const float det = dot(e1, p);
                if (std::fabs(det) < 1e-12f) continue;
                const float invDet = 1.0f / det;
                const Vec3 s = o - tri.a;
                const float u = dot(s, p) * invDet;
                if (u < 0.0f || u > 1.0f) continue;
                const Vec3 q = cross(s, e1);
                const float v = dot(d, q) * invDet;
                if (v < 0.0f || u + v > 1.0f) continue;
                const float t = dot(e2, q) * invDet;
                if (t > tMin && t < best) {
                    best = t;
                    out = {f, t, u, v};
                    found = true;
                }
            }
        } else if (sp + 2 <= 128) {
            stack[sp++] = n.left;
            stack[sp++] = n.right;
        }
    }
    return found;
}

Vec3 TriangleBvh::faceNormal(int face) const
{
    if (face < 0 || face >= int(m_tris.size())) return {};
    const Tri& t = m_tris[face];
    return normalized(cross(t.b - t.a, t.c - t.a));
}

} // namespace GenericSeg
