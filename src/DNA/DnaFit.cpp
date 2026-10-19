#include "DnaFit.h"

#include "DnaAffine.h"

#include <QSet>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <unordered_map>

namespace Dna {
namespace Fit {

using Affine::Aff;
using Affine::compose;
using Affine::inverse;
using Affine::mul;

namespace {

using V3 = std::array<double, 3>;

double dist(const V3& a, const V3& b)
{
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// Solve A x = B for B with `cols` right-hand sides (row-major, n x n / n x cols),
// partial-pivot LU. Returns false on a singular matrix.
bool solveDense(std::vector<double>& A, std::vector<double>& B, int n, int cols)
{
    for (int k = 0; k < n; ++k) {
        int piv = k;
        double best = std::abs(A[size_t(k) * n + k]);
        for (int i = k + 1; i < n; ++i) {
            const double v = std::abs(A[size_t(i) * n + k]);
            if (v > best) { best = v; piv = i; }
        }
        if (best < 1e-14) return false;
        if (piv != k) {
            for (int j = 0; j < n; ++j) std::swap(A[size_t(k) * n + j], A[size_t(piv) * n + j]);
            for (int j = 0; j < cols; ++j) std::swap(B[size_t(k) * cols + j], B[size_t(piv) * cols + j]);
        }
        const double d = A[size_t(k) * n + k];
        for (int i = k + 1; i < n; ++i) {
            const double f = A[size_t(i) * n + k] / d;
            if (f == 0.0) continue;
            for (int j = k; j < n; ++j) A[size_t(i) * n + j] -= f * A[size_t(k) * n + j];
            for (int j = 0; j < cols; ++j) B[size_t(i) * cols + j] -= f * B[size_t(k) * cols + j];
        }
    }
    for (int i = n - 1; i >= 0; --i)
        for (int c = 0; c < cols; ++c) {
            double s = B[size_t(i) * cols + c];
            for (int j = i + 1; j < n; ++j) s -= A[size_t(i) * n + j] * B[size_t(j) * cols + c];
            B[size_t(i) * cols + c] = s / A[size_t(i) * n + i];
        }
    return true;
}

// World matrix of every joint of `rig` with `localRotation` (w,x,y,z) overriding the
// neutral local orientation of the joints in `rotated` (index -> delta quaternion
// composed after the neutral one).
std::vector<Aff> worldMatrices(const Rig& rig, const std::map<int, std::array<double, 4>>& deltas = {})
{
    const size_t n = rig.joints.size();
    std::vector<Aff> local(n), world(n);
    for (size_t j = 0; j < n; ++j) {
        std::array<double, 4> q = rig.joints[j].orientation;
        const auto it = deltas.find(int(j));
        if (it != deltas.end()) {
            const auto& d = it->second;
            q = {q[0] * d[0] - q[1] * d[1] - q[2] * d[2] - q[3] * d[3],
                 q[0] * d[1] + q[1] * d[0] + q[2] * d[3] - q[3] * d[2],
                 q[0] * d[2] - q[1] * d[3] + q[2] * d[0] + q[3] * d[1],
                 q[0] * d[3] + q[1] * d[2] - q[2] * d[1] + q[3] * d[0]};
        }
        local[j] = compose(rig.joints[j].translation, q, {1, 1, 1});
    }
    std::vector<char> done(n, 0);
    std::function<void(size_t)> go = [&](size_t j) {
        if (done[j]) return;
        done[j] = 1;
        const int p = rig.joints[j].parent;
        if (p < 0 || size_t(p) >= n || size_t(p) == j) { world[j] = local[j]; return; }
        go(size_t(p));
        world[j] = mul(world[size_t(p)], local[j]);
    };
    for (size_t j = 0; j < n; ++j) go(j);
    return world;
}

std::vector<std::vector<float>> skinPositions(const Rig& rig, const std::vector<Aff>& neutral,
                                              const std::vector<Aff>& posed)
{
    const size_t nj = rig.joints.size();
    std::vector<Aff> skinM(nj);
    for (size_t j = 0; j < nj; ++j) skinM[j] = mul(posed[j], inverse(neutral[j]));
    std::vector<std::vector<float>> out;
    for (const Mesh& m : rig.meshes) {
        std::vector<float> pos(m.positions);
        const size_t nv = m.positions.size() / 3;
        for (size_t v = 0; v < nv && v < m.skin.size(); ++v) {
            double sum = 0;
            for (const SkinInfluence& i : m.skin[v]) if (i.joint < nj) sum += i.weight;
            if (sum <= 1e-12) continue;
            const double px = m.positions[v * 3], py = m.positions[v * 3 + 1], pz = m.positions[v * 3 + 2];
            double ox = 0, oy = 0, oz = 0;
            for (const SkinInfluence& i : m.skin[v]) {
                if (i.joint >= nj) continue;
                const double w = i.weight / sum;
                const Aff& M = skinM[i.joint];
                ox += w * (M.a[0] * px + M.a[1] * py + M.a[2] * pz + M.t[0]);
                oy += w * (M.a[3] * px + M.a[4] * py + M.a[5] * pz + M.t[1]);
                oz += w * (M.a[6] * px + M.a[7] * py + M.a[8] * pz + M.t[2]);
            }
            pos[v * 3] = float(ox); pos[v * 3 + 1] = float(oy); pos[v * 3 + 2] = float(oz);
        }
        out.push_back(std::move(pos));
    }
    return out;
}

}  // namespace

// ---- warp ---------------------------------------------------------------------

V3 Warp::displacement(const V3& p) const
{
    V3 d = constant;
    for (int a = 0; a < 3; ++a)
        for (int c = 0; c < 3; ++c) d[size_t(c)] += linear[size_t(a)][size_t(c)] * p[size_t(a)];
    for (size_t k = 0; k < centres.size(); ++k) {
        const double r = dist(p, centres[k]);
        for (int c = 0; c < 3; ++c) d[size_t(c)] += weights[k][size_t(c)] * r;
    }
    return d;
}

Warp buildWarp(const std::vector<V3>& source, const std::vector<V3>& target, const Options& opt, QString* error)
{
    Warp w;
    auto fail = [&](const QString& m) { if (error) *error = m; return w; };
    if (source.size() != target.size()) return fail(QStringLiteral("source and target point counts differ"));
    if (source.size() < 8) return fail(QStringLiteral("need at least 8 matched vertices to fit a warp"));

    V3 lo = source[0], hi = source[0];
    for (const V3& p : source)
        for (int a = 0; a < 3; ++a) { lo[size_t(a)] = std::min(lo[size_t(a)], p[size_t(a)]); hi[size_t(a)] = std::max(hi[size_t(a)], p[size_t(a)]); }
    const double diag = dist(lo, hi);
    if (diag < 1e-9) return fail(QStringLiteral("the source mesh is degenerate (zero extent)"));

    // Voxel-subsample: average the points (and displacements) of each occupied cell.
    struct Acc { V3 p{}, d{}; int n = 0; };
    std::map<std::array<long long, 3>, Acc> cells;
    double cell = diag / std::cbrt(double(std::max(8, opt.maxControlPoints)) * 1.4);
    for (int attempt = 0; attempt < 40; ++attempt) {
        cells.clear();
        for (size_t i = 0; i < source.size(); ++i) {
            const std::array<long long, 3> key{(long long)std::floor((source[i][0] - lo[0]) / cell),
                                               (long long)std::floor((source[i][1] - lo[1]) / cell),
                                               (long long)std::floor((source[i][2] - lo[2]) / cell)};
            Acc& a = cells[key];
            for (int c = 0; c < 3; ++c) {
                a.p[size_t(c)] += source[i][size_t(c)];
                a.d[size_t(c)] += target[i][size_t(c)] - source[i][size_t(c)];
            }
            ++a.n;
        }
        if (int(cells.size()) <= opt.maxControlPoints) break;
        cell *= 1.15;
    }
    for (const auto& kv : cells) {
        const Acc& a = kv.second;
        w.centres.push_back({a.p[0] / a.n, a.p[1] / a.n, a.p[2] / a.n});
        w.weights.push_back({a.d[0] / a.n, a.d[1] / a.n, a.d[2] / a.n});   // displacement for now
    }
    const int n = int(w.centres.size());
    if (n < 5) return fail(QStringLiteral("too few distinct control points (%1)").arg(n));

    // [ Phi + ridge  P ] [w]   [d]
    // [ P^T          0 ] [a] = [0],   phi(r) = r, P = [1 x y z]
    const int m = n + 4;
    std::vector<double> A(size_t(m) * m, 0.0), B(size_t(m) * 3, 0.0);
    const double ridge = opt.regularisation * diag;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j)
            A[size_t(i) * m + j] = dist(w.centres[size_t(i)], w.centres[size_t(j)]);
        A[size_t(i) * m + i] += ridge;
        A[size_t(i) * m + n] = 1.0;
        A[size_t(n) * m + i] = 1.0;
        for (int a = 0; a < 3; ++a) {
            A[size_t(i) * m + n + 1 + a] = w.centres[size_t(i)][size_t(a)];
            A[size_t(n + 1 + a) * m + i] = w.centres[size_t(i)][size_t(a)];
        }
        for (int c = 0; c < 3; ++c) B[size_t(i) * 3 + c] = w.weights[size_t(i)][size_t(c)];
    }
    if (!solveDense(A, B, m, 3)) return fail(QStringLiteral(
        "the control points are degenerate (coplanar or collinear) — the warp cannot be solved"));
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) w.weights[size_t(i)][size_t(c)] = B[size_t(i) * 3 + c];
    for (int c = 0; c < 3; ++c) {
        w.constant[size_t(c)] = B[size_t(n) * 3 + c];
        for (int a = 0; a < 3; ++a) w.linear[size_t(a)][size_t(c)] = B[size_t(n + 1 + a) * 3 + c];
    }
    w.valid = true;
    return w;
}

