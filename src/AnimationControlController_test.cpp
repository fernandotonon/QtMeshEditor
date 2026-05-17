#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include "AnimationControlController.h"
#include "CurveEditModel.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "UndoManager.h"
#include <QUndoStack>
#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreKeyFrame.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgrePose.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubMesh.h>
#include <OgreEntity.h>

class AnimationControlControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        AnimationControlController::kill();
        Manager::kill();
        QThread::msleep(20);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
    }

    void TearDown() override {
        SelectionSet::getSingleton()->clear();
        app->processEvents();
        AnimationControlController::kill();
    }

    // Helper: create animated entity and select it
    Ogre::Entity* setupAnimatedEntity(const std::string& name) {
        if (!canLoadMeshFiles()) return nullptr;
        Ogre::Entity* entity = createAnimatedTestEntity(name);
        if (!entity) return nullptr;
        SelectionSet::getSingleton()->selectOne(entity->getParentSceneNode());
        app->processEvents();
        return entity;
    }

    QApplication* app = nullptr;
};

// ── Singleton ──────────────────────────────────────────────────────────────────

TEST_F(AnimationControlControllerTest, InstanceReturnsSameObject) {
    auto* a = AnimationControlController::instance();
    auto* b = AnimationControlController::instance();
    EXPECT_EQ(a, b);
}

TEST_F(AnimationControlControllerTest, KillResetsInstance) {
    auto* a = AnimationControlController::instance();
    // Mutate state so we can verify the new instance starts fresh
    a->setSliderValue(999);
    EXPECT_EQ(a->sliderValue(), 999);
    AnimationControlController::kill();
    // New instance must have default slider value (0), proving it was re-created
    auto* b = AnimationControlController::instance();
    EXPECT_EQ(b->sliderValue(), 0);
}

// ── Theme colors ───────────────────────────────────────────────────────────────

TEST_F(AnimationControlControllerTest, ThemeColorsAreValid) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_TRUE(ctrl->panelColor().isValid());
    EXPECT_TRUE(ctrl->textColor().isValid());
    EXPECT_TRUE(ctrl->borderColor().isValid());
    EXPECT_TRUE(ctrl->inputColor().isValid());
    EXPECT_TRUE(ctrl->highlightColor().isValid());
    EXPECT_TRUE(ctrl->buttonColor().isValid());
    EXPECT_TRUE(ctrl->buttonTextColor().isValid());
    EXPECT_TRUE(ctrl->disabledTextColor().isValid());
}

// ── Initial state ──────────────────────────────────────────────────────────────

TEST_F(AnimationControlControllerTest, InitialStateIsEmpty) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_FALSE(ctrl->hasAnimation());
    EXPECT_FALSE(ctrl->onKeyframe());
    EXPECT_FALSE(ctrl->canDeleteKeyframe());
    EXPECT_FALSE(ctrl->hasPrevKeyframe());
    EXPECT_FALSE(ctrl->hasNextKeyframe());
    EXPECT_TRUE(ctrl->animationTree().isEmpty());
    EXPECT_TRUE(ctrl->boneNames().isEmpty());
    EXPECT_EQ(ctrl->sliderValue(), 0);
    EXPECT_EQ(ctrl->sliderMaximum(), 0);
}

// ── updateAnimationTree ────────────────────────────────────────────────────────

TEST_F(AnimationControlControllerTest, UpdateAnimationTreeWithNoSelectionIsEmpty) {
    SelectionSet::getSingleton()->clear();
    auto* ctrl = AnimationControlController::instance();
    EXPECT_NO_THROW(ctrl->updateAnimationTree());
    EXPECT_TRUE(ctrl->animationTree().isEmpty());
}

TEST_F(AnimationControlControllerTest, UpdateAnimationTreeWithAnimatedEntityPopulatesTree) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_TreeTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();

    EXPECT_FALSE(ctrl->animationTree().isEmpty());
    auto group = ctrl->animationTree().first().toMap();
    EXPECT_EQ(group["entity"].toString().toStdString(), entity->getName());
    EXPECT_FALSE(group["animations"].toStringList().isEmpty());
}

TEST_F(AnimationControlControllerTest, UpdateAnimationTreeEmitsSignal) {
    auto* ctrl = AnimationControlController::instance();
    QSignalSpy spy(ctrl, &AnimationControlController::animationTreeChanged);
    ctrl->updateAnimationTree();
    EXPECT_GE(spy.count(), 1);
}

// ── selectAnimation ────────────────────────────────────────────────────────────

TEST_F(AnimationControlControllerTest, SelectAnimationWithEmptyNamesResetsState) {
    auto* ctrl = AnimationControlController::instance();
    ctrl->selectAnimation("", "");
    EXPECT_FALSE(ctrl->hasAnimation());
    EXPECT_EQ(ctrl->sliderMaximum(), 0);
    EXPECT_TRUE(ctrl->boneNames().isEmpty());
}

TEST_F(AnimationControlControllerTest, SelectAnimationSetsState) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_SelectTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();

    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    EXPECT_TRUE(ctrl->hasAnimation());
    EXPECT_EQ(ctrl->selectedAnimation(), "TestAnim");
    EXPECT_EQ(ctrl->selectedEntityName().toStdString(), entity->getName());
    EXPECT_EQ(ctrl->sliderMaximum(), 1000); // 1.0s * 1000
}

TEST_F(AnimationControlControllerTest, SelectAnimationPopulatesBoneList) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_BoneListTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");

    EXPECT_FALSE(ctrl->boneNames().isEmpty());
}

TEST_F(AnimationControlControllerTest, SelectAnimationEmitsSelectionChanged) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_SignalTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();

    QSignalSpy spy(ctrl, &AnimationControlController::selectionChanged);
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    EXPECT_GE(spy.count(), 1);
}

// ── selectBone ─────────────────────────────────────────────────────────────────

TEST_F(AnimationControlControllerTest, SelectBoneUpdatesSelectedBone) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_BoneSelectTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");

    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    QString bone = ctrl->boneNames().first();
    ctrl->selectBone(bone);
    EXPECT_EQ(ctrl->selectedBone(), bone);
}

TEST_F(AnimationControlControllerTest, SelectBonePopulatesKeyframeTicks) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_TicksTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");

    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    ctrl->selectBone(ctrl->boneNames().first());

    // TestAnim has 3 keyframes — should have 3 ticks
    EXPECT_EQ(ctrl->keyframeTicks().size(), 3);
}

// ── Slider / timeline ──────────────────────────────────────────────────────────

TEST_F(AnimationControlControllerTest, SetSliderValueUpdatesValue) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_SliderTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    ctrl->selectBone(ctrl->boneNames().first());

    ctrl->setSliderValue(500);
    EXPECT_EQ(ctrl->sliderValue(), 500);
}

TEST_F(AnimationControlControllerTest, SetAnimationLengthUpdatesSliderMaximum) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_LenTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");

    ctrl->setAnimationLength(2.0);
    EXPECT_EQ(ctrl->sliderMaximum(), 2000);
    EXPECT_NEAR(ctrl->animationLength(), 2.0, 0.001);
}

// ── Keyframe navigation ────────────────────────────────────────────────────────

TEST_F(AnimationControlControllerTest, NextKeyframeAdvancesPosition) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_NextKfTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    ctrl->selectBone(ctrl->boneNames().first());
    ctrl->setSliderValue(0);

    ctrl->nextKeyframe();
    EXPECT_GT(ctrl->sliderValue(), 0);
}

