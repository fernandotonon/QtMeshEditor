#include "MouthMorphRepair.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace CharacterGen {
namespace {

constexpr double kCmToM = 0.01;

struct Region {
    std::vector<int32_t> faces;                ///< triangles (index of the first corner / 3) fully inside the radius
    std::vector<std::vector<int32_t>> ring;    ///< 1-ring neighbours, for the vertices of those triangles
};

double dist2(const float* a, const std::array<double, 3>& c)
{
    const double x = a[0] - c[0], y = a[1] - c[1], z = a[2] - c[2];
    return x * x + y * y + z * z;
}

Region regionOf(const std::vector<float>& positions, int vertexCount, const std::vector<int32_t>& faces,
                const std::array<double, 3>& centre, double radius)
{
    Region r;
    r.ring.resize(size_t(vertexCount));
    const double r2 = radius * radius;
    for (size_t f = 0; f + 2 < faces.size(); f += 3) {
        bool inside = true;
        for (size_t k = 0; k < 3 && inside; ++k) {
            const int32_t v = faces[f + k];
            inside = v >= 0 && v < vertexCount && dist2(&positions[size_t(v) * 3], centre) <= r2;
        }
        if (!inside) continue;
        r.faces.push_back(int32_t(f));
        for (size_t k = 0; k < 3; ++k) {
            auto& n = r.ring[size_t(faces[f + k])];
            for (size_t j = 1; j < 3; ++j) {
                const int32_t o = faces[f + (k + j) % 3];
                if (std::find(n.begin(), n.end(), o) == n.end()) n.push_back(o);
            }
        }
    }
    return r;
}

// Unit normal of a triangle, rest (w = 0) or posed (w = 1); zero for a degenerate one.
std::array<double, 3> faceNormal(const std::vector<float>& positions, const float* delta, const std::vector<int32_t>& faces, int32_t f,
                                 double w)
{
    std::array<std::array<double, 3>, 3> p{};
    for (size_t k = 0; k < 3; ++k) {
        const size_t v = size_t(faces[size_t(f) + k]) * 3;
        for (size_t c = 0; c < 3; ++c) p[k][c] = positions[v + c] + w * delta[v + c] * kCmToM;
    }
    const double a[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
    const double b[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
    std::array<double, 3> n{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l <= 0.0) return {0.0, 0.0, 0.0};
    for (double& x : n) x /= l;
    return n;
}

// Longest edge of a triangle, rest (w = 0) or posed with the channel's delta (w = 1).
double longestEdge(const std::vector<float>& positions, const float* delta, const std::vector<int32_t>& faces, int32_t f, double w)
{
    double best = 0.0;
    for (int k = 0; k < 3; ++k) {
        const size_t a = size_t(faces[size_t(f) + size_t(k)]) * 3, b = size_t(faces[size_t(f) + size_t((k + 1) % 3)]) * 3;
        double s = 0.0;
        for (size_t c = 0; c < 3; ++c) {
            const double d = (positions[b + c] + w * delta[b + c] * kCmToM) - (positions[a + c] + w * delta[a + c] * kCmToM);
            s += d * d;
        }
        best = std::max(best, std::sqrt(s));
    }
    return best;
}

std::vector<char> tornVertices(const std::vector<float>& positions, const std::vector<int32_t>& faces, const Region& region,
                               const float* delta, int vertexCount, const MouthRepairOptions& o)
{
    std::vector<char> torn(size_t(vertexCount), 0);
    for (int32_t f : region.faces) {
        const double rest = longestEdge(positions, delta, faces, f, 0.0);
        const double posed = longestEdge(positions, delta, faces, f, 1.0);
        const std::array<double, 3> n0 = faceNormal(positions, delta, faces, f, 0.0), n1 = faceNormal(positions, delta, faces, f, 1.0);
        // a triangle the shape turns over: the folded inner lip at the corner moves past its neighbours and flips
        const bool flipped = posed > o.minGrowthM && n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2] < o.flipDot;
        if (flipped || (posed > rest * o.stretchRatio && posed - rest > o.minGrowthM))
            for (int k = 0; k < 3; ++k) torn[size_t(faces[size_t(f) + size_t(k)])] = 1;
    }
    return torn;
}

// Harmonic fill: every torn vertex takes the mean delta of its neighbours; untorn vertices stay fixed and act as the boundary.
int fillHarmonic(const Region& region, const std::vector<char>& torn, float* delta, int iterations)
{
    std::vector<int32_t> free;
    for (size_t v = 0; v < torn.size(); ++v)
        if (torn[v] && !region.ring[v].empty()) free.push_back(int32_t(v));
    if (free.empty()) return 0;
    std::vector<float> next(free.size() * 3);
    for (int it = 0; it < iterations; ++it) {
        for (size_t i = 0; i < free.size(); ++i) {
            const auto& n = region.ring[size_t(free[i])];
            double s[3] = {0.0, 0.0, 0.0};
            for (int32_t o : n)
                for (size_t c = 0; c < 3; ++c) s[c] += delta[size_t(o) * 3 + c];
            for (size_t c = 0; c < 3; ++c) next[i * 3 + c] = float(s[c] / double(n.size()));
        }
        for (size_t i = 0; i < free.size(); ++i)
            for (size_t c = 0; c < 3; ++c) delta[size_t(free[i]) * 3 + c] = next[i * 3 + c];
    }
    return int(free.size());
}

// Grow the torn set by one ring inside the region, so the fill has room to spread the motion instead of only re-placing the
// vertices of the torn triangles.
void growOnce(const Region& region, std::vector<char>& torn)
{
    const std::vector<char> seed = torn;
    for (size_t v = 0; v < seed.size(); ++v)
        if (seed[v])
            for (int32_t o : region.ring[v]) torn[size_t(o)] = 1;
}

}  // namespace

int repairMouthMorphs(const std::vector<float>& positions, int vertexCount, const std::vector<int32_t>& faces,
                      const std::array<double, 3>& centre, double radius, std::vector<float>& arkit, int channels,
                      const MouthRepairOptions& options)
{
    const size_t stride = size_t(vertexCount) * 3;
    if (vertexCount <= 0 || channels <= 0 || positions.size() < stride || arkit.size() < stride * size_t(channels)) return 0;
    const Region region = regionOf(positions, vertexCount, faces, centre, radius);
    if (region.faces.empty()) return 0;
    int replaced = 0;
    for (int e = 0; e < channels; ++e) {
        float* delta = arkit.data() + size_t(e) * stride;
        // A fill can leave a smaller tear at the edge of the area it re-placed, so detect again and widen the area each pass.
        std::vector<char> area(size_t(vertexCount), 0);
        int n = 0;
        for (int pass = 0; pass < options.passes; ++pass) {
            const std::vector<char> torn = tornVertices(positions, faces, region, delta, vertexCount, options);
            if (std::find(torn.begin(), torn.end(), char(1)) == torn.end()) break;
            for (size_t v = 0; v < area.size(); ++v) area[v] = char(area[v] | torn[v]);
            growOnce(region, area);
            n = fillHarmonic(region, area, delta, options.iterations);
        }
        if (n == 0) continue;
        replaced += n;
        if (std::getenv("QTMESH_MOUTH_DEBUG"))
            std::fprintf(stderr, "mouth repair: channel %d, %d vertices, worst stretch now %.2f\n", e, n,
                         worstMouthStretch(positions, vertexCount, faces, centre, radius, arkit, e));
    }
    return replaced;
}

std::vector<int32_t> mouthFacesTurnedOver(const std::vector<float>& positions, int vertexCount, const std::vector<int32_t>& faces,
                                          const std::array<double, 3>& centre, double radius, const std::vector<float>& arkit,
                                          int channels)
{
    std::vector<int32_t> out;
    const size_t stride = size_t(vertexCount) * 3;
    if (vertexCount <= 0 || channels <= 0 || arkit.size() < stride * size_t(channels)) return out;
    const Region region = regionOf(positions, vertexCount, faces, centre, radius);
    for (int32_t f : region.faces) {
        const std::array<double, 3> n0 = faceNormal(positions, arkit.data(), faces, f, 0.0);
        bool turned = false;
        for (int e = 0; e < channels && !turned; ++e) {
            const float* delta = arkit.data() + size_t(e) * stride;
            for (double w : {0.5, 1.0}) {
                const std::array<double, 3> n1 = faceNormal(positions, delta, faces, f, w);
                if (n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2] < 0.0) turned = true;
            }
        }
        if (turned) out.push_back(f / 3);
    }
    return out;
}

