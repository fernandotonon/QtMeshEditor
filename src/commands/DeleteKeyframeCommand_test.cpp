#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include "commands/DeleteKeyframeCommand.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <OgreSkeletonInstance.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>

#include <cmath>

class DeleteKeyframeCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre());
        createStandardOgreMaterials();
        ASSERT_TRUE(canLoadMeshFiles());
    }
    void TearDown() override {
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(20);
    }
    QApplication* app = nullptr;
};

namespace {
Ogre::NodeAnimationTrack* findTrack(Ogre::SkeletonInstance* skel,
                                    const std::string& animName,
                                    const std::string& boneName) {
    if (!skel || !skel->hasAnimation(animName)) return nullptr;
    auto* anim = skel->getAnimation(animName);
    for (const auto& p : anim->_getNodeTrackList()) {
        if (p.second->getAssociatedNode()->getName() == boneName)
            return p.second;
    }
    return nullptr;
}
Ogre::TransformKeyFrame* findKeyframe(Ogre::NodeAnimationTrack* track, float time) {
    if (!track) return nullptr;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - time) <= 0.001f) return kf;
    }
    return nullptr;
}
}

TEST_F(DeleteKeyframeCommandTest, RedoRemovesKeyframe) {
    // The Child bone's track has keyframes at 0.0, 0.5, 1.0 (see
    // TestHelpers.h). Removing the kf at 0.5 must drop the count by 1.
    Ogre::Entity* entity = createAnimatedTestEntity("DKC_Remove");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* track = findTrack(skel, "TestAnim", "Child");
    ASSERT_NE(track, nullptr);
    auto* kf = findKeyframe(track, 0.5f);
    ASSERT_NE(kf, nullptr);
    const auto countBefore = track->getNumKeyFrames();

    DeleteKeyframeCommand cmd(entity->getName(), "TestAnim", "Child", 0.5f,
                              kf->getTranslate(), kf->getRotation(), kf->getScale());
    cmd.redo();
    EXPECT_EQ(track->getNumKeyFrames(), countBefore - 1);
    EXPECT_EQ(findKeyframe(track, 0.5f), nullptr);
}

TEST_F(DeleteKeyframeCommandTest, UndoRestoresKeyframeWithCapturedTRS) {
    // Capture the kf's TRS, delete, undo — the keyframe must reappear
    // with the same TRS values.
    Ogre::Entity* entity = createAnimatedTestEntity("DKC_RestoreTRS");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* track = findTrack(skel, "TestAnim", "Child");
    ASSERT_NE(track, nullptr);
    auto* kf = findKeyframe(track, 0.5f);
    ASSERT_NE(kf, nullptr);
    const Ogre::Vector3    origT = kf->getTranslate();
    const Ogre::Quaternion origR = kf->getRotation();
    const Ogre::Vector3    origS = kf->getScale();

    DeleteKeyframeCommand cmd(entity->getName(), "TestAnim", "Child", 0.5f, origT, origR, origS);
    cmd.redo();
    EXPECT_EQ(findKeyframe(track, 0.5f), nullptr);

    cmd.undo();
    auto* restored = findKeyframe(track, 0.5f);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->getTranslate(), origT);
    EXPECT_EQ(restored->getRotation(), origR);
    EXPECT_EQ(restored->getScale(), origS);
}

TEST_F(DeleteKeyframeCommandTest, RedoAfterUndoRemovesAgain) {
    // Round-trip: delete, undo (restore), redo (remove again).
    Ogre::Entity* entity = createAnimatedTestEntity("DKC_Roundtrip");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* track = findTrack(skel, "TestAnim", "Child");
    ASSERT_NE(track, nullptr);
    auto* kf = findKeyframe(track, 0.5f);
    ASSERT_NE(kf, nullptr);

    DeleteKeyframeCommand cmd(entity->getName(), "TestAnim", "Child", 0.5f,
                              kf->getTranslate(), kf->getRotation(), kf->getScale());
    cmd.redo();
    cmd.undo();
    EXPECT_NE(findKeyframe(track, 0.5f), nullptr);

    cmd.redo();
    EXPECT_EQ(findKeyframe(track, 0.5f), nullptr);
}

TEST_F(DeleteKeyframeCommandTest, RedoIsNoOpWhenKeyframeAlreadyMissing) {
    // Defensive: redo on a kf that doesn't exist (e.g. user deleted
    // it externally) shouldn't crash, just no-op.
    Ogre::Entity* entity = createAnimatedTestEntity("DKC_Missing");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* track = findTrack(skel, "TestAnim", "Child");
    ASSERT_NE(track, nullptr);
    const auto countBefore = track->getNumKeyFrames();

    // Time 0.25 has no keyframe in the test data.
    DeleteKeyframeCommand cmd(entity->getName(), "TestAnim", "Child", 0.25f,
                              Ogre::Vector3::ZERO, Ogre::Quaternion::IDENTITY,
                              Ogre::Vector3::UNIT_SCALE);
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_EQ(track->getNumKeyFrames(), countBefore);
}
