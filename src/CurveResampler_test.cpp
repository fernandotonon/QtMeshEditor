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

TEST_F(CurveResamplerTest, LinearCurveProducesBaseRateSamples) {
    // Linear mode → constant slope → near-zero second derivative →
    // base rate (30 Hz). 1-second segment ≈ 30 samples.
    auto* m = CurveEditModel::instance();
    m->setMode("s", "a", "b", "tx", 0.0, CurveEditModel::ModeLinear);
    m->setMode("s", "a", "b", "tx", 1.0, CurveEditModel::ModeLinear);

    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.0, 1.0,
                                               toVariants({0.0, 1.0}),
                                               toVariants({0.0, 1.0}));
    EXPECT_EQ(out.size(), CurveResampler::kBaseHz);
    // First sample is at t = 1/30 ≈ 0.0333, value linearly ≈ 0.0333.
    EXPECT_NEAR(out.front().time,  1.0 / CurveResampler::kBaseHz, 1e-6);
    EXPECT_NEAR(out.front().value, 1.0 / CurveResampler::kBaseHz, 1e-3);
    // Last sample is at t = 1.0 (segment end), value = 1.0.
    EXPECT_NEAR(out.back().time,  1.0, 1e-6);
    EXPECT_NEAR(out.back().value, 1.0, 1e-3);
}

TEST_F(CurveResamplerTest, SteppedCurveBumpsToBoostRate) {
    // Stepped mode holds the upstream value until the next keyframe,
    // then jumps — that's a near-infinite second derivative, so the
    // resampler must escalate from 30 Hz to 60 Hz.
    auto* m = CurveEditModel::instance();
    m->setMode("s", "a", "b", "tx", 0.0, CurveEditModel::ModeStepped);
    m->setMode("s", "a", "b", "tx", 1.0, CurveEditModel::ModeStepped);

    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.0, 1.0,
                                               toVariants({0.0, 1.0}),
                                               toVariants({0.0, 1.0}));
    EXPECT_EQ(out.size(), CurveResampler::kBoostHz);
}

TEST_F(CurveResamplerTest, LongSegmentClampedToMaxSamples) {
    // 10-second segment with stepped curve would want 600 samples at
    // 60 Hz. The cap brings it back to 200.
    auto* m = CurveEditModel::instance();
    m->setMode("s", "a", "b", "tx", 0.0,  CurveEditModel::ModeStepped);
    m->setMode("s", "a", "b", "tx", 10.0, CurveEditModel::ModeStepped);

    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.0, 10.0,
                                               toVariants({0.0, 10.0}),
                                               toVariants({0.0, 1.0}));
    EXPECT_EQ(out.size(), CurveResampler::kMaxSamples);
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
    auto* m = CurveEditModel::instance();
    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.0, 1.0,
                                               toVariants({0.0, 1.0}),
                                               toVariants({0.0, 1.0}));
    ASSERT_GT(out.size(), 1u);
    for (size_t i = 1; i < out.size(); ++i) {
        EXPECT_GT(out[i].time, out[i-1].time);
    }
}

TEST_F(CurveResamplerTest, BezierWithStrongTangentsBoostsRate) {
    // Aggressive tangents create curvature peaks in the middle of the
    // segment — should escalate to 60 Hz like stepped.
    auto* m = CurveEditModel::instance();
    m->setTangents("s", "a", "b", "tx", 0.0, 0.0, 50.0);
    m->setTangents("s", "a", "b", "tx", 1.0, -50.0, 0.0);

    auto out = CurveResampler::resampleSegment(m, "s", "a", "b", "tx",
                                               0.0, 1.0,
                                               toVariants({0.0, 1.0}),
                                               toVariants({0.0, 1.0}));
    EXPECT_EQ(out.size(), CurveResampler::kBoostHz);
}
