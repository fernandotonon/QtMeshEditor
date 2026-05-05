#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>

#include "CurveEditModel.h"
#include "CurveResampler.h"

class CurveResamplerTest : public ::testing::Test {
protected:
    void SetUp() override {
        CurveEditModel::kill();
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }
    void TearDown() override {
        CurveEditModel::kill();
    }
    QApplication* app = nullptr;
};

namespace {
QVariantList toVariants(const std::vector<double>& v) {
    QVariantList out;
    for (double x : v) out.append(x);
    return out;
}
} // namespace

TEST_F(CurveResamplerTest, EmptyInputReturnsEmpty) {
    auto* m = CurveEditModel::instance();
    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.0, 1.0, {}, {});
    EXPECT_TRUE(out.empty());
}

TEST_F(CurveResamplerTest, NullModelReturnsEmpty) {
    auto out = CurveResampler::resampleSegment(nullptr, "s", "a", "b", "tx",
                                               0.0, 1.0,
                                               toVariants({0.0, 1.0}),
                                               toVariants({0.0, 1.0}));
    EXPECT_TRUE(out.empty());
}

TEST_F(CurveResamplerTest, ZeroDurationReturnsEmpty) {
    auto* m = CurveEditModel::instance();
    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.5, 0.5,
                                               toVariants({0.0, 1.0}),
                                               toVariants({0.0, 1.0}));
    EXPECT_TRUE(out.empty());
}

TEST_F(CurveResamplerTest, MismatchedSizesReturnsEmpty) {
    auto* m = CurveEditModel::instance();
    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.0, 1.0,
                                               toVariants({0.0, 1.0}),
                                               toVariants({0.0}));
    EXPECT_TRUE(out.empty());
}

TEST_F(CurveResamplerTest, LinearCurveCollapsesToSingleEndpoint) {
    // Linear mode = straight line between anchors, which matches Ogre's
    // default linear interpolation between adjacent TransformKeyFrames
    // exactly. Adaptive sampling collapses the entire segment to ZERO
    // new interior keyframes — the caller's existing t0/t1 anchors
    // describe the segment perfectly. Output is just the t1 endpoint.
    auto* m = CurveEditModel::instance();
    m->setMode("s", "a", "b", "tx", 0.0, CurveEditModel::ModeLinear);
    m->setMode("s", "a", "b", "tx", 1.0, CurveEditModel::ModeLinear);

    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.0, 1.0,
                                               toVariants({0.0, 1.0}),
                                               toVariants({0.0, 1.0}));
    EXPECT_EQ(out.size(), 1u);
    EXPECT_NEAR(out.back().time,  1.0, 1e-6);
    EXPECT_NEAR(out.back().value, 1.0, 1e-3);
}

TEST_F(CurveResamplerTest, SteppedCurveProducesNonEmptyOutput) {
    // Stepped mode evaluates as "value of upstream key" — between the
    // two anchors the curve is constant at 0, then jumps to 1 right
    // at t=1. With only two anchors in the model RDP can collapse
    // most of the constant region, but the simplifier must still
    // emit at least the closing endpoint sample.
    auto* m = CurveEditModel::instance();
    m->setMode("s", "a", "b", "tx", 0.0, CurveEditModel::ModeStepped);
    m->setMode("s", "a", "b", "tx", 1.0, CurveEditModel::ModeStepped);

    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.0, 1.0,
                                               toVariants({0.0, 1.0}),
                                               toVariants({0.0, 1.0}));
    EXPECT_GE(out.size(), 1u);
}

TEST_F(CurveResamplerTest, LongSegmentRespectsMaxSamplesCap) {
    // 10-second segment with stepped curve dense-samples at 60 Hz =
    // 600 raw samples, capped at 200 BEFORE simplification. After RDP
    // the kept count is ≤ 200 and ≥ 1.
    auto* m = CurveEditModel::instance();
    m->setMode("s", "a", "b", "tx", 0.0,  CurveEditModel::ModeStepped);
    m->setMode("s", "a", "b", "tx", 10.0, CurveEditModel::ModeStepped);

    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.0, 10.0,
                                               toVariants({0.0, 10.0}),
                                               toVariants({0.0, 1.0}));
    EXPECT_LE(out.size(), static_cast<size_t>(CurveResampler::kMaxSamples));
    EXPECT_GE(out.size(), 1u);
}

TEST_F(CurveResamplerTest, SamplesEndAtSegmentEnd) {
    // Closing endpoint must always be exactly t1 so the caller can
    // drop it cleanly (the t1 anchor keyframe already exists).
    auto* m = CurveEditModel::instance();
    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.0, 0.5,
                                               toVariants({0.0, 0.5}),
                                               toVariants({0.0, 1.0}));
    ASSERT_FALSE(out.empty());
    EXPECT_NEAR(out.back().time, 0.5, 1e-6);
}

TEST_F(CurveResamplerTest, SamplesAreMonotonicInTime) {
    // Use stepped mode so simplification keeps multiple samples.
    auto* m = CurveEditModel::instance();
    m->setMode("s", "a", "b", "tx", 0.0, CurveEditModel::ModeStepped);
    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.0, 1.0,
                                               toVariants({0.0, 1.0}),
                                               toVariants({0.0, 1.0}));
    ASSERT_GT(out.size(), 1u);
    for (size_t i = 1; i < out.size(); ++i) {
        EXPECT_GT(out[i].time, out[i-1].time);
    }
}

TEST_F(CurveResamplerTest, FixedFpsProducesUniformSamples) {
    // Fixed-FPS mode bypasses adaptive sampling + simplification:
    // exactly fps samples per second, regardless of curve shape.
    auto* m = CurveEditModel::instance();
    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.0, 1.0,
                                               toVariants({0.0, 1.0}),
                                               toVariants({0.0, 1.0}),
                                               1.0, 30);
    EXPECT_EQ(out.size(), 30u);
    // Times are uniformly spaced by 1/30s.
    for (size_t i = 1; i < out.size(); ++i) {
        EXPECT_NEAR(out[i].time - out[i-1].time, 1.0 / 30.0, 1e-6);
    }
}

TEST_F(CurveResamplerTest, FixedFpsHonorsUserRequestedRate) {
    // Fixed-FPS bypasses the kMaxSamples cap that protects adaptive
    // bakes from pathological curvature — the cap is for user-
    // unfriendly self-protection, but a user explicitly asking for
    // "60 FPS for a 100s clip" should get exactly that.
    auto* m = CurveEditModel::instance();
    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.0, 100.0,
                                               toVariants({0.0, 100.0}),
                                               toVariants({0.0, 1.0}),
                                               1.0, 60);
    EXPECT_EQ(out.size(), 60u * 100u);
}

TEST_F(CurveResamplerTest, BezierWithStrongTangentsRetainsSamples) {
    // Aggressive tangents create curvature peaks the simplifier
    // can't collapse; output must keep multiple interior samples.
    auto* m = CurveEditModel::instance();
    m->setTangents("s", "a", "b", "tx", 0.0, 0.0, 50.0);
    m->setTangents("s", "a", "b", "tx", 1.0, -50.0, 0.0);

    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.0, 1.0,
                                               toVariants({0.0, 1.0}),
                                               toVariants({0.0, 1.0}));
    EXPECT_GE(out.size(), 5u);
}
