// Audio2Face (#1019): the shared name-binding and key-selection rules.
//
// These are pure logic, and they are the pieces three surfaces (CLI,
// Inspector, MCP) now depend on — getting them wrong is invisible until an
// animation looks subtly bad, so they are pinned here rather than left to an
// end-to-end check that needs a 320 MB model.
#include <gtest/gtest.h>

#include "AudioToFace/A2FPredictor.h"
#include "AudioToFace/LipsyncApply.h"

using AudioToFace::bindPoseNames;
using AudioToFace::selectKeyFrames;
using AudioToFace::PredictResult;

namespace {

/// A take with `n` frames where channel `ch` follows `values`.
PredictResult take(const std::vector<std::vector<float>>& perFrame)
{
    PredictResult r;
    for (size_t i = 0; i < perFrame.size(); ++i) {
        AudioToFace::FaceFrame f;
        f.timeSec = double(i) / 30.0;
        f.weights = perFrame[i];
        r.frames.push_back(std::move(f));
    }
    return r;
}

}  // namespace

// A rig may spell the target `JawOpen`, `jawopen` or `_jawOpen`; all three
// must drive, or half the face silently stops moving.
TEST(LipsyncApply, MatchesNamesIgnoringCaseAndLeadingUnderscore)
{
    const QStringList poses{"jawOpen", "mouthClose", "browInnerUp"};
    const QStringList targets{"JawOpen", "_mouthclose", "somethingElse"};

    const auto b = bindPoseNames(poses, targets);
    ASSERT_TRUE(b.ok()) << b.error.toStdString();
    EXPECT_EQ(b.poseTarget[0], QStringLiteral("JawOpen"));
    EXPECT_EQ(b.poseTarget[1], QStringLiteral("_mouthclose"));
    EXPECT_TRUE(b.poseTarget[2].isEmpty());
    EXPECT_EQ(b.matched.size(), 2);
    ASSERT_EQ(b.unmatched.size(), 1);
    EXPECT_EQ(b.unmatched.first(), QStringLiteral("browInnerUp"));
}

// An override naming a target the mesh lacks must be REFUSED, not quietly
// replaced by name matching — the caller asked for a specific binding, and a
// silent fallback looks like the mapping was honoured.
TEST(LipsyncApply, OverrideToAMissingTargetIsRefused)
{
    const QStringList poses{"jawOpen"};
    const QStringList targets{"jawOpen"};
    QHash<QString, QString> ov;
    ov.insert(QStringLiteral("jawOpen"), QStringLiteral("NoSuchTarget"));

    const auto b = bindPoseNames(poses, targets, ov);
    EXPECT_FALSE(b.ok());
    EXPECT_TRUE(b.error.contains(QStringLiteral("NoSuchTarget"))) << b.error.toStdString();
}

TEST(LipsyncApply, OverrideWinsOverNameMatchingAndIgnoreSkips)
{
    const QStringList poses{"jawOpen", "cheekPuff"};
    const QStringList targets{"jawOpen", "JawDrop", "cheekPuff"};
    QHash<QString, QString> ov;
    ov.insert(QStringLiteral("jawOpen"), QStringLiteral("JawDrop"));
    QSet<QString> ig;
    ig.insert(QStringLiteral("cheekPuff"));

    const auto b = bindPoseNames(poses, targets, ov, ig);
    ASSERT_TRUE(b.ok()) << b.error.toStdString();
    EXPECT_EQ(b.poseTarget[0], QStringLiteral("JawDrop")) << "override must win";
    EXPECT_TRUE(b.poseTarget[1].isEmpty()) << "ignored pose must not bind";
    ASSERT_EQ(b.ignored.size(), 1);
    EXPECT_EQ(b.ignored.first(), QStringLiteral("cheekPuff"));
}

// A mesh with no ARKit-ish names at all is an error the user must see, not a
// clip with zero channels that plays as a frozen face.
TEST(LipsyncApply, NothingMatchingIsAnErrorNotAnEmptyClip)
{
    const auto b = bindPoseNames({"jawOpen"}, {"Bone_01", "Bone_02"});
    EXPECT_FALSE(b.ok());
    EXPECT_FALSE(b.error.isEmpty());
}

// The epsilon decides WHEN to key. First and last frames always key, so the
// clip starts and ends at a defined pose rather than holding whatever the
// previous clip left behind.
TEST(LipsyncApply, KeysFirstLastAndFramesThatMoved)
{
    // Channel 0 moves only at frame 2; frames 1 and 3 are quiet.
    const auto pred = take({{0.00f}, {0.001f}, {0.50f}, {0.501f}, {0.502f}});
    const auto b = bindPoseNames({"jawOpen"}, {"jawOpen"});
    ASSERT_TRUE(b.ok());

    const auto keys = selectKeyFrames(pred, b, 0.01f);
    ASSERT_FALSE(keys.empty());
    EXPECT_EQ(keys.front(), 0u) << "first frame is always keyed";
    EXPECT_EQ(keys.back(), 4u) << "last frame is always keyed";
    EXPECT_NE(std::find(keys.begin(), keys.end(), 2u), keys.end())
        << "the frame where the channel moved must be keyed";
    EXPECT_LT(keys.size(), pred.frames.size())
        << "quiet frames must still be skipped — the saving is the point";
}

// An UNMATCHED channel must not create key times: it is never written, so
// keying for it would add keyframes that carry nothing.
TEST(LipsyncApply, UnmatchedChannelsDoNotDriveKeySelection)
{
    // Channel 1 (unmatched) swings wildly; channel 0 (matched) is static.
    const auto pred = take({{0.1f, 0.0f}, {0.1f, 0.9f}, {0.1f, 0.0f}, {0.1f, 0.9f}});
    const auto b = bindPoseNames({"jawOpen", "browInnerUp"}, {"jawOpen"});
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.poseTarget[1].isEmpty()) << "fixture assumes channel 1 unmatched";

    const auto keys = selectKeyFrames(pred, b, 0.01f);
    EXPECT_EQ(keys.size(), 2u) << "only first and last: the matched channel never moves";
}
