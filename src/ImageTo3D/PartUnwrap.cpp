#include "PartUnwrap.h"

#include <xatlas.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <unordered_map>

namespace PartUnwrap {

namespace {

using Vec3 = std::array<double, 3>;
constexpr double kPi = 3.14159265358979323846;

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }

struct UnionFind {
    std::vector<uint32_t> p;
    explicit UnionFind(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0u); }
    uint32_t find(uint32_t x)
    {
        while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; }
        return x;
    }
    void unite(uint32_t a, uint32_t b)
    {
        a = find(a); b = find(b);
        if (a != b) p[a] = b;
    }
};

// ---- whole-mesh adjacency ----------------------------------------------------
struct MeshTopo {
    size_t nF = 0, nV = 0;
    const std::vector<float>* pos = nullptr;
    const std::vector<uint32_t>* idx = nullptr;
    std::vector<int32_t> sideEdge;   // 3*nF -> edge id
    std::vector<int32_t> nbr;        // 3*nF -> neighbour face across a 2-face edge, else -1
    std::vector<uint32_t> edgeV;     // 2 per edge
    std::vector<uint8_t> edgeFaces;  // faces using the edge (saturating)
    std::vector<double> area;
    std::vector<double> proxyCentroid;  // 3*nF (smoothed)
    std::vector<double> proxyNormal;    // 3*nF (smoothed)
    double diag = 1.0;
    double totalArea = 0.0;

    Vec3 p(uint32_t v) const
    {
        return {(*pos)[v * 3], (*pos)[v * 3 + 1], (*pos)[v * 3 + 2]};
    }
    uint32_t fv(size_t f, int k) const { return (*idx)[f * 3 + k]; }
};

void buildEdges(MeshTopo& m)
{
    struct HalfEdge { uint64_t key; uint32_t side; };
    std::vector<HalfEdge> he(m.nF * 3);
    for (size_t f = 0; f < m.nF; ++f) {
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = m.fv(f, k), b = m.fv(f, (k + 1) % 3);
            const uint64_t key = (uint64_t(std::min(a, b)) << 32) | std::max(a, b);
            he[f * 3 + k] = {key, uint32_t(f * 3 + k)};
        }
    }
    std::sort(he.begin(), he.end(),
              [](const HalfEdge& x, const HalfEdge& y) { return x.key < y.key; });
    m.sideEdge.assign(m.nF * 3, -1);
    m.nbr.assign(m.nF * 3, -1);
    size_t i = 0;
    while (i < he.size()) {
        size_t j = i;
        while (j < he.size() && he[j].key == he[i].key) ++j;
        const int32_t e = int32_t(m.edgeFaces.size());
        m.edgeV.push_back(uint32_t(he[i].key >> 32));
        m.edgeV.push_back(uint32_t(he[i].key & 0xffffffffu));
        m.edgeFaces.push_back(uint8_t(std::min<size_t>(j - i, 255)));
        for (size_t k = i; k < j; ++k) m.sideEdge[he[k].side] = e;
        if (j - i == 2) {
            m.nbr[he[i].side] = int32_t(he[i + 1].side / 3);
            m.nbr[he[i + 1].side] = int32_t(he[i].side / 3);
        }
        i = j;
    }
}

// Taubin-smoothed copy of the positions: decides geometric splits on the
// shape rather than on triangle-scale noise.
bool buildProxy(MeshTopo& m, int passes, const std::function<bool()>& cancelled)
{
    std::vector<double> P(m.nV * 3);
    for (size_t i = 0; i < m.nV * 3; ++i) P[i] = (*m.pos)[i];
    std::vector<uint32_t> deg(m.nV, 0);
    for (size_t e = 0; e < m.edgeFaces.size(); ++e) {
        ++deg[m.edgeV[e * 2]];
        ++deg[m.edgeV[e * 2 + 1]];
    }
    std::vector<double> acc(m.nV * 3);
    for (int it = 0; it < passes; ++it) {
        if (cancelled && cancelled()) return false;
        for (const double lam : {0.5, -0.53}) {
            std::fill(acc.begin(), acc.end(), 0.0);
            for (size_t e = 0; e < m.edgeFaces.size(); ++e) {
                const uint32_t a = m.edgeV[e * 2], b = m.edgeV[e * 2 + 1];
                for (int k = 0; k < 3; ++k) {
                    acc[a * 3 + k] += P[b * 3 + k];
                    acc[b * 3 + k] += P[a * 3 + k];
                }
            }
            for (size_t v = 0; v < m.nV; ++v) {
                if (deg[v] == 0) continue;
                for (int k = 0; k < 3; ++k)
                    P[v * 3 + k] += lam * (acc[v * 3 + k] / deg[v] - P[v * 3 + k]);
            }
        }
    }
    m.proxyCentroid.resize(m.nF * 3);
    m.proxyNormal.resize(m.nF * 3);
    for (size_t f = 0; f < m.nF; ++f) {
        Vec3 c[3];
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = m.fv(f, k);
            c[k] = {P[v * 3], P[v * 3 + 1], P[v * 3 + 2]};
        }
        Vec3 n = cross(sub(c[1], c[0]), sub(c[2], c[0]));
        const double l = norm(n);
        for (int k = 0; k < 3; ++k) {
            m.proxyCentroid[f * 3 + k] = (c[0][k] + c[1][k] + c[2][k]) / 3.0;
            m.proxyNormal[f * 3 + k] = l > 0 ? n[k] / l : 0.0;
        }
    }
    return true;
}

void buildAreas(MeshTopo& m)
{
    m.area.resize(m.nF);
    Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    for (size_t v = 0; v < m.nV; ++v) {
        const Vec3 q = m.p(uint32_t(v));
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], q[k]); hi[k] = std::max(hi[k], q[k]); }
    }
    m.diag = std::max(1e-12, norm(sub(hi, lo)));
    m.totalArea = 0.0;
    for (size_t f = 0; f < m.nF; ++f) {
        const Vec3 a = m.p(m.fv(f, 0)), b = m.p(m.fv(f, 1)), c = m.p(m.fv(f, 2));
        m.area[f] = 0.5 * norm(cross(sub(b, a), sub(c, a)));
        m.totalArea += m.area[f];
    }
}

// ---- colour regions ------------------------------------------------------------
std::vector<std::array<double, 4>> faceFeatures(const MeshTopo& m,
                                                const std::vector<float>& rgb,
                                                const std::vector<float>& extra)
{
    std::vector<std::array<double, 4>> feat(m.nF);
    for (size_t f = 0; f < m.nF; ++f) {
        float lab[3];
        srgbToLab(&rgb[f * 3], lab);
        // Lightness counts half: shading baked into the colour varies L far
        // more than it varies the material.
        feat[f] = {0.5 * lab[0], lab[1], lab[2],
                   extra.empty() ? 0.0 : 60.0 * std::clamp(extra[f], 0.0f, 1.0f)};
    }
    return feat;
}

double dist2(const std::array<double, 4>& a, const std::array<double, 4>& b)
{
    double s = 0;
    for (int k = 0; k < 4; ++k) s += (a[k] - b[k]) * (a[k] - b[k]);
    return s;
}

using Feature = std::array<double, 4>;

// Deterministic farthest-point seeding: the first centre is the face closest
// to the area-weighted mean colour, each next one the face with the largest
// area-weighted squared distance to every centre so far. No random draws, so
// the same mesh always gets the same regions.
std::vector<Feature> seedCentres(const std::vector<Feature>& feat, const std::vector<double>& w,
                                 int K)
{
    const size_t n = feat.size();
    Feature mean{0, 0, 0, 0};
    double wsum = 0;
    for (size_t i = 0; i < n; ++i) {
        for (int k = 0; k < 4; ++k) mean[k] += w[i] * feat[i][k];
        wsum += w[i];
    }
    for (double& v : mean) v /= std::max(wsum, 1e-300);
    size_t first = 0;
    for (size_t i = 1; i < n; ++i)
        if (dist2(feat[i], mean) < dist2(feat[first], mean)) first = i;
    std::vector<Feature> c{feat[first]};
    std::vector<double> d(n, std::numeric_limits<double>::max());
    while (int(c.size()) < K) {
        size_t pick = 0;
        double best = 0;
        for (size_t i = 0; i < n; ++i) {
            d[i] = std::min(d[i], dist2(feat[i], c.back()));
            if (d[i] * w[i] > best) { best = d[i] * w[i]; pick = i; }
        }
        if (!(best > 0)) break;
        c.push_back(feat[pick]);
    }
    return c;
}

