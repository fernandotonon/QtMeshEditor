#include "FaceRigger.h"

#include <QtGlobal>
#include <cstdio>

#include "ArkitTemplate.h"
#include "DeformationTransfer.h"
#include "NonRigidICP.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>

namespace FaceRig {

namespace {

// A uniform spatial-hash grid over a point set for nearest-point queries. The
// resample maps every user vertex to its nearest CORRESPONDENCE point (the
// fitted template verts X, template topology). Same map for all 52 shapes, so
// we build it once. Brute force would be Nu*Nt (~350M on a 27k template);
// the grid keeps it near-linear. Dependency-free.
class PointGrid {
public:
    void build(const std::vector<float>& pts)
    {
        m_pts = &pts;
        const int n = int(pts.size() / 3);
        if (n == 0) return;
        for (int a = 0; a < 3; ++a) { m_lo[a] = 1e30f; m_hi[a] = -1e30f; }
        for (int i = 0; i < n; ++i)
            for (int a = 0; a < 3; ++a) {
                m_lo[a] = std::min(m_lo[a], pts[size_t(i)*3+a]);
                m_hi[a] = std::max(m_hi[a], pts[size_t(i)*3+a]);
            }
        // aim ~1 point per cell on average
        double vol = 1.0;
        for (int a = 0; a < 3; ++a) vol *= std::max(1e-6, double(m_hi[a]-m_lo[a]));
        m_cell = float(std::cbrt(vol / std::max(1, n)));
        if (m_cell <= 1e-9f) m_cell = 1.0f;
        for (int i = 0; i < n; ++i)
            m_cells[key(cellOf(&pts[size_t(i)*3]))].push_back(i);
    }

    // nearest point index to q (3 floats), searching an expanding shell of
    // cells until the nearest is provably found. Returns -1 if empty.
    int nearest(const float* q) const
    {
        if (!m_pts || m_cells.empty()) return -1;
        // Clamp the SEARCH ORIGIN into the populated bounds: for a query far
        // outside the grid, the shell cap below (grid span) could otherwise
        // terminate before any populated cell is reached and silently return
        // -1 (dropping that vertex's transferred delta). Distances are still
        // measured to the real q, so the nearest result is unchanged.
        float qc[3];
        for (int a = 0; a < 3; ++a)
            qc[a] = std::min(std::max(q[a], m_lo[a]), m_hi[a]);
        const std::array<int,3> c = cellOf(qc);
        // absolute cap on the shell radius = span of the grid in cells + 1,
        // guarantees termination even if q is far outside the populated region.
        int spanCells = 1;
        for (int a = 0; a < 3; ++a)
            spanCells = std::max(spanCells,
                                 int(std::ceil((m_hi[a]-m_lo[a]) / m_cell)) + 1);
        const int rMax = spanCells + 1;

        int best = -1;
        double bestD = std::numeric_limits<double>::max();
        for (int r = 0; r <= rMax; ++r) {
            for (int dx = -r; dx <= r; ++dx)
              for (int dy = -r; dy <= r; ++dy)
                for (int dz = -r; dz <= r; ++dz) {
                    // only the shell at Chebyshev radius r (interior scanned)
                    if (std::max({std::abs(dx),std::abs(dy),std::abs(dz)}) != r) continue;
                    auto it = m_cells.find(key({c[0]+dx, c[1]+dy, c[2]+dz}));
                    if (it == m_cells.end()) continue;
                    for (int idx : it->second) {
                        const float* p = &(*m_pts)[size_t(idx)*3];
                        const double d = (double(p[0]-q[0])*(p[0]-q[0]) +
                                          double(p[1]-q[1])*(p[1]-q[1]) +
                                          double(p[2]-q[2])*(p[2]-q[2]));
                        if (d < bestD) { bestD = d; best = idx; }
                    }
                }
            // A point in shell r is at least (r-1)*cell away from q; once the
            // best found is closer than the guaranteed reach of the NEXT shell,
            // no farther shell can beat it. Scan one extra shell to be safe.
            if (best >= 0) {
                const double guaranteed = double(r) * m_cell;   // min dist of shell r+1
                if (guaranteed * guaranteed >= bestD) return best;
            }
        }
        return best;
    }

    /// The k nearest point indices to q, nearest first. The sampler needs
    /// several candidates because the SINGLE nearest vertex can sit on the
    /// wrong surface where two surfaces nearly touch — see
    /// SurfaceSampler::sample.
    void nearestK(const float* q, int k, std::vector<int>& out) const
    {
        out.clear();
        if (!m_pts || m_cells.empty() || k <= 0) return;
        float qc[3];
        for (int a = 0; a < 3; ++a)
            qc[a] = std::min(std::max(q[a], m_lo[a]), m_hi[a]);
        const std::array<int,3> c = cellOf(qc);
        int spanCells = 1;
        for (int a = 0; a < 3; ++a)
            spanCells = std::max(spanCells,
                                 int(std::ceil((m_hi[a]-m_lo[a]) / m_cell)) + 1);
        const int rMax = spanCells + 1;

        std::vector<std::pair<double,int>> best;   // (dist², idx), worst last
        best.reserve(size_t(k) + 1);
        for (int r = 0; r <= rMax; ++r) {
            for (int dx = -r; dx <= r; ++dx)
              for (int dy = -r; dy <= r; ++dy)
                for (int dz = -r; dz <= r; ++dz) {
                    if (std::max({std::abs(dx),std::abs(dy),std::abs(dz)}) != r) continue;
                    auto it = m_cells.find(key({c[0]+dx, c[1]+dy, c[2]+dz}));
                    if (it == m_cells.end()) continue;
                    for (int idx : it->second) {
                        const float* p = &(*m_pts)[size_t(idx)*3];
                        const double d = (double(p[0]-q[0])*(p[0]-q[0]) +
                                          double(p[1]-q[1])*(p[1]-q[1]) +
                                          double(p[2]-q[2])*(p[2]-q[2]));
                        if (int(best.size()) == k && d >= best.back().first) continue;
                        auto pos = std::lower_bound(
                            best.begin(), best.end(), d,
                            [](const std::pair<double,int>& e, double v){ return e.first < v; });
                        best.insert(pos, {d, idx});
                        if (int(best.size()) > k) best.pop_back();
                    }
                }
            if (int(best.size()) == k) {
                const double guaranteed = double(r) * m_cell;
                if (guaranteed * guaranteed >= best.back().first) break;
            }
        }
        out.reserve(best.size());
        for (const auto& e : best) out.push_back(e.second);
    }

private:
    std::array<int,3> cellOf(const float* p) const
    {
        return {int(std::floor((p[0]-m_lo[0]) / m_cell)),
                int(std::floor((p[1]-m_lo[1]) / m_cell)),
                int(std::floor((p[2]-m_lo[2]) / m_cell))};
    }
    static long long key(const std::array<int,3>& c)
    {
        // pack 3 ints into one 64-bit key (21 bits each, offset to positive)
        const long long x = (c[0] + (1<<20)) & 0x1FFFFF;
        const long long y = (c[1] + (1<<20)) & 0x1FFFFF;
        const long long z = (c[2] + (1<<20)) & 0x1FFFFF;
        return (x << 42) | (y << 21) | z;
    }