TEST_F(AnimationControlControllerTest, PrevKeyframeGoesBack) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_PrevKfTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    ctrl->selectBone(ctrl->boneNames().first());
    ctrl->setSliderValue(1000);

    ctrl->prevKeyframe();
    EXPECT_LT(ctrl->sliderValue(), 1000);
}

TEST_F(AnimationControlControllerTest, HasNextKeyframeAtStart) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_HasNextTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    ctrl->selectBone(ctrl->boneNames().first());
    ctrl->setSliderValue(0);

    EXPECT_TRUE(ctrl->hasNextKeyframe());
}

TEST_F(AnimationControlControllerTest, HasPrevKeyframeAtEnd) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_HasPrevTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    ctrl->selectBone(ctrl->boneNames().first());
    ctrl->setSliderValue(1000);

    EXPECT_TRUE(ctrl->hasPrevKeyframe());
}

// ── Add / Delete keyframe ──────────────────────────────────────────────────────

TEST_F(AnimationControlControllerTest, AddKeyframeIncreasesCount) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_AddKfTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    ctrl->selectBone(ctrl->boneNames().first());

    auto* skel  = entity->getSkeleton();
    auto* anim  = skel->getAnimation("TestAnim");
    auto* track = anim->_getNodeTrackList().begin()->second;
    int before  = track->getNumKeyFrames();

    ctrl->setSliderValue(250); // between kf0 and kf1
    ctrl->addKeyframe();
    app->processEvents();

    EXPECT_EQ(track->getNumKeyFrames(), before + 1);
}

TEST_F(AnimationControlControllerTest, AddKeyframeCapturesBonePose) {
    // Move the bone to a non-identity pose, then add a keyframe at a fresh
    // scrub time. The new keyframe must capture the bone's current local
    // TRS — not identity, not the curve interpolation.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ACC_AddKfPoseTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    QString boneName = ctrl->boneNames().first();
    ctrl->selectBone(boneName);

    // Manually offset the bone from its initial pose. addKeyframe should
    // capture that offset rather than re-sampling the curve.
    auto* skel = entity->getSkeleton();
    Ogre::Bone* bone = skel->getBone(boneName.toStdString());
    bone->setManuallyControlled(true);
    bone->setPosition(bone->getInitialPosition() + Ogre::Vector3(2.5f, 0, 0));

    ctrl->setSliderValue(750); // a time that has no existing keyframe
    ctrl->addKeyframe();
    app->processEvents();

    // Find the keyframe at 0.75s and verify translate.x ≈ 2.5
    auto* track = skel->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    bool found = false;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - 0.75f) < 0.001f) {
            EXPECT_NEAR(kf->getTranslate().x, 2.5f, 1e-3);
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(AnimationControlControllerTest, AutoKeyOnTransformPushesKeyframeWhenEnabled) {
    // With autoKey enabled, calling autoKeyOnTransform must add a keyframe
    // at the current scrub time on the active bone-track. Without it, the
    // call is a no-op.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ACC_AutoKeyTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ctrl->selectBone(ctrl->boneNames().first());

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    const int before = track->getNumKeyFrames();

    // autoKey off → no-op.
    ctrl->setAutoKey(false);
    ctrl->setSliderValue(250);
    ctrl->autoKeyOnTransform();
    EXPECT_EQ(track->getNumKeyFrames(), before);

    // autoKey on → adds a keyframe.
    ctrl->setAutoKey(true);
    ctrl->autoKeyOnTransform();
    app->processEvents();
    EXPECT_EQ(track->getNumKeyFrames(), before + 1);

    // Re-firing at the same scrub time must NOT stack a duplicate — it
    // updates the existing keyframe in place. Otherwise auto-key on a
    // single drag would balloon the track on every redundant mouse-release.
    ctrl->autoKeyOnTransform();
    app->processEvents();
    EXPECT_EQ(track->getNumKeyFrames(), before + 1);
    ctrl->setAutoKey(false);
}

TEST_F(AnimationControlControllerTest, DeleteKeyframeDecreasesCount) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_DelKfTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    ctrl->selectBone(ctrl->boneNames().first());

    auto* skel  = entity->getSkeleton();
    auto* anim  = skel->getAnimation("TestAnim");
    auto* track = anim->_getNodeTrackList().begin()->second;

    ctrl->setSliderValue(500); // exact keyframe
    app->processEvents();
    int before = track->getNumKeyFrames();

    ctrl->deleteKeyframe();
    app->processEvents();

    EXPECT_EQ(track->getNumKeyFrames(), before - 1);
}

// ── Keyframe value setters ─────────────────────────────────────────────────────

TEST_F(AnimationControlControllerTest, SetKfTransXUpdatesKeyframe) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_TransXTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    ctrl->selectBone(ctrl->boneNames().first());
    ctrl->setSliderValue(500);
    app->processEvents();

    ASSERT_TRUE(ctrl->onKeyframe());
    ctrl->setKfTransX(99.0);

    auto* skel  = entity->getSkeleton();
    auto* anim  = skel->getAnimation("TestAnim");
    auto* track = anim->_getNodeTrackList().begin()->second;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - 0.5f) < 0.01f) {
            EXPECT_NEAR(kf->getTranslate().x, 99.0f, 0.01f);
            return;
        }
    }
    FAIL() << "Keyframe at t=0.5 not found";
}

TEST_F(AnimationControlControllerTest, SetKfScaleYUpdatesKeyframe) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_ScaleYTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    ctrl->selectBone(ctrl->boneNames().first());
    ctrl->setSliderValue(500);
    app->processEvents();

    ASSERT_TRUE(ctrl->onKeyframe());
    ctrl->setKfScaleY(3.5);

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - 0.5f) < 0.01f) {
            EXPECT_NEAR(kf->getScale().y, 3.5f, 0.01f);
            return;
        }
    }
    FAIL() << "Keyframe at t=0.5 not found";
}

TEST_F(AnimationControlControllerTest, SetKfRotWUpdatesKeyframe) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_RotWTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    ctrl->selectBone(ctrl->boneNames().first());
    ctrl->setSliderValue(500);
    app->processEvents();

    ASSERT_TRUE(ctrl->onKeyframe());
    ctrl->setKfRotW(0.707);

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - 0.5f) < 0.01f) {
            EXPECT_NEAR(kf->getRotation().w, 0.707f, 0.01f);
            return;
        }
    }
    FAIL() << "Keyframe at t=0.5 not found";
}

// ── No-op safety when no animation selected ───────────────────────────────────

TEST_F(AnimationControlControllerTest, AddKeyframeWithNoAnimationDoesNotCrash) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_NO_THROW(ctrl->addKeyframe());
}

TEST_F(AnimationControlControllerTest, DeleteKeyframeWithNoAnimationDoesNotCrash) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_NO_THROW(ctrl->deleteKeyframe());
}

TEST_F(AnimationControlControllerTest, NextKeyframeWithNoAnimationDoesNotCrash) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_NO_THROW(ctrl->nextKeyframe());
}

TEST_F(AnimationControlControllerTest, PrevKeyframeWithNoAnimationDoesNotCrash) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_NO_THROW(ctrl->prevKeyframe());
}

TEST_F(AnimationControlControllerTest, SetKfTransXWithNoKeyframeDoesNotCrash) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_NO_THROW(ctrl->setKfTransX(1.0));
}

