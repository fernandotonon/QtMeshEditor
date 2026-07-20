#include <gtest/gtest.h>

#include <cmath>

#include "FootContact.h"

// #856 pure-data core tests — no Ogre/GL needed.

using FootContact::V3;

namespace {
float dist(const V3& a, const V3& b)
{
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}  // namespace

TEST(FootContactTest, DetectsPlantAndSwingPhases)
{
    // Gait-like trajectory: planted at x=0 for 10 frames, swing forward over
    // 10 frames (lifted), planted at x=1 for 10 frames.
    std::vector<V3> foot;
    for (int f = 0; f < 10; ++f) foot.push_back({0.0f, 0.0f, 0.0f});
    for (int f = 0; f < 10; ++f) {
        const float t = (f + 1) / 11.0f;
        foot.push_back({t, 0.25f * std::sin(t * 3.14159f), 0.0f});
    }
    for (int f = 0; f < 10; ++f) foot.push_back({1.0f, 0.0f, 0.0f});

    const auto spans = FootContact::detectContacts(foot, /*legLength=*/1.0f);
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_EQ(spans[0].start, 0);
    EXPECT_GE(spans[0].end, 7);      // plant 1 covers most of frames 0..9
    EXPECT_LE(spans[0].end, 10);
    EXPECT_GE(spans[1].start, 19);   // plant 2 starts after the swing
    EXPECT_EQ(spans[1].end, 29);
}

TEST(FootContactTest, SlidingFootIsNotAContact)
{
    // On the ground the whole time but translating fast — foot skate, the
    // exact artifact we're pinning. Must NOT be detected as one long contact.
    std::vector<V3> foot;
    for (int f = 0; f < 30; ++f)
        foot.push_back({0.1f * f, 0.0f, 0.0f});     // 0.1 leg-lengths/frame
    const auto spans = FootContact::detectContacts(foot, 1.0f);
    EXPECT_TRUE(spans.empty());
}

TEST(FootContactTest, ShortBlipsAreIgnored)
{
    // Foot is airborne (0.6) most of the clip, dipping to the ground for
    // three SEPARATE single frames. The low-percentile ground level lands
    // near 0, so each dip is an isolated on-ground frame — none forms a
    // run of minFrames consecutive, so no contact span registers.
    std::vector<V3> foot;
    for (int f = 0; f < 20; ++f)
        foot.push_back({0.0f, (f == 3 || f == 10 || f == 17) ? 0.0f : 0.6f, 0.0f});
    FootContact::DetectOptions opt;
    opt.minFrames = 3;
    const auto spans = FootContact::detectContacts(foot, 1.0f, opt);
    EXPECT_TRUE(spans.empty());      // isolated 1-frame touches < minFrames
}

TEST(FootContactTest, SolveKneePreservesLengthsAndReachesTarget)
{
    const V3 hip{0, 1.0f, 0};
    const V3 knee{0.1f, 0.5f, 0.15f};      // bent slightly forward
    const V3 foot{0, 0.0f, 0.05f};
    const float l1 = dist(hip, knee), l2 = dist(knee, foot);

    const V3 target{0.05f, 0.02f, -0.2f};  // pin slightly behind
    const V3 k2 = FootContact::solveKnee(hip, knee, foot, target);
    EXPECT_NEAR(dist(hip, k2), l1, 1e-4f);
    EXPECT_NEAR(dist(k2, target), l2, 1e-4f);
    // knee keeps bending roughly forward (pose's own bend plane, no flip)
    EXPECT_GT(k2[2], -0.05f);
}

TEST(FootContactTest, SolveKneeClampsUnreachableTarget)
{
    const V3 hip{0, 1.0f, 0};
    const V3 knee{0, 0.5f, 0.1f};
    const V3 foot{0, 0.0f, 0};
    const float l1 = dist(hip, knee), l2 = dist(knee, foot);

    const V3 far{3.0f, -2.0f, 0};          // beyond l1+l2
    const V3 k2 = FootContact::solveKnee(hip, knee, foot, far);
    EXPECT_NEAR(dist(hip, k2), l1, 1e-3f);
    // the chain extends straight toward the target: knee sits on hip→far
    const float reach = dist(hip, k2) + l2;
    EXPECT_NEAR(reach, l1 + l2, 1e-3f);
}

TEST(FootContactTest, SolveKneeStraightLegPicksStableBend)
{
    const V3 hip{0, 1.0f, 0};
    const V3 knee{0, 0.5f, 0};             // perfectly straight leg
    const V3 foot{0, 0.0f, 0};
    const V3 target{0, 0.2f, 0};           // shorten: must bend somewhere
    const V3 k2 = FootContact::solveKnee(hip, knee, foot, target);
    EXPECT_NEAR(dist(hip, k2), 0.5f, 1e-4f);
    EXPECT_NEAR(dist(k2, target), 0.5f, 1e-4f);
}

TEST(FootContactTest, BlendWeightRampsAtSpanEdges)
{
    const FootContact::Span s{10, 20};
    EXPECT_FLOAT_EQ(FootContact::blendWeight(s, 9, 3), 0.0f);
    EXPECT_FLOAT_EQ(FootContact::blendWeight(s, 21, 3), 0.0f);
    EXPECT_NEAR(FootContact::blendWeight(s, 10, 3), 1.0f / 3.0f, 1e-5f);
    EXPECT_NEAR(FootContact::blendWeight(s, 11, 3), 2.0f / 3.0f, 1e-5f);
    EXPECT_FLOAT_EQ(FootContact::blendWeight(s, 15, 3), 1.0f);
    EXPECT_NEAR(FootContact::blendWeight(s, 20, 3), 1.0f / 3.0f, 1e-5f);
    // zero blend = hard pin
    EXPECT_FLOAT_EQ(FootContact::blendWeight(s, 10, 0), 1.0f);
}

TEST(FootContactTest, GroundLevelIsRobustToAirTime)
{
    // Foot spends most of the clip in the air (jump) — ground level must
    // still track the low plateau, not the mean.
    std::vector<V3> foot;
    for (int f = 0; f < 40; ++f)
        foot.push_back({0.0f, (f < 6) ? 0.02f : 0.8f, 0.0f});
    EXPECT_NEAR(FootContact::groundLevel(foot, 1), 0.02f, 1e-4f);
}