    const std::vector<float>* m_pts = nullptr;
    float m_lo[3] = {0,0,0}, m_hi[3] = {0,0,0};
    float m_cell = 1.0f;
    std::unordered_map<long long, std::vector<int>> m_cells;
};

// Barycentric resample over the correspondence SURFACE (fitted verts +
// template topology), replacing a nearest-VERTEX pick.
//
// Why (measured 2026-09-18 on the ICT template): a facial mesh has creases
// where spatially ADJACENT vertices carry completely different motion — at
// the lip seam upper and lower lip touch in the neutral pose, but under
// jawOpen one moves ~1.6 units and the other EXACTLY 0. A single
// nearest-vertex lookup there is a coin flip: offsetting the query by half
// a median edge (what a different mesh's vertex placement does) changed the
// sampled motion drastically for 3.2% of vertices near a seam, so a slice
// of lips and eyelids inherited the opposite side's motion or none at all.
// Scattered enough to read as "mushy expressions" rather than a bug, and
// invisible to the fit-residual gate (which measures the NEUTRAL match).
//
// Projecting onto the nearest TRIANGLE and blending its three corner deltas
// by barycentric weight keeps the interpolation ON the surface: a query on
// the upper lip lands on an upper-lip triangle and can only mix upper-lip
// corners — it cannot jump the crease, because no triangle joins the lips.
class SurfaceSampler {
public:
    void build(const std::vector<float>& pts, const std::vector<int>& faces)
    {
        m_pts = &pts;
        m_faces = &faces;
        m_grid.build(pts);
        m_vertN = vertexNormals(pts, faces);
        // vertex → incident triangles: a nearest-vertex hit yields the few
        // triangles worth testing (testing the whole surface is hopeless).
        const int nv = int(pts.size() / 3);
        m_vertFaces.assign(size_t(std::max(0, nv)), {});
        for (size_t f = 0; f + 2 < faces.size(); f += 3)
            for (int k = 0; k < 3; ++k) {
                const int v = faces[f + size_t(k)];
                if (v >= 0 && v < nv) m_vertFaces[size_t(v)].push_back(int(f / 3));
            }
    }

    struct Hit { float w[3] = {0, 0, 0}; int vi[3] = {-1, -1, -1}; };

    /// +1 when the two meshes' normals broadly agree, -1 when the query mesh
    /// is wound the OPPOSITE way and its normals are therefore globally
    /// negated. Review finding (Codex P2): `buildFaceRig`'s contract does not
    /// require template-matching winding, and with a reversed user mesh the
    /// raw sign test picks the surface facing the other way — measured on the
    /// real template, seeding flips from the correct main vertex 5941 to the
    /// mouth-interior island vertex 18154, silently recreating the exact
    /// frozen-vertex bug this seeding is meant to prevent.
    ///
    /// Decided by VOTE over a sample of query vertices rather than by any one
    /// pair, so local disagreement at a seam cannot flip the global answer.
    static float orientationSign(const std::vector<float>& queryPts,
                                 const std::vector<float>& queryN,
                                 const SurfaceSampler& corr)
    {
        const int nq = int(queryN.size() / 3);
        if (nq <= 0 || corr.m_vertN.empty()) return 1.0f;
        const int step = std::max(1, nq / 512);      // ~512 samples, cheap
        double agree = 0.0, disagree = 0.0;
        for (int i = 0; i < nq; i += step) {
            if (size_t(i)*3+2 >= queryPts.size()) break;
            const int nv = corr.m_grid.nearest(&queryPts[size_t(i)*3]);
            if (nv < 0 || size_t(nv)*3+2 >= corr.m_vertN.size()) continue;
            const double d = double(queryN[size_t(i)*3])   * corr.m_vertN[size_t(nv)*3]
                           + double(queryN[size_t(i)*3+1]) * corr.m_vertN[size_t(nv)*3+1]
                           + double(queryN[size_t(i)*3+2]) * corr.m_vertN[size_t(nv)*3+2];
            if (d > 0.1) agree += d;
            else if (d < -0.1) disagree += -d;
        }
        return (disagree > agree) ? -1.0f : 1.0f;
    }

    /// Area-weighted per-vertex normals. Public so the caller can compute the
    /// QUERY mesh's normals with the identical convention.
    static std::vector<float> vertexNormals(const std::vector<float>& pts,
                                            const std::vector<int>& faces)
    {
        std::vector<float> n(pts.size(), 0.0f);
        const int nv = int(pts.size() / 3);
        for (size_t f = 0; f + 2 < faces.size(); f += 3) {
            const int a = faces[f], b = faces[f+1], c = faces[f+2];
            if (a < 0 || b < 0 || c < 0 || a >= nv || b >= nv || c >= nv) continue;
            float e1[3], e2[3], cr[3];
            for (int d = 0; d < 3; ++d) {
                e1[d] = pts[size_t(b)*3+size_t(d)] - pts[size_t(a)*3+size_t(d)];
                e2[d] = pts[size_t(c)*3+size_t(d)] - pts[size_t(a)*3+size_t(d)];
            }
            cr[0] = e1[1]*e2[2] - e1[2]*e2[1];
            cr[1] = e1[2]*e2[0] - e1[0]*e2[2];
            cr[2] = e1[0]*e2[1] - e1[1]*e2[0];
            for (const int v : {a, b, c})
                for (int d = 0; d < 3; ++d) n[size_t(v)*3+size_t(d)] += cr[d];
        }
        for (int v = 0; v < nv; ++v) {
            float L = 0;
            for (int d = 0; d < 3; ++d)
                L += n[size_t(v)*3+size_t(d)] * n[size_t(v)*3+size_t(d)];
            L = std::sqrt(L);
            if (L > 1e-20f)
                for (int d = 0; d < 3; ++d) n[size_t(v)*3+size_t(d)] /= L;
        }
        return n;
    }

