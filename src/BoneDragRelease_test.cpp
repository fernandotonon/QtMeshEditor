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

TEST_F(BoneDragReleaseTest, AutoKeyOffWithAnimReverts) {
    // Drag with auto-key OFF + animation active: the bone must revert
    // to its pre-drag local TRS. Without revert, accumulated drag
    // offsets pile up across drags ("if I moved it down before, it
    // goes further down on my second attempt").
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_RevertOnce");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* bone = entity->getSkeleton()->getBone("Child");

    const Ogre::Vector3 before = bone->getPosition();
    simulateBoneDrag(bone, before + Ogre::Vector3(0.5f, -0.3f, 0.0f));

    auto outcome = BoneDragRelease::apply(bone, before, bone->getInitialOrientation(),
                                          bone->getInitialScale(),
                                          /*hasAnim=*/true, /*autoKey=*/false,
                                          entity);
    EXPECT_EQ(outcome, BoneDragRelease::Result::Revert);
    EXPECT_EQ(bone->getPosition(), before);
}

TEST_F(BoneDragReleaseTest, AutoKeyOffTwoDragsDoNotAccumulate) {
    // Critical regression test for the user-reported "down before going
    // right" teleport. Two consecutive drags with auto-key off must
    // each leave the bone at its pre-drag pose — the second drag's
    // before-state must be the SAME as the first drag's before-state,
    // not the first drag's accumulated offset.
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_TwoDrags");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* bone = entity->getSkeleton()->getBone("Child");

    const Ogre::Vector3 origLocal = bone->getPosition();

    // Drag 1: move +Y by 0.5
    simulateBoneDrag(bone, origLocal + Ogre::Vector3(0, 0.5f, 0));
    BoneDragRelease::apply(bone, origLocal, bone->getInitialOrientation(),
                           bone->getInitialScale(),
                           /*hasAnim=*/true, /*autoKey=*/false, entity);
    const Ogre::Vector3 afterDrag1 = bone->getPosition();
    EXPECT_EQ(afterDrag1, origLocal) << "Drag 1 did not revert";

    // Drag 2: move +X by 0.5
    simulateBoneDrag(bone, origLocal + Ogre::Vector3(0.5f, 0, 0));
    BoneDragRelease::apply(bone, origLocal, bone->getInitialOrientation(),
                           bone->getInitialScale(),
                           /*hasAnim=*/true, /*autoKey=*/false, entity);
    const Ogre::Vector3 afterDrag2 = bone->getPosition();
    EXPECT_EQ(afterDrag2, origLocal) << "Drag 2 did not revert (accumulation bug)";
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
TEST_F(BoneDragReleaseTest, YThenXDragsDoNotLeakYIntoX) {
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_YThenX");
    ASSERT_NE(entity, nullptr);
    Ogre::Bone* bone = entity->getSkeleton()->getBone("Child");

    const Ogre::Vector3 origLocal = bone->getPosition();
    const Ogre::Vector3 origDerived = bone->_getDerivedPosition();

    // Drag 1: Y axis down.
    bone->setManuallyControlled(true);
    bone->_setDerivedPosition(origDerived + Ogre::Vector3(0, -0.7f, 0));
    BoneDragRelease::apply(bone, origLocal, bone->getInitialOrientation(),
                           bone->getInitialScale(),
                           /*hasAnim=*/true, /*autoKey=*/false, entity);
    EXPECT_EQ(bone->getPosition(), origLocal);
    EXPECT_LT((bone->_getDerivedPosition() - origDerived).length(), 1e-5f);

    // Drag 2: X axis right. The bone must start fresh from origLocal,
    // not from any leaked Y offset.
    const Ogre::Vector3 drag2Before = bone->getPosition();
    EXPECT_EQ(drag2Before, origLocal);

    bone->setManuallyControlled(true);
    bone->_setDerivedPosition(origDerived + Ogre::Vector3(0.7f, 0, 0));
    BoneDragRelease::apply(bone, drag2Before, bone->getInitialOrientation(),
                           bone->getInitialScale(),
                           /*hasAnim=*/true, /*autoKey=*/false, entity);
    // Final state: same origLocal (revert), no leaked Y.
    EXPECT_EQ(bone->getPosition(), origLocal);
    EXPECT_NEAR(bone->_getDerivedPosition().y, origDerived.y, 1e-5f)
        << "Y leaked from previous drag";
}

// The critical regression test: simulate the actual TransformOperator
// flow using _setDerivedPosition (not just setPosition), with parent
// state matching the user's "head bone with rotated parent" case, and
// assert that two consecutive drags don't accumulate when reverted.
TEST_F(BoneDragReleaseTest, SetDerivedPositionDragRevertsCleanly) {
    Ogre::Entity* entity = createAnimatedTestEntity("BDR_SetDerivedTwoDrags");
    ASSERT_NE(entity, nullptr);

    // Use the child bone (parent = root). Apply a non-identity rotation
    // to the root bone so _setDerivedPosition has to do real parent-frame
    // math — matches the "head bone with rotated parent mid-animation"
    // scenario where the bug shows up.
    Ogre::Bone* root = entity->getSkeleton()->getBone("Root");
    Ogre::Bone* child = entity->getSkeleton()->getBone("Child");
    root->setOrientation(Ogre::Quaternion(Ogre::Radian(0.5f), Ogre::Vector3::UNIT_Y));
    root->setManuallyControlled(true);

    const Ogre::Vector3 origLocal = child->getPosition();
    const Ogre::Vector3 origDerived = child->_getDerivedPosition();

    // Drag 1: move derived +Y by 0.5 in world space.
    child->setManuallyControlled(true);
    child->_setDerivedPosition(origDerived + Ogre::Vector3(0, 0.5f, 0));
    child->needUpdate(true);
    // Local Y has changed (non-trivially) because of parent rotation.
    EXPECT_NE(child->getPosition(), origLocal);

    auto outcome1 = BoneDragRelease::apply(child, origLocal, child->getInitialOrientation(),
                                          child->getInitialScale(),
                                          /*hasAnim=*/true, /*autoKey=*/false, entity);
    EXPECT_EQ(outcome1, BoneDragRelease::Result::Revert);
    EXPECT_EQ(child->getPosition(), origLocal) << "Drag 1 revert failed";

    // Drag 2 press: capture new before-state (which should equal origLocal).
    const Ogre::Vector3 drag2Local  = child->getPosition();
    EXPECT_EQ(drag2Local, origLocal) << "After drag 1 revert, child has shifted (accumulation)";

    // Drag 2: move derived +X by 0.5
    const Ogre::Vector3 drag2Derived = child->_getDerivedPosition();
    child->setManuallyControlled(true);
    child->_setDerivedPosition(drag2Derived + Ogre::Vector3(0.5f, 0, 0));
    child->needUpdate(true);

    auto outcome2 = BoneDragRelease::apply(child, drag2Local, child->getInitialOrientation(),
                                          child->getInitialScale(),
                                          /*hasAnim=*/true, /*autoKey=*/false, entity);
    EXPECT_EQ(outcome2, BoneDragRelease::Result::Revert);
    EXPECT_EQ(child->getPosition(), origLocal) << "Drag 2 revert failed (accumulation across drags)";
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