// ── Playback / loop / auto-key (pure-data — no Ogre needed) ───────────────────
//
// These tests use a separate fixture that does NOT init Ogre, so they run on
// macOS too (where Ogre plugins fail to load for the test binary).

class AnimationControlControllerPlaybackTest : public ::testing::Test {
protected:
    void SetUp() override {
        AnimationControlController::kill();
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }
    void TearDown() override {
        AnimationControlController::kill();
    }
    QApplication* app = nullptr;
};

TEST_F(AnimationControlControllerPlaybackTest, PlaybackSpeedDefaultsToOne) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_DOUBLE_EQ(ctrl->playbackSpeed(), 1.0);
}

TEST_F(AnimationControlControllerPlaybackTest, SetPlaybackSpeedClampsNegative) {
    auto* ctrl = AnimationControlController::instance();
    ctrl->setPlaybackSpeed(-1.5);
    EXPECT_DOUBLE_EQ(ctrl->playbackSpeed(), 0.0);
}

TEST_F(AnimationControlControllerPlaybackTest, SetPlaybackSpeedEmitsSignalOnChange) {
    auto* ctrl = AnimationControlController::instance();
    QSignalSpy spy(ctrl, &AnimationControlController::playbackSpeedChanged);
    ctrl->setPlaybackSpeed(2.0);
    EXPECT_EQ(spy.count(), 1);
    ctrl->setPlaybackSpeed(2.0); // unchanged — no re-emit
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(AnimationControlControllerPlaybackTest, AdvanceTimeScalesByPlaybackSpeed) {
    auto* ctrl = AnimationControlController::instance();
    ctrl->setPlaybackSpeed(2.0);
    EXPECT_DOUBLE_EQ(ctrl->advanceTime(0.0, 0.5), 1.0);
    ctrl->setPlaybackSpeed(0.5);
    EXPECT_DOUBLE_EQ(ctrl->advanceTime(0.0, 0.5), 0.25);
    ctrl->setPlaybackSpeed(0.0);
    EXPECT_DOUBLE_EQ(ctrl->advanceTime(0.5, 0.5), 0.5);
}

TEST_F(AnimationControlControllerPlaybackTest, LoopRegionDefaultsInactive) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_FALSE(ctrl->loopRegionActive());
    EXPECT_DOUBLE_EQ(ctrl->loopStart(), 0.0);
    EXPECT_DOUBLE_EQ(ctrl->loopEnd(), 0.0);
}

TEST_F(AnimationControlControllerPlaybackTest, LoopRegionInactivePassthrough) {
    auto* ctrl = AnimationControlController::instance();
    ctrl->setLoopStart(0.2);
    ctrl->setLoopEnd(0.8);
    ctrl->setLoopRegionActive(false);
    EXPECT_DOUBLE_EQ(ctrl->advanceTime(0.7, 0.5), 1.2);
}

TEST_F(AnimationControlControllerPlaybackTest, LoopRegionWrapsAtEnd) {
    auto* ctrl = AnimationControlController::instance();
    ctrl->setPlaybackSpeed(1.0);
    ctrl->setLoopStart(0.2);
    ctrl->setLoopEnd(0.8);
    ctrl->setLoopRegionActive(true);
    EXPECT_NEAR(ctrl->advanceTime(0.7, 0.2), 0.3, 1e-9);
}

TEST_F(AnimationControlControllerPlaybackTest, LoopRegionWrapsLargeOverShoot) {
    auto* ctrl = AnimationControlController::instance();
    ctrl->setPlaybackSpeed(1.0);
    ctrl->setLoopStart(0.0);
    ctrl->setLoopEnd(1.0);
    ctrl->setLoopRegionActive(true);
    EXPECT_NEAR(ctrl->advanceTime(0.9, 2.5), 0.4, 1e-9);
}

TEST_F(AnimationControlControllerPlaybackTest, LoopRegionDegenerateNoWrap) {
    auto* ctrl = AnimationControlController::instance();
    ctrl->setLoopStart(0.5);
    ctrl->setLoopEnd(0.5);
    ctrl->setLoopRegionActive(true);
    EXPECT_DOUBLE_EQ(ctrl->advanceTime(0.6, 0.5), 1.1);
}

TEST_F(AnimationControlControllerPlaybackTest, LoopStartClampsToEnd) {
    auto* ctrl = AnimationControlController::instance();
    ctrl->setLoopEnd(0.5);
    ctrl->setLoopStart(0.9);
    EXPECT_LE(ctrl->loopStart(), ctrl->loopEnd());
}

TEST_F(AnimationControlControllerTest, SelectAnimationResetsLoopRegion) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_LoopResetTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->setLoopRegionActive(true);
    ctrl->setLoopStart(0.3);
    ctrl->setLoopEnd(0.7);

    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    EXPECT_DOUBLE_EQ(ctrl->loopStart(), 0.0);
    EXPECT_DOUBLE_EQ(ctrl->loopEnd(), 1.0); // animation length is 1.0s
    EXPECT_FALSE(ctrl->loopRegionActive());
}

// ── Poll timer ─────────────────────────────────────────────────────────────────

TEST_F(AnimationControlControllerTest, PollTimerDoesNotCrashWithNoAnimation) {
    auto* ctrl = AnimationControlController::instance();
    // Let the 16ms poll timer fire several times via event-driven wait
    QTest::qWait(100);
    SUCCEED();
}

TEST_F(AnimationControlControllerTest, PollTimerDoesNotCrashWithAnimation) {
    ASSERT_TRUE(canLoadMeshFiles());

    Ogre::Entity* entity = setupAnimatedEntity("ACC_TimerTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    ctrl->selectBone(ctrl->boneNames().first());

    // Let the poll timer fire several times via event-driven wait
    QTest::qWait(100);
    SUCCEED();
}

// ── Dope sheet API (slice C) ──────────────────────────────────────────────────

TEST_F(AnimationControlControllerPlaybackTest, AllBoneRowsEmptyWithoutSelection) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_TRUE(ctrl->allBoneRows().isEmpty());
}

TEST_F(AnimationControlControllerPlaybackTest, MoveKeyframeNoOpWithoutSelection) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_FALSE(ctrl->moveKeyframe("Bone", 0.5, 0.6));
}

TEST_F(AnimationControlControllerTest, AllBoneRowsReflectsTracks) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("DopeSheet_RowsTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");

    QVariantList rows = ctrl->allBoneRows();
    ASSERT_FALSE(rows.isEmpty());
    auto firstRow = rows.first().toMap();
    EXPECT_TRUE(firstRow.contains("bone"));
    EXPECT_TRUE(firstRow.contains("keyTimes"));
    EXPECT_TRUE(firstRow.contains("channels"));
    // TestAnim has 3 keyframes on the Child track (handle 1)
    auto times = firstRow["keyTimes"].toList();
    EXPECT_EQ(times.size(), 3);
}

TEST_F(AnimationControlControllerPlaybackTest, AllBoneRowsEmptyWhenNoAnimSelected) {
    // Pure-data guard: with no animation selected, allBoneRows must return
    // an empty list (not crash, not return stale shape).
    auto* ctrl = AnimationControlController::instance();
    EXPECT_TRUE(ctrl->allBoneRows().isEmpty());
}