// ---- fit ----------------------------------------------------------------------

Result fit(const Rig& rig, const std::vector<std::vector<float>>& target, const Options& opt)
{
    Result r;
    if (target.size() != rig.meshes.size()) {
        r.error = QStringLiteral("target has %1 mesh arrays but the rig has %2 meshes").arg(target.size()).arg(rig.meshes.size());
        return r;
    }
    std::vector<V3> src, dst;
    for (size_t m = 0; m < rig.meshes.size(); ++m) {
        if (target[m].empty()) continue;
        if (target[m].size() != rig.meshes[m].positions.size()) {
            r.error = QStringLiteral("mesh '%1': target has %2 vertices, the rig has %3 — the topology must match "
                                     "vertex-for-vertex (arbitrary topology is not supported)")
                          .arg(rig.meshes[m].name).arg(target[m].size() / 3).arg(rig.meshes[m].positions.size() / 3);
            return r;
        }
        const auto& s = rig.meshes[m].positions;
        for (size_t v = 0; v + 2 < s.size(); v += 3) {
            src.push_back({s[v], s[v + 1], s[v + 2]});
            dst.push_back({target[m][v], target[m][v + 1], target[m][v + 2]});
        }
    }
    if (src.empty()) { r.error = QStringLiteral("the target contains no vertices"); return r; }
    for (const V3& p : dst)
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
            r.error = QStringLiteral("the target contains non-finite positions");
            return r;
        }

    QString err;
    r.warp = buildWarp(src, dst, opt, &err);
    if (!r.warp.valid) { r.error = err; return r; }
    r.controlPoints = int(r.warp.centres.size());
    r.verticesMatched = int(src.size());

    // Vertex statistics and how faithfully the smooth warp reproduces the edit.
    double sum = 0, mx = 0, resid = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        const double shift = dist(src[i], dst[i]);
        sum += shift; mx = std::max(mx, shift);
        const V3 d = r.warp.displacement(src[i]);
        const V3 got{src[i][0] + d[0], src[i][1] + d[1], src[i][2] + d[2]};
        resid = std::max(resid, dist(got, dst[i]));
    }
    r.meanVertexShiftMm = sum / double(src.size()) * opt.unitToMillimetres;
    r.maxVertexShiftMm = mx * opt.unitToMillimetres;
    r.warpResidualMm = resid * opt.unitToMillimetres;

    // Move the joints through the warp, keeping every world ORIENTATION.
    r.fitted = rig;
    const std::vector<Aff> world = worldMatrices(rig);
    const size_t nj = rig.joints.size();
    std::vector<V3> oldPos(nj), newPos(nj);
    r.jointShift.resize(nj);
    double jsum = 0, jmax = 0;
    for (size_t j = 0; j < nj; ++j) {
        oldPos[j] = {world[j].t[0], world[j].t[1], world[j].t[2]};
        const V3 d = r.warp.displacement(oldPos[j]);
        newPos[j] = {oldPos[j][0] + d[0], oldPos[j][1] + d[1], oldPos[j][2] + d[2]};
        r.jointShift[j] = d;
        const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        jsum += len; jmax = std::max(jmax, len);
    }
    for (size_t j = 0; j < nj; ++j) {
        const int p = rig.joints[j].parent;
        if (p < 0 || size_t(p) >= nj || size_t(p) == j) {
            r.fitted.joints[j].translation = newPos[j];     // a root: local == world
            continue;
        }
        // local = Rparent^-1 * (newWorld - newWorldParent); the parent's world rotation is unchanged.
        const Aff& P = world[size_t(p)];
        const V3 dlt{newPos[j][0] - newPos[size_t(p)][0], newPos[j][1] - newPos[size_t(p)][1],
                     newPos[j][2] - newPos[size_t(p)][2]};
        // P.a is orthonormal here (no scale), so its inverse is its transpose.
        r.fitted.joints[j].translation = {P.a[0] * dlt[0] + P.a[3] * dlt[1] + P.a[6] * dlt[2],
                                          P.a[1] * dlt[0] + P.a[4] * dlt[1] + P.a[7] * dlt[2],
                                          P.a[2] * dlt[0] + P.a[5] * dlt[1] + P.a[8] * dlt[2]};
    }
    r.meanJointShiftMm = nj ? jsum / double(nj) * opt.unitToMillimetres : 0;
    r.maxJointShiftMm = jmax * opt.unitToMillimetres;

    // The bind pose becomes the target; meshes outside the field follow the warp.
    for (size_t m = 0; m < rig.meshes.size(); ++m) {
        auto& pos = r.fitted.meshes[m].positions;
        if (!target[m].empty()) { pos = target[m]; continue; }
        for (size_t v = 0; v + 2 < pos.size(); v += 3) {
            const V3 d = r.warp.displacement({pos[v], pos[v + 1], pos[v + 2]});
            pos[v] += float(d[0]); pos[v + 1] += float(d[1]); pos[v + 2] += float(d[2]);
        }
        r.warnings << QStringLiteral("mesh '%1' was not part of the target; it follows the warp").arg(rig.meshes[m].name);
    }
    if (r.warpResidualMm > 5.0)
        r.warnings << QStringLiteral("the smooth warp misses some vertices by up to %1 mm (a local, non-smooth edit); "
                                     "joints follow the smooth part only").arg(r.warpResidualMm, 0, 'f', 1);
    r.ok = true;
    return r;
}