namespace {

// The shortest directed cycle back to `from` that starts with the edge from -> first, over unused edges, at most maxLen edges.
std::vector<int32_t> shortestCycle(const std::map<int32_t, std::vector<int32_t>>& next, const std::set<std::pair<int32_t, int32_t>>& used,
                                   int32_t from, int32_t first, int maxLen)
{
    std::map<int32_t, int32_t> parent{{first, from}};
    std::vector<int32_t> frontier{first};
    for (int depth = 1; depth < maxLen && !frontier.empty(); ++depth) {
        std::vector<int32_t> grown;
        for (int32_t v : frontier) {
            const auto it = next.find(v);
            if (it == next.end()) continue;
            for (int32_t w : it->second) {
                if (used.count({v, w})) continue;
                if (w == from) {
                    std::vector<int32_t> cycle{v};
                    for (int32_t u = v; u != from; ) cycle.push_back(u = parent.at(u));
                    std::reverse(cycle.begin(), cycle.end());   // from, first, ..., v
                    return cycle;
                }
                if (parent.emplace(w, v).second) grown.push_back(w);
            }
        }
        frontier = std::move(grown);
    }
    return {};
}

// Triangles closing one hole, wound like the loop. A strip zips the loop shut from its sharpest end, which also closes a slit (the
// usual shape here) without the overlapping triangles a fan makes on a non-convex outline.
void capLoop(const std::vector<float>& positions, std::vector<int32_t> loop, std::vector<int32_t>& caps)
{
    const size_t n = loop.size();
    double c[3] = {0.0, 0.0, 0.0};
    for (int32_t v : loop)
        for (size_t k = 0; k < 3; ++k) c[k] += positions[size_t(v) * 3 + k] / double(n);
    size_t tip = 0;
    double far = -1.0;
    for (size_t i = 0; i < n; ++i) {
        double d = 0.0;
        for (size_t k = 0; k < 3; ++k) d += std::pow(positions[size_t(loop[i]) * 3 + k] - c[k], 2.0);
        if (d > far) { far = d; tip = i; }
    }
    std::rotate(loop.begin(), loop.begin() + long(tip), loop.end());
    // indices into the loop, emitted in increasing order so every triangle keeps the loop's winding
    size_t front = 1, back = n - 1;
    caps.insert(caps.end(), {loop[0], loop[front], loop[back]});
    bool advanceFront = true;
    while (back - front > 1) {
        if (advanceFront) {
            caps.insert(caps.end(), {loop[front], loop[front + 1], loop[back]});
            ++front;
        } else {
            caps.insert(caps.end(), {loop[front], loop[back - 1], loop[back]});
            --back;
        }
        advanceFront = !advanceFront;
    }
}

}  // namespace

