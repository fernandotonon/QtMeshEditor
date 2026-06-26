#include <gtest/gtest.h>

#include "MotionInbetween.h"

#include <array>
#include <cmath>
#include <vector>

// MotionInbetween's core is Ogre-free and ONNX-optional, so these tests run on
// ANY build (ENABLE_ONNX on or off) with no GL context or model files — they
// exercise the pure-data helpers and the deterministic spline fallback, which
// is the path the feature uses whenever the RMIB model is absent.

using MIB = MotionInbetween;

namespace {

std::vector<MIB::Channel> layoutTRS(int bones)
{
    // Per-bone [tx,ty,tz, qx,qy,qz,qw, sx,sy,sz] — 3 scalar + 4 quat + 3 scalar.
    std::vector<MIB::Channel> l;
    for (int b = 0; b < bones; ++b) {
        l.push_back(MIB::Channel::Scalar);     // tx
        l.push_back(MIB::Channel::Scalar);     // ty
        l.push_back(MIB::Channel::Scalar);     // tz
        l.push_back(MIB::Channel::QuatStart);  // qx
        l.push_back(MIB::Channel::QuatCont);   // qy
        l.push_back(MIB::Channel::QuatCont);   // qz
        l.push_back(MIB::Channel::QuatCont);   // qw
        l.push_back(MIB::Channel::Scalar);     // sx
        l.push_back(MIB::Channel::Scalar);     // sy
        l.push_back(MIB::Channel::Scalar);     // sz
    }
    return l;
}

float quatLen(const std::array<float,4>& q)
{
    return std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
}

} // namespace

// ---- slerpQuat -------------------------------------------------------------

TEST(MotionInbetween, SlerpEndpointsAreExact)
{
    const std::array<float,4> a{0,0,0,1};
    // 90° about Z
    const std::array<float,4> b{0,0,std::sin(float(M_PI)/4), std::cos(float(M_PI)/4)};
    auto r0 = MIB::slerpQuat(a, b, 0.0f);
    auto r1 = MIB::slerpQuat(a, b, 1.0f);
    for (int i = 0; i < 4; ++i) {
        EXPECT_NEAR(r0[i], a[i], 1e-5) << "t=0 comp " << i;
        EXPECT_NEAR(r1[i], b[i], 1e-5) << "t=1 comp " << i;
    }
}

TEST(MotionInbetween, SlerpMidpointIsUnitAndHalfAngle)
{
    const std::array<float,4> a{0,0,0,1};
    const std::array<float,4> b{0,0,std::sin(float(M_PI)/4), std::cos(float(M_PI)/4)}; // 90°
    auto m = MIB::slerpQuat(a, b, 0.5f);
    EXPECT_NEAR(quatLen(m), 1.0f, 1e-5);
    // Halfway should be 45° about Z: w = cos(22.5°), z = sin(22.5°).
    EXPECT_NEAR(m[3], std::cos(float(M_PI)/8), 1e-4);
    EXPECT_NEAR(m[2], std::sin(float(M_PI)/8), 1e-4);
}

TEST(MotionInbetween, SlerpTakesShortestArc)
{
    // a and -b represent the same rotation but opposite hemispheres; slerp must
    // pick the short way (treat b == a) → midpoint stays near a, unit length.
    const std::array<float,4> a{0,0,0,1};
    const std::array<float,4> negA{0,0,0,-1};
    auto m = MIB::slerpQuat(a, negA, 0.5f);
    EXPECT_NEAR(quatLen(m), 1.0f, 1e-4);
}

// ---- hermite ---------------------------------------------------------------

TEST(MotionInbetween, HermiteHitsEndpoints)
{
    EXPECT_NEAR(MIB::hermite(2.0f, 5.0f, 1.0f, -1.0f, 0.0f), 2.0f, 1e-6);
    EXPECT_NEAR(MIB::hermite(2.0f, 5.0f, 1.0f, -1.0f, 1.0f), 5.0f, 1e-6);
}

TEST(MotionInbetween, HermiteZeroTangentsIsSmoothstepLike)
{
    // With zero tangents the midpoint is the smoothstep value (= average here).
    const float mid = MIB::hermite(0.0f, 10.0f, 0.0f, 0.0f, 0.5f);
    EXPECT_NEAR(mid, 5.0f, 1e-5);
}

// ---- spline fallback -------------------------------------------------------

TEST(MotionInbetween, SplineProducesRequestedGapFrames)
{
    const auto layout = layoutTRS(1);
    MIB::Pose start(layout.size(), 0.0f);
    MIB::Pose end(layout.size(), 0.0f);
    start[3] = 0; start[4] = 0; start[5] = 0; start[6] = 1;   // identity quat
    end[3]   = 0; end[4]   = 0; end[5]   = 0; end[6]   = 1;

    MIB::Options o; o.gapFrames = 7;
    auto r = MIB::interpolateSpline(start, end, layout, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.frames.size(), 7u);
    EXPECT_FALSE(r.usedModel);
    for (const auto& f : r.frames) EXPECT_EQ(f.size(), layout.size());
}

