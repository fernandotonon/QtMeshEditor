#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include "ResampleCurveCommand.h"
#include "../CurveEditModel.h"
#include "../Manager.h"
#include "../TestHelpers.h"

#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>

#include <cmath>

class ResampleCurveCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        CurveEditModel::kill();
        Manager::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
    }
    void TearDown() override {
        CurveEditModel::kill();
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

TEST_F(ResampleCurveCommandTest, RedoInsertsInteriorKeyframes) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("RC_Redo");
    ASSERT_NE(entity, nullptr);

    auto* track = trackOf(entity);
    ASSERT_NE(track, nullptr);
    const int beforeCount = track->getNumKeyFrames();

    // Stepped on the upstream key produces high curvature → 60 Hz boost.
    CurveEditModel::instance()->setMode(
        QString::fromStdString(entity->getName()),
        "TestAnim",
        QString::fromStdString(track->getAssociatedNode()->getName()),
        "tx", 0.0, CurveEditModel::ModeStepped);

    ResampleCurveCommand cmd(entity->getName(), "TestAnim",
                             track->getAssociatedNode()->getName(),
                             "tx", 0.0f, 0.5f);
    cmd.redo();

    EXPECT_GT(track->getNumKeyFrames(), beforeCount);
}

TEST_F(ResampleCurveCommandTest, UndoRestoresOriginalCount) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("RC_Undo");
    ASSERT_NE(entity, nullptr);

    auto* track = trackOf(entity);
    ASSERT_NE(track, nullptr);
    const int beforeCount = track->getNumKeyFrames();

    CurveEditModel::instance()->setMode(
        QString::fromStdString(entity->getName()),
        "TestAnim",
        QString::fromStdString(track->getAssociatedNode()->getName()),
        "tx", 0.0, CurveEditModel::ModeStepped);

    ResampleCurveCommand cmd(entity->getName(), "TestAnim",
                             track->getAssociatedNode()->getName(),
                             "tx", 0.0f, 0.5f);
    cmd.redo();
    cmd.undo();
    EXPECT_EQ(track->getNumKeyFrames(), beforeCount);
}

TEST_F(ResampleCurveCommandTest, AnchorKeyframesSurviveResample) {
    // The two anchor keys at t0 and t1 are not touched — only the
    // interior is replaced. This guarantees the user's neighbor
    // keyframes don't get moved or have their values clobbered.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("RC_Anchors");
    ASSERT_NE(entity, nullptr);

    auto* track = trackOf(entity);
    ASSERT_NE(track, nullptr);
    auto findAt = [&](float t) -> Ogre::TransformKeyFrame* {
        for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
            auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
            if (std::fabs(kf->getTime() - t) < 0.001f) return kf;
        }
        return nullptr;
    };
    auto* anchor0 = findAt(0.0f);
    auto* anchor1 = findAt(0.5f);
    ASSERT_NE(anchor0, nullptr);
    ASSERT_NE(anchor1, nullptr);
    const Ogre::Vector3 t0 = anchor0->getTranslate();
    const Ogre::Vector3 t1 = anchor1->getTranslate();

    ResampleCurveCommand cmd(entity->getName(), "TestAnim",
                             track->getAssociatedNode()->getName(),
                             "tx", 0.0f, 0.5f);
    cmd.redo();

    auto* afterA0 = findAt(0.0f);
    auto* afterA1 = findAt(0.5f);
    ASSERT_NE(afterA0, nullptr);
    ASSERT_NE(afterA1, nullptr);
    EXPECT_NEAR(afterA0->getTranslate().x, t0.x, 1e-4);
    EXPECT_NEAR(afterA1->getTranslate().x, t1.x, 1e-4);
}

TEST_F(ResampleCurveCommandTest, RedoTwiceMatchesFirstResult) {
    // QUndoStack drives redo() repeatedly across redo/undo cycles —
    // the second redo must produce the same track state as the first.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("RC_RedoTwice");
    ASSERT_NE(entity, nullptr);

    auto* track = trackOf(entity);
    ASSERT_NE(track, nullptr);
    CurveEditModel::instance()->setMode(
        QString::fromStdString(entity->getName()),
        "TestAnim",
        QString::fromStdString(track->getAssociatedNode()->getName()),
        "tx", 0.0, CurveEditModel::ModeStepped);

    ResampleCurveCommand cmd(entity->getName(), "TestAnim",
                             track->getAssociatedNode()->getName(),
                             "tx", 0.0f, 0.5f);
    cmd.redo();
    const int afterFirst = track->getNumKeyFrames();
    cmd.undo();
    cmd.redo();
    EXPECT_EQ(track->getNumKeyFrames(), afterFirst);
}

TEST_F(ResampleCurveCommandTest, MissingAnchorsAreNoOp) {
    // t0/t1 not at existing keyframes → command can't resolve the
    // segment and bails. State stays as-is.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("RC_NoAnchor");
    ASSERT_NE(entity, nullptr);

    auto* track = trackOf(entity);
    ASSERT_NE(track, nullptr);
    const int beforeCount = track->getNumKeyFrames();

    ResampleCurveCommand cmd(entity->getName(), "TestAnim",
                             track->getAssociatedNode()->getName(),
                             "tx", 0.13f, 0.42f); // not on any key
    cmd.redo();

    EXPECT_EQ(track->getNumKeyFrames(), beforeCount);
}