std::vector<int32_t> mouthHoleCaps(const std::vector<float>& positions, int vertexCount, const std::vector<int32_t>& faces,
                                   const std::array<double, 3>& centre, double radius, int maxLoop)
{
    std::vector<int32_t> caps;
    if (vertexCount <= 0 || positions.size() < size_t(vertexCount) * 3 || maxLoop < 3) return caps;
    const double r2 = radius * radius;
    const auto inside = [&](int32_t v) { return v >= 0 && v < vertexCount && dist2(&positions[size_t(v) * 3], centre) <= r2; };
    // directed edges of the faces; an edge whose reverse is missing is on a boundary, and a hole runs along it backwards
    std::set<std::pair<int32_t, int32_t>> directed;
    for (size_t f = 0; f + 2 < faces.size(); f += 3)
        for (size_t k = 0; k < 3; ++k) directed.insert({faces[f + k], faces[f + (k + 1) % 3]});
    std::map<int32_t, std::vector<int32_t>> holeNext;   // b -> a for every boundary edge a -> b inside the radius
    std::vector<std::pair<int32_t, int32_t>> edges;
    for (const auto& e : directed)
        if (!directed.count({e.second, e.first}) && inside(e.first) && inside(e.second)) {
            holeNext[e.second].push_back(e.first);
            edges.push_back({e.second, e.first});
        }
    // The boundary near the mouth is the mouth opening plus slits that touch it at single vertices (pinches), so it is split into
    // directed cycles, shortest first: the slits close, the mouth opening (far longer than maxLoop) stays open.
    std::set<std::pair<int32_t, int32_t>> used;
    for (int len = 3; len <= maxLoop; ++len)
        for (const auto& [from, first] : edges) {
            if (used.count({from, first})) continue;
            const std::vector<int32_t> loop = shortestCycle(holeNext, used, from, first, len);
            if (loop.size() < 3) continue;
            for (size_t i = 0; i < loop.size(); ++i) used.insert({loop[i], loop[(i + 1) % loop.size()]});
            capLoop(positions, loop, caps);
        }
    return caps;
}