    // Closest point on the surface to q. Falls back to the nearest VERTEX
    // (weight 1) when it has no incident triangles, so an isolated
    // correspondence point still contributes instead of dropping out.
    /// `qn` (optional, 3 floats) is the query's own surface normal. When
    /// supplied, the SEED vertex is the nearest one whose normal agrees with
    /// it, instead of the nearest one outright.
    ///
    /// The seed decides which surface is searched: only triangles incident to
    /// it are ever considered. Where two surfaces nearly touch that makes the
    /// nearest-vertex pick load-bearing, and at the lip centre it is wrong —
    /// the mouth-interior island passes ~0.14 from the outer lip, so three lip
    /// vertices seeded on the island, and EVERY triangle incident to an island
    /// vertex is an island triangle (measured: 7, 5 and 7 of 7, 5 and 7). The
    /// correct lip triangle was never a candidate, so those vertices inherited
    /// the island's zero motion and froze while every neighbour moved ~4.0 —
    /// the spiky lip line (#1061).
    ///
    /// That is also why filtering the candidates cannot help, and three
    /// attempts confirmed it: widening the seed set to the 8 nearest vertices
    /// made it WORSE (8 frozen became 19) because it added more island
    /// triangles; restricting to the seed's connected component did nothing
    /// because the seed is itself on the island; and rejecting opposed
    /// triangles did nothing because it rejected them ALL and the fallback
    /// restored the island match. The answer was never in the set — so fix
    /// the SEED, not the filter.
    ///
    /// Orientation separates the surfaces decisively. Measured at the three
    /// failing vertices, over the 8 nearest correspondence vertices:
    ///
    ///     nearest overall      v18154  d=0.144  island  dot = -0.974
    ///     nearest AGREEING     v5941   d=0.118  main    dot = +0.993
    ///
    /// The agreeing vertex is both closer AND on the right surface; the
    /// island scores about -0.9 because the two surfaces face opposite ways,
    /// which is exactly what makes them distinct surfaces. If no candidate
    /// agrees, the nearest is used unchanged, so this can never lose a
    /// correspondence.
    Hit sample(const float* q, const float* qn = nullptr, float nsign = 1.0f) const
    {
        Hit h;
        int nv = -1;
        if (qn) {
            // Widening the candidate list is safe here (unlike widening the
            // TRIANGLE set, which made things worse): the first AGREEING
            // vertex still wins, so extra candidates only matter when the
            // near ones all disagree. Exhaust the surface rather than give up
            // at a fixed count — falling back to the unfiltered nearest is
            // what reintroduces the wrong-surface match this exists to stop.
            const int total = int(m_vertN.size() / 3);
            // Grow geometrically, but CLAMP the last request to `total` so the
            // whole surface really is covered. Letting `want` quadruple past
            // the end and exiting on `want <= total` silently stops early:
            // with 26,719 vertices the requests are 8, 32, … 8192 and the next
            // is 32768, so 18,527 vertices are never inspected and the
            // unfiltered fallback can still pick the wrong surface (CodeRabbit
            // finding — my previous claim that this was exhaustive was wrong).
            int want = std::min(kSeedCandidates, total);
            while (nv < 0 && want > 0) {
                m_grid.nearestK(q, want, m_seeds);
                if (m_seeds.empty()) break;
                for (const int cand : m_seeds) {
                    if (cand < 0 || size_t(cand)*3+2 >= m_vertN.size()) continue;
                    const float d = nsign * (qn[0]*m_vertN[size_t(cand)*3]
                                           + qn[1]*m_vertN[size_t(cand)*3+1]
                                           + qn[2]*m_vertN[size_t(cand)*3+2]);
                    if (d > 0.0f) { nv = cand; break; }   // nearest that agrees
                }
                if (int(m_seeds.size()) < want) break;    // surface exhausted
                if (want >= total) break;                 // everything searched
                want = std::min(want * 4, total);
            }
        }
        if (nv < 0) nv = m_grid.nearest(q);
        if (nv < 0) return h;
        h.vi[0] = nv; h.w[0] = 1.0f;
        if (size_t(nv) >= m_vertFaces.size()) return h;

        float best = std::numeric_limits<float>::max();
        for (const int t : m_vertFaces[size_t(nv)]) {
            const int a = (*m_faces)[size_t(t)*3];
            const int b = (*m_faces)[size_t(t)*3+1];
            const int c = (*m_faces)[size_t(t)*3+2];
            float w[3];
            const float d2 = closestOnTri(q, a, b, c, w);
            if (d2 < best) {
                best = d2;
                h.vi[0] = a; h.vi[1] = b; h.vi[2] = c;
                h.w[0] = w[0]; h.w[1] = w[1]; h.w[2] = w[2];
            }
        }
        return h;
    }

private:
    // Squared distance q→triangle, writing the closest point's barycentric
    // weights. Ericson, Real-Time Collision Detection: the standard region
    // test, clamped so an off-triangle query lands on the nearest edge or
    // corner rather than extrapolating past it.
    float closestOnTri(const float* q, int a, int b, int c, float* w) const
    {
        const float* A = &(*m_pts)[size_t(a)*3];
        const float* B = &(*m_pts)[size_t(b)*3];
        const float* C = &(*m_pts)[size_t(c)*3];
        float ab[3], ac[3], ap[3];
        for (int i = 0; i < 3; ++i) { ab[i]=B[i]-A[i]; ac[i]=C[i]-A[i]; ap[i]=q[i]-A[i]; }
        const float d1 = ab[0]*ap[0]+ab[1]*ap[1]+ab[2]*ap[2];
        const float d2 = ac[0]*ap[0]+ac[1]*ap[1]+ac[2]*ap[2];
        if (d1 <= 0 && d2 <= 0) { w[0]=1; w[1]=0; w[2]=0; return dist2(q, A); }
        float bp[3]; for (int i=0;i<3;++i) bp[i]=q[i]-B[i];
        const float d3 = ab[0]*bp[0]+ab[1]*bp[1]+ab[2]*bp[2];
        const float d4 = ac[0]*bp[0]+ac[1]*bp[1]+ac[2]*bp[2];
        if (d3 >= 0 && d4 <= d3) { w[0]=0; w[1]=1; w[2]=0; return dist2(q, B); }
        float cp[3]; for (int i=0;i<3;++i) cp[i]=q[i]-C[i];
        const float d5 = ab[0]*cp[0]+ab[1]*cp[1]+ab[2]*cp[2];
        const float d6 = ac[0]*cp[0]+ac[1]*cp[1]+ac[2]*cp[2];
        if (d6 >= 0 && d5 <= d6) { w[0]=0; w[1]=0; w[2]=1; return dist2(q, C); }
        const float vc = d1*d4 - d3*d2;
        if (vc <= 0 && d1 >= 0 && d3 <= 0) {
            const float v = d1 / std::max(1e-20f, d1 - d3);
            w[0]=1-v; w[1]=v; w[2]=0; return distToSeg(q, A, B, v);
        }
        const float vb = d5*d2 - d1*d6;
        if (vb <= 0 && d2 >= 0 && d6 <= 0) {
            const float v = d2 / std::max(1e-20f, d2 - d6);
            w[0]=1-v; w[1]=0; w[2]=v; return distToSeg(q, A, C, v);
        }
        const float va = d3*d6 - d5*d4;
        if (va <= 0 && (d4-d3) >= 0 && (d5-d6) >= 0) {
            const float v = (d4-d3) / std::max(1e-20f, (d4-d3) + (d5-d6));
            w[0]=0; w[1]=1-v; w[2]=v; return distToSeg(q, B, C, v);
        }
        const float denom = 1.0f / std::max(1e-20f, va + vb + vc);
        const float v = vb * denom, ww = vc * denom;
        w[0] = 1.0f - v - ww; w[1] = v; w[2] = ww;
        float pt[3];
        for (int i = 0; i < 3; ++i) pt[i] = A[i] + ab[i]*v + ac[i]*ww;
        return dist2(q, pt);
    }
    static float dist2(const float* p, const float* q)
    { float s=0; for (int i=0;i<3;++i) { const float d=p[i]-q[i]; s+=d*d; } return s; }
    static float distToSeg(const float* q, const float* A, const float* B, float t)
    { float pt[3]; for (int i=0;i<3;++i) pt[i]=A[i]+(B[i]-A[i])*t; return dist2(q, pt); }