int nearestCentre(const Feature& f, const std::vector<Feature>& c)
{
    int best = 0;
    double bd = std::numeric_limits<double>::max();
    for (size_t k = 0; k < c.size(); ++k) {
        const double dd = dist2(f, c[k]);
        if (dd < bd) { bd = dd; best = int(k); }
    }
    return best;
}

std::vector<int> kmeansLabels(const std::vector<Feature>& feat, const std::vector<double>& w, int K)
{
    std::vector<Feature> c = seedCentres(feat, w, K);
    std::vector<int> lab(feat.size(), 0);
    for (int it = 0; it < 20; ++it) {
        std::vector<Feature> s(c.size(), Feature{0, 0, 0, 0});
        std::vector<double> ws(c.size(), 0);
        for (size_t i = 0; i < feat.size(); ++i) {
            lab[i] = nearestCentre(feat[i], c);
            for (int k = 0; k < 4; ++k) s[lab[i]][k] += w[i] * feat[i][k];
            ws[lab[i]] += w[i];
        }
        for (size_t k = 0; k < c.size(); ++k) {
            if (!(ws[k] > 0)) continue;
            for (int q = 0; q < 4; ++q) c[k][q] = s[k][q] / ws[k];
        }
    }
    return lab;
}

// Area-weighted majority of a face and its neighbours (the face counts 1.5x).
int majorityLabel(const MeshTopo& m, const std::vector<int>& lab, size_t f)
{
    int cand[4] = {lab[f], -1, -1, -1};
    double score[4] = {1.5 * m.area[f], 0, 0, 0};
    for (int k = 0; k < 3; ++k) {
        const int32_t g = m.nbr[f * 3 + k];
        if (g < 0) continue;
        int q = 0;
        while (cand[q] >= 0 && cand[q] != lab[g]) ++q;   // q <= 3: at most 4 labels
        cand[q] = lab[g];
        score[q] += m.area[g];
    }
    int best = 0;
    for (int q = 1; q < 4; ++q)
        if (cand[q] >= 0 && score[q] > score[best]) best = q;
    return cand[best];
}

void smoothLabels(const MeshTopo& m, std::vector<int>& lab, int passes)
{
    std::vector<int> next(lab.size());
    for (int it = 0; it < passes; ++it) {
        for (size_t f = 0; f < m.nF; ++f) next[f] = majorityLabel(m, lab, f);
        lab.swap(next);
    }
}

// Connected components of equal label over manifold edges.
std::vector<int> labelComponents(const MeshTopo& m, const std::vector<int>& lab, int& count)
{
    std::vector<int> comp(m.nF, -1);
    count = 0;
    std::vector<uint32_t> stack;
    for (size_t s = 0; s < m.nF; ++s) {
        if (comp[s] >= 0) continue;
        comp[s] = count;
        stack.assign(1, uint32_t(s));
        while (!stack.empty()) {
            const uint32_t f = stack.back();
            stack.pop_back();
            for (int k = 0; k < 3; ++k) {
                const int32_t g = m.nbr[f * 3 + k];
                if (g >= 0 && comp[g] < 0 && lab[g] == lab[f]) {
                    comp[g] = count;
                    stack.push_back(uint32_t(g));
                }
            }
        }
        ++count;
    }
    return comp;
}

// Merge regions smaller than minArea into the neighbour sharing the most edges.
void mergeSmallRegions(const MeshTopo& m, std::vector<int>& comp, int n, double minArea)
{
    std::vector<double> ra(n, 0.0);
    for (size_t f = 0; f < m.nF; ++f) ra[comp[f]] += m.area[f];
    // shared border LENGTH per neighbour (each manifold edge seen from both sides)
    std::vector<std::unordered_map<int, double>> border(n);
    for (size_t f = 0; f < m.nF; ++f)
        for (int k = 0; k < 3; ++k) {
            const int32_t g = m.nbr[f * 3 + k];
            if (g < 0 || comp[g] == comp[f]) continue;
            border[comp[f]][comp[g]] += norm(sub(m.p(m.fv(f, k)), m.p(m.fv(f, (k + 1) % 3))));
        }
    UnionFind uf(n);
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> heap;
    for (int c = 0; c < n; ++c)
        if (ra[c] < minArea) heap.push({ra[c], c});
    while (!heap.empty()) {
        const auto [a, c] = heap.top();
        heap.pop();
        if (int(uf.find(c)) != c || a != ra[c] || border[c].empty()) continue;
        int target = -1;
        double best = -1;
        for (const auto& [k, w] : border[c])
            if (w > best || (w == best && k < target)) { best = w; target = k; }
        uf.unite(c, target);   // c -> target
        ra[target] += ra[c];
        for (const auto& [k, w] : border[c]) {
            if (k == target) continue;
            border[target][k] += w;
            border[k][target] += w;
            border[k].erase(c);
        }
        border[target].erase(c);
        border[c].clear();
        if (ra[target] < minArea) heap.push({ra[target], target});
    }
    for (size_t f = 0; f < m.nF; ++f) comp[f] = int(uf.find(comp[f]));
}

// ---- charts --------------------------------------------------------------------
struct Chart {
    std::vector<uint32_t> faces;   // global face ids
    std::vector<uint32_t> tri;     // 3 local vertex ids per face
    std::vector<uint32_t> orig;    // local vertex -> input vertex
    std::vector<double> uv;        // 2 per local vertex
};

struct Workspace {
    const MeshTopo& m;
    const Options& opts;
    std::vector<int32_t> localOf;  // global face -> local index while a chart is built
    std::vector<uint8_t> seam;     // per edge
    explicit Workspace(const MeshTopo& mm, const Options& o)
        : m(mm), opts(o), localOf(mm.nF, -1), seam(mm.edgeFaces.size(), 0) {}

    bool interior(uint32_t f, int k) const
    {
        const int32_t g = m.nbr[f * 3 + k];
        return g >= 0 && localOf[g] >= 0 && !seam[m.sideEdge[f * 3 + k]];
    }
};

int cornerOf(const MeshTopo& m, uint32_t f, uint32_t v)
{
    for (int k = 0; k < 3; ++k)
        if (m.fv(f, k) == v) return k;
    return 0;
}

void markLocal(Workspace& ws, const std::vector<uint32_t>& faces, bool on)
{
    for (size_t i = 0; i < faces.size(); ++i)
        ws.localOf[faces[i]] = on ? int32_t(i) : -1;
}

// Corner grouping: corners around a vertex stay one chart vertex while the
// faces between them are joined by non-seam manifold edges.
Chart buildChart(Workspace& ws, const std::vector<uint32_t>& faces)
{
    const MeshTopo& m = ws.m;
    markLocal(ws, faces, true);
    UnionFind uf(faces.size() * 3);
    for (size_t i = 0; i < faces.size(); ++i) {
        const uint32_t f = faces[i];
        for (int k = 0; k < 3; ++k) {
            if (!ws.interior(f, k)) continue;
            const uint32_t g = uint32_t(m.nbr[f * 3 + k]);
            const size_t j = size_t(ws.localOf[g]);
            const uint32_t a = m.fv(f, k), b = m.fv(f, (k + 1) % 3);
            uf.unite(uint32_t(i * 3 + k), uint32_t(j * 3 + cornerOf(m, g, a)));
            uf.unite(uint32_t(i * 3 + (k + 1) % 3), uint32_t(j * 3 + cornerOf(m, g, b)));
        }
    }
    markLocal(ws, faces, false);
    Chart c;
    c.faces = faces;
    c.tri.resize(faces.size() * 3);
    std::unordered_map<uint32_t, uint32_t> id;
    for (size_t i = 0; i < faces.size() * 3; ++i) {
        const uint32_t r = uf.find(uint32_t(i));
        auto it = id.find(r);
        if (it == id.end()) {
            it = id.emplace(r, uint32_t(c.orig.size())).first;
            c.orig.push_back(m.fv(faces[i / 3], int(i % 3)));
        }
        c.tri[i] = it->second;
    }
    return c;
}