void blendCornerNormals(int vertexCount, const std::vector<int32_t>& faces, const std::vector<int32_t>& vertices,
                        std::vector<float>& normals, int iterations)
{
    const size_t V = size_t(std::max(vertexCount, 0));
    if (vertices.empty() || normals.size() < V * 3) return;
    std::vector<char> free(V, 0);
    for (int32_t v : vertices)
        if (v >= 0 && size_t(v) < V) free[size_t(v)] = 1;
    // 1-ring of the free vertices
    std::map<int32_t, std::vector<int32_t>> ring;
    for (size_t f = 0; f + 2 < faces.size(); f += 3)
        for (size_t k = 0; k < 3; ++k) {
            const int32_t v = faces[f + k];
            if (v < 0 || size_t(v) >= V || !free[size_t(v)]) continue;
            auto& n = ring[v];
            for (size_t j = 1; j < 3; ++j) {
                const int32_t o = faces[f + (k + j) % 3];
                if (o >= 0 && size_t(o) < V && o != v && std::find(n.begin(), n.end(), o) == n.end()) n.push_back(o);
            }
        }
    // start from the mean of the fixed neighbours' normals (or keep the vertex's own) so the iteration converges quickly
    std::vector<std::pair<int32_t, std::array<double, 3>>> next;
    next.reserve(ring.size());
    for (int it = 0; it < iterations; ++it) {
        next.clear();
        for (const auto& [v, n] : ring) {
            std::array<double, 3> s{0.0, 0.0, 0.0};
            for (int32_t o : n)
                for (size_t c = 0; c < 3; ++c) s[c] += normals[size_t(o) * 3 + c];
            const double l = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
            if (l > 0.0) next.push_back({v, {s[0] / l, s[1] / l, s[2] / l}});
        }
        for (const auto& [v, n] : next)
            for (size_t c = 0; c < 3; ++c) normals[size_t(v) * 3 + c] = float(n[c]);
    }
}

std::vector<std::vector<int32_t>> boundaryLoops(const std::vector<int32_t>& faces, int vertexCount)
{
    std::set<std::pair<int32_t, int32_t>> edges;
    for (size_t f = 0; f + 2 < faces.size(); f += 3)
        for (size_t k = 0; k < 3; ++k) {
            const int32_t a = faces[f + k], b = faces[f + (k + 1) % 3];
            if (a >= 0 && b >= 0 && a < vertexCount && b < vertexCount && a != b) edges.insert({a, b});
        }
    std::map<int32_t, std::vector<int32_t>> next;
    for (const auto& [a, b] : edges)
        if (!edges.count({b, a})) next[a].push_back(b);
    std::set<std::pair<int32_t, int32_t>> used;
    std::vector<std::vector<int32_t>> out;
    for (const auto& [start, outs] : next)
        for (int32_t first : outs) {
            if (used.count({start, first})) continue;
            std::vector<int32_t> loop{start};
            int32_t v = start, w = first;
            bool closed = false;
            while (!used.count({v, w})) {
                used.insert({v, w});
                if (w == start) { closed = true; break; }
                loop.push_back(w);
                const auto it = next.find(w);
                if (it == next.end()) break;
                int32_t n = -1;
                for (int32_t c : it->second)
                    if (!used.count({w, c})) { n = c; break; }
                if (n < 0) break;
                v = w;
                w = n;
            }
            if (closed && loop.size() >= 3) out.push_back(std::move(loop));
        }
    return out;
}