    const std::vector<float>* m_pts = nullptr;
    const std::vector<int>* m_faces = nullptr;
    PointGrid m_grid;
    std::vector<std::vector<int>> m_vertFaces;
    std::vector<float> m_vertN;          // unit normal per correspondence vertex
    mutable std::vector<int> m_seeds;    // scratch, reused across calls

    // How many nearby vertices to consider before giving up on finding one
    // whose normal agrees. The correct seed was the 1st or 2nd candidate in
    // every measured case; 8 is slack for a denser seam.
    static constexpr int kSeedCandidates = 8;
};


double bboxDiag(const std::vector<float>& v)
{
    if (v.empty()) return 0.0;
    float lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
    for (size_t i = 0; i + 2 < v.size(); i += 3)
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], v[i+a]);
            hi[a] = std::max(hi[a], v[i+a]);
        }
    double s = 0;
    for (int a = 0; a < 3; ++a) s += double(hi[a]-lo[a]) * double(hi[a]-lo[a]);
    return std::sqrt(s);
}

}  // namespace

std::vector<float> rbfWarpByAnchors(const std::vector<float>& tmplV,
                                    const std::vector<NricpLandmark>& anchors)
{
    const int nv = int(tmplV.size() / 3);
    // Collect valid, de-duplicated centers + targets (two anchors on the same
    // template vertex would make the system singular - first one wins).
    std::vector<int> cs;
    std::vector<std::array<double,3>> C, T;      // template center, user target
    for (const auto& a : anchors) {
        if (a.tmplVertex < 0 || a.tmplVertex >= nv) continue;
        bool dup = false;
        for (int c : cs) if (c == a.tmplVertex) { dup = true; break; }
        if (dup) continue;
        cs.push_back(a.tmplVertex);
        C.push_back({tmplV[size_t(a.tmplVertex)*3],
                     tmplV[size_t(a.tmplVertex)*3+1],
                     tmplV[size_t(a.tmplVertex)*3+2]});
        T.push_back({double(a.target[0]), double(a.target[1]),
                     double(a.target[2])});
    }
    const int N = int(cs.size());
    if (N < 4) return {};

    // 1) SIMILARITY prealign (centroid + RMS-spread scale, no rotation - both
    // faces are upright/front-facing by contract). Face markers are nearly
    // COPLANAR, so a full affine/thin-plate warp is ill-conditioned along the
    // depth axis and can shear the back of the head into garbage - the
    // similarity handles the global part robustly, the Gaussian RBF below only
    // carries the local residuals and DECAYS away from the face.
    std::array<double,3> cT{0,0,0}, cU{0,0,0};
    for (int i = 0; i < N; ++i)
        for (int d = 0; d < 3; ++d) {
            cT[size_t(d)] += C[size_t(i)][size_t(d)] / N;
            cU[size_t(d)] += T[size_t(i)][size_t(d)] / N;
        }
    double sT = 0, sU = 0;
    for (int i = 0; i < N; ++i) {
        double dt = 0, du = 0;
        for (int d = 0; d < 3; ++d) {
            const double a = C[size_t(i)][size_t(d)] - cT[size_t(d)];
            const double b = T[size_t(i)][size_t(d)] - cU[size_t(d)];
            dt += a*a; du += b*b;
        }
        sT += std::sqrt(dt); sU += std::sqrt(du);
    }
    if (sT < 1e-9) return {};
    const double scale = (sU > 1e-9) ? sU / sT : 1.0;
    auto prealign = [&](const std::array<double,3>& p) {
        std::array<double,3> q;
        for (int d = 0; d < 3; ++d)
            q[size_t(d)] = (p[size_t(d)] - cT[size_t(d)]) * scale + cU[size_t(d)];
        return q;
    };

    // Prealigned centers + residual displacements the RBF must carry.
    std::vector<std::array<double,3>> Cp(size_t(N), {0,0,0});
    std::vector<std::array<double,3>> R(size_t(N), {0,0,0});
    for (int i = 0; i < N; ++i) {
        Cp[size_t(i)] = prealign(C[size_t(i)]);
        for (int d = 0; d < 3; ++d)
            R[size_t(i)][size_t(d)] =
                T[size_t(i)][size_t(d)] - Cp[size_t(i)][size_t(d)];
    }

    // 2) Gaussian RBF on the residuals, ridge-regularized. sigma = mean
    // nearest-neighbour center spacing (x1.5) so influence blobs overlap
    // smoothly; far from the face the displacement decays to the similarity.
    double sigma = 0;
    for (int i = 0; i < N; ++i) {
        double best = 1e30;
        for (int j = 0; j < N; ++j) {
            if (i == j) continue;
            double d2 = 0;
            for (int d = 0; d < 3; ++d) {
                const double dd = Cp[size_t(i)][size_t(d)] - Cp[size_t(j)][size_t(d)];
                d2 += dd*dd;
            }
            best = std::min(best, d2);
        }
        sigma += std::sqrt(best) / N;
    }
    sigma *= 1.5;
    if (sigma < 1e-9) return {};
    const double inv2s2 = 1.0 / (2.0 * sigma * sigma);
    const double ridge = 1e-3;

    // Solve (A + ridge*I) w = R for the 3 axes with Gaussian elimination.
    std::vector<double> A(size_t(N)*N, 0.0);
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            double d2 = 0;
            for (int d = 0; d < 3; ++d) {
                const double dd = Cp[size_t(i)][size_t(d)] - Cp[size_t(j)][size_t(d)];
                d2 += dd*dd;
            }
            A[size_t(i)*N + j] = std::exp(-d2 * inv2s2) + (i == j ? ridge : 0.0);
        }
    std::vector<std::array<double,3>> rhs = R;
    for (int col = 0; col < N; ++col) {
        int piv = col;
        for (int r = col+1; r < N; ++r)
            if (std::abs(A[size_t(r)*N+col]) > std::abs(A[size_t(piv)*N+col]))
                piv = r;
        if (std::abs(A[size_t(piv)*N+col]) < 1e-12) return {};
        if (piv != col) {
            for (int c = 0; c < N; ++c)
                std::swap(A[size_t(piv)*N+c], A[size_t(col)*N+c]);
            std::swap(rhs[size_t(piv)], rhs[size_t(col)]);
        }
        const double p = A[size_t(col)*N+col];
        for (int r = col+1; r < N; ++r) {
            const double f = A[size_t(r)*N+col] / p;
            if (f == 0.0) continue;
            for (int c = col; c < N; ++c)
                A[size_t(r)*N+c] -= f * A[size_t(col)*N+c];
            for (int d = 0; d < 3; ++d)
                rhs[size_t(r)][size_t(d)] -= f * rhs[size_t(col)][size_t(d)];
        }
    }
    std::vector<std::array<double,3>> w(size_t(N), {0,0,0});
    for (int r = N-1; r >= 0; --r) {
        std::array<double,3> acc = rhs[size_t(r)];
        for (int c = r+1; c < N; ++c)
            for (int d = 0; d < 3; ++d)
                acc[size_t(d)] -= A[size_t(r)*N+c] * w[size_t(c)][size_t(d)];
        for (int d = 0; d < 3; ++d)
            w[size_t(r)][size_t(d)] = acc[size_t(d)] / A[size_t(r)*N+r];
    }

    // Warp every template vertex: similarity, then the decaying residual field.
    std::vector<float> out(tmplV.size());
    for (int v = 0; v < nv; ++v) {
        const std::array<double,3> p = prealign(
            {tmplV[size_t(v)*3], tmplV[size_t(v)*3+1], tmplV[size_t(v)*3+2]});
        std::array<double,3> disp{0,0,0};
        for (int i = 0; i < N; ++i) {
            double d2 = 0;
            for (int d = 0; d < 3; ++d) {
                const double dd = p[size_t(d)] - Cp[size_t(i)][size_t(d)];
                d2 += dd*dd;
            }
            const double phi = std::exp(-d2 * inv2s2);
            for (int d = 0; d < 3; ++d)
                disp[size_t(d)] += w[size_t(i)][size_t(d)] * phi;
        }
        out[size_t(v)*3]   = float(p[0] + disp[0]);
        out[size_t(v)*3+1] = float(p[1] + disp[1]);
        out[size_t(v)*3+2] = float(p[2] + disp[2]);
    }
    return out;
}

