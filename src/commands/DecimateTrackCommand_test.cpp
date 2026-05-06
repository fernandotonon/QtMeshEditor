#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include "DecimateTrackCommand.h"
#include "../Manager.h"
#include "../TestHelpers.h"

#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>

#include <cmath>

class DecimateTrackCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed";
        createStandardOgreMaterials();
    }
    void TearDown() override {
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(20);
    }
    QApplication* app = nullptr;

    static Ogre::NodeAnimationTrack* trackOf(Ogre::Entity* e) {
        auto* anim = e->getSkeleton()->getAnimation("TestAnim");
        const auto& tracks = anim->_getNodeTrackList();
        return tracks.empty() ? nullptr : tracks.begin()->second;
    }
};

TEST_F(DecimateTrackCommandTest, ReducesDenseTrackToTargetFps) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("Decimate_Reduce");
    ASSERT_NE(entity, nullptr);
    auto* track = trackOf(entity);
    ASSERT_NE(track, nullptr);

    // Synthesize a dense 60 FPS-style track (over the existing 1-second
    // length) by inserting 50 evenly-spaced keys between t=0 and t=1.
    for (int i = 1; i < 50; ++i) {
        const float t = static_cast<float>(i) / 50.0f;
        if (t < 0.99f) {
            auto* kf = track->createNodeKeyFrame(t);
            kf->setTranslate(Ogre::Vector3::ZERO);
            kf->setRotation(Ogre::Quaternion::IDENTITY);
            kf->setScale(Ogre::Vector3::UNIT_SCALE);
        }
    }
    const int dense = track->getNumKeyFrames();

    DecimateTrackCommand cmd(entity->getName(), "TestAnim",
                             track->getAssociatedNode()->getName(), 30);
    cmd.redo();

    const int reduced = track->getNumKeyFrames();
    EXPECT_LT(reduced, dense) << "decimation should drop keys";
    // 30 FPS over 1 second ≈ 30 keys at most.
    EXPECT_LE(reduced, 35);
}

TEST_F(DecimateTrackCommandTest, UndoRestoresOriginalCount) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("Decimate_Undo");
    ASSERT_NE(entity, nullptr);
    auto* track = trackOf(entity);
    ASSERT_NE(track, nullptr);

    for (int i = 1; i < 50; ++i) {
        const float t = static_cast<float>(i) / 50.0f;
        if (t < 0.99f) {
            auto* kf = track->createNodeKeyFrame(t);
            kf->setTranslate(Ogre::Vector3::ZERO);
            kf->setRotation(Ogre::Quaternion::IDENTITY);
            kf->setScale(Ogre::Vector3::UNIT_SCALE);
        }
    }
    const int dense = track->getNumKeyFrames();

    DecimateTrackCommand cmd(entity->getName(), "TestAnim",
                             track->getAssociatedNode()->getName(), 30);
    cmd.redo();
    cmd.undo();
    EXPECT_EQ(track->getNumKeyFrames(), dense);
}

TEST_F(DecimateTrackCommandTest, FirstAndLastFramesPreserved) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("Decimate_Endpoints");
    ASSERT_NE(entity, nullptr);
    auto* track = trackOf(entity);
    ASSERT_NE(track, nullptr);

    // Densify the track first so decimate has something to drop —
    // otherwise the original 3 sparse anchors fit any low-fps target
    // without modification and this test wouldn't exercise the
    // endpoint-preservation logic.
    for (int i = 1; i < 50; ++i) {
        const float t = static_cast<float>(i) / 50.0f;
        if (t < 0.99f) {
            auto* kf = track->createNodeKeyFrame(t);
            kf->setTranslate(Ogre::Vector3::ZERO);
            kf->setRotation(Ogre::Quaternion::IDENTITY);
            kf->setScale(Ogre::Vector3::UNIT_SCALE);
        }
    }
    const float firstT = track->getKeyFrame(0)->getTime();
    const float lastT  = track->getKeyFrame(track->getNumKeyFrames() - 1)->getTime();
    const int dense = track->getNumKeyFrames();

    DecimateTrackCommand cmd(entity->getName(), "TestAnim",
                             track->getAssociatedNode()->getName(), 5);
    cmd.redo();

    EXPECT_LT(track->getNumKeyFrames(), dense)
        << "decimate must actually drop keys for the endpoint check to be meaningful";
    EXPECT_NEAR(track->getKeyFrame(0)->getTime(), firstT, 1e-4);
    EXPECT_NEAR(track->getKeyFrame(track->getNumKeyFrames() - 1)->getTime(),
                lastT, 1e-4);
}
