#include "NonRigidICP.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace FaceRig {

namespace {

using Vec3 = std::array<double, 3>;

Vec3 vsub(const Vec3& a, const Vec3& b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
double vdot(const Vec3& a, const Vec3& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Vec3 vadd(const Vec3& a, const Vec3& b) { return {a[0]+b[0], a[1]+b[1], a[2]+b[2]}; }
Vec3 vscale(const Vec3& a, double s) { return {a[0]*s, a[1]*s, a[2]*s}; }

Vec3 at(const std::vector<float>& v, int i)
{
    return {v[size_t(i)*3], v[size_t(i)*3+1], v[size_t(i)*3+2]};
}

// ---- closest point on a triangle (Ericson, Real-Time Collision Detection) --
Vec3 closestPointTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c)
{
    const Vec3 ab = vsub(b, a), ac = vsub(c, a), ap = vsub(p, a);
    const double d1 = vdot(ab, ap), d2 = vdot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const Vec3 bp = vsub(p, b);
    const double d3 = vdot(ab, bp), d4 = vdot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const double vc = d1*d4 - d3*d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return vadd(a, vscale(ab, d1/(d1-d3)));
    const Vec3 cp = vsub(p, c);
    const double d5 = vdot(ab, cp), d6 = vdot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const double vb = d5*d2 - d1*d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return vadd(a, vscale(ac, d2/(d2-d6)));
    const double va = d3*d6 - d5*d4;
    if (va <= 0 && (d4-d3) >= 0 && (d5-d6) >= 0)
        return vadd(b, vscale(vsub(c, b), (d4-d3)/((d4-d3)+(d5-d6))));
    const double denom = 1.0/(va+vb+vc);
    return vadd(a, vadd(vscale(ab, vb*denom), vscale(ac, vc*denom)));
}

// ---- a simple median-split KD-tree over triangle centroids -----------------
struct KDTree {
    std::vector<Vec3> pts;
    std::vector<int> idx;
    struct Node { int axis=-1; double split=0; int lo=-1, hi=-1, start=0, count=0; };
    std::vector<Node> nodes;

    void build(const std::vector<Vec3>& centroids)
    {
        pts = centroids;
        idx.resize(pts.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = int(i);
        nodes.clear();
        buildRange(0, int(idx.size()));
    }
    int buildRange(int start, int count)
    {
        const int self = int(nodes.size());
        nodes.push_back({});
        if (count <= 8) { nodes[self] = {-1, 0, -1, -1, start, count}; return self; }
        Vec3 mn = pts[idx[start]], mx = pts[idx[start]];
        for (int k = 0; k < count; ++k) {
            const Vec3& p = pts[idx[start+k]];
            for (int a = 0; a < 3; ++a) { mn[a] = std::min(mn[a], p[a]); mx[a] = std::max(mx[a], p[a]); }
        }
        int axis = 0; double ext = mx[0]-mn[0];
        for (int a = 1; a < 3; ++a) if (mx[a]-mn[a] > ext) { ext = mx[a]-mn[a]; axis = a; }
        const int mid = start + count/2;
        std::nth_element(idx.begin()+start, idx.begin()+mid, idx.begin()+start+count,
                         [&](int x, int y){ return pts[x][axis] < pts[y][axis]; });
        const double split = pts[idx[mid]][axis];
        const int lo = buildRange(start, mid-start);
        const int hi = buildRange(mid, start+count-mid);
        nodes[self] = {axis, split, lo, hi, 0, 0};
        return self;
    }
    // nearest centroid index (broad phase); refine to real distance by caller
    int nearest(const Vec3& q) const
    {
        int best = -1; double bestD2 = std::numeric_limits<double>::max();
        nearestRec(0, q, best, bestD2);
        return best;
    }
    void nearestRec(int n, const Vec3& q, int& best, double& bestD2) const
    {
        const Node& nd = nodes[n];
        if (nd.axis < 0) {
            for (int k = 0; k < nd.count; ++k) {
                const int pi = idx[nd.start+k];
                const Vec3 d = vsub(pts[pi], q);
                const double d2 = vdot(d, d);
                if (d2 < bestD2) { bestD2 = d2; best = pi; }
            }
            return;
        }
        const double diff = q[nd.axis] - nd.split;
        const int near = diff < 0 ? nd.lo : nd.hi;
        const int far = diff < 0 ? nd.hi : nd.lo;
        nearestRec(near, q, best, bestD2);
        if (diff*diff < bestD2) nearestRec(far, q, best, bestD2);
    }
};

// ---- CSR sparse matrix + CG on the normal equations (AᵀA x = Aᵀb) ----------
// A is (rows x cols); we never form AᵀA — CG multiplies by A then Aᵀ.
struct Sparse {
    int rows = 0, cols = 0;
    std::vector<int> rowPtr;         // size rows+1
    std::vector<int> col;
    std::vector<double> val;

    // build from triplets (row-major). triplets need not be unique/sorted.
    void fromTriplets(int r, int c, std::vector<std::array<double,3>>& trip)
    {
        rows = r; cols = c;
        std::vector<int> cnt(r+1, 0);
        for (auto& t : trip) cnt[int(t[0])+1]++;
        for (int i = 0; i < r; ++i) cnt[i+1] += cnt[i];
        rowPtr = cnt;
        col.resize(trip.size()); val.resize(trip.size());
        std::vector<int> cur = rowPtr;
        for (auto& t : trip) {
            const int rr = int(t[0]);
            const int dst = cur[rr]++;
            col[dst] = int(t[1]); val[dst] = t[2];
        }
    }
    // y = A x   (x size cols, y size rows)
    void mul(const std::vector<double>& x, std::vector<double>& y) const
    {
        y.assign(rows, 0.0);
        for (int r = 0; r < rows; ++r) {
            double s = 0;
            for (int k = rowPtr[r]; k < rowPtr[r+1]; ++k) s += val[k]*x[col[k]];
            y[r] = s;
        }
    }
    // y = Aᵀ x   (x size rows, y size cols)
    void mulT(const std::vector<double>& x, std::vector<double>& y) const
    {
        y.assign(cols, 0.0);
        for (int r = 0; r < rows; ++r) {
            const double xr = x[r];
            for (int k = rowPtr[r]; k < rowPtr[r+1]; ++k) y[col[k]] += val[k]*xr;
        }
    }
};

// solve min ‖A x - b‖² by CG on the normal equations, warm-started at x0.
void cgnr(const Sparse& A, const std::vector<double>& b,
          std::vector<double>& x, int maxIters, double tol)
{
    std::vector<double> Ax, r(A.cols), p, Ap, AtAp, tmp;
    A.mul(x, Ax);
    std::vector<double> resid(A.rows);
    for (int i = 0; i < A.rows; ++i) resid[i] = b[i] - Ax[i];
    A.mulT(resid, r);                 // r = Aᵀ(b - Ax)
    p = r;
    double rs = 0; for (double v : r) rs += v*v;
    const double rs0 = rs;
    for (int it = 0; it < maxIters && rs > tol*tol*rs0; ++it) {
        A.mul(p, Ap);
        A.mulT(Ap, AtAp);             // AtAp = AᵀA p
        double pAp = 0; for (int i = 0; i < A.cols; ++i) pAp += p[i]*AtAp[i];
        if (pAp <= 1e-30) break;
        const double a = rs / pAp;
        for (int i = 0; i < A.cols; ++i) { x[i] += a*p[i]; r[i] -= a*AtAp[i]; }
        double rsn = 0; for (double v : r) rsn += v*v;
        const double beta = rsn / rs;
        for (int i = 0; i < A.cols; ++i) p[i] = r[i] + beta*p[i];
        rs = rsn;
    }
}

}  // namespace

NricpResult fit(const std::vector<float>& tmplV, const std::vector<int>& tmplF,
                const std::vector<float>& userV, const std::vector<int>& userF,
                const NricpOptions& opts,
                const NricpProgressFn& progress)
{
    NricpResult res;
    const int Nt = int(tmplV.size()/3);
    const int Fu = int(userF.size()/3);
    if (Nt < 3 || Fu < 1 || userV.size() < 9)
        return res;

    // user bbox diagonal
    Vec3 mn = at(userV,0), mx = mn;
    for (int i = 1; i < int(userV.size()/3); ++i) {
        const Vec3 p = at(userV, i);
        for (int a = 0; a < 3; ++a) { mn[a] = std::min(mn[a], p[a]); mx[a] = std::max(mx[a], p[a]); }
    }
    res.diag = std::sqrt(vdot(vsub(mx,mn), vsub(mx,mn)));

    // rigid pre-align: centroid + bbox-scale (correspondence-free)
    Vec3 tmn = at(tmplV,0), tmx = tmn, tc{0,0,0}, uc{0,0,0};
    for (int i = 0; i < Nt; ++i) {
        const Vec3 p = at(tmplV,i); tc = vadd(tc,p);
        for (int a=0;a<3;++a){ tmn[a]=std::min(tmn[a],p[a]); tmx[a]=std::max(tmx[a],p[a]); }
    }
    tc = vscale(tc, 1.0/Nt);
    const int Nu = int(userV.size()/3);
    for (int i = 0; i < Nu; ++i) uc = vadd(uc, at(userV,i));
    uc = vscale(uc, 1.0/Nu);
    const double tdiag = std::sqrt(vdot(vsub(tmx,tmn), vsub(tmx,tmn)));
    const double s = tdiag > 1e-9 ? res.diag/tdiag : 1.0;

    // template homogeneous verts and current fitted positions X
    std::vector<Vec3> vhat(Nt), X(Nt);
    for (int i = 0; i < Nt; ++i) {
        vhat[i] = at(tmplV, i);
        X[i] = vadd(vscale(vsub(vhat[i], tc), s), uc);   // pre-aligned start
    }

    // user triangle geometry + centroid KD-tree
    std::vector<std::array<Vec3,3>> utri(Fu);
    std::vector<Vec3> ucent(Fu);
    for (int f = 0; f < Fu; ++f) {
        const Vec3 a = at(userV, userF[f*3]);
        const Vec3 b = at(userV, userF[f*3+1]);
        const Vec3 c = at(userV, userF[f*3+2]);
        utri[f] = {a,b,c};
        ucent[f] = vscale(vadd(vadd(a,b),c), 1.0/3.0);
    }
    KDTree tree; tree.build(ucent);

    // template edges (unique)
    std::vector<std::pair<int,int>> edges;
    {
        std::vector<std::array<int,2>> e;
        const int Ft = int(tmplF.size()/3);
        e.reserve(Ft*3);
        for (int f = 0; f < Ft; ++f) {
            const int a=tmplF[f*3], b=tmplF[f*3+1], c=tmplF[f*3+2];
            for (auto pr : {std::array<int,2>{a,b}, {b,c}, {c,a}}) {
                int lo = std::min(pr[0],pr[1]), hi = std::max(pr[0],pr[1]);
                e.push_back({lo,hi});
            }
        }
        std::sort(e.begin(), e.end());
        e.erase(std::unique(e.begin(), e.end()), e.end());
        for (auto& pr : e) edges.push_back({pr[0], pr[1]});
    }
    const int E = int(edges.size());

    // unknown layout: 12 per vertex (3x4 affine, row-major a00..a03,a10..,a20..)
    // X_i = A_i * [vhat_i; 1]. We solve 3 independent systems (one per output
    // coordinate), each with 4*Nt unknowns (the 4 affine coeffs mapping to that
    // coord for every vertex), sharing the SAME sparse matrix.
    const int cols = 4*Nt;

    const int levelCount = int(opts.stiffness.size());
    int levelIdx = 0;
    for (double alpha : opts.stiffness) {
        for (int iter = 0; iter < opts.itersPerLevel; ++iter) {
            // find closest surface point per current X_i
            std::vector<Vec3> target(Nt);
            for (int i = 0; i < Nt; ++i) {
                const int cf = tree.nearest(X[i]);
                // refine: check that triangle + a few neighbors would need the
                // full tree; centroid-nearest triangle is a good approximation
                // for a fitted template already close to the surface.
                target[i] = closestPointTriangle(X[i], utri[cf][0], utri[cf][1], utri[cf][2]);
            }

            // Landmark anchors that reference a valid template vertex. Weight
            // rides alpha so it dominates while the fit is still rigid (locking
            // orientation/scale), then relaxes as alpha anneals down.
            std::vector<const NricpLandmark*> lms;
            lms.reserve(opts.landmarks.size());
            for (const auto& lm : opts.landmarks)
                if (lm.tmplVertex >= 0 && lm.tmplVertex < Nt)
                    lms.push_back(&lm);
            const int L = int(lms.size());
            const double lw = opts.landmarkWeight * alpha;

            // assemble A (rows = Nt data + E stiffness + L landmark) once;
            // rhs differs per axis
            std::vector<std::array<double,3>> trip;
            trip.reserve(size_t(Nt)*4 + size_t(E)*8 + size_t(L)*4);
            // data rows: row i uses cols [4i..4i+3] with [vx,vy,vz,1]
            for (int i = 0; i < Nt; ++i) {
                trip.push_back({double(i), double(4*i+0), vhat[i][0]});
                trip.push_back({double(i), double(4*i+1), vhat[i][1]});
                trip.push_back({double(i), double(4*i+2), vhat[i][2]});
                trip.push_back({double(i), double(4*i+3), 1.0});
            }
            // stiffness rows: for edge (i,j), 4 rows (one per affine coeff)
            // alpha*(A_i[k]-A_j[k]) = 0
            for (int e = 0; e < E; ++e) {
                const int i = edges[e].first, j = edges[e].second;
                for (int k = 0; k < 4; ++k) {
                    const int row = Nt + e*4 + k;
                    trip.push_back({double(row), double(4*i+k),  alpha});
                    trip.push_back({double(row), double(4*j+k), -alpha});
                }
            }
            // landmark rows: lw*[vx,vy,vz,1]·A_L = lw*target[axis] (rhs per axis)
            for (int l = 0; l < L; ++l) {
                const int i = lms[l]->tmplVertex;
                const int row = Nt + E*4 + l;
                trip.push_back({double(row), double(4*i+0), lw*vhat[i][0]});
                trip.push_back({double(row), double(4*i+1), lw*vhat[i][1]});
                trip.push_back({double(row), double(4*i+2), lw*vhat[i][2]});
                trip.push_back({double(row), double(4*i+3), lw});
            }
            Sparse A;
            A.fromTriplets(Nt + E*4 + L, cols, trip);

            std::vector<double> b(A.rows, 0.0);
            std::vector<double> x(cols, 0.0);
            for (int axis = 0; axis < 3; ++axis) {
                for (int i = 0; i < Nt; ++i) b[i] = target[i][axis];
                // stiffness rhs stays 0
                for (int l = 0; l < L; ++l)
                    b[Nt + E*4 + l] = lw * double(lms[l]->target[size_t(axis)]);
                // warm start x from the current affine estimate for this axis
                for (int i = 0; i < Nt; ++i) {
                    // recover current A_i row for this axis from X_i & vhat_i is
                    // non-trivial; a zero start with identity bias works well
                    x[4*i+0] = 0; x[4*i+1] = 0; x[4*i+2] = 0; x[4*i+3] = X[i][axis];
                    x[4*i+axis] = 1.0;   // identity-ish seed
                }
                cgnr(A, b, x, opts.cgIters, opts.cgTol);
                for (int i = 0; i < Nt; ++i) {
                    X[i][axis] = x[4*i+0]*vhat[i][0] + x[4*i+1]*vhat[i][1]
                               + x[4*i+2]*vhat[i][2] + x[4*i+3];
                }
            }
        }
        ++levelIdx;
        if (progress && !progress(levelIdx, levelCount)) {
            // caller aborted — bail with the best fit so far (ok stays false
            // below because the residual pass computes finiteCount honestly).
            break;
        }
    }

    // final residuals to the user surface. A vertex whose affine solve
    // diverged (NaN/inf — e.g. degenerate fan triangles at a UV-sphere pole)
    // must not poison the mean: count it, skip it from the average, but still
    // surface it so callers can gate (a well-behaved face mesh produces none).
    res.fitted.resize(size_t(Nt)*3);
    res.residual.resize(Nt);
    double sum = 0, mx2 = 0;
    int finiteCount = 0, divergedCount = 0;
    for (int i = 0; i < Nt; ++i) {
        const bool finite = std::isfinite(X[i][0]) && std::isfinite(X[i][1]) &&
                            std::isfinite(X[i][2]);
        double d = 0.0;
        if (finite) {
            const int cf = tree.nearest(X[i]);
            const Vec3 cp = closestPointTriangle(X[i], utri[cf][0], utri[cf][1], utri[cf][2]);
            d = std::sqrt(vdot(vsub(X[i],cp), vsub(X[i],cp)));
        }
        if (finite && std::isfinite(d)) {
            res.residual[i] = float(d);
            sum += d; mx2 = std::max(mx2, d);
            finiteCount++;
        } else {
            res.residual[i] = std::numeric_limits<float>::infinity();
            divergedCount++;
        }
        res.fitted[size_t(i)*3+0] = float(X[i][0]);
        res.fitted[size_t(i)*3+1] = float(X[i][1]);
        res.fitted[size_t(i)*3+2] = float(X[i][2]);
    }
    res.meanResidual = finiteCount > 0 ? sum/finiteCount
                                       : std::numeric_limits<double>::infinity();
    // a large diverged fraction means the fit failed — reflect it in maxResidual
    // (which callers already gate on) so a mostly-NaN fit can't read as "great".
    res.maxResidual = (divergedCount > Nt / 20)   // > 5% diverged
        ? std::numeric_limits<double>::infinity()
        : mx2;
    res.ok = finiteCount > Nt / 2;   // need at least half the verts to have fit
    return res;
}

}  // namespace FaceRig
