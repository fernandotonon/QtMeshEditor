/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — line-stabilizer unit tests (Paint v2 Slice E, issue #548)

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>

#include "TexturePaintController.h"

#include <QPointF>
#include <cmath>
#include <deque>

namespace {
// Total successive-delta length of a path — a jitter/variance proxy.
double pathVariation(const std::vector<QPointF>& pts)
{
    double sum = 0.0;
    for (size_t i = 1; i < pts.size(); ++i) {
        const QPointF d = pts[i] - pts[i - 1];
        sum += std::hypot(d.x(), d.y());
    }
    return sum;
}
} // namespace

TEST(TexturePaintStabilizerTest, WindowGrowsWithAmount) {
    EXPECT_EQ(TexturePaintController::stabilizerWindow(0.0), 1);   // passthrough
    EXPECT_GT(TexturePaintController::stabilizerWindow(100.0),
              TexturePaintController::stabilizerWindow(10.0));
    EXPECT_GE(TexturePaintController::stabilizerWindow(50.0), 2);
}

TEST(TexturePaintStabilizerTest, AverageSmoothsJitter) {
    // A zig-zag raw input; feed it through a growing moving-average window and
    // confirm the smoothed path has strictly less variation than the raw one,
    // and more smoothing at a higher amount.
    std::vector<QPointF> raw;
    for (int i = 0; i < 40; ++i)
        raw.emplace_back(i, (i % 2 == 0) ? 0.0 : 20.0);   // 20px zig-zag

    auto smoothPath = [&](double amount) {
        const int N = TexturePaintController::stabilizerWindow(amount);
        std::deque<QPointF> win;
        std::vector<QPointF> out;
        for (const QPointF& p : raw) {
            win.push_back(p);
            while (static_cast<int>(win.size()) > N) win.pop_front();
            out.push_back(TexturePaintController::stabilizeAveragePoint(win, N));
        }
        return out;
    };

    const double rawVar = pathVariation(raw);
    const double lowVar = pathVariation(smoothPath(20.0));
    const double highVar = pathVariation(smoothPath(90.0));

    EXPECT_LT(lowVar, rawVar) << "stabilizer must reduce jitter";
    EXPECT_LT(highVar, lowVar) << "more smoothing at a higher amount";
}

TEST(TexturePaintStabilizerTest, TrailKeepsLagDistanceThenCatchesUp) {
    // With the cursor far from the trail, the trail steps toward it but stays
    // `lag` behind; when the cursor stops, repeated steps converge onto it.
    const double lag = 30.0;
    QPointF trail(0, 0);
    const QPointF cursor(100, 0);
    trail = TexturePaintController::stabilizeTrailPoint(trail, cursor, lag);
    const double d1 = std::hypot((cursor - trail).x(), (cursor - trail).y());
    EXPECT_NEAR(d1, lag, 1e-3) << "trail should sit exactly `lag` behind";

    // "Catch-up": once the cursor is within `lag`, the trail does not move.
    const QPointF nearCursor = trail + QPointF(lag * 0.5, 0);
    const QPointF held = TexturePaintController::stabilizeTrailPoint(trail, nearCursor, lag);
    EXPECT_EQ(held, trail) << "no movement while the cursor is within lag";
}

TEST(TexturePaintStabilizerTest, AmountZeroIsPassthrough) {
    auto* ctrl = TexturePaintController::instance();
    ASSERT_NE(ctrl, nullptr);
    ctrl->setStabilizerAmount(0.0);
    EXPECT_EQ(ctrl->stabilizerAmount(), 0.0);
    // Window of 1 → the average of a single sample is that sample (identity).
    std::deque<QPointF> one{ QPointF(7, 3) };
    EXPECT_EQ(TexturePaintController::stabilizeAveragePoint(one, 1), QPointF(7, 3));
}

TEST(TexturePaintStabilizerTest, SettersClampAndPersistState) {
    auto* ctrl = TexturePaintController::instance();
    ASSERT_NE(ctrl, nullptr);
    ctrl->setStabilizerAmount(150.0);   // over max
    EXPECT_LE(ctrl->stabilizerAmount(), 100.0);
    ctrl->setStabilizerAmount(-5.0);    // under min
    EXPECT_GE(ctrl->stabilizerAmount(), 0.0);
    ctrl->setStabilizerMode(static_cast<int>(TexturePaintController::StabTrail));
    EXPECT_EQ(ctrl->stabilizerMode(), static_cast<int>(TexturePaintController::StabTrail));
    ctrl->setStabilizerMode(static_cast<int>(TexturePaintController::StabAverage));
    EXPECT_EQ(ctrl->stabilizerMode(), static_cast<int>(TexturePaintController::StabAverage));
}