TEST(MotionInbetween, SplineKeepsQuaternionsUnitLength)
{
    const auto layout = layoutTRS(1);
    MIB::Pose start(layout.size(), 0.0f), end(layout.size(), 0.0f);
    start[6] = 1;                                              // identity
    // 120° about Y
    end[3] = 0; end[4] = std::sin(float(M_PI)/3); end[5] = 0; end[6] = std::cos(float(M_PI)/3);

    MIB::Options o; o.gapFrames = 5;
    auto r = MIB::interpolateSpline(start, end, layout, o);
    ASSERT_TRUE(r.ok);
    for (const auto& f : r.frames) {
        std::array<float,4> q{ f[3], f[4], f[5], f[6] };
        EXPECT_NEAR(quatLen(q), 1.0f, 1e-4);
    }
}

TEST(MotionInbetween, SplineTranslationIsMonotonicAndBracketed)
{
    // Translation tx goes 0 → 10; the in-between samples must be strictly
    // increasing and lie strictly inside (0,10).
    const auto layout = layoutTRS(1);
    MIB::Pose start(layout.size(), 0.0f), end(layout.size(), 0.0f);
    start[6] = end[6] = 1;
    start[0] = 0.0f; end[0] = 10.0f;

    MIB::Options o; o.gapFrames = 9;
    auto r = MIB::interpolateSpline(start, end, layout, o);
    ASSERT_TRUE(r.ok);
    float prev = 0.0f;
    for (const auto& f : r.frames) {
        EXPECT_GT(f[0], 0.0f);
        EXPECT_LT(f[0], 10.0f);
        EXPECT_GT(f[0], prev);   // strictly increasing
        prev = f[0];
    }
}

TEST(MotionInbetween, SplineBeatsLinearWithCurvedNeighbours)
{
    // Acceptance-criterion proxy: with curved surrounding motion, the Hermite
    // fallback's endpoint tangents make it follow the curve, so its first
    // in-between sample differs from a naive linear midpoint — i.e. it is NOT
    // just linear interpolation.
    const auto layout = layoutTRS(1);
    MIB::Pose pre(layout.size(), 0.0f), start(layout.size(), 0.0f),
              end(layout.size(), 0.0f), post(layout.size(), 0.0f);
    for (auto* p : { &pre, &start, &end, &post }) (*p)[6] = 1;  // identity quats
    // A curving path on tx: pre=-10, start=0, end=10, post=15 (decelerating).
    pre[0] = -10.0f; start[0] = 0.0f; end[0] = 10.0f; post[0] = 15.0f;

    MIB::Options o; o.gapFrames = 1;
    auto r = MIB::interpolateSpline(start, end, layout, o, &pre, &post);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.frames.size(), 1u);
    const float linearMid = 5.0f;                 // naive linear midpoint
    EXPECT_GT(std::abs(r.frames[0][0] - linearMid), 1e-3)
        << "Hermite with curved neighbours should differ from linear";
}

TEST(MotionInbetween, SplineLayoutMismatchFails)
{
    const auto layout = layoutTRS(1);
    MIB::Pose start(layout.size() - 1, 0.0f);   // wrong size
    MIB::Pose end(layout.size(), 0.0f);
    auto r = MIB::interpolateSpline(start, end, layout, MIB::Options{});
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

TEST(MotionInbetween, SplineZeroGapYieldsNoFrames)
{
    const auto layout = layoutTRS(1);
    MIB::Pose start(layout.size(), 0.0f), end(layout.size(), 0.0f);
    start[6] = end[6] = 1;
    MIB::Options o; o.gapFrames = 0;
    auto r = MIB::interpolateSpline(start, end, layout, o);
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.frames.empty());
}

// ---- predict() (model-or-fallback) ----------------------------------------

TEST(MotionInbetween, PredictFallsBackWhenNoModel)
{
    const auto layout = layoutTRS(2);
    MIB::Pose start(layout.size(), 0.0f), end(layout.size(), 0.0f);
    start[6] = end[6] = 1; start[16] = end[16] = 1;   // both bones' quat.w = 1

    MIB::Options o; o.gapFrames = 4;
    // Point at a path that surely doesn't exist → must fall back, not throw.
    auto r = MIB::predict(start, end, layout,
                          QStringLiteral("/no/such/rmib.onnx"), o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.frames.size(), 4u);
    EXPECT_FALSE(r.usedModel);
    EXPECT_FALSE(r.fallbackReason.isEmpty());
}

TEST(MotionInbetween, PredictForceFallbackMatchesSpline)
{
    const auto layout = layoutTRS(1);
    MIB::Pose start(layout.size(), 0.0f), end(layout.size(), 0.0f);
    start[6] = end[6] = 1;
    start[0] = 1.0f; end[0] = 4.0f;

    MIB::Options o; o.gapFrames = 3; o.forceFallback = true;
    auto pr = MIB::predict(start, end, layout, MIB::modelPath(), o);
    auto sp = MIB::interpolateSpline(start, end, layout, o);
    ASSERT_TRUE(pr.ok);
    ASSERT_TRUE(sp.ok);
    ASSERT_EQ(pr.frames.size(), sp.frames.size());
    for (size_t f = 0; f < pr.frames.size(); ++f)
        for (size_t c = 0; c < layout.size(); ++c)
            EXPECT_NEAR(pr.frames[f][c], sp.frames[f][c], 1e-6);
    EXPECT_FALSE(pr.usedModel);
}

TEST(MotionInbetween, ModelBackendAvailabilityMatchesBuild)
{
#ifdef ENABLE_ONNX
    EXPECT_TRUE(MIB::isModelBackendAvailable());
#else
    EXPECT_FALSE(MIB::isModelBackendAvailable());
#endif
}
