#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include "commands/AddKeyframeCommand.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <OgreSkeletonInstance.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>

#include <cmath>

class AddKeyframeCommandTest : public ::testing::Test {
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

TEST_F(AddKeyframeCommandTest, KeyframeUpdatedRoundTrips) {
    // The "Child" bone has a track with keyframes at 0.0, 0.5, 1.0
    // (see TestHelpers.h). Updating the kf at 0.5 must be undoable.
    Ogre::Entity* entity = createAnimatedTestEntity("AKC_Updated");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* track = findTrack(skel, "TestAnim", "Child");
    ASSERT_NE(track, nullptr);
    auto* kf = findKeyframe(track, 0.5f);
    ASSERT_NE(kf, nullptr);
    const Ogre::Vector3 origT = kf->getTranslate();

    AddKeyframeCommand cmd(
        skel, "TestAnim", "Child", 0.5f,
        AddKeyframeCommand::Mode::KeyframeUpdated,
        origT, kf->getRotation(), kf->getScale(),
        Ogre::Vector3(99, 0, 0), kf->getRotation(), kf->getScale());
    cmd.redo();
    auto* kfAfter = findKeyframe(track, 0.5f);
    ASSERT_NE(kfAfter, nullptr);
    EXPECT_EQ(kfAfter->getTranslate(), Ogre::Vector3(99, 0, 0));

    cmd.undo();
    auto* kfRestored = findKeyframe(track, 0.5f);
    ASSERT_NE(kfRestored, nullptr);
    EXPECT_EQ(kfRestored->getTranslate(), origT);
}

TEST_F(AddKeyframeCommandTest, KeyframeCreatedUndoRemovesKeyframe) {
    // Create a new keyframe at 0.25 (time with no existing kf), then
    // undo it. The keyframe must be removed; existing kfs untouched.
    Ogre::Entity* entity = createAnimatedTestEntity("AKC_Created");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* track = findTrack(skel, "TestAnim", "Child");
    ASSERT_NE(track, nullptr);
    const auto countBefore = track->getNumKeyFrames();

    AddKeyframeCommand cmd(
        skel, "TestAnim", "Child", 0.25f,
        AddKeyframeCommand::Mode::KeyframeCreated,
        Ogre::Vector3::ZERO, Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE,
        Ogre::Vector3(2, 0, 0), Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE);
    cmd.redo();
    EXPECT_EQ(track->getNumKeyFrames(), countBefore + 1);
    EXPECT_NE(findKeyframe(track, 0.25f), nullptr);

    cmd.undo();
    EXPECT_EQ(track->getNumKeyFrames(), countBefore);
    EXPECT_EQ(findKeyframe(track, 0.25f), nullptr);
}

TEST_F(AddKeyframeCommandTest, TrackCreatedUndoDestroysTrack) {
    // Lazy-creating a track (e.g., for a non-rigged bone the user
    // wants to animate) and undoing must destroy the track entirely.
    Ogre::Entity* entity = createAnimatedTestEntity("AKC_TrackCreated");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    // Add a bone with no track in TestAnim — simulating a non-rigged
    // bone the user just edited via the gizmo.
    Ogre::Bone* extra = skel->createBone("Extra", 2);
    extra->setPosition(Ogre::Vector3(0, 0, 1));
    skel->getBone("Root")->addChild(extra);

    ASSERT_EQ(findTrack(skel, "TestAnim", "Extra"), nullptr);

    AddKeyframeCommand cmd(
        skel, "TestAnim", "Extra", 0.0f,
        AddKeyframeCommand::Mode::TrackCreated,
        Ogre::Vector3::ZERO, Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE,
        Ogre::Vector3(0.5f, 0, 0), Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE);
    cmd.redo();
    EXPECT_NE(findTrack(skel, "TestAnim", "Extra"), nullptr);

    cmd.undo();
    EXPECT_EQ(findTrack(skel, "TestAnim", "Extra"), nullptr)
        << "Undo did not destroy the lazy-created track";
}

TEST_F(AddKeyframeCommandTest, RedoAfterUndoRestoresKeyframe) {
    // Round-trip through undo and redo to confirm symmetry.
    Ogre::Entity* entity = createAnimatedTestEntity("AKC_Redo");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* track = findTrack(skel, "TestAnim", "Child");
    ASSERT_NE(track, nullptr);
    const auto countBefore = track->getNumKeyFrames();

    AddKeyframeCommand cmd(
        skel, "TestAnim", "Child", 0.75f,
        AddKeyframeCommand::Mode::KeyframeCreated,
        Ogre::Vector3::ZERO, Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE,
        Ogre::Vector3(3, 0, 0), Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE);
    cmd.redo();
    EXPECT_EQ(track->getNumKeyFrames(), countBefore + 1);

    cmd.undo();
    EXPECT_EQ(track->getNumKeyFrames(), countBefore);

    cmd.redo();
    EXPECT_EQ(track->getNumKeyFrames(), countBefore + 1);
    auto* kf = findKeyframe(track, 0.75f);
    ASSERT_NE(kf, nullptr);
    EXPECT_EQ(kf->getTranslate(), Ogre::Vector3(3, 0, 0));
}