uint64_t ekey(uint32_t a, uint32_t b)
{
    return (uint64_t(std::min(a, b)) << 32) | std::max(a, b);
}

// Disc = one boundary loop, Euler characteristic 1, no bow-tie vertex.
bool isDisc(const Chart& c, std::vector<uint32_t>* loop)
{
    std::unordered_map<uint64_t, int> ec;
    for (size_t t = 0; t < c.tri.size(); t += 3)
        for (int k = 0; k < 3; ++k) ++ec[ekey(c.tri[t + k], c.tri[t + (k + 1) % 3])];
    const long chi = long(c.orig.size()) - long(ec.size()) + long(c.tri.size() / 3);
    if (chi != 1) return false;
    std::unordered_map<uint32_t, uint32_t> next;
    for (size_t t = 0; t < c.tri.size(); t += 3)
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = c.tri[t + k], b = c.tri[t + (k + 1) % 3];
            if (ec[ekey(a, b)] != 1) continue;
            if (!next.emplace(a, b).second) return false;   // bow-tie
        }
    if (next.empty()) return false;   // closed
    std::vector<uint32_t> walk;
    uint32_t v = next.begin()->first;
    const uint32_t start = v;
    do {
        walk.push_back(v);
        auto it = next.find(v);
        if (it == next.end() || walk.size() > next.size()) return false;
        v = it->second;
    } while (v != start);
    if (walk.size() != next.size()) return false;   // several loops
    if (loop) *loop = std::move(walk);
    return true;
}

Vec3 proxyCentroidOf(const MeshTopo& m, uint32_t f)
{
    return {m.proxyCentroid[f * 3], m.proxyCentroid[f * 3 + 1], m.proxyCentroid[f * 3 + 2]};
}

// Local index of the face closest to the region's (proxy) centroid.
size_t centralFace(const MeshTopo& m, const std::vector<uint32_t>& faces)
{
    Vec3 mean{0, 0, 0};
    for (uint32_t f : faces) {
        const Vec3 c = proxyCentroidOf(m, f);
        for (int k = 0; k < 3; ++k) mean[k] += c[k] / double(faces.size());
    }
    size_t root = 0;
    double bestD = 1e300;
    for (size_t i = 0; i < faces.size(); ++i) {
        const double d = norm(sub(proxyCentroidOf(m, faces[i]), mean));
        if (d < bestD) { bestD = d; root = i; }
    }
    return root;
}

// Edges crossed by the dual shortest-path tree rooted at the central face
// (ws.localOf must be marked).
std::vector<uint8_t> dualTree(const Workspace& ws, const std::vector<uint32_t>& faces)
{
    const MeshTopo& m = ws.m;
    const size_t root = centralFace(m, faces);
    std::vector<double> dist(faces.size(), 1e300);
    std::vector<int32_t> viaEdge(faces.size(), -1);
    using Item = std::pair<double, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> pq;
    dist[root] = 0;
    pq.push({0, uint32_t(root)});
    std::vector<uint8_t> inTree(m.edgeFaces.size(), 0);
    while (!pq.empty()) {
        const auto [d, i] = pq.top();
        pq.pop();
        if (d > dist[i]) continue;
        if (viaEdge[i] >= 0) inTree[viaEdge[i]] = 1;
        const uint32_t f = faces[i];
        for (int k = 0; k < 3; ++k) {
            if (!ws.interior(f, k)) continue;
            const uint32_t g = uint32_t(m.nbr[f * 3 + k]);
            const uint32_t j = uint32_t(ws.localOf[g]);
            const double nd = d + norm(sub(proxyCentroidOf(m, f), proxyCentroidOf(m, g)));
            if (nd >= dist[j]) continue;
            dist[j] = nd;
            viaEdge[j] = m.sideEdge[f * 3 + k];
            pq.push({nd, j});
        }
    }
    return inTree;
}

// Closed surface: pin the two ends of the longest cut path so pruning keeps a
// path between two far-apart vertices.
void anchorClosedSurface(const MeshTopo& m, const std::vector<int32_t>& cut,
                         std::unordered_map<uint32_t, bool>& anchor)
{
    const uint32_t a = m.edgeV[cut.front() * 2];
    uint32_t b = a;
    double far = -1;
    for (int32_t e : cut)
        for (int s = 0; s < 2; ++s) {
            const uint32_t v = m.edgeV[e * 2 + s];
            const double d = norm(sub(m.p(v), m.p(a)));
            if (d > far) { far = d; b = v; }
        }
    anchor[a] = anchor[b] = true;
}

// Remove dangling branches (degree-1 vertices that are not anchors).
std::vector<int32_t> pruneDangling(const MeshTopo& m, const std::vector<int32_t>& cut,
                                   const std::unordered_map<uint32_t, bool>& anchor)
{
    std::unordered_map<uint32_t, int> deg;
    std::unordered_map<uint32_t, std::vector<int32_t>> inc;
    for (int32_t e : cut)
        for (int s = 0; s < 2; ++s) {
            ++deg[m.edgeV[e * 2 + s]];
            inc[m.edgeV[e * 2 + s]].push_back(e);
        }
    std::vector<uint8_t> alive(m.edgeFaces.size(), 0);
    for (int32_t e : cut) alive[e] = 1;
    auto prunable = [&](uint32_t v) { return deg[v] == 1 && !anchor.count(v); };
    std::vector<uint32_t> q;
    for (const auto& kv : deg)
        if (prunable(kv.first)) q.push_back(kv.first);
    while (!q.empty()) {
        const uint32_t v = q.back();
        q.pop_back();
        if (!prunable(v)) continue;
        const auto& edges = inc[v];
        const auto it = std::find_if(edges.begin(), edges.end(),
                                     [&alive](int32_t e) { return alive[e] != 0; });
        if (it == edges.end()) continue;
        alive[*it] = 0;
        for (int s = 0; s < 2; ++s) {
            const uint32_t u = m.edgeV[*it * 2 + s];
            if (--deg[u] == 1 && !anchor.count(u)) q.push_back(u);
        }
    }
    std::vector<int32_t> kept;
    for (int32_t e : cut)
        if (alive[e]) kept.push_back(e);
    return kept;
}

// Tree-cotree cut graph: every interior edge the dual shortest-path tree does
// not cross is cut, then dangling branches are pruned back to the region
// boundary. The faces stay connected through the tree, so what remains opens
// every extra boundary loop and every handle.
std::vector<int32_t> cutGraph(Workspace& ws, const std::vector<uint32_t>& faces)
{
    const MeshTopo& m = ws.m;
    markLocal(ws, faces, true);
    const std::vector<uint8_t> inTree = dualTree(ws, faces);
    std::vector<int32_t> cut;
    std::unordered_map<uint32_t, bool> anchor;
    for (uint32_t f : faces)
        for (int k = 0; k < 3; ++k) {
            const int32_t e = m.sideEdge[f * 3 + k];
            if (!ws.interior(f, k)) {
                anchor[m.edgeV[e * 2]] = true;
                anchor[m.edgeV[e * 2 + 1]] = true;
            } else if (!inTree[e] && uint32_t(m.nbr[f * 3 + k]) > f) {
                cut.push_back(e);
            }
        }
    markLocal(ws, faces, false);
    if (cut.empty()) return cut;
    if (anchor.empty()) anchorClosedSurface(m, cut, anchor);
    return pruneDangling(m, cut, anchor);
}

// ---- sparse SPD solve (Jacobi-preconditioned CG) ---------------------------------
struct Csr {
    int n = 0;
    std::vector<int> rowPtr, col;
    std::vector<double> val;
    void build(int size, std::vector<std::tuple<int, int, double>>& trip)
    {
        n = size;
        std::sort(trip.begin(), trip.end());
        rowPtr.assign(n + 1, 0);
        col.clear();
        val.clear();
        for (size_t i = 0; i < trip.size();) {
            const auto [r, c, v0] = trip[i];
            double v = v0;
            size_t j = i + 1;
            while (j < trip.size() && std::get<0>(trip[j]) == r && std::get<1>(trip[j]) == c)
                v += std::get<2>(trip[j++]);
            col.push_back(c);
            val.push_back(v);
            ++rowPtr[r + 1];
            i = j;
        }
        for (int r = 0; r < n; ++r) rowPtr[r + 1] += rowPtr[r];
    }
    void mul(const std::vector<double>& x, std::vector<double>& y) const
    {
        y.assign(n, 0.0);
        for (int r = 0; r < n; ++r)
            for (int i = rowPtr[r]; i < rowPtr[r + 1]; ++i) y[r] += val[i] * x[col[i]];
    }
    double diag(int r) const
    {
        for (int i = rowPtr[r]; i < rowPtr[r + 1]; ++i)
            if (col[i] == r) return val[i];
        return 1.0;
    }
};

