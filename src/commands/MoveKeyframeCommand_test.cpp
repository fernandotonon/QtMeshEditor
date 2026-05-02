#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <QUndoStack>

#include "MoveKeyframeCommand.h"
#include "../Manager.h"
#include "../TestHelpers.h"

#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>

#include <cmath>

class MoveKeyframeCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
    }
    void TearDown() override {
        if (app) app->processEvents();
    }
    QApplication* app = nullptr;

    static float keyTimeAt(Ogre::NodeAnimationTrack* track, unsigned short i) {
        return track->getKeyFrame(i)->getTime();
    }
};

TEST_F(MoveKeyframeCommandTest, RedoMovesUndoRestores) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("MKF_RedoUndoTest");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();

    // TestAnim has keyframes at 0.0, 0.5, 1.0 on the Child bone.
    QUndoStack stack;
    stack.push(new MoveKeyframeCommand(skel, "TestAnim", "Child", 0.5f, 0.7f));

    auto* track = skel->getAnimation("TestAnim")->_getNodeTrackList().begin()->second;
    bool found07 = false;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        if (std::fabs(keyTimeAt(track, i) - 0.7f) < 0.001f) { found07 = true; break; }
    }
    EXPECT_TRUE(found07);

    stack.undo();
    bool found05 = false; found07 = false;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        const float t = keyTimeAt(track, i);
        if (std::fabs(t - 0.5f) < 0.001f) found05 = true;
        if (std::fabs(t - 0.7f) < 0.001f) found07 = true;
    }
    EXPECT_TRUE(found05);
    EXPECT_FALSE(found07);

    stack.redo();
    found07 = false;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        if (std::fabs(keyTimeAt(track, i) - 0.7f) < 0.001f) { found07 = true; break; }
    }
    EXPECT_TRUE(found07);
}

TEST_F(MoveKeyframeCommandTest, PreservesTRSValuesAcrossMove) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("MKF_PreservesTRSTest");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* track = skel->getAnimation("TestAnim")->_getNodeTrackList().begin()->second;

    // Capture the keyframe at 0.5 before the move.
    Ogre::Vector3 origT, origS;
    Ogre::Quaternion origR;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - 0.5f) < 0.001f) {
            origT = kf->getTranslate();
            origR = kf->getRotation();
            origS = kf->getScale();
            break;
        }
    }

    QUndoStack stack;
    stack.push(new MoveKeyframeCommand(skel, "TestAnim", "Child", 0.5f, 0.7f));

    // Find the moved keyframe and verify TRS is preserved.
    bool found = false;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - 0.7f) < 0.001f) {
            EXPECT_EQ(kf->getTranslate(), origT);
            EXPECT_EQ(kf->getRotation(), origR);
            EXPECT_EQ(kf->getScale(), origS);
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(MoveKeyframeCommandTest, NoOpWhenSearchTimeMissing) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("MKF_MissingTest");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* track = skel->getAnimation("TestAnim")->_getNodeTrackList().begin()->second;
    const int before = track->getNumKeyFrames();

    QUndoStack stack;
    // No keyframe at 0.42 — command runs but moves nothing.
    stack.push(new MoveKeyframeCommand(skel, "TestAnim", "Child", 0.42f, 0.6f));
    EXPECT_EQ(track->getNumKeyFrames(), before);
}