// ---- correspondence -----------------------------------------------------------

std::vector<size_t> importedMeshIndices(const Rig& rig, bool flipV)
{
    std::vector<size_t> out;
    for (size_t m = 0; m < rig.meshes.size(); ++m)
        if (!expand(rig.meshes[m], flipV).indices.empty()) out.push_back(m);
    return out;
}

Correspondence positionsFromExpanded(const Rig& rig, const std::vector<std::vector<float>>& expanded, bool flipV,
                                     double unitToMm)
{
    Correspondence c;
    const std::vector<size_t> imported = importedMeshIndices(rig, flipV);
    if (expanded.size() != imported.size()) {
        c.error = QStringLiteral("the target has %1 submeshes but the character imports as %2 — it must keep the "
                                 "imported submesh structure").arg(expanded.size()).arg(imported.size());
        return c;
    }
    c.positions.assign(rig.meshes.size(), {});
    double worst = 0;
    for (size_t s = 0; s < imported.size(); ++s) {
        const Mesh& m = rig.meshes[imported[s]];
        const ExpandedMesh ex = expand(m, flipV);
        if (expanded[s].size() != ex.positionOf.size() * 3) {
            c.error = QStringLiteral("submesh %1 ('%2') has %3 vertices, the character's has %4 — the vertex count "
                                     "and order must be unchanged (topology edits are not supported)")
                          .arg(s).arg(m.name).arg(expanded[s].size() / 3).arg(ex.positionOf.size());
            return c;
        }
        const size_t np = m.positions.size() / 3;
        std::vector<double> sum(np * 3, 0.0);
        std::vector<int> cnt(np, 0);
        std::vector<float> first(np * 3, 0.f);
        std::vector<char> have(np, 0);
        double meshWorst = 0;
        for (size_t v = 0; v < ex.positionOf.size(); ++v) {
            const size_t p = ex.positionOf[v];
            if (p >= np) continue;
            if (have[p]) {
                const double dx = expanded[s][v * 3] - first[p * 3], dy = expanded[s][v * 3 + 1] - first[p * 3 + 1],
                             dz = expanded[s][v * 3 + 2] - first[p * 3 + 2];
                meshWorst = std::max(meshWorst, std::sqrt(dx * dx + dy * dy + dz * dz));
            } else {
                have[p] = 1;
                for (int k = 0; k < 3; ++k) first[p * 3 + size_t(k)] = expanded[s][v * 3 + size_t(k)];
            }
            for (int k = 0; k < 3; ++k) sum[p * 3 + size_t(k)] += expanded[s][v * 3 + size_t(k)];
            ++cnt[p];
        }
        worst = std::max(worst, meshWorst);
        auto& out = c.positions[imported[s]];
        out.assign(np * 3, 0.f);
        for (size_t p = 0; p < np; ++p) {
            if (!cnt[p]) {   // a position no face uses keeps its source value
                for (int k = 0; k < 3; ++k) out[p * 3 + size_t(k)] = m.positions[p * 3 + size_t(k)];
                continue;
            }
            for (int k = 0; k < 3; ++k) out[p * 3 + size_t(k)] = float(sum[p * 3 + size_t(k)] / cnt[p]);
        }
    }
    c.maxSeamSplitMm = worst * unitToMm;
    c.ok = true;
    return c;
}

