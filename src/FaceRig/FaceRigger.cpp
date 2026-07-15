#include "FaceRigger.h"

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
        const std::array<int,3> c = cellOf(q);
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
            if (a < 0 || b < 0 || c < 0) continue;
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

    // 1) NRICP: template neutral → user neutral = correspondence X. Report each
    // annealing level so the (long) fit phase visibly advances.
    const NricpResult fit = FaceRig::fit(
        tmpl.neutral(), tmpl.faces(), fitV, fitF, fitOpts,
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
    // as a hard reject alongside the mean-residual threshold. The max residual
    // catches shapes that fit on average but blow up in a region.
    const bool nonFinite = !std::isfinite(r.fitMeanResidualPct) ||
                           !std::isfinite(r.fitMaxResidualPct);
    if (nonFinite || r.fitMeanResidualPct > opts.maxFitResidualPct ||
        r.fitMaxResidualPct > opts.maxFitResidualPct * 6.0) {
        r.error = "mesh does not fit the human face template (mean residual " +
                  std::to_string(r.fitMeanResidualPct) + "%, max " +
                  std::to_string(r.fitMaxResidualPct) +
                  "%); this does not look like a human face mesh";
        return r;
    }

    // sanitize the correspondence: a handful of template verts may have
    // diverged (NaN/inf) even in an accepted fit (< 5% by the NRICP gate).
    // Fall those back to the template neutral so they don't poison the
    // deformation-transfer solve — they simply won't deform meaningfully.
    std::vector<float> fitted = fit.fitted;
    const std::vector<float>& tn = tmpl.neutral();
    for (size_t i = 0; i < fitted.size() && i < tn.size(); ++i)
        if (!std::isfinite(fitted[i]))
            fitted[i] = tn[i];

    // 2) DeformationTransfer over the fixed (topology + fit) system.
    DeformationTransfer dt;
    if (!dt.init(tmpl.neutral(), tmpl.faces(), fitted)) {
        r.error = "deformation-transfer setup failed";
        return r;
    }

    // 3) resample map: fit vertex → nearest correspondence vertex (built once).
    PointGrid grid;
    grid.build(fitted);
    std::vector<int> userToTmpl(size_t(nu), -1);
    for (int i = 0; i < nu; ++i)
        userToTmpl[size_t(i)] = grid.nearest(&fitV[size_t(i)*3]);

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
        for (int i = 0; i < nu; ++i) {
            const int t = userToTmpl[size_t(i)];
            if (t < 0) continue;
            float dvec[3] = {tmplDelta[size_t(t)*3], tmplDelta[size_t(t)*3+1],
                             tmplDelta[size_t(t)*3+2]};
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

    r.ok = true;
    return r;
}

}  // namespace FaceRig
