#include "HDR/HdrTonemap.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace HdrTonemap;

TEST(HdrTonemapTest, ExposureOneStopDoublesLinear)
{
    const Rgb input{0.25f, 0.5f, 1.0f};
    const Rgb ev0 = applyExposure(input, 0.f);
    const Rgb ev1 = applyExposure(input, 1.f);
    EXPECT_NEAR(ev1.r, ev0.r * 2.f, 1e-5f);
    EXPECT_NEAR(ev1.g, ev0.g * 2.f, 1e-5f);
    EXPECT_NEAR(ev1.b, ev0.b * 2.f, 1e-5f);
}

TEST(HdrTonemapTest, ReinhardConstantGrey_MatchesReference)
{
    const Rgb grey{2.f, 2.f, 2.f};
    const Rgb mapped = tonemapReinhard(grey, 1.f);
    const Rgb unit = tonemapReinhard({1.f, 1.f, 1.f}, 1.f);
    EXPECT_NEAR(mapped.r, unit.r, 1e-5f);
    EXPECT_NEAR(mapped.g, unit.g, 1e-5f);
    EXPECT_NEAR(mapped.b, unit.b, 1e-5f);
    EXPECT_GT(mapped.r, 0.5f);
    EXPECT_LE(mapped.r, 1.f);
}

TEST(HdrTonemapTest, OperatorsProduceDifferentOutputs)
{
    const Rgb hdr{4.f, 2.f, 1.f};
    const Rgb reinhard = tonemap(hdr, Operator::Reinhard, 0.f, 1.f);
    const Rgb aces = tonemap(hdr, Operator::ACES, 0.f, 1.f);
    const Rgb agx = tonemap(hdr, Operator::AgX, 0.f, 1.f);
    EXPECT_NE(reinhard.r, aces.r);
    EXPECT_NE(aces.r, agx.r);
}

TEST(HdrTonemapTest, ExposureScalesBeforeTonemap)
{
    const Rgb base{0.25f, 0.25f, 0.25f};
    const Rgb oneStop = tonemap(base, Operator::Reinhard, 1.f, 1.f);
    const Rgb zeroStop = tonemap(base, Operator::Reinhard, 0.f, 1.f);
    EXPECT_GT(oneStop.r, zeroStop.r);
}

TEST(HdrTonemapTest, LinearToSrgb_IsMonotonic)
{
    const Rgb lo = linearToSrgb({0.1f, 0.1f, 0.1f});
    const Rgb hi = linearToSrgb({0.5f, 0.5f, 0.5f});
    EXPECT_LT(lo.r, hi.r);
}
