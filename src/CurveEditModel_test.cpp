#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>

#include "CurveEditModel.h"

class CurveEditModelTest : public ::testing::Test {
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

// ── Tangent storage round-trip ────────────────────────────────────────────────

TEST_F(CurveEditModelTest, DefaultTangentsAreZeroBezier) {
    auto* m = CurveEditModel::instance();
    QVariantList t = m->tangentsAt("skel", "Walk", "Bone1", "tx", 0.5);
    ASSERT_EQ(t.size(), 3);
    EXPECT_DOUBLE_EQ(t[0].toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(t[1].toDouble(), 0.0);
    EXPECT_EQ(t[2].toInt(), CurveEditModel::ModeBezier);
}

TEST_F(CurveEditModelTest, SetTangentsRoundTrip) {
    auto* m = CurveEditModel::instance();
    m->setTangents("skel", "Walk", "Bone1", "tx", 0.5, 1.5, -0.75);
    QVariantList t = m->tangentsAt("skel", "Walk", "Bone1", "tx", 0.5);
    EXPECT_DOUBLE_EQ(t[0].toDouble(), 1.5);
    EXPECT_DOUBLE_EQ(t[1].toDouble(), -0.75);
    EXPECT_EQ(t[2].toInt(), CurveEditModel::ModeBezier);
}

TEST_F(CurveEditModelTest, SetModePersists) {
    auto* m = CurveEditModel::instance();
    m->setMode("skel", "Walk", "Bone1", "tx", 0.5, CurveEditModel::ModeLinear);
    QVariantList t = m->tangentsAt("skel", "Walk", "Bone1", "tx", 0.5);
    EXPECT_EQ(t[2].toInt(), CurveEditModel::ModeLinear);
}

TEST_F(CurveEditModelTest, SetModeRejectsInvalidValue) {
    auto* m = CurveEditModel::instance();
    m->setMode("skel", "Walk", "Bone1", "tx", 0.5, 99); // invalid
    QVariantList t = m->tangentsAt("skel", "Walk", "Bone1", "tx", 0.5);
    EXPECT_EQ(t[2].toInt(), CurveEditModel::ModeBezier); // default unchanged
}

TEST_F(CurveEditModelTest, EditingTangentsPromotesToBezier) {
    auto* m = CurveEditModel::instance();
    // Start in Linear mode.
    m->setMode("skel", "Walk", "Bone1", "tx", 0.5, CurveEditModel::ModeLinear);
    // Editing tangents implies Bezier intent — should auto-promote.
    m->setTangents("skel", "Walk", "Bone1", "tx", 0.5, 1.0, 1.0);
    QVariantList t = m->tangentsAt("skel", "Walk", "Bone1", "tx", 0.5);
    EXPECT_EQ(t[2].toInt(), CurveEditModel::ModeBezier);
}

TEST_F(CurveEditModelTest, KeysAreScopedPerSkeletonAnimBoneChannel) {
    auto* m = CurveEditModel::instance();
    m->setTangents("skelA", "Walk", "Bone1", "tx", 0.5, 1.0, 1.0);
    // Same time, different skeleton → independent.
    QVariantList t = m->tangentsAt("skelB", "Walk", "Bone1", "tx", 0.5);
    EXPECT_DOUBLE_EQ(t[0].toDouble(), 0.0);
}

TEST_F(CurveEditModelTest, ClearAnimationDropsAllItsEntries) {
    auto* m = CurveEditModel::instance();
    m->setTangents("skel", "Walk", "Bone1", "tx", 0.5, 1.0, 1.0);
    m->setTangents("skel", "Walk", "Bone1", "ty", 0.5, 2.0, 2.0);
    m->setTangents("skel", "Run",  "Bone1", "tx", 0.5, 3.0, 3.0); // different anim
    m->clearAnimation("skel", "Walk");
    EXPECT_DOUBLE_EQ(
        m->tangentsAt("skel", "Walk", "Bone1", "tx", 0.5)[0].toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(
        m->tangentsAt("skel", "Walk", "Bone1", "ty", 0.5)[0].toDouble(), 0.0);
    // Different animation must survive.
    EXPECT_DOUBLE_EQ(
        m->tangentsAt("skel", "Run", "Bone1", "tx", 0.5)[0].toDouble(), 3.0);
}

// ── evaluate() — interpolation modes ──────────────────────────────────────────

namespace {
    QVariantList vec(std::initializer_list<double> xs) {
        QVariantList out;
        for (double x : xs) out << x;
        return out;
    }
}

TEST_F(CurveEditModelTest, EvaluateEmptyTrackReturnsZero) {
    auto* m = CurveEditModel::instance();
    EXPECT_DOUBLE_EQ(
        m->evaluate("skel", "Walk", "Bone", "tx", 0.5,
                    QVariantList{}, QVariantList{}), 0.0);
}

TEST_F(CurveEditModelTest, EvaluateSingleKeyReturnsConstant) {
    auto* m = CurveEditModel::instance();
    EXPECT_DOUBLE_EQ(
        m->evaluate("skel", "Walk", "Bone", "tx", 0.5,
                    vec({0.0}), vec({3.5})), 3.5);
}

TEST_F(CurveEditModelTest, EvaluateLinearMode) {
    auto* m = CurveEditModel::instance();
    m->setMode("skel", "Walk", "Bone", "tx", 0.0, CurveEditModel::ModeLinear);
    // Times 0 and 1, values 0 and 10 → at t=0.25 expect 2.5.
    EXPECT_NEAR(
        m->evaluate("skel", "Walk", "Bone", "tx", 0.25,
                    vec({0.0, 1.0}), vec({0.0, 10.0})),
        2.5, 1e-9);
}

TEST_F(CurveEditModelTest, EvaluateSteppedMode) {
    auto* m = CurveEditModel::instance();
    m->setMode("skel", "Walk", "Bone", "tx", 0.0, CurveEditModel::ModeStepped);
    // At t=0.99 we should still hold the value at t=0 (= 1.0).
    EXPECT_DOUBLE_EQ(
        m->evaluate("skel", "Walk", "Bone", "tx", 0.99,
                    vec({0.0, 1.0}), vec({1.0, 5.0})), 1.0);
}

TEST_F(CurveEditModelTest, EvaluateAtKeyReturnsExactValue) {
    auto* m = CurveEditModel::instance();
    // Default Bezier with zero tangents — at the keyframe time itself,
    // the result must equal the stored value (regardless of mode).
    EXPECT_DOUBLE_EQ(
        m->evaluate("skel", "Walk", "Bone", "tx", 0.0,
                    vec({0.0, 1.0}), vec({7.0, 9.0})), 7.0);
    EXPECT_DOUBLE_EQ(
        m->evaluate("skel", "Walk", "Bone", "tx", 1.0,
                    vec({0.0, 1.0}), vec({7.0, 9.0})), 9.0);
}

TEST_F(CurveEditModelTest, EvaluateBeyondLastReturnsLast) {
    auto* m = CurveEditModel::instance();
    EXPECT_DOUBLE_EQ(
        m->evaluate("skel", "Walk", "Bone", "tx", 5.0,
                    vec({0.0, 1.0}), vec({7.0, 9.0})), 9.0);
}

TEST_F(CurveEditModelTest, EvaluateAutoModeMatchesCatmullRomMidpoint) {
    auto* m = CurveEditModel::instance();
    m->setMode("skel", "Walk", "Bone", "tx", 1.0, CurveEditModel::ModeAuto);
    // Symmetric data around the bracket [1.0, 2.0]; CR midpoint should
    // pass through the linear interpolant when neighbors are linear.
    // values 0,1,2,3 at times 0,1,2,3 — at t=1.5 expect ~1.5.
    const double v = m->evaluate(
        "skel", "Walk", "Bone", "tx", 1.5,
        vec({0.0, 1.0, 2.0, 3.0}),
        vec({0.0, 1.0, 2.0, 3.0}));
    EXPECT_NEAR(v, 1.5, 1e-9);
}

TEST_F(CurveEditModelTest, EvaluateBeforeFirstKeyClampsToFirst) {
    // A query time before the first keyframe must hold the first value, not
    // extrapolate. Bezier mode: explicit defaults still need clamping.
    auto* m = CurveEditModel::instance();
    EXPECT_DOUBLE_EQ(
        m->evaluate("skel", "Walk", "Bone", "tx", -0.5,
                    vec({0.0, 1.0}), vec({2.0, 5.0})), 2.0);
    // Linear shouldn't extrapolate either.
    m->setMode("skel", "Walk", "Bone", "tx", 0.0, CurveEditModel::ModeLinear);
    EXPECT_DOUBLE_EQ(
        m->evaluate("skel", "Walk", "Bone", "tx", -1.0,
                    vec({0.0, 1.0}), vec({2.0, 5.0})), 2.0);
}

TEST_F(CurveEditModelTest, EvaluateAutoModeNonUniformSpacing) {
    // Non-uniform keyframe spacing: times {0, 1, 5} with values {0, 1, 5}
    // is on a perfect linear ramp, so any time-normalized tangent scheme
    // (including Catmull-Rom with proper dt-normalization) should
    // reproduce the line within tolerance. The pre-fix code used an
    // unnormalized half-difference and produced a wildly wrong slope at
    // the bracket [1, 5] because the right-side neighbor is far away.
    auto* m = CurveEditModel::instance();
    m->setMode("skel", "Walk", "Bone", "tx", 1.0, CurveEditModel::ModeAuto);
    const double v = m->evaluate(
        "skel", "Walk", "Bone", "tx", 3.0,
        vec({0.0, 1.0, 5.0}),
        vec({0.0, 1.0, 5.0}));
    // On a perfect line, t=3 should give value 3. Allow some Hermite
    // smoothing (~10 % of the segment) because Auto tangents derived
    // from only two neighbors aren't a true natural-spline solver.
    EXPECT_NEAR(v, 3.0, 0.5);
}
