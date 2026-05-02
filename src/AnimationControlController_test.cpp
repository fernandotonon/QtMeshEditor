#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include "AnimationControlController.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreKeyFrame.h>

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