FaceRigResult buildFaceRig(const std::vector<float>& userV,
                           const std::vector<int>& userF,
                           const ArkitTemplate& tmpl,
                           const FaceRigOptions& opts,
                           const std::vector<char>& headMask,
                           const std::vector<NricpLandmark>& landmarks,
                           const FaceRigProgressFn& progress)
{
    FaceRigResult r;
    if (userV.size() < 9 || userF.size() < 3) {
        r.error = "user mesh has no geometry";
        return r;
    }
    if (!tmpl.valid()) {
        r.error = "ARKit template not loaded";
        return r;
    }
    const int nuFull = int(userV.size() / 3);
    r.userVertexCount = nuFull;

    // Head isolation: if a mask is supplied, build a head-only sub-mesh and fit
    // THAT (so the face template lands on the face, not the whole body). We
    // keep a fit→full-mesh vertex index map so the resampled deltas scatter
    // back to the right original vertices; non-head vertices get zero delta.
    // Without a mask, fit the whole mesh (a bare-face crop).
    std::vector<float> subV;
    std::vector<int> subF;
    std::vector<int> subToFull;         // fit vertex index → full-mesh index
    const bool isolate = int(headMask.size()) == nuFull;
    if (isolate) {
        std::vector<int> fullToSub(size_t(nuFull), -1);
        for (int v = 0; v < nuFull; ++v) {
            if (!headMask[size_t(v)]) continue;
            fullToSub[size_t(v)] = int(subToFull.size());
            subToFull.push_back(v);
            subV.insert(subV.end(),
                        {userV[size_t(v)*3], userV[size_t(v)*3+1], userV[size_t(v)*3+2]});
        }
        // keep faces whose 3 verts are all head; remap to sub indices
        for (size_t f = 0; f + 2 < userF.size(); f += 3) {
            const int a = userF[f], b = userF[f+1], c = userF[f+2];
            const int nFull = int(fullToSub.size());
            if (a < 0 || b < 0 || c < 0 || a >= nFull || b >= nFull || c >= nFull)
                continue;
            const int sa = fullToSub[size_t(a)], sb = fullToSub[size_t(b)],
                      sc = fullToSub[size_t(c)];
            if (sa >= 0 && sb >= 0 && sc >= 0)
                subF.insert(subF.end(), {sa, sb, sc});
        }
        if (subV.size() < 9 || subF.size() < 3) {
            // head region had no usable surface — fall back to whole-mesh fit
            subV.clear(); subF.clear(); subToFull.clear();
        }
    }
    const bool useSub = !subToFull.empty();
    const std::vector<float>& fitV = useSub ? subV : userV;
    const std::vector<int>&   fitF = useSub ? subF : userF;
    const int nu = int(fitV.size() / 3);

    // Progress model: the NRICP fit anneals over N stiffness levels (each a
    // step under "Fitting…"), then one step per transferred shape. Total =
    // fitLevels + shapeCount so the bar advances through BOTH phases.
    NricpOptions fitOpts;
    fitOpts.landmarks = landmarks;   // anchor the fit to detected face features
    const int fitLevels = int(fitOpts.stiffness.size());
    const int shapeTotal = opts.maxShapes > 0
        ? std::min<int>(opts.maxShapes, tmpl.shapeCount())
        : tmpl.shapeCount();
    const int total = fitLevels + shapeTotal;
    bool cancelled = false;
    auto tick = [&](int done, const char* phase) -> bool {
        return progress ? progress(done, total, phase) : true;
    };

    // Marker-driven RBF pre-warp: with a small, CURATED anchor set (the
    // user-adjusted markers — ≤ ~16), warp the whole template into the user's
    // face proportions before the fit, so the mouth/eyes/chin START on the
    // marked positions and the space between interpolates smoothly. Soft
    // in-fit constraints alone let un-anchored regions slide on faces far from
    // the template (cartoon proportions), smearing the transferred shapes.
    // Deliberately NOT applied to bulk auto-detected anchor sets (hundreds of
    // points): a garbage detection would fold the template.
    std::vector<float> fitTmplV = tmpl.neutral();
    if (!landmarks.empty() && landmarks.size() <= 32) {
        std::vector<float> warped = rbfWarpByAnchors(tmpl.neutral(), landmarks);
        if (!warped.empty()) fitTmplV = std::move(warped);
    }

    // ── TEMPLATE COMPONENT SPLIT (the eyes/teeth fix) ───────────────────────
    // The ICT template is ~191 connected components: the outer face surface
    // (14k verts) plus eyeballs, corneas, teeth, mouth interior, lashes… The
    // surface fit drags INTERIOR component verts onto the OUTER skin (closest-
    // point has no better answer), destroying their correspondence — measured:
    // eyeLook/eyeBlink deltas landed nowhere and the eyes never moved, on the
    // TEMPLATE ITSELF. Fit ONLY the main component; place each satellite by the
    // local AFFINE its surrounding main-surface region underwent, preserving
    // the eyeball/teeth structure inside the fitted head.
    const int tvc = tmpl.vertexCount();
    std::vector<int> comp(size_t(tvc), 0);
    int compCount = 1;
    {
        std::vector<int> par(size_t(tvc), 0);
        for (int i = 0; i < tvc; ++i) par[size_t(i)] = i;
        std::function<int(int)> findRoot = [&](int x) {
            while (par[size_t(x)] != x) {
                par[size_t(x)] = par[size_t(par[size_t(x)])];
                x = par[size_t(x)];
            }
            return x;
        };
        const auto& tf = tmpl.faces();
        for (size_t f = 0; f + 2 < tf.size(); f += 3) {
            int a = findRoot(tf[f]), b = findRoot(tf[f+1]);
            if (a != b) par[size_t(a)] = b;
            a = findRoot(tf[f+1]); b = findRoot(tf[f+2]);
            if (a != b) par[size_t(a)] = b;
        }
        std::unordered_map<int,int> remap;
        compCount = 0;
        for (int i = 0; i < tvc; ++i) {
            const int root = findRoot(i);
            auto it = remap.find(root);
            if (it == remap.end()) { remap.emplace(root, compCount); comp[size_t(i)] = compCount++; }
            else comp[size_t(i)] = it->second;
        }
    }
    int mainComp = 0;
    {
        std::vector<int> cnt(size_t(compCount), 0);
        for (int i = 0; i < tvc; ++i) cnt[size_t(comp[size_t(i)])]++;
        for (int c = 1; c < compCount; ++c)
            if (cnt[size_t(c)] > cnt[size_t(mainComp)]) mainComp = c;
    }

    // Extract the main-component sub-template (from the possibly-warped verts)
    // and remap anchors onto it (markers sit on the outer surface; any anchor
    // that resolved onto a satellite is dropped).
    std::vector<float> mainV; std::vector<int> mainF; std::vector<int> mainToFull;
    std::vector<int> fullToMainIdx(size_t(tvc), -1);
    if (compCount > 1) {
        for (int i = 0; i < tvc; ++i) {
            if (comp[size_t(i)] != mainComp) continue;
            fullToMainIdx[size_t(i)] = int(mainToFull.size());
            mainToFull.push_back(i);
            mainV.insert(mainV.end(), {fitTmplV[size_t(i)*3],
                                       fitTmplV[size_t(i)*3+1],
                                       fitTmplV[size_t(i)*3+2]});
        }
        const auto& tf = tmpl.faces();
        for (size_t f = 0; f + 2 < tf.size(); f += 3) {
            const int a = fullToMainIdx[size_t(tf[f])],
                      b = fullToMainIdx[size_t(tf[f+1])],
                      c = fullToMainIdx[size_t(tf[f+2])];
            if (a >= 0 && b >= 0 && c >= 0) mainF.insert(mainF.end(), {a, b, c});
        }
        std::vector<NricpLandmark> mainAnchors;
        for (auto lm : fitOpts.landmarks) {
            if (lm.tmplVertex < 0 || lm.tmplVertex >= tvc) continue;
            const int mi = fullToMainIdx[size_t(lm.tmplVertex)];
            if (mi >= 0) { lm.tmplVertex = mi; mainAnchors.push_back(lm); }
        }
        fitOpts.landmarks = std::move(mainAnchors);
    }
    const bool splitTmpl = compCount > 1 && mainV.size() >= 9 && mainF.size() >= 3;
    const std::vector<float>& fitTV = splitTmpl ? mainV : fitTmplV;
    const std::vector<int>&   fitTF = splitTmpl ? mainF : tmpl.faces();

    // Pre-align on the WHOLE template vs the WHOLE fit-user mesh, even though
    // only the main surface is FITTED. The component split removes the
    // template's eyeballs/teeth/lashes while the user mesh still has its own,
    // so aligning those two directly compares different subsets of a head:
    // measured on the template fitted to ITSELF the centroids differed by
    // 0.875 in Z, and the anneal warped the surface by ~0.55 mean to close a
    // gap that should not exist — detuning the transfer's rest frames and
    // leaving every blendshape 4-9x too weak.
    if (splitTmpl) {
        fitOpts.prealignTmplV = fitTmplV;   // whole template (pre-warped)
        fitOpts.prealignUserV = fitV;       // whole fit-side user mesh
    }
    // 1) NRICP: (pre-warped) template MAIN SURFACE → user neutral.
    // Report each annealing level so the (long) fit phase visibly advances.
    const NricpResult fit = FaceRig::fit(
        fitTV, fitTF, fitV, fitF, fitOpts,
        [&](int level, int /*levelCount*/) -> bool {
            if (!tick(level, "Fitting face template…")) { cancelled = true; return false; }
            return true;
        });
    if (cancelled) { r.error = "cancelled"; return r; }
    if (!fit.ok || fit.diag <= 0.0) {
        r.error = "non-rigid fit failed";
        return r;
    }
    r.fitMeanResidualPct = 100.0 * fit.meanResidual / fit.diag;
    r.fitMaxResidualPct = 100.0 * fit.maxResidual / fit.diag;
    if (!tick(fitLevels, "Transferring shapes…")) { r.error = "cancelled"; return r; }

    // humanoid-only gate: a bad fit means this isn't a face — refuse. A NRICP
    // fit that couldn't converge onto the surface reports non-finite or huge
    // residuals (e.g. a plane template forced onto a sphere), which we treat
    // as a hard reject. `maxFitResidualPct` gates the MAX residual directly —
    // the knob is advertised as `--max-residual`, so it must mean what it says
    // (it previously allowed up to 6x the supplied value). The mean gate at a
    // quarter of it catches fits that never blow up locally but drape the
    // whole surface badly (healthy fits measure mean <= 0.1%, max <= ~4%).
    const bool nonFinite = !std::isfinite(r.fitMeanResidualPct) ||
                           !std::isfinite(r.fitMaxResidualPct);
    if (nonFinite || r.fitMaxResidualPct > opts.maxFitResidualPct ||
        r.fitMeanResidualPct > opts.maxFitResidualPct * 0.25) {
        r.error = "mesh does not fit the human face template (mean residual " +
                  std::to_string(r.fitMeanResidualPct) + "%, max " +
                  std::to_string(r.fitMaxResidualPct) +
                  "%); this does not look like a human face mesh";
        return r;
    }

    // Assemble the FULL fitted correspondence: main verts from the fit;
    // satellites by the local affine their neighbouring main region underwent
    // (least-squares over the K nearest FINITE main verts).
    const std::vector<float>& tn = tmpl.neutral();
    std::vector<float> fitted;
    if (!splitTmpl) {
        fitted = fit.fitted;
    } else {
        fitted.assign(size_t(tvc) * 3, 0.0f);
        for (size_t m = 0; m < mainToFull.size(); ++m)
            for (int d = 0; d < 3; ++d)
                fitted[size_t(mainToFull[m])*3 + d] = fit.fitted[m*3 + d];

        // Place the satellite islands (mouth interior, teeth, eyeballs,
        // lashes) by transporting them with the displacement the main surface
        // underwent, sampled as a globally-supported inverse-distance blend.
        //
        // The previous rule fitted a least-squares AFFINE to the 60 main verts
        // nearest each island's centroid. That is an extrapolation problem in
        // disguise, and it failed badly on the mouth interior (#1059): its
        // deepest vertices sit up to 2.6 units from ANY main-surface vertex on
        // a head only ~22 units across, so their nearest neighbours form a
        // distant, nearly co-planar patch of outer skin. The fitted affine was
        // ill-conditioned there and extrapolated wildly — measured on the
        // template fitted to ITSELF (where a correct placement is exact), it
        // misplaced the island by 0.347 mean / 3.29 max, inverting the
        // front-to-back ordering of the throat vertices. That is the artifact
        // the user saw as the mouth interior lagging the teeth on jawOpen.
        //
        // An affine is the wrong model because it has a linear part that must
        // be EXTRAPOLATED far outside its support. A displacement blend has
        // no linear part to diverge: every sample is a convex combination of
        // displacements that actually occurred, so the island can never leave
        // the convex hull of the motion around it. Measured on the same
        // control mesh, sweeping neighbourhood size and falloff exponent:
        //
        //   rule                                mean      max
        //   affine, K=60   (what shipped)      0.3468    3.2853
        //   affine, K=2000                     0.1839    0.4482
        //   IDW p=1, K=60                      0.3051    1.3607
        //   IDW p=1, K=2000                    0.1038    0.3820
        //   IDW p=1, all main                  0.0274    0.0759
        //   IDW p=0.5, all main                0.0172    0.0340   <- chosen
        //
        // Both knobs are monotone: more support and a gentler falloff are
        // always better, because the true displacement field is smooth and a
        // wide blend is what recovers that smoothness. Using ALL main verts
        // also removes the neighbourhood-size parameter entirely. The result
        // is more accurate than the main surface's own fit drift (0.120 mean),
        // i.e. satellite placement is no longer a meaningful error source.
        //
        // End to end, as per-island jawOpen amplitude ratios on the control
        // mesh (1.000 is exact; the main surface sits at 1.008 either way):
        //
        //   island           before   after
        //   mouth interior    0.912   0.997
        //   212               0.915   0.997
        //   179               0.902   0.997
        //   176               0.898   0.997
        //
        // The p10 outliers go with them: islands 212 and 176 had vertices
        // receiving ZERO motion (p10 = 0.000), and the worst island p10 is
        // now 0.995. Every island was lagging, not just the mouth — the
        // reported symptom was simply where it was most visible.
        //
        // Cost is O(satellites x main) — 12,657 x 14,062 = 178M distance
        // evaluations for the ICT template, measured at 0.24 s against a
        // ~10 min total run, i.e. dwarfed by the deformation-transfer solves.
        // A spatial index would cut it further but is not worth the code:
        // the whole point of the rule is that EVERY main vertex contributes.

        // Main verts with a finite fit, with their displacement precomputed.
        std::vector<int>    srcFull;     // template vertex index
        std::vector<double> srcDisp;     // fitted - warped rest, xyz
        srcFull.reserve(mainToFull.size());
        srcDisp.reserve(mainToFull.size() * 3);
        for (size_t m = 0; m < mainToFull.size(); ++m) {
            bool finite = true;
            for (int d = 0; d < 3; ++d)
                if (!std::isfinite(fit.fitted[m*3+d])) { finite = false; break; }
            if (!finite) continue;
            const int fv = mainToFull[m];
            srcFull.push_back(fv);
            for (int d = 0; d < 3; ++d)
                srcDisp.push_back(double(fit.fitted[m*3+size_t(d)])
                                - double(fitTmplV[size_t(fv)*3+size_t(d)]));
        }

        // No usable main correspondence at all (a fit that diverged wholesale):
        // leave every satellite at the template rest. It simply won't deform,
        // which is the same degradation the old code chose, and the NRICP gate
        // above would normally have rejected such a fit already.
        const bool haveSources = srcFull.size() >= 4;

        // Fit the global SIMILARITY (uniform scale + translation) that the main
        // surface underwent, and apply it EXACTLY; blend only what is left.
        //
        // Without this the blend is wrong under a global scale, which NRICP's
        // bbox prealign produces routinely for meshes in different units. A
        // uniform scale s about the origin gives every main vertex the
        // displacement (s-1)*p — a POSITION-DEPENDENT term. Averaging those
        // and adding the result to a satellite at q yields
        // q + (s-1)*weightedMean(mainPositions) instead of s*q, so an eye or
        // tooth sitting away from the main surface's centroid keeps roughly
        // its original position and size while the head scales around it.
        // Measured on the ICT template under a synthetic pure scale, placing
        // 12,657 satellites (error vs the exact s*p):
        //
        //   scale   mean     max      worst relative
        //   1.00    0.0000   0.0000    0.0 %
        //   1.05    0.1991   0.3430    8.0 %
        //   1.20    0.7963   1.3721   28.1 %
        //   2.00    3.9817   6.8604   84.4 %
        //
        // Even 5 % costs more than the main surface's own fit drift (0.120).
        // The residual after removing the similarity carries no such term, so
        // the blend sees only local, position-independent deformation — which
        // is the thing an average is valid for.
        double gScale = 1.0;
        std::array<double,3> srcCtr{0,0,0}, dstCtr{0,0,0};
        if (haveSources) {
            for (size_t k = 0; k < srcFull.size(); ++k) {
                const int fv = srcFull[k];
                for (int d = 0; d < 3; ++d) {
                    const double p = double(fitTmplV[size_t(fv)*3+size_t(d)]);
                    srcCtr[size_t(d)] += p;
                    dstCtr[size_t(d)] += p + srcDisp[k*3+size_t(d)];
                }
            }
            const double inv = 1.0 / double(srcFull.size());
            for (int d = 0; d < 3; ++d) { srcCtr[size_t(d)] *= inv; dstCtr[size_t(d)] *= inv; }
            // Uniform scale from the RMS radius about each centroid. Rotation
            // is deliberately NOT extracted: the prealign is axis-aligned
            // (centroid + bbox), so scale + translation is the part that
            // carries a position-dependent term, and a wrong rotation would
            // be worse than none.
            double sn = 0.0, sd = 0.0;
            for (size_t k = 0; k < srcFull.size(); ++k) {
                const int fv = srcFull[k];
                for (int d = 0; d < 3; ++d) {
                    const double a = double(fitTmplV[size_t(fv)*3+size_t(d)]) - srcCtr[size_t(d)];
                    const double b = a + srcDisp[k*3+size_t(d)] - (dstCtr[size_t(d)] - srcCtr[size_t(d)]);
                    sn += a * b; sd += a * a;
                }
            }
            if (sd > 1e-12) {
                const double cand = sn / sd;
                // Guard against a degenerate estimate; 1.0 falls back to the
                // pure-blend behaviour, which is correct when there is no
                // global scale.
                if (std::isfinite(cand) && cand > 1e-3 && cand < 1e3) gScale = cand;
            }
            // Re-express every source displacement as the RESIDUAL left after
            // the global similarity, so the blend never averages (s-1)*p.
            for (size_t k = 0; k < srcFull.size(); ++k) {
                const int fv = srcFull[k];
                for (int d = 0; d < 3; ++d) {
                    const double p = double(fitTmplV[size_t(fv)*3+size_t(d)]);
                    const double sim = dstCtr[size_t(d)] + gScale * (p - srcCtr[size_t(d)]);
                    srcDisp[k*3+size_t(d)] = (p + srcDisp[k*3+size_t(d)]) - sim;
                }
            }
        }

        for (int i = 0; i < tvc; ++i) {
            if (comp[size_t(i)] == mainComp) continue;
            if (!haveSources) {
                for (int d = 0; d < 3; ++d)
                    fitted[size_t(i)*3+size_t(d)] = tn[size_t(i)*3+size_t(d)];
                continue;
            }
            const double px = fitTmplV[size_t(i)*3];
            const double py = fitTmplV[size_t(i)*3+1];
            const double pz = fitTmplV[size_t(i)*3+2];
            // Inverse-distance blend with exponent 1/2 (w = d^-0.5, i.e.
            // 1/sqrt(d)), which the sweep above picked over 1 and 1.5.
            double acc[3] = {0,0,0}, wsum = 0.0;
            for (size_t n = 0; n < srcFull.size(); ++n) {
                const int fv = srcFull[n];
                const double dx = double(fitTmplV[size_t(fv)*3])   - px;
                const double dy = double(fitTmplV[size_t(fv)*3+1]) - py;
                const double dz = double(fitTmplV[size_t(fv)*3+2]) - pz;
                const double d2 = dx*dx + dy*dy + dz*dz;
                // w = d^-0.5 = (d2)^-0.25; the floor keeps a coincident
                // vertex from producing an infinite weight.
                const double w = 1.0 / std::sqrt(std::sqrt(std::max(d2, 1e-12)));
                wsum += w;
                for (int d = 0; d < 3; ++d) acc[d] += w * srcDisp[n*3+size_t(d)];
            }
            for (int d = 0; d < 3; ++d) {
                const double p = double(fitTmplV[size_t(i)*3+size_t(d)]);
                const double sim = dstCtr[size_t(d)] + gScale * (p - srcCtr[size_t(d)]);
                fitted[size_t(i)*3+size_t(d)] = float(sim + acc[d] / wsum);
            }
        }
    }

    // sanitize the correspondence: a handful of template verts may have
    // diverged (NaN/inf) even in an accepted fit (< 5% by the NRICP gate).
    // Fall those back to the template neutral so they don't poison the
    // deformation-transfer solve — they simply won't deform meaningfully.
    for (size_t i = 0; i < fitted.size() && i < tn.size(); ++i)
        if (!std::isfinite(fitted[i]))
            fitted[i] = tn[i];

    // 2) DeformationTransfer over the fixed (topology + fit) system.
    DeformationTransfer dt;
    if (!dt.init(tmpl.neutral(), tmpl.faces(), fitted)) {
        r.error = "deformation-transfer setup failed";
        return r;
    }

    // 3) resample map: fit vertex → closest point ON the correspondence
    //    SURFACE (barycentric, template topology), built once for all shapes.
    //    Deliberately not a nearest-VERTEX pick — see SurfaceSampler.
    SurfaceSampler sampler;
    sampler.build(fitted, tmpl.faces());
    // NB braces, not parens: `vector<Hit> userHit(size_t(nu))` is the most
    // vexing parse — the compiler reads it as a function declaration.
    std::vector<SurfaceSampler::Hit> userHit{size_t(nu)};
    // The query's own normal lets the sampler seed on the RIGHT surface where
    // two surfaces nearly touch — see SurfaceSampler::sample.
    const std::vector<float> userN = SurfaceSampler::vertexNormals(fitV, fitF);
    // A user mesh wound opposite to the template has globally negated normals;
    // without this the sign test would pick the WRONG surface every time.
    const float nsign = SurfaceSampler::orientationSign(fitV, userN, sampler);
    for (int i = 0; i < nu; ++i) {
        const float* qn = (size_t(i)*3+2 < userN.size()) ? &userN[size_t(i)*3] : nullptr;
        userHit[size_t(i)] = sampler.sample(&fitV[size_t(i)*3], qn, nsign);
    }

    // noise floor scaled by the FIT region diagonal (a head is smaller than a
    // whole body, so scaling on the full-body diag would swallow real motion).
    const double diag = bboxDiag(fitV);
    const double eps = opts.deltaEpsPct / 100.0 * diag;

    const auto& shapes = tmpl.shapes();
    const int maxShapes = opts.maxShapes > 0
        ? std::min<int>(opts.maxShapes, int(shapes.size()))
        : int(shapes.size());

    for (int s = 0; s < maxShapes; ++s) {
        if (!tick(fitLevels + s, "Transferring shapes…")) { r.error = "cancelled"; return r; }
        // per-TEMPLATE-vertex delta on the user identity
        const std::vector<float> tmplDelta = dt.transfer(shapes[size_t(s)].deltas);
        if (int(tmplDelta.size() / 3) != tmpl.vertexCount()) {
            r.error = "transfer produced an unexpected vertex count";
            return r;
        }

        FaceRigShape out;
        out.name = shapes[size_t(s)].name;
        // Deltas are always full-mesh sized; head isolation writes only the
        // head vertices (via subToFull), leaving the body at zero.
        out.userDeltas.assign(size_t(nuFull) * 3, 0.0f);
        const float amp = float(std::clamp(opts.amplitude, 0.1, 5.0));
        for (int i = 0; i < nu; ++i) {
            const SurfaceSampler::Hit& h = userHit[size_t(i)];
            if (h.vi[0] < 0) continue;
            // Blend the hit triangle's corner deltas by barycentric weight.
            // The corners belong to ONE triangle, so a query on the upper
            // lip mixes only upper-lip motion; it cannot reach across the
            // seam the way a nearest-vertex pick could.
            float dvec[3] = {0.0f, 0.0f, 0.0f};
            for (int k = 0; k < 3; ++k) {
                if (h.vi[k] < 0 || h.w[k] == 0.0f) continue;
                for (int a = 0; a < 3; ++a)
                    dvec[a] += h.w[k] * tmplDelta[size_t(h.vi[k])*3 + size_t(a)];
            }
            for (int a = 0; a < 3; ++a) dvec[a] *= amp;
            const double mag = std::sqrt(double(dvec[0])*dvec[0] +
                                         double(dvec[1])*dvec[1] +
                                         double(dvec[2])*dvec[2]);
            if (mag < eps) continue;   // noise floor → keep sparse
            const int dst = useSub ? subToFull[size_t(i)] : i;
            out.userDeltas[size_t(dst)*3]   = dvec[0];
            out.userDeltas[size_t(dst)*3+1] = dvec[1];
            out.userDeltas[size_t(dst)*3+2] = dvec[2];
            out.nonZeroVerts++;
            out.maxDisp = std::max(out.maxDisp, float(mag));
        }
        r.shapes.push_back(std::move(out));
    }

    // Amplitude safety net: a poisoned anchor set (garbage landmarks that
    // slipped every gate) crushes the fit so the transferred shapes come out
    // technically-attached but INVISIBLE (~0.05% of the head, vs ~5% for a
    // healthy jawOpen). If the anchored run produced nothing visible, retry
    // once WITHOUT anchors — a plain head-isolated fit always beats an
    // invisible one. (Field-reproduced failure mode; do not remove.)
    if (!landmarks.empty()) {
        double maxAmp = 0;
        for (const auto& sh : r.shapes)
            maxAmp = std::max(maxAmp, double(sh.maxDisp));
        // maxDisp includes the user amplitude — normalise it out so a healthy
        // rig at amplitude 0.1 doesn't read as "invisible" and rerun.
        maxAmp /= std::clamp(opts.amplitude, 0.1, 5.0);
        if (maxAmp < 0.005 * diag) {
            std::fprintf(stderr, "[facerig] anchored fit produced invisible "
                         "shapes (max %.5f on diag %.3f) — retrying "
                         "unanchored\n", maxAmp, diag);
            return buildFaceRig(userV, userF, tmpl, opts, headMask, {},
                                progress);
        }
    }

    r.ok = true;
    return r;
}

}  // namespace FaceRig
