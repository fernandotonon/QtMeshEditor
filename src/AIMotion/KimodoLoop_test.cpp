// KimodoLoop: cycle search, the cut-and-cross-fade, and closing a pinned loop, on synthetic motions.

#include "KimodoLoop.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace AIMotion::KimodoLoop;

namespace {

constexpr double kPi = 3.14159265358979323846;

void rotY(double deg, float* m)
{
    const double r = deg * kPi / 180.0;
    const float c = float(std::cos(r));
    const float s = float(std::sin(r));
    const float v[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    std::copy(v, v + 9, m);
}

// Two joints: joint 0 swings about Y by angleAt(t), joint 1 holds identity. The root walks +x 2 cm a frame.
template <typename F>
Motion makeMotion(int T, F angleAt)
{
    Motion m;
    m.T = T;
    m.J = 2;
    m.C = 4;
    m.local.resize(size_t(T) * 2 * 9);
    m.root.resize(size_t(T) * 3);
    m.contacts.assign(size_t(T) * 4, 0.0f);
    for (int t = 0; t < T; ++t) {
        rotY(angleAt(t), &m.local[size_t(t) * 18]);
        rotY(0.0, &m.local[size_t(t) * 18 + 9]);
        m.root[size_t(t) * 3] = 0.02f * float(t) + 1.0f;
        m.root[size_t(t) * 3 + 1] = 0.9f;
        m.root[size_t(t) * 3 + 2] = 0.5f;
    }
    return m;
}

Motion swing(int T, double period)
{
    return makeMotion(T, [period](int t) { return 30.0 * std::sin(2.0 * kPi * t / period); });
}

double stepDeg(const Motion& m, int a, int b) { return frameCostDeg(m, a, b); }

} // namespace

TEST(KimodoLoop, ModeIds)
{
    Mode m = Mode::Cut;
    EXPECT_TRUE(modeFromId(QString(), &m));
    EXPECT_EQ(m, Mode::Auto);
    EXPECT_TRUE(modeFromId(QStringLiteral("Regenerate"), &m));
    EXPECT_EQ(m, Mode::Regenerate);
    EXPECT_TRUE(modeFromId(QStringLiteral("cut"), &m));
    EXPECT_EQ(m, Mode::Cut);
    EXPECT_FALSE(modeFromId(QStringLiteral("forever"), &m));
    for (const QString& id : modeIds()) {
        ASSERT_TRUE(modeFromId(id, &m));
        EXPECT_EQ(modeId(m), id);
    }
}

TEST(KimodoLoop, FindsThePeriodOfACycle)
{
    const Motion m = swing(150, 30.0);
    SearchOptions o;
    o.minStart = 15;
    o.minPeriod = 20;
    o.maxPeriod = 45;
    o.targetPeriod = 36;
    const Cycle c = findCycle(m, o);
    ASSERT_TRUE(c.found);
    EXPECT_EQ(c.period, 30);
    EXPECT_GE(c.start, 15);
    EXPECT_LT(c.costDeg, 0.01);
}

TEST(KimodoLoop, PrefersTheLengthAskedFor)
{
    // Two strides fit a 2 s request better than one (both match perfectly).
    const Motion m = swing(200, 30.0);
    SearchOptions o;
    o.minStart = 15;
    o.minPeriod = 20;
    o.maxPeriod = 80;
    o.targetPeriod = 58;
    const Cycle c = findCycle(m, o);
    ASSERT_TRUE(c.found);
    EXPECT_EQ(c.period, 60);
}

TEST(KimodoLoop, AStillHoldIsNotACycle)
{
    // One big swing, then the pose holds: the hold matches itself perfectly but does not move, so it must not
    // be picked as the loop (an attack looped as a frozen end pose).
    const Motion m = makeMotion(140, [](int t) { return t < 45 ? 60.0 * std::sin(kPi * t / 45.0) : 0.0; });
    SearchOptions o;
    o.minStart = 5;
    o.minPeriod = 20;
    o.maxPeriod = 40;
    o.targetPeriod = 30;
    const Cycle c = findCycle(m, o);
    if (c.found) {
        EXPECT_LT(c.start, 45) << "a cycle inside the still hold was accepted";
    }
}

TEST(KimodoLoop, CutClosesOnTheFirstFramePlusTheStride)
{
    const Motion m = swing(150, 30.0);
    const Motion loop = cutLoop(m, 20, 30, 8);
    ASSERT_EQ(loop.T, 31);
    EXPECT_LT(stepDeg(loop, 0, 30), 0.05);   // last pose == first pose (acos rounding near 0)
    EXPECT_NEAR(loop.root[0], 0.0f, 1e-6f);                 // re-based
    EXPECT_NEAR(loop.root[2], 0.0f, 1e-6f);
    EXPECT_NEAR(loop.root[30 * 3], 0.6f, 1e-4f);            // one stride: 30 frames x 2 cm
    EXPECT_NEAR(loop.root[30 * 3 + 1], 0.9f, 1e-6f);
}

TEST(KimodoLoop, CrossFadeHidesAMismatchedWrap)
{
    // Cut 28 frames of a 30-frame cycle: without the blend the wrap jumps; with it the last steps stay smooth.
    const Motion m = swing(150, 30.0);
    const Motion hard = cutLoop(m, 20, 28, 0);
    const Motion soft = cutLoop(m, 20, 28, 8);
    double interior = 0.0;
    for (int t = 0; t + 1 < 20; ++t) interior = std::max(interior, stepDeg(soft, t, t + 1));
    double softMax = 0.0;
    for (int t = 20; t < 28; ++t) softMax = std::max(softMax, stepDeg(soft, t, t + 1));
    const double hardJump = stepDeg(hard, 27, 28);
    EXPECT_GT(hardJump, interior * 1.5);
    EXPECT_LT(softMax, interior * 1.6);
}

TEST(KimodoLoop, CloseLoopCopiesTheFirstFrame)
{
    Motion m = swing(40, 30.0);
    const Motion loop = closeLoop(m, 30, {0.5f, -0.25f});
    ASSERT_EQ(loop.T, 31);
    EXPECT_LT(stepDeg(loop, 0, 30), 0.05);
    EXPECT_NEAR(loop.root[30 * 3] - loop.root[0], 0.5f, 1e-6f);
    EXPECT_NEAR(loop.root[30 * 3 + 2] - loop.root[2], -0.25f, 1e-6f);
    const auto s = strideOf(m, 0, 30);
    EXPECT_NEAR(s[0], 0.6f, 1e-5f);
    EXPECT_NEAR(s[1], 0.0f, 1e-6f);
}
