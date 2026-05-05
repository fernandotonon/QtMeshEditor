#include "CurveResampler.h"

#include "CurveEditModel.h"

#include <cmath>

namespace CurveResampler {

namespace {

// Probe the curve at `samples` evenly-spaced points, return the peak
// |second derivative|. Used to decide whether to bump from 30 Hz to
// 60 Hz over this segment.
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

} // namespace

std::vector<Sample> resampleSegment(const CurveEditModel* model,
                                    const QString& skeleton,
                                    const QString& anim,
                                    const QString& bone,
                                    const QString& channel,
                                    double t0, double t1,
                                    const QVariantList& kfTimes,
                                    const QVariantList& kfValues)
{
    std::vector<Sample> out;
    if (!model || t1 <= t0) return out;
    if (kfTimes.size() != kfValues.size() || kfTimes.isEmpty()) return out;

    // Decide sample rate: probe with 16 points; bump to kBoostHz when
    // peak |d²/dt²| crosses kCurvatureEps. Sharp peaks (e.g. Bezier
    // overshoot or near-stepped transitions) get the denser rate.
    const double curvature = peakCurvature(model, skeleton, anim, bone, channel,
                                           t0, t1, kfTimes, kfValues, 16);
    int hz = (curvature > kCurvatureEps) ? kBoostHz : kBaseHz;

    // Honor the segment-size cap: a long segment at high Hz can exceed
    // 200 samples. Walk the cap back to fit.
    const double duration = t1 - t0;
    int sampleCount = static_cast<int>(std::ceil(duration * hz));
    if (sampleCount > kMaxSamples) {
        sampleCount = kMaxSamples;
        hz = static_cast<int>(std::round(sampleCount / duration));
    }
    if (sampleCount < 1) sampleCount = 1;

    // Emit `sampleCount` interior samples + the closing endpoint t1.
    // Skip t0 — the caller keeps the existing start keyframe. Step
    // size lays the samples uniformly across (t0, t1].
    out.reserve(static_cast<size_t>(sampleCount) + 1);
    const double step = duration / sampleCount;
    for (int i = 1; i <= sampleCount; ++i) {
        const double t = t0 + i * step;
        const double v = model->evaluate(skeleton, anim, bone, channel,
                                         t, kfTimes, kfValues);
        out.push_back({t, v});
    }
    return out;
}

} // namespace CurveResampler