TEST_F(AnimationControlControllerTest, AllBoneRowsReportsActiveChannels) {
    // TestAnim's middle keyframe sets translate.x = 0.5 and rotates 30°
    // around Y. The channel-detection should mark tx, rw, and ry as active
    // (the rotation around Y leaves rw < 1.0 and ry > 0); ty/tz/rx/rz/s* are
    // identity throughout and must NOT be marked active.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("DopeSheet_ChannelsTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");

    QVariantList rows = ctrl->allBoneRows();
    ASSERT_FALSE(rows.isEmpty());
    auto channels = rows.first().toMap()["channels"].toMap();

    EXPECT_TRUE(channels["tx"].toBool());
    EXPECT_FALSE(channels["ty"].toBool());
    EXPECT_FALSE(channels["tz"].toBool());

    EXPECT_TRUE(channels["rw"].toBool());
    EXPECT_FALSE(channels["rx"].toBool());
    EXPECT_TRUE(channels["ry"].toBool());
    EXPECT_FALSE(channels["rz"].toBool());

    EXPECT_FALSE(channels["sx"].toBool());
    EXPECT_FALSE(channels["sy"].toBool());
    EXPECT_FALSE(channels["sz"].toBool());
}

TEST_F(AnimationControlControllerTest, AllBoneRowsTreatsNegatedQuaternionAsIdentity) {
    // q and -q encode the same rotation. Set every keyframe's rotation to
    // (-1, 0, 0, 0) — the negative of identity — and verify no rotation
    // channel is flagged active. A naive component check would flag rw
    // because -1 != 1, producing a bogus rotation chevron in the dope sheet.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("DopeSheet_NegIdentityTest");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* track = skel->getAnimation("TestAnim")->_getNodeTrackList().begin()->second;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        kf->setTranslate(Ogre::Vector3::ZERO);
        kf->setRotation(Ogre::Quaternion(-1.0f, 0.0f, 0.0f, 0.0f));
        kf->setScale(Ogre::Vector3::UNIT_SCALE);
    }

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");

    auto channels = ctrl->allBoneRows().first().toMap()["channels"].toMap();
    EXPECT_FALSE(channels["rw"].toBool());
    EXPECT_FALSE(channels["rx"].toBool());
    EXPECT_FALSE(channels["ry"].toBool());
    EXPECT_FALSE(channels["rz"].toBool());
}

TEST_F(AnimationControlControllerTest, AllBoneRowsAllChannelsFalseForIdentityOnlyTrack) {
    // Build an animation whose every keyframe is identity (zero translate,
    // identity rotation, unit scale). No channel should be flagged active —
    // QML uses this to skip painting empty per-channel sub-rows.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("DopeSheet_IdentityOnlyTest");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();

    // Replace TestAnim's middle keyframe values with identity so every
    // sample matches bind pose.
    auto* track = skel->getAnimation("TestAnim")->_getNodeTrackList().begin()->second;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        kf->setTranslate(Ogre::Vector3::ZERO);
        kf->setRotation(Ogre::Quaternion::IDENTITY);
        kf->setScale(Ogre::Vector3::UNIT_SCALE);
    }

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");

    QVariantList rows = ctrl->allBoneRows();
    ASSERT_FALSE(rows.isEmpty());
    auto channels = rows.first().toMap()["channels"].toMap();
    for (const QString& key : { "tx", "ty", "tz",
                                "rw", "rx", "ry", "rz",
                                "sx", "sy", "sz" }) {
        EXPECT_FALSE(channels[key].toBool())
            << "channel " << key.toStdString()
            << " should be inactive on identity-only track";
    }
}

// ── Curve editor APIs (slice D3b) ──────────────────────────────────────────────

TEST_F(AnimationControlControllerPlaybackTest, ChannelValuesEmptyWhenNoSelection) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_TRUE(ctrl->channelValuesAt("Bone", "tx").isEmpty());
}

TEST_F(AnimationControlControllerPlaybackTest, ChannelValuesUnknownChannelReturnsEmpty) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_TRUE(ctrl->channelValuesAt("Bone", "unknown").isEmpty());
}

TEST_F(AnimationControlControllerPlaybackTest, SetKeyframeValueRejectsUnknownChannel) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_FALSE(ctrl->setKeyframeValue("Bone", "qq", 0.5, 1.0));
}

TEST_F(AnimationControlControllerPlaybackTest, SetKeyframeValueRejectsWithoutSelection) {
    auto* ctrl = AnimationControlController::instance();
    // No animation selected → should return false, not silently push a
    // no-op command onto the undo stack.
    EXPECT_FALSE(ctrl->setKeyframeValue("Bone", "tx", 0.5, 1.0));
}

TEST_F(AnimationControlControllerTest, ChannelValuesReadsTrack) {
    // TestAnim's middle keyframe has translate.x = 0.5 (rest are 0). The
    // channelValuesAt API must return [0, 0.5, 0] for "tx" in time order.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_ChannelValuesTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    QVariantList tx = ctrl->channelValuesAt(bone, "tx");
    ASSERT_EQ(tx.size(), 3);
    EXPECT_NEAR(tx[0].toDouble(), 0.0, 1e-4);
    EXPECT_NEAR(tx[1].toDouble(), 0.5, 1e-4);
    EXPECT_NEAR(tx[2].toDouble(), 0.0, 1e-4);
}

TEST_F(AnimationControlControllerTest, SetKeyframeValueRejectsMissingTime) {
    // No keyframe at 0.42 — the controller must return false instead of
    // pushing a no-op SetKeyframeValueCommand onto the undo stack.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_MissingTimeTest");
    ASSERT_NE(entity, nullptr);
    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();
    EXPECT_FALSE(ctrl->setKeyframeValue(bone, "tx", 0.42, 5.0));
}

TEST_F(AnimationControlControllerTest, SetKeyframeValueWritesOneChannelOnly) {
    // Setting tx must leave ty/tz/r*/s* on the same keyframe alone.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_SetValueTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    EXPECT_TRUE(ctrl->setKeyframeValue(bone, "tx", 0.5, 7.5));

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - 0.5f) < 0.001f) {
            EXPECT_NEAR(kf->getTranslate().x, 7.5f, 1e-4);
            // The original middle keyframe had ty/tz = 0 — must still be 0.
            EXPECT_NEAR(kf->getTranslate().y, 0.0f, 1e-4);
            EXPECT_NEAR(kf->getTranslate().z, 0.0f, 1e-4);
            // Rotation around Y (30°) — verify it survived the tx write.
            EXPECT_NEAR(kf->getRotation().y, 0.2588f, 1e-3);
            return;
        }
    }
    FAIL() << "Keyframe at t=0.5 not found";
}

TEST_F(AnimationControlControllerTest, MoveKeyframeShiftsTime) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("DopeSheet_MoveTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    QString bone = ctrl->boneNames().first();

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    const int beforeCount = track->getNumKeyFrames();

    EXPECT_TRUE(ctrl->moveKeyframe(bone, 0.5, 0.7));

    // Same number of keyframes, none at 0.5, one at 0.7.
    EXPECT_EQ(track->getNumKeyFrames(), beforeCount);
    bool foundOriginal = false, foundMoved = false;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        const float t = track->getKeyFrame(i)->getTime();
        if (std::fabs(t - 0.5f) < 0.001f) foundOriginal = true;
        if (std::fabs(t - 0.7f) < 0.001f) foundMoved = true;
    }
    EXPECT_FALSE(foundOriginal);
    EXPECT_TRUE(foundMoved);
}

