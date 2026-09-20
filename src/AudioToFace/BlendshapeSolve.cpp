#include "BlendshapeSolve.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace AudioToFace {

namespace {

// Projected gradient descent with box constraints.
//
// Why not a normal-equation solve: the constraint 0 <= w <= 1 is not optional
// here. An unconstrained fit happily returns a weight of -0.4 or 2.3, and a
// negative blendshape weight is not a pose — it is the shape inverted, which
// renders as the face turning inside out at that vertex. Clamping AFTER an
// unconstrained solve is worse than useless: the clamped vector is no longer
// the least-squares answer, so the error lands somewhere unpredictable.
//
// The L1 term also rules out a plain linear solve, since |w| is not
// differentiable at zero. Projected gradient handles both naturally: the
// gradient step ignores the constraint, the projection restores it, and the
// L1 subgradient is well defined away from zero (at zero the projection pins
// it, which is the behaviour wanted anyway).
//
// The system is tiny (52 unknowns), so iteration count is not a concern; the
// per-frame cost is dominated by forming Dᵀdv, not by the iterations.
double dot(const double* a, const double* b, size_t n)
{
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += a[i] * b[i];
    return s;
}

}  // namespace

void PoseBasis::build(const std::vector<PoseDelta>& poses)
{
    m_poseCount = 0;
    m_valueCount = 0;
    m_gram.clear();
    m_owned.clear();
    if (poses.empty()) return;

    // Every pose must describe the same mesh; a ragged basis means the caller
    // mixed characters or mis-parsed the archive, and silently using the
    // shortest would fit garbage.
    const size_t vc = poses.front().size();
    if (vc == 0) return;
    for (const auto& p : poses)
        if (p.size() != vc) return;

    m_owned = poses;
    m_poseCount = poses.size();
    m_valueCount = vc;

    // Gram matrix Dᵀ D. Symmetric, so only half is computed.
    m_gram.assign(m_poseCount * m_poseCount, 0.0);
    for (size_t i = 0; i < m_poseCount; ++i) {
        const float* pi = m_owned[i].data();
        for (size_t j = i; j < m_poseCount; ++j) {
            const float* pj = m_owned[j].data();
            double s = 0.0;
            for (size_t k = 0; k < vc; ++k) s += double(pi[k]) * double(pj[k]);
            m_gram[i * m_poseCount + j] = s;
            m_gram[j * m_poseCount + i] = s;
        }
    }
}

