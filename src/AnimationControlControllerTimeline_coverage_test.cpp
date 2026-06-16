// Coverage tests for AnimationControlController timeline / poll-timer / loop
// reclamp APIs. Distinct suite names + filename from
// AnimationControlController_test.cpp to avoid ODR / duplicate-registration.
//
// Targets:
//   - suspendPollTimer / resumePollTimer  (suspend/resume/no-op branches)
//   - setAnimationLength                  (slider clamp + AnimationState propagation)
//   - setLoopEnd                          (loopStart reclamp when end < start)
//   - setSliderValue                      (same-value-with-entity still syncs Ogre)
//   - qmlInstance                         (returns singleton, CppOwnership)

#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QQmlEngine>
#include <cmath>

#include "AnimationControlController.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreEntity.h>

// ── Pure-data fixture (no Ogre) ────────────────────────────────────────────────

class AnimationControlControllerTimelineCoverageTest : public ::testing::Test {
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

// qmlInstance returns the same object instance() does, with ownership set.
TEST_F(AnimationControlControllerTimelineCoverageTest, QmlInstanceReturnsSingleton) {
    auto* viaQml = AnimationControlController::qmlInstance(nullptr, nullptr);
    auto* viaInstance = AnimationControlController::instance();
    EXPECT_NE(viaQml, nullptr);
    EXPECT_EQ(viaQml, viaInstance);
    // CppOwnership means Qt's QML engine will not delete it; verify the
    // singleton survives a second qmlInstance call (idempotent).
    auto* viaQml2 = AnimationControlController::qmlInstance(nullptr, nullptr);
    EXPECT_EQ(viaQml2, viaInstance);
}

// resumePollTimer with no prior suspend is a safe no-op.
TEST_F(AnimationControlControllerTimelineCoverageTest, ResumeWithoutSuspendIsNoOp) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_NO_THROW(ctrl->resumePollTimer());
    // Calling it again must still be safe.
    EXPECT_NO_THROW(ctrl->resumePollTimer());
}

// suspend then resume on the freshly-created controller (timer running by
// default) toggles cleanly without throwing.
TEST_F(AnimationControlControllerTimelineCoverageTest, SuspendThenResumeNoThrow) {
    auto* ctrl = AnimationControlController::instance();
    // Timer is started in the constructor, so suspend takes the active branch.
    EXPECT_NO_THROW(ctrl->suspendPollTimer());
    // Resume takes the suspended branch.
    EXPECT_NO_THROW(ctrl->resumePollTimer());
}

// suspend twice: second call hits the "already stopped" no-op branch.
TEST_F(AnimationControlControllerTimelineCoverageTest, DoubleSuspendIsNoOp) {
    auto* ctrl = AnimationControlController::instance();
    EXPECT_NO_THROW(ctrl->suspendPollTimer());
    // Timer already stopped — second suspend must be a no-op.
    EXPECT_NO_THROW(ctrl->suspendPollTimer());
    // And a single resume restores it.
    EXPECT_NO_THROW(ctrl->resumePollTimer());
}

// resume twice after a single suspend: second resume hits the no-op branch
// because m_pollSuspended was cleared on the first resume.
TEST_F(AnimationControlControllerTimelineCoverageTest, DoubleResumeIsNoOp) {
    auto* ctrl = AnimationControlController::instance();
    ctrl->suspendPollTimer();
    EXPECT_NO_THROW(ctrl->resumePollTimer());
    EXPECT_NO_THROW(ctrl->resumePollTimer());
}

// setLoopEnd reclamp: pure-data path (no Ogre). Setting loopEnd below the
// current loopStart pulls loopStart down to loopEnd.
TEST_F(AnimationControlControllerTimelineCoverageTest, SetLoopEndReclampsLoopStartDown) {
    auto* ctrl = AnimationControlController::instance();
    // setLoopStart(0.8) requires loopEnd already large enough not to clamp it.
    ctrl->setLoopEnd(1.0);
    ctrl->setLoopStart(0.8);
    EXPECT_DOUBLE_EQ(ctrl->loopStart(), 0.8);

    // Now drop loopEnd below loopStart — loopStart should follow it down.
    ctrl->setLoopEnd(0.5);
    EXPECT_DOUBLE_EQ(ctrl->loopEnd(), 0.5);
    EXPECT_LE(ctrl->loopStart(), ctrl->loopEnd());
    EXPECT_DOUBLE_EQ(ctrl->loopStart(), 0.5);
}

// setLoopEnd negative input clamps to 0; same-value re-set is a no-op (no
// signal). Covers the s<0 clamp and the qFuzzyCompare early-out.
TEST_F(AnimationControlControllerTimelineCoverageTest, SetLoopEndClampsNegativeAndDedups) {
    auto* ctrl = AnimationControlController::instance();
    ctrl->setLoopEnd(0.6);
    EXPECT_DOUBLE_EQ(ctrl->loopEnd(), 0.6);

    QSignalSpy spy(ctrl, &AnimationControlController::loopRegionChanged);
    ctrl->setLoopEnd(0.6); // unchanged → no emit
    EXPECT_EQ(spy.count(), 0);

    ctrl->setLoopEnd(-3.0); // clamps to 0.0
    EXPECT_DOUBLE_EQ(ctrl->loopEnd(), 0.0);
}