TEST_F(AnimationControlControllerTest, MoveKeyframeRejectsCollision) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("DopeSheet_CollisionTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    // TestAnim has keyframes at 0.0, 0.5, 1.0. Moving 0.5 onto 1.0 must fail.
    EXPECT_FALSE(ctrl->moveKeyframe(bone, 0.5, 1.0));

    // The 0.5 keyframe is still there.
    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    bool stillThere = false;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        if (std::fabs(track->getKeyFrame(i)->getTime() - 0.5f) < 0.001f) {
            stillThere = true; break;
        }
    }
    EXPECT_TRUE(stillThere);
}

// ── Preview API (curve editor live-drag) ──────────────────────────────────────

TEST_F(AnimationControlControllerTest, MoveKeyframePreviewRetimesWithoutUndoPush) {
    // The curve editor calls this on every mouseMove during a keyframe
    // drag — it must NOT push onto the undo stack (otherwise MainWindow's
    // indexChanged handler resets the skeleton and the bone blinks to
    // T-pose between events).
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_MovePreview");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    const int undoBefore = UndoManager::getSingleton()->stack()->count();

    EXPECT_TRUE(ctrl->moveKeyframePreview(bone, 0.5, 0.7));

    EXPECT_EQ(UndoManager::getSingleton()->stack()->count(), undoBefore)
        << "moveKeyframePreview must not push undo commands";

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    bool foundMoved = false, foundOriginal = false;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        const float t = track->getKeyFrame(i)->getTime();
        if (std::fabs(t - 0.5f) < 0.001f) foundOriginal = true;
        if (std::fabs(t - 0.7f) < 0.001f) foundMoved = true;
    }
    EXPECT_FALSE(foundOriginal);
    EXPECT_TRUE(foundMoved);
}

TEST_F(AnimationControlControllerTest, MoveKeyframePreviewPreservesTRS) {
    // The retime must preserve the keyframe's translate/rotate/scale.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_MovePreservesTRS");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    Ogre::TransformKeyFrame* before = nullptr;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - 0.5f) < 0.001f) { before = kf; break; }
    }
    ASSERT_NE(before, nullptr);
    const Ogre::Vector3 t = before->getTranslate();
    const Ogre::Quaternion r = before->getRotation();
    const Ogre::Vector3 s = before->getScale();

    EXPECT_TRUE(ctrl->moveKeyframePreview(bone, 0.5, 0.65));

    Ogre::TransformKeyFrame* after = nullptr;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - 0.65f) < 0.001f) { after = kf; break; }
    }
    ASSERT_NE(after, nullptr);
    EXPECT_NEAR(after->getTranslate().x, t.x, 1e-4);
    EXPECT_NEAR(after->getTranslate().y, t.y, 1e-4);
    EXPECT_NEAR(after->getTranslate().z, t.z, 1e-4);
    EXPECT_NEAR(after->getRotation().w, r.w, 1e-4);
    EXPECT_NEAR(after->getScale().x,    s.x, 1e-4);
}

TEST_F(AnimationControlControllerTest, MoveKeyframePreviewRejectsCollision) {
    // Same collision rules as moveKeyframe so the preview can't silently
    // overwrite an adjacent keyframe mid-drag.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_MovePreviewCollision");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    EXPECT_FALSE(ctrl->moveKeyframePreview(bone, 0.5, 1.0));

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    bool stillThere = false;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        if (std::fabs(track->getKeyFrame(i)->getTime() - 0.5f) < 0.001f) {
            stillThere = true; break;
        }
    }
    EXPECT_TRUE(stillThere);
}

TEST_F(AnimationControlControllerTest, MoveKeyframePreviewRejectsMissingTime) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_MovePreviewMissing");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    EXPECT_FALSE(ctrl->moveKeyframePreview(bone, 0.42, 0.6));
}

TEST_F(AnimationControlControllerTest, MoveKeyframePreviewNoOpOnIdenticalTime) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_MovePreviewSame");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    EXPECT_FALSE(ctrl->moveKeyframePreview(bone, 0.5, 0.5));
}

TEST_F(AnimationControlControllerPlaybackTest, MoveKeyframePreviewNoOpWithoutSelection) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_FALSE(ctrl->moveKeyframePreview("Bone", 0.5, 0.6));
}

// ── setCurveHandle / Ogre interp sync / explicit Bake ────────────────────────

TEST_F(AnimationControlControllerTest, SetCurveHandleDoesNotInsertKeyframes) {
    // A mode/tangent edit must update the side-table + Ogre's per-anim
    // interp mode WITHOUT exploding the keyframe count. The user opts
    // into resampling explicitly via the Bake button.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_NoBake");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    const int before = track->getNumKeyFrames();

    EXPECT_TRUE(ctrl->setCurveHandle(bone, "tx", 0.5, 0.0, 0.0,
                                      CurveEditModel::ModeBezier));

    EXPECT_EQ(track->getNumKeyFrames(), before)
        << "setCurveHandle must not insert keyframes";
}

TEST_F(AnimationControlControllerTest, SetCurveHandleDoesNotMutateAnimInterp) {
    // Animation::setInterpolationMode is per-Animation, not per-track —
    // touching it on a single-bone curve edit distorts every other
    // bone's track. setCurveHandle must leave the animation's interp
    // mode untouched; the user opts in to a resample via Bake instead.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_NoInterpFlip");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();
    Ogre::Animation* anim = entity->getSkeleton()->getAnimation("TestAnim");
    const auto before = anim->getInterpolationMode();

    EXPECT_TRUE(ctrl->setCurveHandle(bone, "tx", 0.5, 1.0, 1.0,
                                      CurveEditModel::ModeBezier));
    EXPECT_EQ(anim->getInterpolationMode(), before);

    EXPECT_TRUE(ctrl->setCurveHandle(bone, "tx", 0.5, 1.0, 1.0,
                                      CurveEditModel::ModeStepped));
    EXPECT_EQ(anim->getInterpolationMode(), before);
}

TEST_F(AnimationControlControllerTest, SetCurveHandleUndoRestoresModelEntry) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_HandleUndo");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();
    QString skel = QString::fromStdString(entity->getName());

    EXPECT_TRUE(ctrl->setCurveHandle(bone, "tx", 0.5, 1.0, 1.0,
                                      CurveEditModel::ModeStepped));

    auto* m = CurveEditModel::instance();
    auto after = m->tangentsAt(skel, "TestAnim", bone, "tx", 0.5);
    EXPECT_EQ(after[2].toInt(), CurveEditModel::ModeStepped);

    UndoManager::getSingleton()->stack()->undo();

    auto restored = m->tangentsAt(skel, "TestAnim", bone, "tx", 0.5);
    EXPECT_EQ(restored[2].toInt(), CurveEditModel::ModeBezier);
}

TEST_F(AnimationControlControllerTest, ResampleAllSegmentsForBoneIsExplicit) {
    // The Bake button calls this to commit the visual curve into
    // dense TransformKeyFrames. Returns the number of segments
    // resampled, and the keyframe count grows substantially.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_BakeButton");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    // Stepped on the start key drives the resampler to keep dense
    // samples through the discontinuity.
    ctrl->setCurveHandle(bone, "tx", 0.0, 0.0, 0.0,
                         CurveEditModel::ModeStepped);

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    const int before = track->getNumKeyFrames();

    // Dense level keeps fidelity at the lowest tolerance multiplier,
    // so the bake reliably adds keys for a stepped curve.
    const int segments = ctrl->resampleAllSegmentsForBone(bone, "tx", 2);
    EXPECT_GT(segments, 0);
    EXPECT_GT(track->getNumKeyFrames(), before)
        << "Bake should densify the track";
}

