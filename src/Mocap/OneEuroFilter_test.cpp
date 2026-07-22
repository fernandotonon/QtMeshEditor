#ifdef ENABLE_MOCAP

#include <gtest/gtest.h>

#include <cmath>

#include "Mocap/OneEuroFilter.h"

TEST(OneEuroFilter, FirstSamplePassesThrough)
{
    OneEuroFilter f;
    EXPECT_DOUBLE_EQ(f.filter(0.7, 0.0), 0.7);
}

TEST(OneEuroFilter, StepResponseSmooths)
{
    OneEuroFilter f(OneEuroFilter::Params{1.0, 0.0, 1.0});  // beta 0: pure low-pass
    f.filter(0.0, 0.0);
    const double stepped = f.filter(1.0, 1.0 / 30.0);
    EXPECT_GT(stepped, 0.0);
    EXPECT_LT(stepped, 0.6);  // strongly smoothed at 30 fps with 1 Hz cutoff

    // converges toward the target
    double v = stepped;
    for (int i = 2; i < 90; ++i)
        v = f.filter(1.0, i / 30.0);
    EXPECT_GT(v, 0.95);
}

TEST(OneEuroFilter, BetaLetsFastMotionTrack)
{
    OneEuroFilter slow(OneEuroFilter::Params{1.0, 0.0, 1.0});
    OneEuroFilter fast(OneEuroFilter::Params{1.0, 5.0, 1.0});
    slow.filter(0.0, 0.0);
    fast.filter(0.0, 0.0);
    double vSlow = 0.0, vFast = 0.0;
    for (int i = 1; i <= 10; ++i) {
        const double target = i * 0.5;  // fast ramp
        vSlow = slow.filter(target, i / 30.0);
        vFast = fast.filter(target, i / 30.0);
    }
    EXPECT_GT(vFast, vSlow);  // high beta tracks the ramp closer
}

TEST(OneEuroFilter, NonMonotonicTimeIsIgnored)
{
    OneEuroFilter f;
    f.filter(1.0, 1.0);
    EXPECT_DOUBLE_EQ(f.filter(99.0, 1.0), f.filter(99.0, 0.5));
}

namespace {

std::array<float, 4> quatAboutZ(double rad)
{
    return {0.f, 0.f, static_cast<float>(std::sin(rad / 2)),
            static_cast<float>(std::cos(rad / 2))};
}

double quatAngle(const std::array<float, 4>& a, const std::array<float, 4>& b)
{
    double dot = 0;
    for (int i = 0; i < 4; ++i)
        dot += a[i] * b[i];
    return 2.0 * std::acos(std::fmin(1.0, std::fabs(dot)));
}

}  // namespace

TEST(OneEuroQuatFilter, FirstSamplePassesThrough)
{
    OneEuroQuatFilter f;
    const auto q = quatAboutZ(0.3);
    const auto out = f.filter(q, 0.0);
    // float storage: acos near 1.0 amplifies rounding, so ~1e-3 rad is exact
    EXPECT_NEAR(quatAngle(out, q), 0.0, 1e-3);
}

TEST(OneEuroQuatFilter, SmoothsTowardSample)
{
    OneEuroQuatFilter f(OneEuroFilter::Params{1.0, 0.0, 1.0});
    f.filter(quatAboutZ(0.0), 0.0);
    const auto target = quatAboutZ(1.0);
    const auto out = f.filter(target, 1.0 / 30.0);
    const double remaining = quatAngle(out, target);
    EXPECT_GT(remaining, 0.3);  // did not jump straight to the target
    EXPECT_LT(remaining, 1.0);  // but moved toward it
}

TEST(OneEuroQuatFilter, HemisphereConsistency)
{
    OneEuroQuatFilter f;
    auto q = quatAboutZ(0.2);
    const auto first = f.filter(q, 0.0);
    // feed the SAME rotation with flipped sign: output must stay in the
    // previous output's hemisphere (dot >= 0), no 360-degree pop
    for (auto& c : q)
        c = -c;
    const auto out = f.filter(q, 1.0 / 30.0);
    double dot = 0;
    for (int i = 0; i < 4; ++i)
        dot += out[i] * first[i];
    EXPECT_GE(dot, 0.0);
    EXPECT_NEAR(quatAngle(out, first), 0.0, 1e-4);
}

TEST(OneEuroQuatFilter, ContinuousUnderIncrementalRotation)
{
    OneEuroQuatFilter f(OneEuroFilter::Params{1.5, 0.5, 1.0});
    auto prev = f.filter(quatAboutZ(0.0), 0.0);
    for (int i = 1; i <= 60; ++i) {
        const auto out = f.filter(quatAboutZ(i * 0.05), i / 30.0);
        EXPECT_LT(quatAngle(out, prev), 0.2);  // no jumps between frames
        double n = 0;
        for (int k = 0; k < 4; ++k)
            n += out[k] * out[k];
        EXPECT_NEAR(n, 1.0, 1e-4);  // stays unit length
        prev = out;
    }
}

#endif  // ENABLE_MOCAP