void solveCg(const Csr& A, const std::vector<double>& b, std::vector<double>& x, int maxIt)
{
    const int n = A.n;
    if (int(x.size()) != n) x.assign(n, 0.0);
    std::vector<double> r(n), z(n), p(n), Ap(n), dinv(n);
    for (int i = 0; i < n; ++i) dinv[i] = 1.0 / std::max(1e-30, A.diag(i));
    A.mul(x, Ap);
    double bn = 0;
    for (int i = 0; i < n; ++i) { r[i] = b[i] - Ap[i]; bn += b[i] * b[i]; }
    bn = std::sqrt(std::max(bn, 1e-300));
    for (int i = 0; i < n; ++i) { z[i] = dinv[i] * r[i]; p[i] = z[i]; }
    double rz = 0;
    for (int i = 0; i < n; ++i) rz += r[i] * z[i];
    for (int it = 0; it < maxIt; ++it) {
        A.mul(p, Ap);
        double pAp = 0;
        for (int i = 0; i < n; ++i) pAp += p[i] * Ap[i];
        if (!(pAp > 0)) break;
        const double alpha = rz / pAp;
        double rn = 0;
        for (int i = 0; i < n; ++i) {
            x[i] += alpha * p[i];
            r[i] -= alpha * Ap[i];
            rn += r[i] * r[i];
        }
        if (std::sqrt(rn) < 1e-9 * bn) break;
        double rz2 = 0;
        for (int i = 0; i < n; ++i) { z[i] = dinv[i] * r[i]; rz2 += r[i] * z[i]; }
        const double beta = rz2 / rz;
        rz = rz2;
        for (int i = 0; i < n; ++i) p[i] = z[i] + beta * p[i];
    }
}

// ---- flattening -----------------------------------------------------------------
// Isometric 2D frame of every chart triangle.
std::vector<std::array<double, 6>> localFrames(const MeshTopo& m, const Chart& c)
{
    std::vector<std::array<double, 6>> X(c.faces.size());
    for (size_t t = 0; t < c.faces.size(); ++t) {
        const Vec3 p0 = m.p(c.orig[c.tri[t * 3]]), p1 = m.p(c.orig[c.tri[t * 3 + 1]]),
                   p2 = m.p(c.orig[c.tri[t * 3 + 2]]);
        const Vec3 e1 = sub(p1, p0), e2 = sub(p2, p0);
        const double l = norm(e1);
        const Vec3 n = cross(e1, e2);
        const double ln = norm(n);
        if (!(l > 0) || !(ln > 0)) { X[t] = {0, 0, 0, 0, 0, 0}; continue; }
        const Vec3 ex{e1[0] / l, e1[1] / l, e1[2] / l};
        const Vec3 nn{n[0] / ln, n[1] / ln, n[2] / ln};
        const Vec3 ey = cross(nn, ex);
        X[t] = {0, 0, l, 0, dot(e2, ex), dot(e2, ey)};
    }
    return X;
}

// Tutte embedding: boundary on a circle (arc-length spaced), interior at the
// average of its neighbours. Always fold-free for a disc.
std::vector<double> tutte(const MeshTopo& m, const Chart& c, const std::vector<uint32_t>& loop)
{
    const size_t n = c.orig.size();
    std::vector<double> uv(n * 2, 0.0);
    std::vector<int> unknown(n, -1);
    std::vector<uint8_t> onBoundary(n, 0);
    double perim = 0;
    for (size_t i = 0; i < loop.size(); ++i)
        perim += norm(sub(m.p(c.orig[loop[(i + 1) % loop.size()]]), m.p(c.orig[loop[i]])));
    double area = 0;
    for (uint32_t f : c.faces) area += m.area[f];
    const double R = std::sqrt(std::max(area, 1e-30) / kPi);
    double s = 0;
    for (size_t i = 0; i < loop.size(); ++i) {
        const double a = perim > 0 ? 2 * kPi * s / perim : 2 * kPi * i / loop.size();
        uv[loop[i] * 2] = R * std::cos(a);
        uv[loop[i] * 2 + 1] = R * std::sin(a);
        onBoundary[loop[i]] = 1;
        s += norm(sub(m.p(c.orig[loop[(i + 1) % loop.size()]]), m.p(c.orig[loop[i]])));
    }
    int nu = 0;
    for (size_t v = 0; v < n; ++v)
        if (!onBoundary[v]) unknown[v] = nu++;
    if (nu == 0) return uv;
    std::unordered_map<uint64_t, char> edges;
    for (size_t t = 0; t < c.tri.size(); t += 3)
        for (int k = 0; k < 3; ++k) edges[ekey(c.tri[t + k], c.tri[t + (k + 1) % 3])] = 1;
    std::vector<std::tuple<int, int, double>> trip;
    std::vector<double> bx(nu, 0.0), by(nu, 0.0);
    for (const auto& kv : edges) {
        const uint32_t a = uint32_t(kv.first >> 32), b = uint32_t(kv.first & 0xffffffffu);
        for (int s2 = 0; s2 < 2; ++s2) {
            const uint32_t i = s2 ? b : a, j = s2 ? a : b;
            if (unknown[i] < 0) continue;
            trip.emplace_back(unknown[i], unknown[i], 1.0);
            if (unknown[j] >= 0) {
                trip.emplace_back(unknown[i], unknown[j], -1.0);
            } else {
                bx[unknown[i]] += uv[j * 2];
                by[unknown[i]] += uv[j * 2 + 1];
            }
        }
    }
    Csr A;
    A.build(nu, trip);
    std::vector<double> x, y;
    solveCg(A, bx, x, 4000);
    solveCg(A, by, y, 4000);
    for (size_t v = 0; v < n; ++v)
        if (unknown[v] >= 0) {
            uv[v * 2] = x[unknown[v]];
            uv[v * 2 + 1] = y[unknown[v]];
        }
    return uv;
}

using Frames = std::vector<std::array<double, 6>>;

// Edge (i, j) of triangle t in its isometric 2D frame.
inline void frameEdge(const Frames& X, size_t t, int i, int j, double& x, double& y)
{
    x = X[t][i * 2] - X[t][j * 2];
    y = X[t][i * 2 + 1] - X[t][j * 2 + 1];
}

// Half the (clamped positive) cotangent of the angle at corner k: the weight
// of the opposite edge (k+1, k+2).
std::vector<std::array<double, 3>> cotWeights(const Frames& X)
{
    std::vector<std::array<double, 3>> w(X.size());
    for (size_t t = 0; t < X.size(); ++t)
        for (int k = 0; k < 3; ++k) {
            double ax, ay, bx, by;
            frameEdge(X, t, (k + 1) % 3, k, ax, ay);
            frameEdge(X, t, (k + 2) % 3, k, bx, by);
            const double cr = std::fabs(ax * by - ay * bx);
            const double ct = cr > 1e-30 ? (ax * bx + ay * by) / cr : 0.0;
            w[t][k] = 0.5 * std::clamp(ct, 1e-3, 1e3);
        }
    return w;
}

// Cotangent Laplacian with vertex 0 pinned (removes the translation null space).
Csr arapMatrix(const Chart& c, const std::vector<std::array<double, 3>>& cotw)
{
    std::vector<std::tuple<int, int, double>> trip;
    for (size_t t = 0; t < cotw.size(); ++t)
        for (int k = 0; k < 3; ++k) {
            const int a = int(c.tri[t * 3 + (k + 1) % 3]), b = int(c.tri[t * 3 + (k + 2) % 3]);
            const double w = cotw[t][k];
            if (a != 0) trip.emplace_back(a - 1, a - 1, w);
            if (b != 0) trip.emplace_back(b - 1, b - 1, w);
            if (a == 0 || b == 0) continue;
            trip.emplace_back(a - 1, b - 1, -w);
            trip.emplace_back(b - 1, a - 1, -w);
        }
    Csr L;
    L.build(int(c.orig.size() - 1), trip);
    return L;
}