Correspondence matchImported(const Rig& rig, const std::vector<ImportedSubmesh>& subs, bool flipV, double unitToMm)
{
    Correspondence c;
    const std::vector<size_t> imported = importedMeshIndices(rig, flipV);
    if (subs.size() != imported.size()) {
        c.error = QStringLiteral("the target has %1 submeshes but the character imports as %2 — it must keep the "
                                 "imported submesh structure").arg(subs.size()).arg(imported.size());
        return c;
    }
    bool anyUv = false;
    for (const auto& s : subs) if (!s.uvs.empty()) anyUv = true;
    if (!anyUv) {   // no UVs to match on: fall back to vertex order
        std::vector<std::vector<float>> pos;
        for (const auto& s : subs) pos.push_back(s.positions);
        return positionsFromExpanded(rig, pos, flipV, unitToMm);
    }

    c.positions.assign(rig.meshes.size(), {});
    c.matchedByUv = true;
    long long total = 0, matched = 0;
    double worstSeam = 0;
    for (size_t s = 0; s < subs.size(); ++s) {
        const Mesh& m = rig.meshes[imported[s]];
        const ExpandedMesh ex = expand(m, flipV);
        const size_t nsrc = ex.positionOf.size(), ntgt = subs[s].positions.size() / 3;
        if (subs[s].uvs.size() != ntgt * 2) {
            c.error = QStringLiteral("submesh %1 ('%2') has no UVs to match on").arg(s).arg(m.name);
            return c;
        }
        // Source UV hash. Quantise, then also probe the neighbouring cells so a value
        // sitting on a bucket edge still matches.
        constexpr double q = 2e4;    // 5e-5 cells
        auto cell = [&](double u, double v) { return std::make_pair((long long)std::floor(u * q), (long long)std::floor(v * q)); };
        std::map<std::pair<long long, long long>, std::vector<uint32_t>> grid;
        for (size_t v = 0; v < nsrc; ++v) grid[cell(ex.uvs[v * 2], ex.uvs[v * 2 + 1])].push_back(uint32_t(v));

        auto candidates = [&](double u, double v, std::vector<uint32_t>& out) {
            out.clear();
            const auto k = cell(u, v);
            for (long long du = -1; du <= 1; ++du)
                for (long long dv = -1; dv <= 1; ++dv) {
                    const auto it = grid.find({k.first + du, k.second + dv});
                    if (it == grid.end()) continue;
                    for (uint32_t sv : it->second)
                        if (std::abs(ex.uvs[sv * 2] - u) < 1.5e-4 && std::abs(ex.uvs[sv * 2 + 1] - v) < 1.5e-4) out.push_back(sv);
                }
        };
        // Which V convention does the target use? Score a sample of both.
        auto score = [&](bool invert) {
            int hits = 0, tries = 0;
            std::vector<uint32_t> cand;
            for (size_t t = 0; t < ntgt && tries < 3000; t += std::max<size_t>(1, ntgt / 3000), ++tries) {
                double v = subs[s].uvs[t * 2 + 1];
                if (invert) v = 1.0 - v;
                candidates(subs[s].uvs[t * 2], v, cand);
                if (!cand.empty()) ++hits;
            }
            return tries ? double(hits) / tries : 0.0;
        };
        const double plain = score(false), inv = score(true);
        const bool invert = inv > plain;
        if (invert) c.flippedV = true;

        const size_t np = m.positions.size() / 3;
        std::vector<double> sum(np * 3, 0.0);
        std::vector<int> cnt(np, 0);
        std::vector<float> first(np * 3, 0.f);
        std::vector<char> have(np, 0);
        std::vector<uint32_t> cand;
        for (size_t t = 0; t < ntgt; ++t) {
            ++total;
            double v = subs[s].uvs[t * 2 + 1];
            if (invert) v = 1.0 - v;
            candidates(subs[s].uvs[t * 2], v, cand);
            if (cand.empty()) continue;
            ++matched;
            const float* tp = &subs[s].positions[t * 3];
            uint32_t best = cand[0];
            if (cand.size() > 1) {
                ++c.ambiguousVertices;
                double bd = 1e300;
                for (uint32_t sv : cand) {
                    const double dx = ex.positions[sv * 3] - tp[0], dy = ex.positions[sv * 3 + 1] - tp[1], dz = ex.positions[sv * 3 + 2] - tp[2];
                    const double d = dx * dx + dy * dy + dz * dz;
                    if (d < bd) { bd = d; best = sv; }
                }
            }
            const size_t p = ex.positionOf[best];
            if (p >= np) continue;
            if (have[p]) {
                const double dx = tp[0] - first[p * 3], dy = tp[1] - first[p * 3 + 1], dz = tp[2] - first[p * 3 + 2];
                worstSeam = std::max(worstSeam, std::sqrt(dx * dx + dy * dy + dz * dz));
            } else {
                have[p] = 1;
                for (int k = 0; k < 3; ++k) first[p * 3 + size_t(k)] = tp[k];
            }
            for (int k = 0; k < 3; ++k) sum[p * 3 + size_t(k)] += tp[k];
            ++cnt[p];
        }
        auto& out = c.positions[imported[s]];
        out.assign(np * 3, 0.f);
        for (size_t p = 0; p < np; ++p) {
            if (!cnt[p]) {
                for (int k = 0; k < 3; ++k) out[p * 3 + size_t(k)] = m.positions[p * 3 + size_t(k)];
                continue;
            }
            for (int k = 0; k < 3; ++k) out[p * 3 + size_t(k)] = float(sum[p * 3 + size_t(k)] / cnt[p]);
        }
    }
    c.matchedFraction = total ? double(matched) / double(total) : 0.0;
    c.maxSeamSplitMm = worstSeam * unitToMm;
    if (c.matchedFraction < 0.995) {
        c.error = QStringLiteral("only %1 % of the target's vertices could be matched to the character by UV — the UVs or the "
                                 "topology were changed, which is not supported (the target must be a position-only edit of the "
                                 "imported character)").arg(c.matchedFraction * 100.0, 0, 'f', 1);
        return c;
    }
    c.ok = true;
    return c;
}

