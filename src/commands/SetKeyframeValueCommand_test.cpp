#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <QUndoStack>

#include "SetKeyframeValueCommand.h"
#include "../Manager.h"
#include "../TestHelpers.h"

#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>

#include <cmath>

class SetKeyframeValueCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
    }
    void TearDown() override { if (app) app->processEvents(); }
    QApplication* app = nullptr;

    static Ogre::TransformKeyFrame* keyAt(Ogre::Entity* entity, float time) {
        auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                             ->_getNodeTrackList().begin()->second;
        for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
            auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
            if (std::fabs(kf->getTime() - time) < 0.001f) return kf;
        }
        return nullptr;
    }
};

TEST_F(SetKeyframeValueCommandTest, RedoUndoRoundTrip) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("SKV_RedoUndoTest");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();

    QUndoStack stack;
    stack.push(new SetKeyframeValueCommand(entity->getName(), "TestAnim", "Child", "tx", 0.5f, 9.0));

    auto* kf = keyAt(entity, 0.5f);
    ASSERT_NE(kf, nullptr);
    EXPECT_NEAR(kf->getTranslate().x, 9.0f, 1e-4);

    stack.undo();
    EXPECT_NEAR(kf->getTranslate().x, 0.5f, 1e-4); // original

    stack.redo();
    EXPECT_NEAR(kf->getTranslate().x, 9.0f, 1e-4);
}

TEST_F(SetKeyframeValueCommandTest, NoOpWhenChannelUnknown) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("SKV_UnknownChannelTest");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();

    QUndoStack stack;
    stack.push(new SetKeyframeValueCommand(entity->getName(), "TestAnim", "Child", "qq", 0.5f, 9.0));
    // Original 0.5 keyframe should be untouched.
    auto* kf = keyAt(entity, 0.5f);
    EXPECT_NEAR(kf->getTranslate().x, 0.5f, 1e-4);
}

TEST_F(SetKeyframeValueCommandTest, NoOpWhenTimeNotFound) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("SKV_MissingTimeTest");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();

    QUndoStack stack;
    stack.push(new SetKeyframeValueCommand(entity->getName(), "TestAnim", "Child", "tx", 0.42f, 9.0));
    // No keyframe at 0.42 → the original 0.0/0.5/1.0 keyframes stay intact.
    EXPECT_NEAR(keyAt(entity, 0.0f)->getTranslate().x, 0.0f, 1e-4);
    EXPECT_NEAR(keyAt(entity, 0.5f)->getTranslate().x, 0.5f, 1e-4);
    EXPECT_NEAR(keyAt(entity, 1.0f)->getTranslate().x, 0.0f, 1e-4);
}