// Local step: the rotation closest to each triangle's current map.
std::vector<std::array<double, 4>> bestRotations(const Chart& c, const Frames& X,
                                                 const std::vector<std::array<double, 3>>& cotw,
                                                 const std::vector<double>& uv)
{
    std::vector<std::array<double, 4>> R(X.size());
    for (size_t t = 0; t < X.size(); ++t) {
        double S[4] = {0, 0, 0, 0};
        for (int k = 0; k < 3; ++k) {
            const int i = (k + 1) % 3, j = (k + 2) % 3;
            const uint32_t vi = c.tri[t * 3 + i], vj = c.tri[t * 3 + j];
            const double du0 = uv[vi * 2] - uv[vj * 2], du1 = uv[vi * 2 + 1] - uv[vj * 2 + 1];
            double dx0, dx1;
            frameEdge(X, t, i, j, dx0, dx1);
            const double w = cotw[t][k];
            S[0] += w * du0 * dx0; S[1] += w * du0 * dx1;
            S[2] += w * du1 * dx0; S[3] += w * du1 * dx1;
        }
        // polar decomposition of the 2x2 S, restricted to det > 0
        const double a = S[0] + S[3], b = S[2] - S[1];
        const double l = std::hypot(a, b);
        const double cs = l > 0 ? a / l : 1.0, sn = l > 0 ? b / l : 0.0;
        R[t] = {cs, -sn, sn, cs};
    }
    return R;
}

// Global step right-hand side (the pinned vertex 0 moves to the rhs).
void arapRhs(const Chart& c, const Frames& X, const std::vector<std::array<double, 3>>& cotw,
             const std::vector<std::array<double, 4>>& R, const std::vector<double>& uv,
             std::vector<double>& bx, std::vector<double>& by)
{
    std::fill(bx.begin(), bx.end(), 0.0);
    std::fill(by.begin(), by.end(), 0.0);
    auto add = [&](uint32_t v, double x, double y) {
        if (v == 0) return;
        bx[v - 1] += x;
        by[v - 1] += y;
    };
    for (size_t t = 0; t < X.size(); ++t)
        for (int k = 0; k < 3; ++k) {
            const int i = (k + 1) % 3, j = (k + 2) % 3;
            const uint32_t vi = c.tri[t * 3 + i], vj = c.tri[t * 3 + j];
            double dx0, dx1;
            frameEdge(X, t, i, j, dx0, dx1);
            const double w = cotw[t][k];
            const double r0 = w * (R[t][0] * dx0 + R[t][1] * dx1);
            const double r1 = w * (R[t][2] * dx0 + R[t][3] * dx1);
            add(vi, r0, r1);
            add(vj, -r0, -r1);
            if (vi == 0) add(vj, w * uv[0], w * uv[1]);
            if (vj == 0) add(vi, w * uv[0], w * uv[1]);
        }
}

// As-rigid-as-possible refinement (Liu et al. 2008, local/global).
void arap(const Chart& c, const Frames& X, std::vector<double>& uv, int iterations)
{
    const size_t n = c.orig.size();
    if (n < 3 || iterations <= 0) return;
    const auto cotw = cotWeights(X);
    const Csr L = arapMatrix(c, cotw);
    std::vector<double> x(n - 1), y(n - 1), bx(n - 1), by(n - 1);
    for (size_t v = 1; v < n; ++v) { x[v - 1] = uv[v * 2]; y[v - 1] = uv[v * 2 + 1]; }
    for (int it = 0; it < iterations; ++it) {
        const auto R = bestRotations(c, X, cotw, uv);
        arapRhs(c, X, cotw, R, uv, bx, by);
        solveCg(L, bx, x, 400);
        solveCg(L, by, y, 400);
        for (size_t v = 1; v < n; ++v) { uv[v * 2] = x[v - 1]; uv[v * 2 + 1] = y[v - 1]; }
    }
}

struct Quality { double flipFraction = 1, distortion = 1e9, overlap = 1; };

double signedUvArea(const Chart& c, const std::vector<double>& uv, size_t t)
{
    const uint32_t a = c.tri[t * 3], b = c.tri[t * 3 + 1], d = c.tri[t * 3 + 2];
    return 0.5 * ((uv[b * 2] - uv[a * 2]) * (uv[d * 2 + 1] - uv[a * 2 + 1])
                  - (uv[d * 2] - uv[a * 2]) * (uv[b * 2 + 1] - uv[a * 2 + 1]));
}

struct Raster {
    int n = 128;
    std::vector<uint8_t> count;
    explicit Raster(int size) : n(size), count(size_t(size) * size, 0) {}

    // Texel centres strictly inside the triangle (counter-clockwise).
    void add(const double px[3], const double py[3])
    {
        const int x0 = std::max(0, int(std::floor(std::min({px[0], px[1], px[2]}))));
        const int x1 = std::min(n - 1, int(std::ceil(std::max({px[0], px[1], px[2]}))));
        const int y0 = std::max(0, int(std::floor(std::min({py[0], py[1], py[2]}))));
        const int y1 = std::min(n - 1, int(std::ceil(std::max({py[0], py[1], py[2]}))));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                if (inside(px, py, x + 0.5, y + 0.5) && count[size_t(y) * n + x] < 255)
                    ++count[size_t(y) * n + x];
    }

    static bool inside(const double px[3], const double py[3], double qx, double qy)
    {
        for (int k = 0; k < 3; ++k) {
            const int j = (k + 1) % 3;
            if ((px[j] - px[k]) * (qy - py[k]) - (py[j] - py[k]) * (qx - px[k]) <= 0) return false;
        }
        return true;
    }
};

// Fraction of covered texels hit twice by non-folded triangles, at ~128 texels
// across the chart.
double overlapFraction(const Chart& c, const std::vector<double>& uv,
                       const std::vector<double>& sa)
{
    double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
    for (size_t v = 0; v < c.orig.size(); ++v)
        for (int k = 0; k < 2; ++k) {
            lo[k] = std::min(lo[k], uv[v * 2 + k]);
            hi[k] = std::max(hi[k], uv[v * 2 + k]);
        }
    const double ext = std::max(hi[0] - lo[0], hi[1] - lo[1]);
    if (!(ext > 0)) return 1.0;
    // ~8 texels per triangle edge across the chart (128..1024): a fixed 128
    // raster let narrow overlaps of large charts fall between texel centres
    Raster ras(std::clamp(int(8.0 * std::sqrt(double(c.faces.size()))), 128, 1024));
    const double sc = (ras.n - 1) / ext;
    for (size_t t = 0; t < c.faces.size(); ++t) {
        if (!(sa[t] > 0)) continue;
        double px[3], py[3];
        for (int k = 0; k < 3; ++k) {
            px[k] = (uv[c.tri[t * 3 + k] * 2] - lo[0]) * sc;
            py[k] = (uv[c.tri[t * 3 + k] * 2 + 1] - lo[1]) * sc;
        }
        ras.add(px, py);
    }
    size_t covered = 0, twice = 0;
    for (uint8_t v : ras.count) { covered += v > 0; twice += v > 1; }
    return covered ? double(twice) / double(covered) : 1.0;
}

Quality evaluate(const MeshTopo& m, const Chart& c, std::vector<double>& uv)
{
    Quality q;
    std::vector<double> sa(c.faces.size());
    double total = 0;
    for (size_t t = 0; t < c.faces.size(); ++t) { sa[t] = signedUvArea(c, uv, t); total += sa[t]; }
    if (total < 0) {   // mirror so the chart winds positive
        for (size_t v = 0; v < c.orig.size(); ++v) uv[v * 2 + 1] = -uv[v * 2 + 1];
        for (double& a : sa) a = -a;
        total = -total;
    }
    double a3 = 0, flipped = 0;
    for (size_t t = 0; t < c.faces.size(); ++t) {
        a3 += m.area[c.faces[t]];
        if (!(sa[t] > 0)) flipped += m.area[c.faces[t]];
    }
    if (!(a3 > 0) || !(total > 0)) return q;
    const double k = total / a3;
    double dist = 0, w = 0;
    for (size_t t = 0; t < c.faces.size(); ++t) {
        const double A = m.area[c.faces[t]];
        if (!(sa[t] > 0) || !(A > 0)) continue;
        dist += A * std::fabs(std::log(sa[t] / (k * A)));
        w += A;
    }
    q.flipFraction = flipped / a3;
    q.distortion = w > 0 ? dist / w : 1e9;
    q.overlap = overlapFraction(c, uv, sa);
    return q;
}