// ---- pose quality -------------------------------------------------------------

QList<PoseSpec> referencePoses()
{
    auto P = [](const char* name, std::initializer_list<QPair<QString, std::array<double, 3>>> rot) {
        PoseSpec s;
        s.name = QString::fromLatin1(name);
        for (const auto& r : rot) s.rotations << r;
        return s;
    };
    using R = QPair<QString, std::array<double, 3>>;
    return {
        P("knee_l_100", {R{"calf_l", {0, 0, -100}}}),
        P("hip_l_60", {R{"thigh_l", {0, -60, 0}}}),
        P("hip_knee_l", {R{"thigh_l", {0, -60, 0}}, R{"calf_l", {0, 0, -100}}}),
        P("shoulder_l_80", {R{"upperarm_l", {0, 0, -80}}}),
        P("shoulder_forward_l_70", {R{"upperarm_l", {0, 70, 0}}}),
        P("elbow_l_90", {R{"lowerarm_l", {0, 0, -90}}}),
        P("knee_r_100", {R{"calf_r", {0, 0, -100}}}),
        P("shoulder_r_80", {R{"upperarm_r", {0, 0, 80}}}),
    };
}

std::vector<std::vector<float>> posedPositions(const Rig& rig, const PoseSpec& pose, QStringList* missing)
{
    std::map<int, std::array<double, 4>> deltas;
    for (const auto& r : pose.rotations) {
        int idx = -1;
        for (size_t j = 0; j < rig.joints.size(); ++j)
            if (rig.joints[j].name == r.first) { idx = int(j); break; }
        if (idx < 0) { if (missing && !missing->contains(r.first)) missing->append(r.first); continue; }
        deltas[idx] = eulerToQuaternion(r.second, QStringLiteral("xyz"), true);
    }
    return skinPositions(rig, worldMatrices(rig), worldMatrices(rig, deltas));
}

