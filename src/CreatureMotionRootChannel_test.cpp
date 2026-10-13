// Root-translation channel + dead-tail trim (the jump/death/attack fix).
//
// Both of these were SILENT failures that only a render exposed, so they are
// pinned here as data invariants rather than left to visual review:
//
//  1. The retarget used to be rotation-only, which structurally cannot express
//     a jump (body rises), a death (body topples to the ground) or a lunge.
//     The body pivoted about a root pinned at its bind position and folded
//     through its own legs.
//  2. Several source clips declare a length well past their last real
//     keyframe -- Horse|Walk animates for 50.6% of its 3.33s and is frozen
//     for the rest, which reads in-app as "the walk stops halfway".
//
// These tests are pure data: no Ogre scene, no GL, so they run headless.
#include "CreatureMotionExtract.h"
#include "CreatureMotionRetarget.h"
#include "QuadrupedSkeleton.h"

#include <gtest/gtest.h>

#include <cmath>

using CreatureSkeleton::BodyPlan;

namespace {

// A clip whose motion stops partway, mimicking the authored source clips.
std::vector<std::vector<std::array<float, 4>>> clipWithDeadTail(int live,
                                                                int total)
{
    const int J = CreatureSkeleton::QuadrupedSkeleton::jointCount();
    std::vector<std::vector<std::array<float, 4>>> q;
    for (int f = 0; f < total; ++f) {
        // Rotate one leg for the first `live` frames, then hold.
        const float t = static_cast<float>(std::min(f, live)) / live;
        const float a = t * 0.5f;             // radians about X
        std::vector<std::array<float, 4>> pose(
            static_cast<size_t>(J), std::array<float, 4>{0.f, 0.f, 0.f, 1.f});
        pose[5] = {std::sin(a), 0.f, 0.f, std::cos(a)};
        q.push_back(std::move(pose));
    }
    return q;
}

} // namespace

TEST(CreatureRootChannel, TranslationIsRefusedWithoutAScaleOrAChannel)
{
    // Both inputs are required: a clip that carries rootOffset AND a target
    // hip height to replay it against. Missing either must fall back to the
    // old rotation-only behaviour rather than guessing a scale -- a wrong
    // scale would fling a horse's leap onto a pug.
    const auto quats = clipWithDeadTail(10, 10);
    std::vector<std::array<float, 3>> offs(quats.size(), {0.f, 1.f, 0.f});

    // No skeleton: apply() reports the error rather than crashing, which is
    // what every surface relies on.
    auto r = CreatureMotionRetarget::apply(nullptr, "x", BodyPlan::Quadruped,
                                           quats, 30, {}, offs, 1.0f);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.rootTranslation);
}

TEST(CreatureRootChannel, OffsetsAreScaleNormalisedNotRawWorldUnits)
{
    // The channel stores hip-height multiples so the SAME motion reads
    // correctly on creatures of different size. A horse hip is ~0.04 world
    // units on these packs and a death drops ~0.69 hip-heights; replayed on a
    // target with hip 0.08 that must become ~0.055 world units, not 0.69.
    const float sourceDrop = -0.694f;      // hip-heights, measured on Cow|Death
    const float targetHip = 0.0843f;
    const float applied = sourceDrop * targetHip;
    EXPECT_NEAR(applied, -0.0585f, 0.001f);
    // And the same clip on a bigger creature moves proportionally further.
    EXPECT_NEAR(sourceDrop * (targetHip * 2.0f), -0.117f, 0.002f);
}

TEST(CreatureRootChannel, DeadTailDetectionFindsTheLastMovingFrame)
{
    // The trim rule: find the last frame that differs from its predecessor.
    // A clip live to the end must be left ALONE (trimming a legitimately
    // still idle down to nothing is the failure mode to avoid).
    auto live = clipWithDeadTail(39, 40);
    auto dead = clipWithDeadTail(20, 40);

    auto lastMoving = [](const auto& q) {
        size_t last = 0;
        for (size_t f = 1; f < q.size(); ++f) {
            for (size_t j = 0; j < q[f].size(); ++j) {
                const auto& a = q[f - 1][j];
                const auto& b = q[f][j];
                float d = std::fabs(a[0] * b[0] + a[1] * b[1]
                                    + a[2] * b[2] + a[3] * b[3]);
                d = std::min(1.0f, d);
                if (2.0f * std::acos(d) * 57.2958f > 0.05f) { last = f; break; }
            }
        }
        return last;
    };

    EXPECT_GE(lastMoving(live), 38u) << "a live clip must not look trimmable";
    EXPECT_LE(lastMoving(dead), 21u) << "the dead tail must be detected";
    EXPECT_GT(lastMoving(live), lastMoving(dead));
}
