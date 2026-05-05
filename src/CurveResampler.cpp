#include "CurveResampler.h"

#include "CurveEditModel.h"

#include <algorithm>
#include <cmath>

namespace CurveResampler {

namespace {

// Probe the curve at `samples` evenly-spaced points, return the peak
// |second derivative|. Used to pick the dense-sampling rate.
double peakCurvature(const CurveEditModel* model,
                     const QString& skeleton, const QString& anim,
                     const QString& bone, const QString& channel,
                     double t0, double t1,
                     const QVariantList& kfTimes,
                     const QVariantList& kfValues,
                     int samples)
{
    if (samples < 3 || t1 <= t0) return 0.0;
    const double dt = (t1 - t0) / (samples - 1);
    if (dt <= 0.0) return 0.0;
    double peak = 0.0;
    double prev = model->evaluate(skeleton, anim, bone, channel,
                                  t0, kfTimes, kfValues);
    double curr = model->evaluate(skeleton, anim, bone, channel,
                                  t0 + dt, kfTimes, kfValues);
    for (int i = 2; i < samples; ++i) {
        const double t = t0 + i * dt;
        const double next = model->evaluate(skeleton, anim, bone, channel,
                                            t, kfTimes, kfValues);
        const double d2 = (next - 2.0 * curr + prev) / (dt * dt);
        const double mag = std::fabs(d2);
        if (mag > peak) peak = mag;
        prev = curr;
        curr = next;
    }
    return peak;
}

// Linearly interpolate between (t0, v0) and (t1, v1) at time t.
double lerpAt(double t0, double v0, double t1, double v1, double t) {
    if (t1 == t0) return v0;
    const double u = (t - t0) / (t1 - t0);
    return v0 + (v1 - v0) * u;
}

// Douglas-Peucker simplification: keep first and last; recursively
// keep any interior point that deviates from the line between its
// kept neighbors by more than `tol`. Drops every point that's
// faithfully captured by linear interpolation between two anchors.
//
// Operates on `dense` (which includes both endpoints t0 and t1).
// Returns indices of the points to keep, in ascending order. The
// caller drops the t0 entry — only t0's anchor is preserved by the
// existing track keyframe; we only insert (t0, t1] samples.
void rdpRecurse(const std::vector<Sample>& dense,
                size_t lo, size_t hi, double tol,
                std::vector<size_t>& keepIdx)
{
    if (hi <= lo + 1) return;
    double maxDev = 0.0;
    size_t maxIdx = lo;
    for (size_t i = lo + 1; i < hi; ++i) {
        const double interp = lerpAt(dense[lo].time,  dense[lo].value,
                                     dense[hi].time,  dense[hi].value,
                                     dense[i].time);
        const double dev = std::fabs(dense[i].value - interp);
        if (dev > maxDev) { maxDev = dev; maxIdx = i; }
    }
    if (maxDev <= tol) return;
    keepIdx.push_back(maxIdx);
    rdpRecurse(dense, lo, maxIdx, tol, keepIdx);
    rdpRecurse(dense, maxIdx, hi, tol, keepIdx);
}

std::vector<Sample> simplify(const std::vector<Sample>& dense, double tol) {
    if (dense.size() < 3) return dense;
    std::vector<size_t> keepIdx{ 0, dense.size() - 1 };
    rdpRecurse(dense, 0, dense.size() - 1, tol, keepIdx);
    std::sort(keepIdx.begin(), keepIdx.end());
    std::vector<Sample> out;
    out.reserve(keepIdx.size());
    for (size_t i : keepIdx) out.push_back(dense[i]);
    return out;
}

} // namespace

std::vector<Sample> resampleSegment(const CurveEditModel* model,
                                    const QString& skeleton,
                                    const QString& anim,
                                    const QString& bone,
                                    const QString& channel,
                                    double t0, double t1,
                                    const QVariantList& kfTimes,
                                    const QVariantList& kfValues,
                                    double toleranceMul)
{
    std::vector<Sample> out;
    if (!model || t1 <= t0) return out;
    if (kfTimes.size() != kfValues.size() || kfTimes.isEmpty()) return out;

    const double curvature = peakCurvature(model, skeleton, anim, bone, channel,
                                           t0, t1, kfTimes, kfValues, 16);
    int hz = (curvature > kCurvatureEps) ? kBoostHz : kBaseHz;

    const double duration = t1 - t0;
    int sampleCount = static_cast<int>(std::ceil(duration * hz));
    if (sampleCount > kMaxSamples) {
        sampleCount = kMaxSamples;
        hz = static_cast<int>(std::round(sampleCount / duration));
    }
    if (sampleCount < 1) sampleCount = 1;

    // Build a dense pass that includes BOTH endpoints so the
    // simplifier can decide whether interior points add information
    // beyond linear interpolation between t0 and t1. The final return
    // strips the t0 entry (caller keeps the existing anchor).
    std::vector<Sample> dense;
    dense.reserve(static_cast<size_t>(sampleCount) + 1);
    dense.push_back({t0, model->evaluate(skeleton, anim, bone, channel,
                                          t0, kfTimes, kfValues)});
    const double step = duration / sampleCount;
    for (int i = 1; i <= sampleCount; ++i) {
        const double t = t0 + i * step;
        dense.push_back({t, model->evaluate(skeleton, anim, bone, channel,
                                             t, kfTimes, kfValues)});
    }

    // Tolerance: 0.5% of the segment's value range, with a 1e-3 floor
    // so flat-ish curves still simplify cleanly. Linear segments
    // collapse to {t0, t1} → caller drops t0 → empty output, and Ogre's
    // default interpolation between the existing anchors matches the
    // curve exactly. Stepped segments retain dense samples around the
    // discontinuity.
    double minV = dense.front().value, maxV = minV;
    for (const auto& s : dense) {
        if (s.value < minV) minV = s.value;
        if (s.value > maxV) maxV = s.value;
    }
    const double tol = std::max(kSimplifyFloor * toleranceMul,
                                 kSimplifyRel * toleranceMul * (maxV - minV));

    auto kept = simplify(dense, tol);

    // Drop the t0 entry — caller's anchor keyframe stays put.
    out.reserve(kept.size());
    for (size_t i = 1; i < kept.size(); ++i) out.push_back(kept[i]);
    return out;
}

} // namespace CurveResampler