namespace {
// Unique position-index edges of every mesh (a, b, mesh).
struct Edge { size_t mesh; uint32_t a, b; };
std::vector<Edge> collectEdges(const Rig& rig)
{
    std::vector<Edge> edges;
    for (size_t m = 0; m < rig.meshes.size(); ++m) {
        const Mesh& mesh = rig.meshes[m];
        std::vector<std::pair<uint32_t, uint32_t>> e;
        for (const auto& f : mesh.faces) {
            for (size_t i = 0; i < f.size(); ++i) {
                const uint32_t la = f[i], lb = f[(i + 1) % f.size()];
                if (la >= mesh.layouts.size() || lb >= mesh.layouts.size()) continue;
                uint32_t a = mesh.layouts[la].position, b = mesh.layouts[lb].position;
                if (a == b) continue;
                if (a > b) std::swap(a, b);
                e.push_back({a, b});
            }
        }
        std::sort(e.begin(), e.end());
        e.erase(std::unique(e.begin(), e.end()), e.end());
        for (const auto& pr : e) edges.push_back({m, pr.first, pr.second});
    }
    return edges;
}
double edgeLen(const std::vector<std::vector<float>>& pos, const Edge& e)
{
    const auto& p = pos[e.mesh];
    if (size_t(e.a) * 3 + 2 >= p.size() || size_t(e.b) * 3 + 2 >= p.size()) return 0.0;
    const double dx = p[e.a * 3] - p[e.b * 3], dy = p[e.a * 3 + 1] - p[e.b * 3 + 1], dz = p[e.a * 3 + 2] - p[e.b * 3 + 2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}  // namespace

std::vector<PoseReport> evaluatePoses(const Rig& original, const Rig& fitted, const QList<PoseSpec>& poses,
                                      double uniformScale, const Options& opt)
{
    std::vector<PoseReport> out;
    const std::vector<Edge> edges = collectEdges(original);
    std::vector<std::vector<float>> bindO, bindF;
    for (const Mesh& m : original.meshes) bindO.push_back(m.positions);
    for (const Mesh& m : fitted.meshes) bindF.push_back(m.positions);
    for (const PoseSpec& pose : poses) {
        PoseReport rep;
        rep.name = pose.name;
        QStringList missing;
        const auto po = posedPositions(original, pose, &missing);
        const auto pf = posedPositions(fitted, pose, nullptr);
        rep.joints = missing.isEmpty();
        double mv = 0;
        for (size_t m = 0; m < pf.size(); ++m)
            for (size_t i = 0; i + 2 < pf[m].size(); i += 3) {
                const double dx = pf[m][i] - bindF[m][i], dy = pf[m][i + 1] - bindF[m][i + 1], dz = pf[m][i + 2] - bindF[m][i + 2];
                mv = std::max(mv, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        rep.maxMovementMm = mv * opt.unitToMillimetres;

        std::vector<double> diffs;
        diffs.reserve(edges.size());
        for (const Edge& e : edges) {
            const double bo = edgeLen(bindO, e), bf = edgeLen(bindF, e);
            if (bo < 1e-7 || bf < 1e-7) continue;
            diffs.push_back(std::abs(edgeLen(pf, e) / bf - edgeLen(po, e) / bo));
        }
        if (!diffs.empty()) {
            std::sort(diffs.begin(), diffs.end());
            rep.stretchMaxDelta = diffs.back();
            rep.stretchP99Delta = diffs[size_t(double(diffs.size() - 1) * 0.99)];
        }
        if (uniformScale > 0.0) {
            double mx = 0, sum = 0; size_t cnt = 0;
            for (size_t m = 0; m < pf.size(); ++m)
                for (size_t i = 0; i + 2 < pf[m].size(); i += 3) {
                    double e2 = 0;
                    for (int k = 0; k < 3; ++k) {
                        const double d = pf[m][i + size_t(k)] - uniformScale * po[m][i + size_t(k)];
                        e2 += d * d;
                    }
                    const double e = std::sqrt(e2);
                    mx = std::max(mx, e); sum += e; ++cnt;
                }
            rep.similarityMaxMm = mx * opt.unitToMillimetres;
            rep.similarityMeanMm = cnt ? sum / double(cnt) * opt.unitToMillimetres : 0.0;
        }
        out.push_back(rep);
    }
    return out;
}

// ---- LOD propagation ----------------------------------------------------------

namespace {
struct Grid {
    double cell = 1;
    V3 lo{};
    std::unordered_map<long long, std::vector<V3>> cells;
    static long long key(long long x, long long y, long long z) { return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL); }
    void build(const std::vector<V3>& pts, double c)
    {
        cell = c;
        lo = pts.empty() ? V3{} : pts[0];
        for (const V3& p : pts) for (int a = 0; a < 3; ++a) lo[size_t(a)] = std::min(lo[size_t(a)], p[size_t(a)]);
        for (const V3& p : pts)
            cells[key((long long)std::floor((p[0] - lo[0]) / cell), (long long)std::floor((p[1] - lo[1]) / cell),
                      (long long)std::floor((p[2] - lo[2]) / cell))].push_back(p);
    }
    double nearest(const V3& q) const
    {
        const long long cx = (long long)std::floor((q[0] - lo[0]) / cell), cy = (long long)std::floor((q[1] - lo[1]) / cell),
                        cz = (long long)std::floor((q[2] - lo[2]) / cell);
        double best = 1e30;
        for (int ring = 0; ring < 12; ++ring) {
            for (long long x = cx - ring; x <= cx + ring; ++x)
                for (long long y = cy - ring; y <= cy + ring; ++y)
                    for (long long z = cz - ring; z <= cz + ring; ++z) {
                        if (std::max({std::llabs(x - cx), std::llabs(y - cy), std::llabs(z - cz)}) != ring) continue;
                        const auto it = cells.find(key(x, y, z));
                        if (it == cells.end()) continue;
                        for (const V3& p : it->second) best = std::min(best, dist(p, q));
                    }
            if (best < ring * cell) break;
        }
        return best;
    }
};
std::vector<V3> allPositions(const Rig& r)
{
    std::vector<V3> v;
    for (const Mesh& m : r.meshes)
        for (size_t i = 0; i + 2 < m.positions.size(); i += 3) v.push_back({m.positions[i], m.positions[i + 1], m.positions[i + 2]});
    return v;
}
}  // namespace

LodPropagation evaluateLodPropagation(const Rig& lod0Original, const Rig& lod0Fitted, const Rig& lodN,
                                      const Warp& warp, const Options& opt)
{
    LodPropagation lp;
    const std::vector<V3> fitted0 = allPositions(lod0Fitted), orig0 = allPositions(lod0Original), lodPts = allPositions(lodN);
    if (fitted0.empty() || lodPts.empty()) return lp;
    V3 lo = fitted0[0], hi = fitted0[0];
    for (const V3& p : fitted0) for (int a = 0; a < 3; ++a) { lo[size_t(a)] = std::min(lo[size_t(a)], p[size_t(a)]); hi[size_t(a)] = std::max(hi[size_t(a)], p[size_t(a)]); }
    const double cell = std::max(1e-6, dist(lo, hi) / 60.0);
    Grid gFit, gOrig;
    gFit.build(fitted0, cell);
    gOrig.build(orig0, cell);
    double sum = 0, mx = 0, base = 0;
    for (const V3& p : lodPts) {
        const V3 d = warp.displacement(p);
        const double gap = gFit.nearest({p[0] + d[0], p[1] + d[1], p[2] + d[2]});
        sum += gap; mx = std::max(mx, gap);
        base += gOrig.nearest(p);
    }
    lp.vertices = int(lodPts.size());
    lp.meanGapMm = sum / double(lodPts.size()) * opt.unitToMillimetres;
    lp.maxGapMm = mx * opt.unitToMillimetres;
    lp.baselineMeanGapMm = base / double(lodPts.size()) * opt.unitToMillimetres;
    return lp;
}

}  // namespace Fit
}  // namespace Dna