// setLoopEnd above loopStart does NOT reclamp loopStart.
TEST_F(AnimationControlControllerTimelineCoverageTest, SetLoopEndAboveStartLeavesStart) {
    auto* ctrl = AnimationControlController::instance();
    ctrl->setLoopEnd(1.0);
    ctrl->setLoopStart(0.3);
    ctrl->setLoopEnd(0.9); // still above 0.3 → no reclamp
    EXPECT_DOUBLE_EQ(ctrl->loopStart(), 0.3);
    EXPECT_DOUBLE_EQ(ctrl->loopEnd(), 0.9);
}

// ── Ogre-backed fixture (identical to AnimationControlControllerTest) ──────────

class AnimationControlControllerTimelineOgreCoverageTest : public ::testing::Test {
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

// setAnimationLength slider-value-clamp branch: scrub past the new max, then
// shorten the animation; sliderValue must be clamped down and sliderValueChanged
// must fire.
TEST_F(AnimationControlControllerTimelineOgreCoverageTest, SetAnimationLengthClampsSliderValue) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ACCT_ClampTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");

    // TestAnim length is 1.0s → sliderMaximum 1000. Scrub near the end.
    ctrl->setSliderValue(900);
    EXPECT_EQ(ctrl->sliderValue(), 900);

    QSignalSpy spy(ctrl, &AnimationControlController::sliderValueChanged);
    ctrl->setAnimationLength(0.5); // new max 500 → 900 must clamp to 500
    EXPECT_LE(ctrl->sliderValue(), 500);
    EXPECT_EQ(ctrl->sliderMaximum(), 500);
    EXPECT_GE(spy.count(), 1);
}

// setAnimationLength AnimationState propagation branch: the entity's
// AnimationState length is updated and its time position clamped to the new
// length.
TEST_F(AnimationControlControllerTimelineOgreCoverageTest, SetAnimationLengthPropagatesToAnimationState) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ACCT_StatePropTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");

    ASSERT_TRUE(entity->hasAnimationState("TestAnim"));
    Ogre::AnimationState* state = entity->getAnimationState("TestAnim");

    // Push the state time near the old end so the clamp branch executes.
    ctrl->setSliderValue(900);
    app->processEvents();

    ctrl->setAnimationLength(0.4); // new length 0.4s
    app->processEvents();

    EXPECT_NEAR(state->getLength(), 0.4f, 1e-3);
    EXPECT_LE(state->getTimePosition(), 0.4f + 1e-3f);
    EXPECT_NEAR(ctrl->animationLength(), 0.4, 1e-3);
}

// setAnimationLength is a no-op when nothing is selected (early-out branch).
TEST_F(AnimationControlControllerTimelineOgreCoverageTest, SetAnimationLengthNoOpWithoutSelection) {
    auto* ctrl = AnimationControlController::instance();
    SelectionSet::getSingleton()->clear();
    app->processEvents();
    EXPECT_NO_THROW(ctrl->setAnimationLength(2.0));
    EXPECT_EQ(ctrl->sliderMaximum(), 0);
}

// setSliderValue same-value-with-selected-entity path: re-setting the current
// value still drives setAnimationFrame to keep Ogre's AnimationState in sync.
TEST_F(AnimationControlControllerTimelineOgreCoverageTest, SetSliderValueSameValueStillSyncsOgre) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ACCT_SameValueTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    ctrl->selectBone(ctrl->boneNames().first());

    ctrl->setSliderValue(300);
    EXPECT_EQ(ctrl->sliderValue(), 300);
    app->processEvents();

    ASSERT_TRUE(entity->hasAnimationState("TestAnim"));
    Ogre::AnimationState* state = entity->getAnimationState("TestAnim");

    // Force the AnimationState out of sync behind the controller's back, then
    // re-set the SAME slider value. The same-value-with-entity path must still
    // call setAnimationFrame and pull the state back to 0.3s.
    state->setTimePosition(0.05f);
    ctrl->setSliderValue(300); // unchanged value, entity selected
    app->processEvents();

    EXPECT_NEAR(state->getTimePosition(), 0.3f, 1e-2);
    EXPECT_EQ(ctrl->sliderValue(), 300);
}

// Poll timer behavioral test: while suspended, a manually-set AnimationState
// position is not overwritten by the poll loop; after resume, scrubbing still
// works and the controller does not crash.
TEST_F(AnimationControlControllerTimelineOgreCoverageTest, SuspendPollTimerStopsAutoAdvance) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ACCT_PollTest");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    ctrl->updateAnimationTree();
    ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
    ASSERT_FALSE(ctrl->boneNames().isEmpty());
    ctrl->selectBone(ctrl->boneNames().first());

    ASSERT_TRUE(entity->hasAnimationState("TestAnim"));
    Ogre::AnimationState* state = entity->getAnimationState("TestAnim");

    // Suspend the poll timer, then set a distinctive state position. While
    // suspended the poll loop must not fire, so the controller's sliderValue
    // is NOT pulled to match the state position we set behind its back.
    ctrl->suspendPollTimer();
    ctrl->setSliderValue(100);
    const int before = ctrl->sliderValue();
    state->setTimePosition(0.85f); // 850ms — would pull sliderValue if polled

    QTest::qWait(80); // long enough for several 16ms ticks if running
    app->processEvents();

    // The poll loop is suspended → sliderValue did not chase the state.
    EXPECT_EQ(ctrl->sliderValue(), before);

    // Resume and verify scrubbing still works and nothing crashes.
    EXPECT_NO_THROW(ctrl->resumePollTimer());
    EXPECT_NO_THROW(ctrl->setSliderValue(400));
    app->processEvents();
    EXPECT_EQ(ctrl->sliderValue(), 400);

    QTest::qWait(40);
    app->processEvents();
    SUCCEED();
}
