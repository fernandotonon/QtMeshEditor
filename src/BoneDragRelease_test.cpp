#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include "BoneDragRelease.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <OgreSkeletonInstance.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreKeyFrame.h>

class BoneDragReleaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
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

// Simulate what TransformOperator does during a drag: set the bone's local
// position to a target. The release helper observes the diff against
// before-state and decides what to do.
static void simulateBoneDrag(Ogre::Bone* bone, const Ogre::Vector3& newLocal) {
    bone->setManuallyControlled(true);
    bone->setPosition(newLocal);
    bone->needUpdate(true);
}

TEST_F(BoneDragReleaseTest, NoChangeIsNoOp) {
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_NoChange");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* bone = entity->getSkeleton()->getBone("Child");

    Ogre::Vector3 before = bone->getPosition();
    auto outcome = BoneDragRelease::apply(bone, before, bone->getOrientation(),
                                          bone->getScale(),
                                          /*hasAnim=*/true, /*autoKey=*/false);
    EXPECT_EQ(outcome, BoneDragRelease::Result::NoOp);
}

TEST_F(BoneDragReleaseTest, AutoKeyOffCommitsToBindPose) {
    // Auto-key OFF: dragged pose becomes the new bind. After release,
    // bone->getPosition() == dragged pose AND bone->getInitialPosition()
    // also == dragged pose (so Skeleton::reset restores to the new bind).
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_BindCommit");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* bone = entity->getSkeleton()->getBone("Child");

    const Ogre::Vector3 before = bone->getPosition();
    const Ogre::Vector3 dragged = before + Ogre::Vector3(0.5f, -0.3f, 0.0f);
    simulateBoneDrag(bone, dragged);

    auto outcome = BoneDragRelease::apply(bone, before, bone->getInitialOrientation(),
                                          bone->getInitialScale(),
                                          /*hasAnim=*/true, /*autoKey=*/false,
                                          entity);
    EXPECT_EQ(outcome, BoneDragRelease::Result::CommitBind);
    EXPECT_EQ(bone->getPosition(), dragged);
    EXPECT_EQ(bone->getInitialPosition(), dragged);

    // Reset (what Skeleton::reset does) must land at the new bind.
    bone->setPosition(Ogre::Vector3::ZERO);
    bone->resetToInitialState();
    EXPECT_EQ(bone->getPosition(), dragged) << "New bind pose did not survive reset";
}

TEST_F(BoneDragReleaseTest, AutoKeyOffTwoDragsAccumulateAsBindUpdates) {
    // Two consecutive drags with auto-key off → each commits to bind.
    // Drag 2's pose should be exactly what was set, not relative to
    // any prior offset (the BlendMask muting in TransformOperator
    // ensures the bone's local at drag-2 press equals drag-1's commit).
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_TwoBindUpdates");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* bone = entity->getSkeleton()->getBone("Child");

    const Ogre::Vector3 origLocal = bone->getPosition();

    simulateBoneDrag(bone, origLocal + Ogre::Vector3(0, 0.5f, 0));
    BoneDragRelease::apply(bone, origLocal, bone->getInitialOrientation(),
                           bone->getInitialScale(),
                           /*hasAnim=*/true, /*autoKey=*/false, entity);
    const Ogre::Vector3 afterDrag1 = bone->getPosition();
    EXPECT_EQ(afterDrag1, origLocal + Ogre::Vector3(0, 0.5f, 0));
    EXPECT_EQ(bone->getInitialPosition(), afterDrag1);

    // Drag 2 starts from afterDrag1 (which is the new bind).
    const Ogre::Vector3 drag2Target = afterDrag1 + Ogre::Vector3(0.3f, 0, 0);
    simulateBoneDrag(bone, drag2Target);
    BoneDragRelease::apply(bone, afterDrag1, bone->getInitialOrientation(),
                           bone->getInitialScale(),
                           /*hasAnim=*/true, /*autoKey=*/false, entity);
    EXPECT_EQ(bone->getPosition(), drag2Target);
    EXPECT_EQ(bone->getInitialPosition(), drag2Target);
}

TEST_F(BoneDragReleaseTest, AutoKeyOnWithAnimKeepsDraggedPose) {
    // Auto-key ON + animation: the dragged TRS must persist (the caller
    // is expected to write a keyframe at the slider time). Manual
    // control must be released so playback drives the bone through
    // the new key.
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_KeepAutoKey");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* bone = entity->getSkeleton()->getBone("Child");

    const Ogre::Vector3 before = bone->getPosition();
    const Ogre::Vector3 after  = before + Ogre::Vector3(0.5f, 0, 0);
    simulateBoneDrag(bone, after);
    auto outcome = BoneDragRelease::apply(bone, before, bone->getInitialOrientation(),
                                          bone->getInitialScale(),
                                          /*hasAnim=*/true, /*autoKey=*/true,
                                          entity);
    EXPECT_EQ(outcome, BoneDragRelease::Result::Commit);
    EXPECT_EQ(bone->getPosition(), after);
    EXPECT_FALSE(bone->isManuallyControlled());
}

// User report: "first moved Y down, then tried X — went down before
// going right." Simulates that exact sequence at the helper level.
// Without revert, the accumulated Y delta from drag 1 would leak into
// drag 2's before-state, and the bone would re-show the Y offset.
// The user-reported regression: edit T-pose bone, enable animation,
// disable animation, expect bone to remain at the edited T-pose. The
// bone's initial state must reflect the edit so Skeleton::reset (called
// when animations are toggled or applied) restores the new bind, not
// the original.
TEST_F(BoneDragReleaseTest, BindEditSurvivesAnimationToggle) {
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_BindSurvivesToggle");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* bone = entity->getSkeleton()->getBone("Child");

    const Ogre::Vector3 origLocal = bone->getPosition();
    const Ogre::Vector3 dragged = origLocal + Ogre::Vector3(0.5f, 0, 0);
    simulateBoneDrag(bone, dragged);
    BoneDragRelease::apply(bone, origLocal, bone->getInitialOrientation(),
                           bone->getInitialScale(),
                           /*hasAnim=*/true, /*autoKey=*/false, entity);
    EXPECT_EQ(bone->getInitialPosition(), dragged);

    // Simulate "animation toggled off" → Skeleton::reset → bone back
    // to its initial state. Our edit must survive.
    entity->getSkeleton()->reset();
    EXPECT_EQ(bone->getPosition(), dragged) << "Bind edit was lost on Skeleton::reset";
}

TEST_F(BoneDragReleaseTest, NoAnimSetsInitial) {
    // No active animation: drag commits to bind pose via setInitialState.
    // Subsequent reset would land at the new bind pose — confirm by
    // calling resetToInitialState afterwards.
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_BindPose");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* bone = entity->getSkeleton()->getBone("Child");

    const Ogre::Vector3 before = bone->getPosition();
    const Ogre::Vector3 after  = before + Ogre::Vector3(0.5f, 0, 0);
    simulateBoneDrag(bone, after);
    auto outcome = BoneDragRelease::apply(bone, before, bone->getInitialOrientation(),
                                          bone->getInitialScale(),
                                          /*hasAnim=*/false, /*autoKey=*/false);
    EXPECT_EQ(outcome, BoneDragRelease::Result::CommitBind);
    EXPECT_EQ(bone->getPosition(), after);
    // Reset to initial — should land back at `after` since we just rebound.
    bone->setPosition(Ogre::Vector3::ZERO);
    bone->resetToInitialState();
    EXPECT_EQ(bone->getPosition(), after);
}
