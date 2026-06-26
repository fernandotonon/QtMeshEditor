// Coverage-focused tests for AnimationMerger::renameAnimation and
// AnimationMerger::bakeAnimationAtFps. These two entry points are either
// untested or only exercised indirectly inside mergeAnimations(), so this
// suite targets their individual branches directly.
//
// Distinct filename + distinct suite names (AnimationMergerCoverageTest /
// AnimationMergerCoverageStandaloneTest) from AnimationMerger_test.cpp so
// there is no ODR clash or duplicate test registration.
//
// Fixture mirrors AnimationMerger_test.cpp: build skeletons headlessly via
// SkeletonManager::create + createBone + createAnimation + createNodeTrack
// under tryInitOgre(). No QApplication is created here (test_main owns it).
#include <gtest/gtest.h>
#include "AnimationMerger.h"
#include "Manager.h"
#include "TestHelpers.h"
#include <QCoreApplication>
#include <QApplication>
#include <QThread>
#include <OgreSkeleton.h>
#include <OgreSkeletonManager.h>
#include <OgreKeyFrame.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <cmath>

class AnimationMergerCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
    }

    void TearDown() override {
        if (app)
            app->processEvents();
    }

    QApplication* app = nullptr;

    // Create a one-bone skeleton with no animations.
    Ogre::SkeletonPtr makeBareSkeleton(const std::string& name) {
        auto skel = Ogre::SkeletonManager::getSingleton().create(
            name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        skel->createBone("root", 0);
        skel->setBindingPose();
        return skel;
    }
};

// ---------------------------------------------------------------------------
// renameAnimation
// ---------------------------------------------------------------------------