TEST_F(AnimationControlControllerTest, BakeAt60FpsRegridsTrack) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_Bake60");
    ASSERT_NE(entity, nullptr);
    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    const int before = track->getNumKeyFrames();
    ctrl->resampleAllSegmentsForBone(bone, "tx", 6);  // 6 = 60 FPS
    const int after = track->getNumKeyFrames();
    EXPECT_GT(after, before + 30) << "60 FPS bake must noticeably densify a sparse track";
}

TEST_F(AnimationControlControllerTest, BakeUndoEmitsBoneRowsChanged) {
    // Ctrl+Z after a Bake must fire boneRowsChanged so the QML
    // dope-sheet + curve-editor refresh their cached keyTimes —
    // otherwise the views show stale dense keyframes even though
    // the underlying track has been reverted ("intermediate state"
    // bug user reported).
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_BakeUndoSignal");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    QSignalSpy spy(ctrl, &AnimationControlController::boneRowsChanged);
    ctrl->resampleAllSegmentsForBone(bone, "tx", 2);
    spy.clear();

    ctrl->onUndoRedoCommandApplied();
    EXPECT_GE(spy.count(), 1) << "undo path must emit boneRowsChanged";
}

TEST_F(AnimationControlControllerTest, BakeFixedFpsProducesUniformDensity) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_BakeFps");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    const int baseCount = track->getNumKeyFrames();

    // density=5 → 30 FPS exact. TestAnim is a 1s clip, expect ~30
    // uniform keys after baking. Just verify the count grew well
    // beyond the original sparse anchors.
    ctrl->resampleAllSegmentsForBone(bone, "tx", 5);
    EXPECT_GT(track->getNumKeyFrames(), baseCount + 10)
        << "30 FPS bake should produce predictable dense keys";
}

TEST_F(AnimationControlControllerTest, BakeDensityLevelsProduceDifferentCounts) {
    // Sparse < Medium < Dense in resulting keyframe counts (for a
    // curve sharp enough that simplification doesn't collapse the
    // medium and dense passes to identical counts).
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_BakeDensity");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();
    ctrl->setCurveHandle(bone, "tx", 0.0, 0.0, 0.0,
                         CurveEditModel::ModeStepped);

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    const int baseCount = track->getNumKeyFrames();

    // Sparse pass.
    ctrl->resampleAllSegmentsForBone(bone, "tx", 0);
    const int sparseCount = track->getNumKeyFrames();
    UndoManager::getSingleton()->stack()->undo();
    EXPECT_EQ(track->getNumKeyFrames(), baseCount);

    // Dense pass.
    ctrl->resampleAllSegmentsForBone(bone, "tx", 2);
    const int denseCount = track->getNumKeyFrames();

    EXPECT_GT(denseCount, sparseCount)
        << "Dense bake must produce more keyframes than Sparse";
}

TEST_F(AnimationControlControllerTest, ResampleAllSegmentsForBoneIsSingleUndoStep) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_BakeMacro");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    ctrl->setCurveHandle(bone, "tx", 0.0, 0.0, 0.0,
                         CurveEditModel::ModeStepped);
    auto* stack = UndoManager::getSingleton()->stack();
    const int before = stack->count();

    ctrl->resampleAllSegmentsForBone(bone, "tx");

    EXPECT_EQ(stack->count(), before + 1)
        << "Bake must collapse all per-segment resamples into ONE undo entry";
}

TEST_F(AnimationControlControllerTest, SetKeyframeValuePreviewWritesWithoutUndoPush) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_ValuePreview");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    const int undoBefore = UndoManager::getSingleton()->stack()->count();
    EXPECT_TRUE(ctrl->setKeyframeValuePreview(bone, "tx", 0.5, 9.25));
    EXPECT_EQ(UndoManager::getSingleton()->stack()->count(), undoBefore);

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - 0.5f) < 0.001f) {
            EXPECT_NEAR(kf->getTranslate().x, 9.25f, 1e-4);
            return;
        }
    }
    FAIL() << "Keyframe at t=0.5 not found";
}

TEST_F(AnimationControlControllerTest, SetKeyframeValuePreviewRejectsUnknownChannel) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("CurveEditor_ValuePreviewUnknown");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    EXPECT_FALSE(ctrl->setKeyframeValuePreview(bone, "qq", 0.5, 1.0));
}

TEST_F(AnimationControlControllerPlaybackTest, SetKeyframeValuePreviewNoOpWithoutSelection) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_FALSE(ctrl->setKeyframeValuePreview("Bone", "tx", 0.5, 1.0));
}

// ── Bulk keyframe ops (slice D1) ──────────────────────────────────────────────

TEST_F(AnimationControlControllerPlaybackTest, MoveKeyframesEmptySelectionReturnsFalse) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_FALSE(ctrl->moveKeyframes(QVariantList{}, 0.1));
}

TEST_F(AnimationControlControllerPlaybackTest, SerializeKeyframesEmptyReturnsEmpty) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_TRUE(ctrl->serializeKeyframes(QVariantList{}).isEmpty());
}

TEST_F(AnimationControlControllerPlaybackTest, PasteEmptyReturnsZero) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_EQ(ctrl->pasteKeyframesAt("", 0.0), 0);
}

TEST_F(AnimationControlControllerPlaybackTest, PasteRejectsMalformedJson) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_EQ(ctrl->pasteKeyframesAt("not-json", 0.0), 0);
    EXPECT_EQ(ctrl->pasteKeyframesAt("{\"kind\":\"wrong.kind\"}", 0.0), 0);
}

TEST_F(AnimationControlControllerPlaybackTest, AutoKeyDefaultsOff) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_FALSE(ctrl->autoKey());
}

TEST_F(AnimationControlControllerPlaybackTest, AutoKeyToggleEmitsSignal) {
    auto* ctrl = AnimationControlController::instance();
    ctrl->setAutoKey(false);
    QSignalSpy spy(ctrl, &AnimationControlController::autoKeyChanged);
    ctrl->setAutoKey(true);
    EXPECT_EQ(spy.count(), 1);
    ctrl->setAutoKey(true);
    EXPECT_EQ(spy.count(), 1);
    ctrl->setAutoKey(false);
}

TEST_F(AnimationControlControllerPlaybackTest, AutoKeyOnTransformNoOpWithoutSelection) {
    auto* ctrl = AnimationControlController::instance();
    ctrl->setAutoKey(true);
    EXPECT_NO_THROW(ctrl->autoKeyOnTransform());
    ctrl->setAutoKey(false);
}