// ---- splitting --------------------------------------------------------------------
std::vector<std::vector<uint32_t>> faceComponents(Workspace& ws, const std::vector<uint32_t>& faces,
                                                  const std::vector<int>& lab)
{
    const MeshTopo& m = ws.m;
    markLocal(ws, faces, true);
    std::vector<int> comp(faces.size(), -1);
    std::vector<std::vector<uint32_t>> out;
    std::vector<uint32_t> stack;
    for (size_t s = 0; s < faces.size(); ++s) {
        if (comp[s] >= 0) continue;
        out.emplace_back();
        comp[s] = int(out.size() - 1);
        stack.assign(1, uint32_t(s));
        while (!stack.empty()) {
            const uint32_t i = stack.back();
            stack.pop_back();
            out.back().push_back(faces[i]);
            for (int k = 0; k < 3; ++k) {
                const int32_t g = m.nbr[faces[i] * 3 + k];
                if (g < 0 || ws.localOf[g] < 0) continue;
                const int32_t j = ws.localOf[g];
                if (comp[j] < 0 && lab[j] == lab[i]) {
                    comp[j] = comp[s];
                    stack.push_back(uint32_t(j));
                }
            }
        }
    }
    markLocal(ws, faces, false);
    return out;
}

std::vector<int> twoMeans(const MeshTopo& m, const std::vector<uint32_t>& faces)
{
    const size_t n = faces.size();
    std::vector<std::array<double, 6>> f(n);
    for (size_t i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) {
            f[i][k] = 4.0 * m.proxyCentroid[faces[i] * 3 + k] / m.diag;
            f[i][3 + k] = m.proxyNormal[faces[i] * 3 + k];
        }
    auto d2 = [](const std::array<double, 6>& a, const std::array<double, 6>& b) {
        double s = 0;
        for (int k = 0; k < 6; ++k) s += (a[k] - b[k]) * (a[k] - b[k]);
        return s;
    };
    std::array<double, 6> mean{};
    for (const auto& x : f)
        for (int k = 0; k < 6; ++k) mean[k] += x[k] / n;
    size_t a = 0, b = 0;
    for (size_t i = 0; i < n; ++i) if (d2(f[i], mean) > d2(f[a], mean)) a = i;
    for (size_t i = 0; i < n; ++i) if (d2(f[i], f[a]) > d2(f[b], f[a])) b = i;
    std::array<std::array<double, 6>, 2> c{f[a], f[b]};
    std::vector<int> lab(n, 0);
    for (int it = 0; it < 10; ++it) {
        std::array<std::array<double, 6>, 2> s{};
        double w[2] = {0, 0};
        for (size_t i = 0; i < n; ++i) {
            lab[i] = d2(f[i], c[1]) < d2(f[i], c[0]) ? 1 : 0;
            const double A = m.area[faces[i]];
            for (int k = 0; k < 6; ++k) s[lab[i]][k] += A * f[i][k];
            w[lab[i]] += A;
        }
        for (int q = 0; q < 2; ++q)
            if (w[q] > 0)
                for (int k = 0; k < 6; ++k) c[q][k] = s[q][k] / w[q];
    }
    return lab;
}

// Fallback split: halve the region at the area-weighted median along its
// longest principal axis (proxy centroids). Always yields two non-empty
// halves when the region has more than one face.
std::vector<int> medianSplit(const MeshTopo& m, const std::vector<uint32_t>& faces)
{
    const size_t n = faces.size();
    Vec3 mean{0, 0, 0};
    double wsum = 0;
    for (uint32_t f : faces) {
        const Vec3 c = proxyCentroidOf(m, f);
        for (int k = 0; k < 3; ++k) mean[k] += m.area[f] * c[k];
        wsum += m.area[f];
    }
    for (int k = 0; k < 3; ++k) mean[k] /= std::max(wsum, 1e-300);
    double C[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (uint32_t f : faces) {
        const Vec3 d = sub(proxyCentroidOf(m, f), mean);
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) C[a][b] += m.area[f] * d[a] * d[b];
    }
    Vec3 axis{1, 0.5, 0.25};   // power iteration for the dominant axis
    for (int it = 0; it < 50; ++it) {
        Vec3 nx{0, 0, 0};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) nx[a] += C[a][b] * axis[b];
        const double l = norm(nx);
        if (!(l > 0)) break;
        axis = {nx[0] / l, nx[1] / l, nx[2] / l};
    }
    std::vector<std::pair<double, size_t>> proj(n);
    for (size_t i = 0; i < n; ++i)
        proj[i] = {dot(sub(proxyCentroidOf(m, faces[i]), mean), axis), i};
    std::sort(proj.begin(), proj.end());
    std::vector<int> lab(n, 1);
    double acc = 0;
    for (size_t i = 0; i < n; ++i) {
        lab[proj[i].second] = 0;
        acc += m.area[faces[proj[i].second]];
        if (acc >= 0.5 * wsum && i + 1 < n) break;
    }
    return lab;
}

// Relabel fragments much smaller than their half into the surrounding label,
// so a split yields two pieces instead of a spray of slivers.
void absorbFragments(Workspace& ws, const std::vector<uint32_t>& faces, std::vector<int>& lab)
{
    const MeshTopo& m = ws.m;
    for (int pass = 0; pass < 3; ++pass) {
        const auto comps = faceComponents(ws, faces, lab);
        if (comps.size() <= 2) return;
        markLocal(ws, faces, true);
        bool changed = false;
        const double limit = 0.075 * double(faces.size());
        for (const auto& comp : comps) {
            if (double(comp.size()) >= limit) continue;
            const int own = lab[ws.localOf[comp.front()]];
            int votes[2] = {0, 0};
            for (uint32_t f : comp)
                for (int k = 0; k < 3; ++k) {
                    const int32_t g = m.nbr[f * 3 + k];
                    if (g >= 0 && ws.localOf[g] >= 0 && lab[ws.localOf[g]] != own)
                        ++votes[lab[ws.localOf[g]]];
                }
            if (votes[0] + votes[1] == 0) continue;
            const int nl = votes[1] > votes[0] ? 1 : 0;
            for (uint32_t f : comp) lab[ws.localOf[f]] = nl;
            changed = true;
        }
        markLocal(ws, faces, false);
        if (!changed) return;
    }
}

struct Unwrapper {
    Workspace ws;
    std::vector<Chart> charts;
    std::vector<uint32_t> leftover;
    bool cancelled = false;

    Unwrapper(const MeshTopo& m, const Options& o) : ws(m, o) {}

    int rejTopology = 0, rejFolds = 0, rejDistortion = 0, rejOverlap = 0;

    bool tryFlatten(Chart& c, const std::vector<uint32_t>& loop)
    {
        c.uv = tutte(ws.m, c, loop);
        arap(c, localFrames(ws.m, c), c.uv, ws.opts.arapIterations);
        if (!std::all_of(c.uv.begin(), c.uv.end(), [](double v) { return std::isfinite(v); })) {
            ++rejFolds;
            return false;
        }
        const Quality q = evaluate(ws.m, c, c.uv);
        if (q.flipFraction > ws.opts.maxFlipFraction) { ++rejFolds; return false; }
        if (q.distortion > ws.opts.maxAreaDistortion) { ++rejDistortion; return false; }
        if (q.overlap > ws.opts.maxOverlapFraction) { ++rejOverlap; return false; }
        return true;
    }

    // Cut `faces` to a disc and flatten it. On failure the cuts made here are
    // undone (a split's children cut themselves).
    bool flattenRegion(const std::vector<uint32_t>& faces, Chart& c)
    {
        c = buildChart(ws, faces);
        std::vector<uint32_t> loop;
        std::vector<int32_t> cuts;
        if (!isDisc(c, &loop)) {
            cuts = cutGraph(ws, faces);
            for (int32_t e : cuts) ws.seam[e] = 1;
            c = buildChart(ws, faces);
        }
        const bool disc = (cuts.empty() || isDisc(c, &loop)) && !loop.empty();
        if (!disc) ++rejTopology;
        if (disc && tryFlatten(c, loop)) return true;
        for (int32_t e : cuts) ws.seam[e] = 0;
        return false;
    }

