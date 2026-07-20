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

TEST_F(BoneDragReleaseTest, AutoKeyOffWithAnimCommitsBind) {
    // Auto-key OFF: bone gizmo is rest-pose authoring (matches Inspector
    // copy). Keep the dragged TRS; caller commits bind via SetRestPoseCommand.
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_CommitBindOnce");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* bone = entity->getSkeleton()->getBone("Child");

    const Ogre::Vector3 before = bone->getPosition();
    const Ogre::Vector3 after = before + Ogre::Vector3(0.5f, -0.3f, 0.0f);
    simulateBoneDrag(bone, after);

    auto outcome = BoneDragRelease::apply(bone, before, bone->getInitialOrientation(),
                                          bone->getInitialScale(),
                                          /*hasAnim=*/true, /*autoKey=*/false,
                                          entity);
    EXPECT_EQ(outcome, BoneDragRelease::Result::CommitBind);
    EXPECT_EQ(bone->getPosition(), after);
    EXPECT_FALSE(bone->isManuallyControlled());
}

TEST_F(BoneDragReleaseTest, AutoKeyOffTwoDragsKeepLatestPose) {
    // With CommitBind, each drag keeps its after-TRS (caller then writes
    // bind). Second drag's before-state is the first drag's after-state.
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_TwoDrags");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* bone = entity->getSkeleton()->getBone("Child");

    const Ogre::Vector3 origLocal = bone->getPosition();
    const Ogre::Vector3 after1 = origLocal + Ogre::Vector3(0, 0.5f, 0);
    const Ogre::Vector3 after2 = after1 + Ogre::Vector3(0.5f, 0, 0);

    simulateBoneDrag(bone, after1);
    EXPECT_EQ(BoneDragRelease::apply(bone, origLocal, bone->getInitialOrientation(),
                                     bone->getInitialScale(),
                                     /*hasAnim=*/true, /*autoKey=*/false, entity),
              BoneDragRelease::Result::CommitBind);
    EXPECT_EQ(bone->getPosition(), after1);

    simulateBoneDrag(bone, after2);
    EXPECT_EQ(BoneDragRelease::apply(bone, after1, bone->getInitialOrientation(),
                                     bone->getInitialScale(),
                                     /*hasAnim=*/true, /*autoKey=*/false, entity),
              BoneDragRelease::Result::CommitBind);
    EXPECT_EQ(bone->getPosition(), after2);
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

// Derived-position drags with auto-key off keep the after-pose (CommitBind).
TEST_F(BoneDragReleaseTest, YThenXDragsKeepEachAfterPose) {
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_YThenX");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* bone = entity->getSkeleton()->getBone("Child");

    const Ogre::Vector3 origLocal = bone->getPosition();
    const Ogre::Vector3 origDerived = bone->_getDerivedPosition();

    bone->setManuallyControlled(true);
    bone->_setDerivedPosition(origDerived + Ogre::Vector3(0, -0.7f, 0));
    const Ogre::Vector3 after1 = bone->getPosition();
    EXPECT_EQ(BoneDragRelease::apply(bone, origLocal, bone->getInitialOrientation(),
                                     bone->getInitialScale(),
                                     /*hasAnim=*/true, /*autoKey=*/false, entity),
              BoneDragRelease::Result::CommitBind);
    EXPECT_EQ(bone->getPosition(), after1);

    const Ogre::Vector3 drag2Before = bone->getPosition();
    bone->setManuallyControlled(true);
    bone->_setDerivedPosition(bone->_getDerivedPosition() + Ogre::Vector3(0.7f, 0, 0));
    const Ogre::Vector3 after2 = bone->getPosition();
    EXPECT_EQ(BoneDragRelease::apply(bone, drag2Before, bone->getInitialOrientation(),
                                     bone->getInitialScale(),
                                     /*hasAnim=*/true, /*autoKey=*/false, entity),
              BoneDragRelease::Result::CommitBind);
    EXPECT_EQ(bone->getPosition(), after2);
}

TEST_F(BoneDragReleaseTest, SetDerivedPositionDragCommitsBind) {
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_SetDerivedTwoDrags");
    ASSERT_NE(entity, nullptr);

    Ogre::Bone* root = entity->getSkeleton()->getBone("Root");
    Ogre::Bone* child = entity->getSkeleton()->getBone("Child");
    root->setOrientation(Ogre::Quaternion(Ogre::Radian(0.5f), Ogre::Vector3::UNIT_Y));
    root->setManuallyControlled(true);

    const Ogre::Vector3 origLocal = child->getPosition();
    const Ogre::Vector3 origDerived = child->_getDerivedPosition();

    child->setManuallyControlled(true);
    child->_setDerivedPosition(origDerived + Ogre::Vector3(0, 0.5f, 0));
    child->needUpdate(true);
    const Ogre::Vector3 after1 = child->getPosition();
    EXPECT_NE(after1, origLocal);

    EXPECT_EQ(BoneDragRelease::apply(child, origLocal, child->getInitialOrientation(),
                                     child->getInitialScale(),
                                     /*hasAnim=*/true, /*autoKey=*/false, entity),
              BoneDragRelease::Result::CommitBind);
    EXPECT_EQ(child->getPosition(), after1);

    const Ogre::Vector3 drag2Local = child->getPosition();
    const Ogre::Vector3 drag2Derived = child->_getDerivedPosition();
    child->setManuallyControlled(true);
    child->_setDerivedPosition(drag2Derived + Ogre::Vector3(0.5f, 0, 0));
    child->needUpdate(true);
    const Ogre::Vector3 after2 = child->getPosition();

    EXPECT_EQ(BoneDragRelease::apply(child, drag2Local, child->getInitialOrientation(),
                                     child->getInitialScale(),
                                     /*hasAnim=*/true, /*autoKey=*/false, entity),
              BoneDragRelease::Result::CommitBind);
    EXPECT_EQ(child->getPosition(), after2);
}

TEST_F(BoneDragReleaseTest, NoAnimSetsInitial) {
    // No active animation: drag returns CommitBind; caller (TransformOperator)
    // commits via SetRestPoseCommand. apply() itself does not call
    // setInitialState — that belongs to the rest-pose path.
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_BindPose");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* bone = entity->getSkeleton()->getBone("Child");

    const Ogre::Vector3 before = bone->getPosition();
    const Ogre::Vector3 after  = before + Ogre::Vector3(0.5f, 0, 0);
    const Ogre::Vector3 initialBefore = bone->getInitialPosition();
    simulateBoneDrag(bone, after);
    auto outcome = BoneDragRelease::apply(bone, before, bone->getInitialOrientation(),
                                          bone->getInitialScale(),
                                          /*hasAnim=*/false, /*autoKey=*/false);
    EXPECT_EQ(outcome, BoneDragRelease::Result::CommitBind);
    EXPECT_EQ(bone->getPosition(), after);
    EXPECT_EQ(bone->getInitialPosition(), initialBefore)
        << "apply() must leave initial unchanged for the commit caller";
}

TEST_F(BoneDragReleaseTest, EditRestModeCommitsBindEvenWithAnim) {
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_EditRest");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* bone = entity->getSkeleton()->getBone("Child");

    const Ogre::Vector3 before = bone->getPosition();
    const Ogre::Vector3 after  = before + Ogre::Vector3(0.25f, 0, 0);
    simulateBoneDrag(bone, after);
    auto outcome = BoneDragRelease::apply(bone, before, bone->getOrientation(),
                                          bone->getScale(),
                                          /*hasAnim=*/true, /*autoKey=*/true,
                                          entity, /*editRestMode=*/true);
    EXPECT_EQ(outcome, BoneDragRelease::Result::CommitBind);
    EXPECT_EQ(bone->getPosition(), after);
}

TEST_F(BoneDragReleaseTest, MultiMoveSetUpdateSequenceCommitsBind) {
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_MultiMoveSeq");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* root = entity->getSkeleton()->getBone("Root");
    Ogre::Bone* child = entity->getSkeleton()->getBone("Child");

    root->setOrientation(Ogre::Quaternion(Ogre::Radian(0.5f), Ogre::Vector3::UNIT_Y));
    root->setManuallyControlled(true);

    const Ogre::Vector3 origLocal   = child->getPosition();
    const Ogre::Vector3 origDerived = child->_getDerivedPosition();

    child->setManuallyControlled(true);
    const Ogre::Vector3 drag1Target(0.0f, 0.6f, 0.0f);
    for (int i = 1; i <= 5; ++i) {
        const float frac = static_cast<float>(i) / 5.0f;
        child->_setDerivedPosition(origDerived + drag1Target * frac);
        child->needUpdate(true);
    }
    const Ogre::Vector3 after1 = child->getPosition();
    EXPECT_EQ(BoneDragRelease::apply(child, origLocal,
                                     child->getInitialOrientation(),
                                     child->getInitialScale(),
                                     /*hasAnim=*/true, /*autoKey=*/false,
                                     entity),
              BoneDragRelease::Result::CommitBind);
    EXPECT_EQ(child->getPosition(), after1);
}

// Reproduces a subtle variant of the bug: a mouse-move event with a
// near-zero delta (sub-epsilon) happens at press time, but the helper
// must still treat the bone as "touched" if the manualControlled flag
// was set, and clear it on release — otherwise subsequent ticks see
// stale manual control and produce visible compound rotation.
TEST_F(BoneDragReleaseTest, ZeroDeltaSetUpdateClearsManualControlled) {
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_ZeroDelta");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* child = entity->getSkeleton()->getBone("Child");

    const Ogre::Vector3 origLocal = child->getPosition();

    // Simulate a press that flipped manualControlled but produced
    // zero motion (the user clicked then released without dragging).
    child->setManuallyControlled(true);
    child->_setDerivedPosition(child->_getDerivedPosition());
    child->needUpdate(true);

    auto outcome = BoneDragRelease::apply(child, origLocal,
                                          child->getInitialOrientation(),
                                          child->getInitialScale(),
                                          /*hasAnim=*/true, /*autoKey=*/false,
                                          entity);
    EXPECT_EQ(outcome, BoneDragRelease::Result::NoOp);
    EXPECT_FALSE(child->isManuallyControlled())
        << "Zero-delta drag left manualControlled set — animation playback would freeze the bone";
}

// Documents Ogre's parentless-bone behavior that drove the
// TransformOperator move-handler fix: _setDerivedPosition is a no-op on
// nodes with no parent, so the bone-drag handler falls back to
// setPosition for true skeleton roots. Without this, root bones (the
// locomotion bones) silently refused to translate.
TEST_F(BoneDragReleaseTest, OgreSetDerivedPositionIsNoOpOnParentlessBone) {
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_ParentlessRoot");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* root = entity->getSkeleton()->getBone("Root");
    ASSERT_FALSE(root->getParent());

    const Ogre::Vector3 origLocal = root->getPosition();
    root->_setDerivedPosition(Ogre::Vector3(5, 0, 0));
    EXPECT_EQ(root->getPosition(), origLocal)
        << "Ogre changed _setDerivedPosition behavior for parentless nodes; "
           "the parentless-bone fallback in TransformOperator may now be unnecessary.";

    // For parentless bones, plain setPosition is the right call.
    root->setPosition(Ogre::Vector3(5, 0, 0));
    EXPECT_EQ(root->getPosition(), Ogre::Vector3(5, 0, 0));
}