TEST_F(AnimationControlControllerTest, MoveKeyframesShiftsAllByDt) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("DopeSheet_BulkMoveTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    // TestAnim has length 1.0 — extend so 0.0+0.1 / 0.5+0.1 fit.
    ctrl->setAnimationLength(2.0);
    QString bone = ctrl->boneNames().first();

    QVariantList sel;
    QVariantMap a; a["bone"] = bone; a["time"] = 0.0;
    QVariantMap b; b["bone"] = bone; b["time"] = 0.5;
    sel << a << b;

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    const int beforeCount = track->getNumKeyFrames();

    EXPECT_TRUE(ctrl->moveKeyframes(sel, 0.1));

    bool found01 = false, found06 = false;
    bool foundOriginal00 = false, foundOriginal05 = false;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        const float t = track->getKeyFrame(i)->getTime();
        if (std::fabs(t - 0.1f) < 0.001f) found01 = true;
        if (std::fabs(t - 0.6f) < 0.001f) found06 = true;
        if (std::fabs(t - 0.0f) < 0.001f) foundOriginal00 = true;
        if (std::fabs(t - 0.5f) < 0.001f) foundOriginal05 = true;
    }
    EXPECT_TRUE(found01);
    EXPECT_TRUE(found06);
    // Originals must be gone — a faulty implementation that inserts new
    // keyframes at the shifted times without removing the originals would
    // double the keyframe count and break the round-trip undo.
    EXPECT_FALSE(foundOriginal00);
    EXPECT_FALSE(foundOriginal05);
    EXPECT_EQ(track->getNumKeyFrames(), beforeCount);
}

TEST_F(AnimationControlControllerTest, MoveKeyframesClampsAtClipBoundary) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("DopeSheet_BulkBoundsTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    // Two keyframes at 0.0 and 0.5 selected; requesting dt = -0.5 would push
    // the first to -0.5. The controller now clamps the batch delta so the
    // selection lands on the nearest legal time instead of snapping back.
    // The largest legal *negative* dt for {0.0, 0.5} is 0 (since the leftmost
    // member is already at 0); the move becomes a no-op and returns false.
    QVariantList atZero;
    QVariantMap a; a["bone"] = bone; a["time"] = 0.0;
    atZero << a;
    EXPECT_FALSE(ctrl->moveKeyframes(atZero, -0.5));

    // Now try a partial-clamp case: select 0.5 and ask for -0.3. The largest
    // legal negative dt is -0.5 (since 0.5 - 0.5 = 0), so -0.3 lands fully —
    // the keyframe ends up at 0.2.
    QVariantList atHalf;
    QVariantMap b; b["bone"] = bone; b["time"] = 0.5;
    atHalf << b;
    EXPECT_TRUE(ctrl->moveKeyframes(atHalf, -0.3));
    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    bool found02 = false;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        if (std::fabs(track->getKeyFrame(i)->getTime() - 0.2f) < 0.001f) {
            found02 = true; break;
        }
    }
    EXPECT_TRUE(found02);
}

TEST_F(AnimationControlControllerTest, SerializePasteRoundTrip) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("DopeSheet_RoundTripTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ctrl->setAnimationLength(2.0);
    QString bone = ctrl->boneNames().first();

    // Serialize the existing keyframes at 0.0 and 0.5.
    QVariantList sel;
    QVariantMap a; a["bone"] = bone; a["time"] = 0.0;
    QVariantMap b; b["bone"] = bone; b["time"] = 0.5;
    sel << a << b;
    QString json = ctrl->serializeKeyframes(sel);
    EXPECT_FALSE(json.isEmpty());

    // Paste at t=1.2 — copies should land at 1.2 and 1.7.
    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    const int before = track->getNumKeyFrames();
    const int pasted = ctrl->pasteKeyframesAt(json, 1.2);
    EXPECT_EQ(pasted, 2);
    EXPECT_EQ(track->getNumKeyFrames(), before + 2);
    bool found12 = false, found17 = false;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        const float t = track->getKeyFrame(i)->getTime();
        if (std::fabs(t - 1.2f) < 0.001f) found12 = true;
        if (std::fabs(t - 1.7f) < 0.001f) found17 = true;
    }
    EXPECT_TRUE(found12);
    EXPECT_TRUE(found17);
}

TEST_F(AnimationControlControllerTest, PasteSkipsCollisions) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("DopeSheet_PasteCollisionTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    QString bone = ctrl->boneNames().first();

    // Paste back onto the existing 0.5 → collision; existing 0.0 paste lands at 0.0
    // → also a collision. Expect 0 pasted.
    QVariantList sel;
    QVariantMap a; a["bone"] = bone; a["time"] = 0.0;
    QVariantMap b; b["bone"] = bone; b["time"] = 0.5;
    sel << a << b;
    QString json = ctrl->serializeKeyframes(sel);
    const int n = ctrl->pasteKeyframesAt(json, 0.0);
    EXPECT_EQ(n, 0);
}

// ── boneCanTranslate ──────────────────────────────────────────────────────────
//
// Translation is restricted by the gizmo's press handler so users can't tear
// rigged bones away from their parent (which would produce broken poses on
// playback). The rules: skeleton roots are always translatable (locomotion
// bones), and unrigged bones — typical "attachment point" sockets for
// swords / shields / hats — are also translatable since they don't deform
// any geometry.

TEST_F(AnimationControlControllerTest, BoneCanTranslateNullBoneIsSafe) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_TRUE(ctrl->boneCanTranslate(nullptr));
}

TEST_F(AnimationControlControllerTest, BoneCanTranslateRootBoneAllowed) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ACC_BCT_Root");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");

    Ogre::Bone* root = entity->getSkeleton()->getBone("Root");
    ASSERT_FALSE(root->getParent());
    EXPECT_TRUE(ctrl->boneCanTranslate(root));
}

TEST_F(AnimationControlControllerTest, BoneCanTranslateRiggedNonRootBlocked) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ACC_BCT_Rigged");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");

    // "Child" is rigged — every vertex of the test mesh weights to it.
    Ogre::Bone* child = entity->getSkeleton()->getBone("Child");
    ASSERT_TRUE(child->getParent());
    EXPECT_FALSE(ctrl->boneCanTranslate(child));
}

TEST_F(AnimationControlControllerTest, BoneCanTranslateUnriggedNonRootAllowed) {
    // Add a third bone to the test skeleton with no vertex weights — this
    // simulates an attachment point (sword/shield/hat). Translation must
    // be allowed since moving it doesn't deform any geometry.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ACC_BCT_Attach");
    ASSERT_NE(entity, nullptr);

    auto* skel = entity->getSkeleton();
    Ogre::Bone* attach = skel->createBone("AttachPoint", 2);
    attach->setPosition(Ogre::Vector3(0, 1, 0.5f));
    skel->getBone("Child")->addChild(attach);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");

    ASSERT_TRUE(attach->getParent());
    EXPECT_TRUE(ctrl->boneCanTranslate(attach));
}

// ── onUndoRedoCommandApplied ──────────────────────────────────────────────────
//
// Called from MainWindow's QUndoStack::indexChanged handler after a
// keyframe-affecting command runs. Must invalidate stale track/keyframe
// pointers (an AddKeyframeCommand undo can destroy a lazy-created track)
// and preserve the user's current bone selection (rebuilding via
// refreshBoneList would reset it to the first bone — jarring UX).

TEST_F(AnimationControlControllerTest, OnUndoRedoCommandAppliedPreservesBoneSelection) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ACC_UndoPreservesBone");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());

    // Pick a non-default bone (the second in the list, if any). With
    // only one bone, this still validates "preserves selection".
    const QString picked = ctrl->boneNames().size() > 1
        ? ctrl->boneNames().at(1)
        : ctrl->boneNames().first();
    ctrl->selectBone(picked);
    ASSERT_EQ(ctrl->selectedBone(), picked);

    ctrl->onUndoRedoCommandApplied();
    EXPECT_EQ(ctrl->selectedBone(), picked)
        << "onUndoRedoCommandApplied reset bone selection (regression)";
}