    // Neighbouring charts of a leftover piece, most shared edges first.
    std::vector<int> neighbourCharts(const std::vector<uint32_t>& piece,
                                     const std::vector<int>& chartOf) const
    {
        std::unordered_map<int, int> shared;
        for (uint32_t f : piece)
            for (int k = 0; k < 3; ++k) {
                const int32_t g = ws.m.nbr[f * 3 + k];
                if (g >= 0 && chartOf[g] >= 0) ++shared[chartOf[g]];
            }
        std::vector<std::pair<int, int>> order(shared.begin(), shared.end());
        std::sort(order.begin(), order.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second || (a.second == b.second && a.first < b.first); });
        std::vector<int> out;
        for (const auto& kv : order) out.push_back(kv.first);
        return out;
    }

    // Attach each leftover piece (a fragment under minChartFaces, or a split
    // that failed) to the chart it borders most when the merged chart still
    // flattens within the limits; xatlas only gets what cannot be attached.
    void absorbLeftovers()
    {
        if (leftover.empty() || charts.empty()) return;
        std::vector<int> chartOf(ws.m.nF, -1);
        for (size_t i = 0; i < charts.size(); ++i)
            for (uint32_t f : charts[i].faces) chartOf[f] = int(i);
        const std::vector<int> same(leftover.size(), 0);
        std::vector<uint32_t> still;
        for (const auto& piece : faceComponents(ws, leftover, same)) {
            if (cancelled || (ws.opts.cancelled && ws.opts.cancelled())) { cancelled = true; return; }
            bool attached = false;
            const std::vector<int> cand = neighbourCharts(piece, chartOf);
            for (size_t k = 0; k < cand.size() && k < 2 && !attached; ++k) {
                std::vector<uint32_t> merged = charts[cand[k]].faces;
                merged.insert(merged.end(), piece.begin(), piece.end());
                Chart c;
                if (!flattenRegion(merged, c)) continue;
                for (uint32_t f : piece) chartOf[f] = cand[k];
                charts[cand[k]] = std::move(c);
                attached = true;
            }
            if (!attached) still.insert(still.end(), piece.begin(), piece.end());
        }
        leftover.swap(still);
    }

    void process(const std::vector<uint32_t>& faces, int depth)
    {
        if (cancelled || (ws.opts.cancelled && ws.opts.cancelled())) { cancelled = true; return; }
        if (int(faces.size()) < ws.opts.minChartFaces || depth > ws.opts.maxSplitDepth) {
            leftover.insert(leftover.end(), faces.begin(), faces.end());
            return;
        }
        Chart c;
        if (flattenRegion(faces, c)) {
            charts.push_back(std::move(c));
            return;
        }
        std::vector<int> lab = twoMeans(ws.m, faces);
        absorbFragments(ws, faces, lab);
        auto parts = faceComponents(ws, faces, lab);
        if (parts.size() < 2) {
            // 2-means found no split (or fragment absorption undid it): cut at
            // the median instead of handing the whole region to xatlas.
            lab = medianSplit(ws.m, faces);
            parts = faceComponents(ws, faces, lab);
        }
        if (parts.size() < 2) {
            leftover.insert(leftover.end(), faces.begin(), faces.end());
            return;
        }
        for (const auto& p : parts) process(p, depth + 1);
    }
};

std::vector<int> initialRegions(const MeshTopo& m, const std::vector<float>& rgb,
                                const std::vector<float>& extra, const Options& opts,
                                int& count)
{
    std::vector<int> lab(m.nF, 0);
    if (!rgb.empty() && opts.colourClusters > 1) {
        lab = kmeansLabels(faceFeatures(m, rgb, extra), m.area, opts.colourClusters);
        smoothLabels(m, lab, opts.labelSmoothPasses);
    }
    std::vector<int> comp = labelComponents(m, lab, count);
    if (!rgb.empty() && opts.colourClusters > 1)
        mergeSmallRegions(m, comp, count, opts.minRegionFraction * m.totalArea);
    // compact ids
    std::unordered_map<int, int> remap;
    for (int& c : comp) {
        auto it = remap.emplace(c, int(remap.size())).first;
        c = it->second;
    }
    count = int(remap.size());
    return comp;
}

void assemble(const MeshTopo& m, Unwrapper& u, Result& r)
{
    r.indices.assign(m.nF * 3, 0);
    for (const Chart& c : u.charts) {
        double a3 = 0, auv = 0;
        for (size_t t = 0; t < c.faces.size(); ++t) {
            a3 += m.area[c.faces[t]];
            auv += std::fabs(signedUvArea(c, c.uv, t));
        }
        const double s = auv > 0 ? std::sqrt(a3 / auv) : 1.0;
        const uint32_t base = uint32_t(r.xref.size());
        for (size_t v = 0; v < c.orig.size(); ++v) {
            r.xref.push_back(c.orig[v]);
            r.uvs.push_back(float(c.uv[v * 2] * s));
            r.uvs.push_back(float(c.uv[v * 2 + 1] * s));
        }
        for (size_t t = 0; t < c.faces.size(); ++t)
            for (int k = 0; k < 3; ++k) r.indices[c.faces[t] * 3 + k] = base + c.tri[t * 3 + k];
    }
    std::unordered_map<uint32_t, uint32_t> shared;   // leftover faces share by input vertex
    for (uint32_t f : u.leftover)
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = m.fv(f, k);
            auto it = shared.find(v);
            if (it == shared.end()) {
                it = shared.emplace(v, uint32_t(r.xref.size())).first;
                r.xref.push_back(v);
                r.uvs.push_back(0.0f);
                r.uvs.push_back(0.0f);
            }
            r.indices[f * 3 + k] = it->second;
        }
    r.charts = int(u.charts.size());
    r.facesToXatlas = int(u.leftover.size());
    r.rejectedTopology = u.rejTopology;
    r.rejectedFolds = u.rejFolds;
    r.rejectedDistortion = u.rejDistortion;
    r.rejectedOverlap = u.rejOverlap;
}

}  // namespace

void srgbToLab(const float rgb[3], float lab[3])
{
    double lin[3];
    for (int k = 0; k < 3; ++k) {
        const double c = std::clamp(double(rgb[k]), 0.0, 1.0);
        lin[k] = c > 0.04045 ? std::pow((c + 0.055) / 1.055, 2.4) : c / 12.92;
    }
    const double X = (0.4124 * lin[0] + 0.3576 * lin[1] + 0.1805 * lin[2]) / 0.95047;
    const double Y = 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2];
    const double Z = (0.0193 * lin[0] + 0.1192 * lin[1] + 0.9505 * lin[2]) / 1.08883;
    auto fn = [](double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116.0; };
    const double fx = fn(X), fy = fn(Y), fz = fn(Z);
    lab[0] = float(116.0 * fy - 16.0);
    lab[1] = float(500.0 * (fx - fy));
    lab[2] = float(200.0 * (fy - fz));
}

std::vector<float> faceCentroids(const std::vector<float>& positions,
                                 const std::vector<uint32_t>& indices)
{
    std::vector<float> c(indices.size());
    for (size_t t = 0; t + 2 < indices.size(); t += 3)
        for (int k = 0; k < 3; ++k)
            c[t + k] = (positions[indices[t] * 3 + k] + positions[indices[t + 1] * 3 + k]
                        + positions[indices[t + 2] * 3 + k]) / 3.0f;
    return c;
}