SolveResult PoseBasis::solve(const std::vector<float>& deltaVertices,
                             const std::vector<float>& previous,
                             const SolveOptions& options) const
{
    SolveResult r;
    if (!valid() || deltaVertices.size() != m_valueCount) {
        // Not an error: an unusable basis or a mismatched frame yields a
        // neutral face, which is the safe degradation for a lipsync take.
        r.weights.assign(m_poseCount, 0.0f);
        return r;
    }

    const size_t n = m_poseCount;
    const auto& poses = m_owned;

    // Dᵀ dv — the only per-frame pass over the full vertex count.
    std::vector<double> dtv(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const float* pi = poses[i].data();
        double s = 0.0;
        for (size_t k = 0; k < m_valueCount; ++k) s += double(pi[k]) * double(deltaVertices[k]);
        dtv[i] = s;
    }

    const bool hasPrev = previous.size() == n;
    auto isActive = [&](size_t i) {
        return options.active.empty() || (i < options.active.size() && options.active[i] != 0);
    };
    auto partner = [&](size_t i) -> int {
        if (i >= options.symmetryPartner.size()) return -1;
        const int p = options.symmetryPartner[i];
        return (p >= 0 && size_t(p) < n && size_t(p) != i) ? p : -1;
    };

    // Step size = 1 / L, where L bounds the gradient's Lipschitz constant.
    //
    // Two things this must get right, both of which were wrong at first and
    // showed up as the solve under-shooting (0.4 recovered where 0.7 was the
    // answer) rather than as anything obviously broken:
    //
    //  * the gradient carries a factor of 2 (d/dw of |Dw-t|² is 2Dᵀ(Dw-t)),
    //    so L is 2*lambda_max, not lambda_max;
    //  * the max DIAGONAL of the Gram matrix is not a bound on its largest
    //    eigenvalue. Gershgorin's circle theorem gives a real one: every
    //    eigenvalue lies within |row sum of absolute values| of the diagonal,
    //    so max over rows of Σ|gram[i][j]| bounds lambda_max.
    //
    // A too-large step oscillates and the projection pins the result short of
    // the optimum — which reads as a plausible-but-wrong weight, the worst
    // kind of failure.
    double lmax = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double rowSum = 0.0;
        for (size_t j = 0; j < n; ++j) rowSum += std::abs(m_gram[i * n + j]);
        lmax = std::max(lmax, rowSum);
    }
    const double L = 2.0 * (lmax + options.l2 + options.temporal + options.symmetry);
    const double step = (L > 0.0) ? 1.0 / L : 0.0;
    if (step <= 0.0) {
        r.weights.assign(n, 0.0f);
        return r;
    }

    // Warm-start from the previous frame: consecutive speech frames are
    // similar, so this converges in a handful of iterations instead of from
    // scratch, and it biases toward temporal continuity before the temporal
    // term even applies.
    std::vector<double> w(n, 0.0);
    if (hasPrev)
        for (size_t i = 0; i < n; ++i) w[i] = double(previous[i]);
    for (size_t i = 0; i < n; ++i)
        if (!isActive(i)) w[i] = 0.0;

    std::vector<double> grad(n, 0.0);
    for (int it = 0; it < options.maxIterations; ++it) {
        // grad = 2 (Gram w - Dᵀdv) + 2 l2 w + l1 sign(w)
        //        + 2 temporal (w - wPrev) + 2 symmetry (w - wPartner)
        for (size_t i = 0; i < n; ++i) {
            if (!isActive(i)) { grad[i] = 0.0; continue; }
            double g = 2.0 * (dot(&m_gram[i * n], w.data(), n) - dtv[i]);
            g += 2.0 * options.l2 * w[i];
            if (w[i] > 0.0) g += options.l1;           // subgradient; at w=0 the
                                                       // projection pins it anyway
            if (hasPrev) g += 2.0 * options.temporal * (w[i] - double(previous[i]));
            const int p = partner(i);
            if (p >= 0 && isActive(size_t(p)))
                g += 2.0 * options.symmetry * (w[i] - w[size_t(p)]);
            grad[i] = g;
        }

        double maxMove = 0.0;
        for (size_t i = 0; i < n; ++i) {
            if (!isActive(i)) continue;
            const double next = std::min(1.0, std::max(0.0, w[i] - step * grad[i]));
            maxMove = std::max(maxMove, std::abs(next - w[i]));
            w[i] = next;
        }
        r.iterations = it + 1;
        if (maxMove < options.tolerance) { r.converged = true; break; }
    }

    // Residual against the ORIGINAL target, before multipliers/offsets skew it.
    double num = 0.0, den = 0.0;
    for (size_t k = 0; k < m_valueCount; ++k) {
        double fit = 0.0;
        for (size_t i = 0; i < n; ++i)
            if (w[i] != 0.0) fit += w[i] * double(poses[i][k]);
        const double d = double(deltaVertices[k]);
        num += (fit - d) * (fit - d);
        den += d * d;
    }
    r.residual = (den > 0.0) ? std::sqrt(num / den) : 0.0;

    r.weights.resize(n);
    for (size_t i = 0; i < n; ++i) {
        double v = w[i];
        if (i < options.multipliers.size()) v *= double(options.multipliers[i]);
        if (i < options.offsets.size())     v += double(options.offsets[i]);
        r.weights[i] = float(std::min(1.0, std::max(0.0, v)));
    }
    return r;
}

SolveResult solveBlendshapeWeights(const std::vector<PoseDelta>& poses,
                                   const std::vector<float>& deltaVertices,
                                   const std::vector<float>& previous,
                                   const SolveOptions& options)
{
    PoseBasis basis;
    basis.build(poses);
    return basis.solve(deltaVertices, previous, options);
}

}  // namespace AudioToFace