TEST_F(AnimationControlControllerTest, OnUndoRedoCommandAppliedSurvivesMissingBone) {
    // Defensive: if m_selectedBone is empty (no bone picked yet),
    // the helper must not crash.
    auto* ctrl = AnimationControlController::instance();
    EXPECT_NO_THROW(ctrl->onUndoRedoCommandApplied());
}

// ── addKeyframe + undo/redo (integration) ────────────────────────────────────
//
// Exercises the +KF / auto-key wiring through AnimationControlController::
// addKeyframe() and the QUndoStack rather than the AddKeyframeCommand
// helper directly. The TrackCreated path in particular is fragile because
// the controller pre-creates the track before pushing the command, so a
// pure unit test misses what production code actually runs.

TEST_F(AnimationControlControllerTest, AddKeyframeIsUndoableViaQUndoStack) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ACC_AddKfUndo");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ctrl->selectBone(ctrl->boneNames().first());

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    const auto countBefore = track->getNumKeyFrames();

    // Add a kf at a slider position that doesn't already have one
    // (TestAnim has keys at 0.0, 0.5, 1.0 — pick 0.25).
    ctrl->setSliderValue(250);
    ctrl->addKeyframe();
    app->processEvents();
    EXPECT_EQ(track->getNumKeyFrames(), countBefore + 1);

    UndoManager::getSingleton()->stack()->undo();
    app->processEvents();
    EXPECT_EQ(track->getNumKeyFrames(), countBefore);

    UndoManager::getSingleton()->stack()->redo();
    app->processEvents();
    EXPECT_EQ(track->getNumKeyFrames(), countBefore + 1);
}

TEST_F(AnimationControlControllerTest, AddKeyframeOnUntrackedBoneIsUndoable) {
    // The TrackCreated path: lazy-create a track for a bone that
    // doesn't have one in the active animation, then undo. The
    // whole track should be destroyed, and the controller's cached
    // m_selectedTrack pointer must be invalidated so subsequent
    // slider scrubs don't crash.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ACC_AddKfTrackCreated");
    ASSERT_NE(entity, nullptr);

    auto* skel = entity->getSkeleton();
    Ogre::Bone* extra = skel->createBone("ExtraBone", 2);
    extra->setPosition(Ogre::Vector3(0, 0, 1));
    skel->getBone("Root")->addChild(extra);
    auto* anim = skel->getAnimation("TestAnim");
    ASSERT_FALSE(anim->hasNodeTrack(extra->getHandle()));

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ctrl->selectBone("ExtraBone");
    ctrl->setSliderValue(250);

    ctrl->addKeyframe();
    app->processEvents();
    EXPECT_TRUE(anim->hasNodeTrack(extra->getHandle()));

    UndoManager::getSingleton()->stack()->undo();
    app->processEvents();
    EXPECT_FALSE(anim->hasNodeTrack(extra->getHandle()));

    // Slider scrub after undo must not crash on a stale m_selectedTrack
    // (regression for the user-reported "crash after undo" bug).
    EXPECT_NO_THROW(ctrl->setSliderValue(500));
}

TEST_F(AnimationControlControllerTest, DeleteKeyframeIsUndoableViaQUndoStack) {
    // Verify -KF integration with the QUndoStack: deleting a keyframe,
    // undoing restores it; redoing removes it again.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ACC_DelKfUndo");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ctrl->selectBone(ctrl->boneNames().first());

    auto* track = entity->getSkeleton()->getAnimation("TestAnim")
                         ->_getNodeTrackList().begin()->second;
    const auto countBefore = track->getNumKeyFrames();

    // Land on the keyframe at 0.5s, then delete it.
    ctrl->setSliderValue(500);
    app->processEvents();
    ASSERT_TRUE(ctrl->onKeyframe());
    ctrl->deleteKeyframe();
    app->processEvents();
    EXPECT_EQ(track->getNumKeyFrames(), countBefore - 1);

    UndoManager::getSingleton()->stack()->undo();
    app->processEvents();
    EXPECT_EQ(track->getNumKeyFrames(), countBefore);

    UndoManager::getSingleton()->stack()->redo();
    app->processEvents();
    EXPECT_EQ(track->getNumKeyFrames(), countBefore - 1);
}

// ── Slice A5: allMorphRows for dope sheet ─────────────────────────────────

TEST_F(AnimationControlControllerTest, AllMorphRowsEmptyWhenNoSelection) {
    SelectionSet::getSingleton()->clear();
    auto* ctrl = AnimationControlController::instance();
    EXPECT_TRUE(ctrl->allMorphRows().isEmpty());
}

TEST_F(AnimationControlControllerTest, AllMorphRowsEmptyForMeshWithoutPoses) {
    // createAnimatedTestEntity has bones but no morph targets;
    // allMorphRows must return an empty list (not crash).
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("Morph_NoPoses");
    ASSERT_NE(entity, nullptr);
    auto* ctrl = AnimationControlController::instance();
    EXPECT_TRUE(ctrl->allMorphRows().isEmpty());
}

TEST_F(AnimationControlControllerTest, AllMorphRowsListsPoseNamesAndKeyTimes) {
    ASSERT_TRUE(canLoadMeshFiles());

    // Build a fresh mesh + entity with two named poses + matching
    // VAT_POSE animations — mirrors what MeshProcessor produces from
    // an FBX blend shape. Pose targets handle 0 (shared-vertex
    // submesh) to match createInMemoryTriangleMesh's layout.
    auto mesh = createInMemoryTriangleMesh("Morph_AllRows");
    ASSERT_NE(mesh, nullptr);
    {
        Ogre::Pose* p = mesh->createPose(0, "JawOpen");
        p->addVertex(0, Ogre::Vector3(0, -0.1f, 0));
    }
    {
        Ogre::Pose* p = mesh->createPose(0, "Smile");
        p->addVertex(1, Ogre::Vector3(0.05f, 0.02f, 0));
    }
    const auto& poseList = mesh->getPoseList();
    for (unsigned short pi = 0; pi < poseList.size(); ++pi) {
        Ogre::Animation* a = mesh->createAnimation(poseList[pi]->getName(), 0.0f);
        auto* track = a->createVertexTrack(poseList[pi]->getTarget(), Ogre::VAT_POSE);
        auto* kf = track->createVertexPoseKeyFrame(0.0f);
        kf->addPoseReference(pi, 1.0f);
    }

    auto* node = Manager::getSingleton()->addSceneNode("Morph_AllRows_Node");
    auto* entity = Manager::getSingleton()->getSceneMgr()->createEntity(node->getName(), mesh->getName());
    node->attachObject(entity);
    SelectionSet::getSingleton()->selectOne(node);
    app->processEvents();

    auto* ctrl = AnimationControlController::instance();
    QVariantList rows = ctrl->allMorphRows();
    ASSERT_EQ(rows.size(), 2);
    QSet<QString> names;
    for (const QVariant& v : rows) {
        const QVariantMap row = v.toMap();
        names.insert(row["name"].toString());
        // Each pose's Animation has a single keyframe at t=0.
        const QVariantList times = row["keyTimes"].toList();
        ASSERT_EQ(times.size(), 1);
        EXPECT_NEAR(times.first().toDouble(), 0.0, 1e-6);
    }
    EXPECT_TRUE(names.contains(QStringLiteral("JawOpen")));
    EXPECT_TRUE(names.contains(QStringLiteral("Smile")));
}