Result unwrap(const std::vector<float>& positions,
              const std::vector<uint32_t>& indices,
              const std::vector<float>& faceRgb,
              const std::vector<float>& faceExtra,
              const Options& opts)
{
    Result r;
    MeshTopo m;
    m.nF = indices.size() / 3;
    m.nV = positions.size() / 3;
    m.pos = &positions;
    m.idx = &indices;
    if (m.nF == 0 || indices.size() % 3 != 0) {
        r.error = QStringLiteral("part unwrap: empty mesh.");
        return r;
    }
    if (m.nF > opts.maxFaces) {
        r.error = QStringLiteral("part unwrap: %1 triangles is above the %2 limit.")
                      .arg(m.nF).arg(opts.maxFaces);
        return r;
    }
    for (uint32_t i : indices)
        if (i >= m.nV) {
            r.error = QStringLiteral("part unwrap: index out of range.");
            return r;
        }
    const bool haveRgb = faceRgb.size() == m.nF * 3;
    const bool haveExtra = haveRgb && faceExtra.size() == m.nF;
    buildEdges(m);
    buildAreas(m);
    if (!buildProxy(m, std::max(0, opts.proxySmoothPasses), opts.cancelled)) {
        r.cancelled = true;
        r.error = QStringLiteral("cancelled");
        return r;
    }

    int regionCount = 0;
    const std::vector<int> region = initialRegions(
        m, haveRgb ? faceRgb : std::vector<float>{},
        haveExtra ? faceExtra : std::vector<float>{}, opts, regionCount);
    r.colourRegions = regionCount;
    std::vector<std::vector<uint32_t>> regions(regionCount);
    for (size_t f = 0; f < m.nF; ++f) regions[region[f]].push_back(uint32_t(f));

    Unwrapper u(m, opts);
    for (const auto& faces : regions) {
        // a colour region may still be several face-connected pieces
        const std::vector<int> same(faces.size(), 0);
        for (const auto& piece : faceComponents(u.ws, faces, same)) u.process(piece, 0);
        if (u.cancelled) break;
    }
    if (!u.cancelled) u.absorbLeftovers();
    if (u.cancelled) {
        r = Result{};
        r.cancelled = true;
        r.error = QStringLiteral("cancelled");
        return r;
    }
    assemble(m, u, r);
    r.colourRegions = regionCount;
    r.ok = true;
    return r;
}

namespace {

// Part islands for buildAtlas. Returns false only when cancelled; on any other
// failure `out.partFallbackReason` says why and the caller uses plain xatlas.
bool partIslandCharts(const std::vector<float>& positions, const std::vector<uint32_t>& indices,
                      const std::vector<float>& faceRgb, const std::vector<float>& faceExtra,
                      const AtlasOptions& opts, Atlas& out, std::vector<uint32_t>& xref,
                      std::vector<uint32_t>& idx, std::vector<float>& uv)
{
    Options po = opts.parts;
    const std::function<bool()> partCancel = opts.parts.cancelled;
    const std::function<bool()> atlasCancel = opts.cancelled;
    po.cancelled = [partCancel, atlasCancel]() {
        return (partCancel && partCancel()) || (atlasCancel && atlasCancel());
    };
    Result pr = unwrap(positions, indices, faceRgb, faceExtra, po);
    if (pr.cancelled) {
        out.cancelled = true;
        out.error = QStringLiteral("cancelled");
        return false;
    }
    if (!pr.ok || pr.charts == 0) {
        out.partFallbackReason = pr.ok ? QStringLiteral("no island passed the quality limits")
                                       : pr.error;
        return true;
    }
    xref = std::move(pr.xref);
    idx = std::move(pr.indices);
    uv = std::move(pr.uvs);
    out.usedPartIslands = true;
    out.partCharts = pr.charts;
    out.partFacesToXatlas = pr.facesToXatlas;
    return true;
}

}  // namespace

Atlas buildAtlas(const std::vector<float>& positions,
                 const std::vector<uint32_t>& indices,
                 const std::vector<float>& faceRgb,
                 const std::vector<float>& faceExtra,
                 const AtlasOptions& opts)
{
    Atlas out;
    const size_t nv = positions.size() / 3;
    if (nv == 0 || indices.size() < 3 || indices.size() % 3 != 0) {
        out.error = QStringLiteral("unwrap: empty mesh.");
        return out;
    }
    // ---- 1. charts: part islands, or the mesh as-is for xatlas -------------
    std::vector<uint32_t> declXref, declIdx;
    std::vector<float> declUv;
    if (opts.partIslands && !partIslandCharts(positions, indices, faceRgb, faceExtra, opts, out,
                                              declXref, declIdx, declUv))
        return out;   // cancelled
    if (!out.usedPartIslands) {
        declXref.resize(nv);
        std::iota(declXref.begin(), declXref.end(), 0u);
        declIdx = indices;
    }
    // ---- 2. scaled copy + xatlas ---------------------------------------------
    float lo[3] = {positions[0], positions[1], positions[2]};
    float hi[3] = {lo[0], lo[1], lo[2]};
    for (size_t v = 0; v < nv; ++v)
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], positions[3 * v + a]);
            hi[a] = std::max(hi[a], positions[3 * v + a]);
        }
    const float diag = std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1])
                                 + (hi[2] - lo[2]) * (hi[2] - lo[2]));
    const float scale = diag > 0.0f ? 1000.0f / diag : 1.0f;
    std::vector<float> declPos(declXref.size() * 3);
    for (size_t v = 0; v < declXref.size(); ++v)
        for (int k = 0; k < 3; ++k) declPos[v * 3 + k] = positions[declXref[v] * 3 + k] * scale;
    for (float& u : declUv) u *= scale;

    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::MeshDecl decl;
    decl.vertexCount = uint32_t(declXref.size());
    decl.vertexPositionData = declPos.data();
    decl.vertexPositionStride = sizeof(float) * 3;
    if (out.usedPartIslands) {
        decl.vertexUvData = declUv.data();
        decl.vertexUvStride = sizeof(float) * 2;
    }
    decl.indexCount = uint32_t(declIdx.size());
    decl.indexData = declIdx.data();
    decl.indexFormat = xatlas::IndexFormat::UInt32;
    const auto err = xatlas::AddMesh(atlas, decl);
    if (err != xatlas::AddMeshError::Success) {
        out.error = QStringLiteral("xatlas::AddMesh failed: %1")
                        .arg(QString::fromLatin1(xatlas::StringForEnum(err)));
        xatlas::Destroy(atlas);
        return out;
    }
    // ComputeCharts can run for minutes on dense meshes: xatlas's progress
    // callback returning false aborts it. xatlas reports per percent of each
    // stage, so polling every notification is cheap.
    struct CancelCtx { const std::function<bool()>* cancelled; int counter; bool hit; } cc{
        &opts.cancelled, 0, false};
    if (opts.cancelled) {
        xatlas::SetProgressCallback(
            atlas,
            [](xatlas::ProgressCategory, int, void* user) -> bool {
                auto* c = static_cast<CancelCtx*>(user);
                ++c->counter;
                if ((*c->cancelled)()) { c->hit = true; return false; }
                return true;
            },
            &cc);
    }
    xatlas::ChartOptions chart;
    chart.useInputMeshUvs = out.usedPartIslands;
    xatlas::PackOptions pack;
    pack.resolution = uint32_t(std::clamp(opts.textureSize, 64, 8192));
    pack.padding = uint32_t(std::max(1, opts.padding));
    pack.bilinear = true;
    xatlas::Generate(atlas, chart, pack);
    if (cc.hit || (opts.cancelled && opts.cancelled())) {
        xatlas::Destroy(atlas);
        out = Atlas{};
        out.cancelled = true;
        out.error = QStringLiteral("cancelled");
        return out;
    }
    if (atlas->meshCount != 1 || atlas->width == 0 || atlas->height == 0) {
        out.error = QStringLiteral("xatlas produced no atlas.");
        xatlas::Destroy(atlas);
        return out;
    }
    const xatlas::Mesh& xm = atlas->meshes[0];
    out.width = int(atlas->width);
    out.height = int(atlas->height);
    out.chartCount = int(atlas->chartCount);
    out.xref.resize(xm.vertexCount);
    out.uv.resize(size_t(xm.vertexCount) * 2);
    for (uint32_t v = 0; v < xm.vertexCount; ++v) {
        out.xref[v] = declXref[xm.vertexArray[v].xref];
        out.uv[v * 2] = xm.vertexArray[v].uv[0];
        out.uv[v * 2 + 1] = xm.vertexArray[v].uv[1];
    }
    out.indices.assign(xm.indexArray, xm.indexArray + xm.indexCount);
    xatlas::Destroy(atlas);
    out.ok = true;
    return out;
}

}  // namespace PartUnwrap