namespace {

// Each vertex's parameter along a closed loop, in [0, 2): [0, 1] along the half from the rightmost vertex (largest x) to the leftmost,
// [1, 2) back. The loop is first turned to run counter-clockwise about the axis in the xy plane.
std::vector<double> halfArcParams(std::vector<std::array<float, 3>>& loop, std::vector<int32_t>& order, const std::array<double, 3>& axis)
{
    const size_t n = loop.size();
    if (n == 0) return {};
    double area = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const auto& p = loop[i];
        const auto& q = loop[(i + 1) % n];
        area += (p[0] - axis[0]) * (q[1] - axis[1]) - (q[0] - axis[0]) * (p[1] - axis[1]);
    }
    if (area < 0.0) {
        std::reverse(loop.begin(), loop.end());
        std::reverse(order.begin(), order.end());
    }
    size_t right = 0, left = 0;
    for (size_t i = 0; i < n; ++i) {
        if (loop[i][0] > loop[right][0]) right = i;
        if (loop[i][0] < loop[left][0]) left = i;
    }
    std::rotate(loop.begin(), loop.begin() + long(right), loop.end());
    std::rotate(order.begin(), order.begin() + long(right), order.end());
    left = (left + n - right) % n;
    std::vector<double> arc(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const auto& p = loop[i];
        const auto& q = loop[(i + 1) % n];
        arc[i + 1] = arc[i] + std::sqrt(std::pow(q[0] - p[0], 2.0) + std::pow(q[1] - p[1], 2.0) + std::pow(q[2] - p[2], 2.0));
    }
    std::vector<double> t(n);
    const double half = std::max(arc[left], 1e-12), rest = std::max(arc[n] - arc[left], 1e-12);
    for (size_t i = 0; i < n; ++i) t[i] = i <= left ? arc[i] / half : 1.0 + (arc[i] - arc[left]) / rest;
    if (left == 0) for (size_t i = 0; i < n; ++i) t[i] = 2.0 * arc[i] / std::max(arc[n], 1e-12);
    return t;
}

}  // namespace

std::vector<std::array<int32_t, 3>> bridgeLoops(const std::vector<std::array<float, 3>>& A0, const std::vector<std::array<float, 3>>& B0,
                                               const std::array<double, 3>& axis)
{
    std::vector<std::array<int32_t, 3>> out;
    if (A0.size() < 3 || B0.size() < 3) return out;
    std::vector<std::array<float, 3>> A = A0, B = B0;
    std::vector<int32_t> ia(A.size()), ib(B.size());
    for (size_t i = 0; i < ia.size(); ++i) ia[i] = int32_t(i);
    for (size_t i = 0; i < ib.size(); ++i) ib[i] = int32_t(A.size() + i);
    const std::vector<double> ta = halfArcParams(A, ia, axis), tb = halfArcParams(B, ib, axis);
    const size_t nA = A.size(), nB = B.size();
    if (nA == 0 || nB == 0 || ta.size() != nA || tb.size() != nB) return out;
    const auto pos = [&](int32_t v) { return v < int32_t(nA) ? A0[size_t(v)] : B0[size_t(v) - nA]; };
    const auto push = [&](int32_t a, int32_t b, int32_t c) {
        const auto p0 = pos(a), p1 = pos(b), p2 = pos(c);
        const double e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const double nrm[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const double cx = (p0[0] + p1[0] + p2[0]) / 3.0, cy = (p0[1] + p1[1] + p2[1]) / 3.0;
        const double towards = nrm[0] * (axis[0] - cx) + nrm[1] * (axis[1] - cy);
        if (towards < 0.0) out.push_back({a, c, b});
        else out.push_back({a, b, c});
    };
    size_t i = 0, j = 0;
    while (i < nA || j < nB) {
        const double nextA = i < nA ? (i + 1 < nA ? ta[i + 1] : 2.0) : 1e9;
        const double nextB = j < nB ? (j + 1 < nB ? tb[j + 1] : 2.0) : 1e9;
        if (nextA <= nextB) {
            push(ia[i], ia[(i + 1) % nA], ib[j % nB]);
            ++i;
        } else {
            push(ia[i % nA], ib[(j + 1) % nB], ib[j]);
            ++j;
        }
    }
    return out;
}

double worstMouthStretch(const std::vector<float>& positions, int vertexCount, const std::vector<int32_t>& faces,
                         const std::array<double, 3>& centre, double radius, const std::vector<float>& arkit, int channel)
{
    const size_t stride = size_t(vertexCount) * 3;
    if (channel < 0 || arkit.size() < stride * size_t(channel + 1)) return 0.0;
    const Region region = regionOf(positions, vertexCount, faces, centre, radius);
    const float* delta = arkit.data() + size_t(channel) * stride;
    double worst = 0.0;
    for (int32_t f : region.faces) {
        const double rest = longestEdge(positions, delta, faces, f, 0.0);
        if (rest > 0.0) worst = std::max(worst, longestEdge(positions, delta, faces, f, 1.0) / rest);
    }
    return worst;
}

}  // namespace CharacterGen