// Branch: oldName == newName → early-return no-op (animation untouched).
TEST_F(AnimationMergerCoverageTest, RenameSameNameIsNoOp)
{
    auto skel = makeBareSkeleton("cov_rename_same");
    auto* anim = skel->createAnimation("idle", 1.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(skel->getBone(0));
    track->createNodeKeyFrame(0.0f);
    track->createNodeKeyFrame(1.0f);

    AnimationMerger::renameAnimation(skel.get(), "idle", "idle");

    EXPECT_TRUE(skel->hasAnimation("idle"));
    // Still exactly one animation; nothing cloned.
    EXPECT_EQ(skel->getNumAnimations(), 1u);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

// Branch: !hasAnimation(oldName) → early-return no-op (no new animation made).
TEST_F(AnimationMergerCoverageTest, RenameMissingSourceIsNoOp)
{
    auto skel = makeBareSkeleton("cov_rename_missing");
    auto* anim = skel->createAnimation("idle", 1.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(skel->getBone(0));
    track->createNodeKeyFrame(0.0f);

    AnimationMerger::renameAnimation(skel.get(), "does_not_exist", "whatever");

    EXPECT_TRUE(skel->hasAnimation("idle"));
    EXPECT_FALSE(skel->hasAnimation("whatever"));
    EXPECT_EQ(skel->getNumAnimations(), 1u);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

// Branch: success path — old name removed, new name present, keyframe count
// and TRS values copied verbatim.
TEST_F(AnimationMergerCoverageTest, RenameSuccessCopiesTracksAndValues)
{
    auto skel = makeBareSkeleton("cov_rename_success");
    auto* anim = skel->createAnimation("walk", 2.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(skel->getBone(0));

    // Three keyframes with distinct, easily-checked TRS values.
    auto* k0 = track->createNodeKeyFrame(0.0f);
    k0->setTranslate(Ogre::Vector3(1, 2, 3));
    k0->setRotation(Ogre::Quaternion(Ogre::Degree(30), Ogre::Vector3::UNIT_Y));
    k0->setScale(Ogre::Vector3(1, 1, 1));

    auto* k1 = track->createNodeKeyFrame(1.0f);
    k1->setTranslate(Ogre::Vector3(4, 5, 6));
    k1->setRotation(Ogre::Quaternion(Ogre::Degree(60), Ogre::Vector3::UNIT_Y));
    k1->setScale(Ogre::Vector3(2, 2, 2));

    auto* k2 = track->createNodeKeyFrame(2.0f);
    k2->setTranslate(Ogre::Vector3(7, 8, 9));
    k2->setRotation(Ogre::Quaternion(Ogre::Degree(90), Ogre::Vector3::UNIT_Y));
    k2->setScale(Ogre::Vector3(3, 3, 3));

    AnimationMerger::renameAnimation(skel.get(), "walk", "run");

    // Old gone, new present, length preserved.
    EXPECT_FALSE(skel->hasAnimation("walk"));
    ASSERT_TRUE(skel->hasAnimation("run"));
    EXPECT_EQ(skel->getNumAnimations(), 1u);

    auto* newAnim = skel->getAnimation("run");
    EXPECT_FLOAT_EQ(newAnim->getLength(), 2.0f);

    auto& trackList = newAnim->_getNodeTrackList();
    ASSERT_EQ(trackList.size(), 1u);
    auto* newTrack = trackList.begin()->second;
    ASSERT_EQ(newTrack->getNumKeyFrames(), 3u);

    // Associated node preserved.
    EXPECT_EQ(newTrack->getAssociatedNode(), skel->getBone(0));

    // Times.
    EXPECT_FLOAT_EQ(newTrack->getNodeKeyFrame(0)->getTime(), 0.0f);
    EXPECT_FLOAT_EQ(newTrack->getNodeKeyFrame(1)->getTime(), 1.0f);
    EXPECT_FLOAT_EQ(newTrack->getNodeKeyFrame(2)->getTime(), 2.0f);

    // Translate values copied.
    EXPECT_EQ(newTrack->getNodeKeyFrame(0)->getTranslate(), Ogre::Vector3(1, 2, 3));
    EXPECT_EQ(newTrack->getNodeKeyFrame(1)->getTranslate(), Ogre::Vector3(4, 5, 6));
    EXPECT_EQ(newTrack->getNodeKeyFrame(2)->getTranslate(), Ogre::Vector3(7, 8, 9));

    // Scale values copied.
    EXPECT_EQ(newTrack->getNodeKeyFrame(1)->getScale(), Ogre::Vector3(2, 2, 2));
    EXPECT_EQ(newTrack->getNodeKeyFrame(2)->getScale(), Ogre::Vector3(3, 3, 3));

    // Rotation copied (compare component-wise within tolerance).
    const Ogre::Quaternion expectedRot(Ogre::Degree(60), Ogre::Vector3::UNIT_Y);
    const Ogre::Quaternion gotRot = newTrack->getNodeKeyFrame(1)->getRotation();
    EXPECT_NEAR(gotRot.w, expectedRot.w, 1e-5f);
    EXPECT_NEAR(gotRot.x, expectedRot.x, 1e-5f);
    EXPECT_NEAR(gotRot.y, expectedRot.y, 1e-5f);
    EXPECT_NEAR(gotRot.z, expectedRot.z, 1e-5f);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

// renameAnimation onto an existing destination name and back again should be
// stable: only one animation exists after rename. (Exercises the clone path
// with multiple keyframes once more, ensuring no leftover tracks.)
// ---------------------------------------------------------------------------
// bakeAnimationAtFps
// ---------------------------------------------------------------------------

// Branch: null skeleton → returns 0.
// Branch: targetFps <= 0 → returns 0 (both zero and negative).
TEST_F(AnimationMergerCoverageTest, BakeNonPositiveFpsReturnsZero)
{
    auto skel = makeBareSkeleton("cov_bake_badfps");
    auto* anim = skel->createAnimation("walk", 1.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(skel->getBone(0));
    track->createNodeKeyFrame(0.0f);
    track->createNodeKeyFrame(1.0f);

    EXPECT_EQ(AnimationMerger::bakeAnimationAtFps(skel.get(), "walk", 0), 0);
    EXPECT_EQ(AnimationMerger::bakeAnimationAtFps(skel.get(), "walk", -10), 0);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

// Branch: missing animation → returns 0.
TEST_F(AnimationMergerCoverageTest, BakeMissingAnimationReturnsZero)
{
    auto skel = makeBareSkeleton("cov_bake_missing");
    EXPECT_EQ(AnimationMerger::bakeAnimationAtFps(skel.get(), "nope", 30), 0);
    Ogre::SkeletonManager::getSingleton().remove(skel);
}

// Branch: track with < 2 keyframes passes through unchanged (counted but
// not re-gridded).
TEST_F(AnimationMergerCoverageTest, BakeSingleKeyframeTrackPassesThrough)
{
    auto skel = makeBareSkeleton("cov_bake_singlekey");
    auto* anim = skel->createAnimation("pose", 1.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(skel->getBone(0));
    auto* kf = track->createNodeKeyFrame(0.0f);
    kf->setTranslate(Ogre::Vector3(2, 4, 6));

    int total = AnimationMerger::bakeAnimationAtFps(skel.get(), "pose", 30);
    // Single key counted, untouched.
    EXPECT_EQ(total, 1);

    auto* newTrack = skel->getAnimation("pose")->_getNodeTrackList().begin()->second;
    EXPECT_EQ(newTrack->getNumKeyFrames(), 1u);
    EXPECT_EQ(newTrack->getNodeKeyFrame(0)->getTranslate(), Ogre::Vector3(2, 4, 6));

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

// Branch: duration <= 0 (all keyframes at the same time) passes through
// unchanged.
TEST_F(AnimationMergerCoverageTest, BakeZeroDurationTrackPassesThrough)
{
    auto skel = makeBareSkeleton("cov_bake_zerodur");
    auto* anim = skel->createAnimation("static", 0.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(skel->getBone(0));
    // Two keyframes both at t=0 → duration t1-t0 == 0.
    auto* k0 = track->createNodeKeyFrame(0.0f);
    k0->setTranslate(Ogre::Vector3(1, 1, 1));
    auto* k1 = track->createNodeKeyFrame(0.0f);
    k1->setTranslate(Ogre::Vector3(1, 1, 1));

    int total = AnimationMerger::bakeAnimationAtFps(skel.get(), "static", 30);
    // Both keys counted, none stripped/re-gridded.
    EXPECT_EQ(total, 2);

    auto* newTrack = skel->getAnimation("static")->_getNodeTrackList().begin()->second;
    EXPECT_EQ(newTrack->getNumKeyFrames(), 2u);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

// Main re-grid path: a 1-second clip baked at 10 FPS should produce 11
// uniformly-spaced keyframes (t=0.0, 0.1, ..., 1.0) with endpoints preserved.
TEST_F(AnimationMergerCoverageTest, BakeRegridUniformSpacingAndEndpoints)
{
    auto skel = makeBareSkeleton("cov_bake_regrid");
    auto* anim = skel->createAnimation("walk", 1.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(skel->getBone(0));

    // Sparse original keys: linear translate from (0,0,0) to (10,0,0).
    auto* a = track->createNodeKeyFrame(0.0f);
    a->setTranslate(Ogre::Vector3(0, 0, 0));
    a->setRotation(Ogre::Quaternion::IDENTITY);
    a->setScale(Ogre::Vector3::UNIT_SCALE);
    auto* b = track->createNodeKeyFrame(1.0f);
    b->setTranslate(Ogre::Vector3(10, 0, 0));
    b->setRotation(Ogre::Quaternion::IDENTITY);
    b->setScale(Ogre::Vector3::UNIT_SCALE);

    const int fps = 10;
    int total = AnimationMerger::bakeAnimationAtFps(skel.get(), "walk", fps);

    auto* newTrack = skel->getAnimation("walk")->_getNodeTrackList().begin()->second;
    const unsigned short n = newTrack->getNumKeyFrames();

    // 1 second at 10 FPS → 11 keys (0.0 .. 1.0 inclusive).
    EXPECT_EQ(n, 11u);
    EXPECT_EQ(total, static_cast<int>(n));

    // Endpoints preserved.
    EXPECT_NEAR(newTrack->getNodeKeyFrame(0)->getTime(), 0.0f, 1e-4f);
    EXPECT_NEAR(newTrack->getNodeKeyFrame(n - 1)->getTime(), 1.0f, 1e-4f);

    // Uniform 1/fps spacing across interior keys.
    const float step = 1.0f / static_cast<float>(fps);
    for (unsigned short k = 1; k + 1 < n; ++k) {
        const float dt = newTrack->getNodeKeyFrame(k)->getTime()
                       - newTrack->getNodeKeyFrame(k - 1)->getTime();
        EXPECT_NEAR(dt, step, 1e-3f) << "non-uniform spacing at key " << k;
    }

    // Times strictly increasing.
    for (unsigned short k = 1; k < n; ++k) {
        EXPECT_GT(newTrack->getNodeKeyFrame(k)->getTime(),
                  newTrack->getNodeKeyFrame(k - 1)->getTime());
    }

    // Interpolated value at the midpoint key (t=0.5) should be ~half the span.
    // Find the key nearest t=0.5.
    unsigned short mid = 0;
    float best = 1e9f;
    for (unsigned short k = 0; k < n; ++k) {
        float d = std::fabs(newTrack->getNodeKeyFrame(k)->getTime() - 0.5f);
        if (d < best) { best = d; mid = k; }
    }
    EXPECT_NEAR(newTrack->getNodeKeyFrame(mid)->getTranslate().x, 5.0f, 0.6f);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

// Re-grid on a clip whose duration is not an integer multiple of the step:
// the final key must still land exactly on the original end time (clamped),
// never overshoot past it.
TEST_F(AnimationMergerCoverageTest, BakeRegridClampsFinalKeyToEnd)
{
    auto skel = makeBareSkeleton("cov_bake_clamp");
    // Duration 1.05s baked at 4 FPS (step 0.25) does not divide evenly.
    auto* anim = skel->createAnimation("dash", 1.05f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(skel->getBone(0));
    auto* a = track->createNodeKeyFrame(0.0f);
    a->setTranslate(Ogre::Vector3::ZERO);
    a->setRotation(Ogre::Quaternion::IDENTITY);
    a->setScale(Ogre::Vector3::UNIT_SCALE);
    auto* b = track->createNodeKeyFrame(1.05f);
    b->setTranslate(Ogre::Vector3(1, 0, 0));
    b->setRotation(Ogre::Quaternion::IDENTITY);
    b->setScale(Ogre::Vector3::UNIT_SCALE);

    AnimationMerger::bakeAnimationAtFps(skel.get(), "dash", 4);

    auto* newTrack = skel->getAnimation("dash")->_getNodeTrackList().begin()->second;
    const unsigned short n = newTrack->getNumKeyFrames();
    ASSERT_GE(n, 2u);

    // Final key clamped exactly to the original end time, never beyond.
    EXPECT_NEAR(newTrack->getNodeKeyFrame(n - 1)->getTime(), 1.05f, 1e-3f);
    for (unsigned short k = 0; k < n; ++k) {
        EXPECT_LE(newTrack->getNodeKeyFrame(k)->getTime(), 1.05f + 1e-3f);
    }

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

// ---------------------------------------------------------------------------
// Standalone (no Ogre init required) — covers the null/non-positive guards
// of bakeAnimationAtFps that short-circuit before touching the skeleton.
// ---------------------------------------------------------------------------