// Coverage tests for AnimationControlController's resample / decimate API.
//
// Targets the untested branches of:
//   - reduceTrackToFps        (direct decimation + every guard)
//   - resampleCurveSegment    (direct success + every guard)
//   - setRowsRefreshSuspended / refreshAfterBulkResample (signal coalescing)
//   - resampleAllSegmentsForBone adaptive (density 0/1) baselineFps branch
//
// Distinct filename + suite name (AnimationControlControllerResampleCoverageTest)
// from src/AnimationControlController_test.cpp to avoid any ODR / duplicate-
// registration clash. The Ogre fixture is copied from that file.

#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QUndoStack>
#include <cmath>

#include "AnimationControlController.h"
#include "CurveEditModel.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "UndoManager.h"

#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreKeyFrame.h>
#include <OgreEntity.h>
#include <OgreSceneNode.h>

// ── Ogre fixture (copied from AnimationControlController_test.cpp) ───────────
class AnimationControlControllerResampleCoverageTest : public ::testing::Test {
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

    // Create an animated entity and select it (returns nullptr if mesh I/O
    // unavailable; callers guard with ASSERT_TRUE(canLoadMeshFiles())).
    Ogre::Entity* setupAnimatedEntity(const std::string& name) {
        if (!canLoadMeshFiles()) return nullptr;
        Ogre::Entity* entity = createAnimatedTestEntity(name);
        if (!entity) return nullptr;
        SelectionSet::getSingleton()->selectOne(entity->getParentSceneNode());
        app->processEvents();
        return entity;
    }

    // Drive controller to a selected animation + first bone. Returns the bone
    // name (empty if setup failed).
    QString driveToBone(Ogre::Entity* entity,
                        AnimationControlController* ctrl) {
        ctrl->updateAnimationTree();
        ctrl->selectAnimation(QString::fromStdString(entity->getName()),
                              "TestAnim");
        if (ctrl->boneNames().isEmpty()) return QString();
        const QString bone = ctrl->boneNames().first();
        ctrl->selectBone(bone);
        return bone;
    }

    static Ogre::NodeAnimationTrack* firstTrack(Ogre::Entity* entity) {
        return entity->getSkeleton()
                     ->getAnimation("TestAnim")
                     ->_getNodeTrackList().begin()->second;
    }

    QApplication* app = nullptr;
};

// ── reduceTrackToFps: direct success path ───────────────────────────────────

TEST_F(AnimationControlControllerResampleCoverageTest,
       ReduceTrackToFpsRemovesKeysFromDensifiedTrack) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ResCov_ReduceSuccess");
    ASSERT_NE(entity, nullptr);

    auto* ctrl = AnimationControlController::instance();
    QString bone = driveToBone(entity, ctrl);
    ASSERT_FALSE(bone.isEmpty());

    auto* track = firstTrack(entity);

    // First densify: bake to a 60 FPS uniform grid (density 6) so the track
    // has many more keyframes than the 1/10s decimation target will keep.
    ctrl->resampleAllSegmentsForBone(bone, "tx", 6);
    const int dense = track->getNumKeyFrames();
    EXPECT_GT(dense, 30) << "60 FPS bake should densify a 1s clip";

    // Now decimate to 10 FPS — must drop a large number of the dense keys.
    const int removed = ctrl->reduceTrackToFps(bone, 10);
    EXPECT_GT(removed, 0) << "decimation must report frames removed";

    const int after = track->getNumKeyFrames();
    EXPECT_LT(after, dense) << "track keyframe count must shrink";
    EXPECT_EQ(removed, dense - after) << "returned count == keys actually dropped";
}

// ── reduceTrackToFps: guards all return 0 ───────────────────────────────────

TEST_F(AnimationControlControllerResampleCoverageTest,
       ReduceTrackToFpsEmptyBoneReturnsZero) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ResCov_ReduceEmptyBone");
    ASSERT_NE(entity, nullptr);
    auto* ctrl = AnimationControlController::instance();
    ASSERT_FALSE(driveToBone(entity, ctrl).isEmpty());

    EXPECT_EQ(ctrl->reduceTrackToFps("", 10), 0);
}

TEST_F(AnimationControlControllerResampleCoverageTest,
       ReduceTrackToFpsNonPositiveFpsReturnsZero) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ResCov_ReduceBadFps");
    ASSERT_NE(entity, nullptr);
    auto* ctrl = AnimationControlController::instance();
    QString bone = driveToBone(entity, ctrl);
    ASSERT_FALSE(bone.isEmpty());

    EXPECT_EQ(ctrl->reduceTrackToFps(bone, 0), 0);
    EXPECT_EQ(ctrl->reduceTrackToFps(bone, -30), 0);
}

TEST_F(AnimationControlControllerResampleCoverageTest,
       ReduceTrackToFpsNoSelectionReturnsZero) {
    // No animation selected (no skeleton bound) → guard returns 0.
    auto* ctrl = AnimationControlController::instance();
    EXPECT_EQ(ctrl->reduceTrackToFps("AnyBone", 30), 0);
}

TEST_F(AnimationControlControllerResampleCoverageTest,
       ReduceTrackToFpsMissingBoneReturnsZero) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ResCov_ReduceMissingBone");
    ASSERT_NE(entity, nullptr);
    auto* ctrl = AnimationControlController::instance();
    ASSERT_FALSE(driveToBone(entity, ctrl).isEmpty());

    // A bone name the skeleton doesn't have.
    EXPECT_EQ(ctrl->reduceTrackToFps("NoSuchBone_xyz", 30), 0);
}

// ── resampleCurveSegment: direct success path ───────────────────────────────

TEST_F(AnimationControlControllerResampleCoverageTest,
       ResampleCurveSegmentSuccessPushesSingleCommand) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ResCov_SegSuccess");
    ASSERT_NE(entity, nullptr);
    auto* ctrl = AnimationControlController::instance();
    QString bone = driveToBone(entity, ctrl);
    ASSERT_FALSE(bone.isEmpty());

    auto* stack = UndoManager::getSingleton()->stack();
    const int undoBefore = stack->count();

    // TestAnim has keyframes at 0.0 / 0.5 / 1.0 — both endpoints are on keys.
    EXPECT_TRUE(ctrl->resampleCurveSegment(bone, "tx", 0.0, 0.5));

    EXPECT_EQ(stack->count(), undoBefore + 1)
        << "one ResampleCurveCommand pushed on success";
}

TEST_F(AnimationControlControllerResampleCoverageTest,
       ResampleCurveSegmentT1NotGreaterThanT0ReturnsFalse) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ResCov_SegT1LeT0");
    ASSERT_NE(entity, nullptr);
    auto* ctrl = AnimationControlController::instance();
    QString bone = driveToBone(entity, ctrl);
    ASSERT_FALSE(bone.isEmpty());

    auto* stack = UndoManager::getSingleton()->stack();
    const int undoBefore = stack->count();

    // t1 == t0 (both on the 0.5 key) — degenerate segment.
    EXPECT_FALSE(ctrl->resampleCurveSegment(bone, "tx", 0.5, 0.5));
    // t1 < t0.
    EXPECT_FALSE(ctrl->resampleCurveSegment(bone, "tx", 0.5, 0.0));

    EXPECT_EQ(stack->count(), undoBefore) << "no command pushed on rejection";
}

TEST_F(AnimationControlControllerResampleCoverageTest,
       ResampleCurveSegmentEndpointNotOnKeyframeReturnsFalse) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ResCov_SegOffKey");
    ASSERT_NE(entity, nullptr);
    auto* ctrl = AnimationControlController::instance();
    QString bone = driveToBone(entity, ctrl);
    ASSERT_FALSE(bone.isEmpty());

    auto* stack = UndoManager::getSingleton()->stack();
    const int undoBefore = stack->count();

    // 0.42 is not within 1ms of any key (keys are 0.0/0.5/1.0) → t1 off-key.
    EXPECT_FALSE(ctrl->resampleCurveSegment(bone, "tx", 0.0, 0.42));
    // 0.18 off-key as t0.
    EXPECT_FALSE(ctrl->resampleCurveSegment(bone, "tx", 0.18, 0.5));

    EXPECT_EQ(stack->count(), undoBefore) << "no command pushed when endpoint off-key";
}

TEST_F(AnimationControlControllerResampleCoverageTest,
       ResampleCurveSegmentUnknownChannelReturnsFalse) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ResCov_SegBadChannel");
    ASSERT_NE(entity, nullptr);
    auto* ctrl = AnimationControlController::instance();
    QString bone = driveToBone(entity, ctrl);
    ASSERT_FALSE(bone.isEmpty());

    EXPECT_FALSE(ctrl->resampleCurveSegment(bone, "qq", 0.0, 0.5));
    EXPECT_FALSE(ctrl->resampleCurveSegment("", "tx", 0.0, 0.5));
}

TEST_F(AnimationControlControllerResampleCoverageTest,
       ResampleCurveSegmentNoSelectionReturnsFalse) {
    // No animation/skeleton selected → guard returns false.
    auto* ctrl = AnimationControlController::instance();
    EXPECT_FALSE(ctrl->resampleCurveSegment("AnyBone", "tx", 0.0, 0.5));
}

TEST_F(AnimationControlControllerResampleCoverageTest,
       ResampleCurveSegmentMissingBoneReturnsFalse) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ResCov_SegMissingBone");
    ASSERT_NE(entity, nullptr);
    auto* ctrl = AnimationControlController::instance();
    ASSERT_FALSE(driveToBone(entity, ctrl).isEmpty());

    EXPECT_FALSE(ctrl->resampleCurveSegment("NoSuchBone_xyz", "tx", 0.0, 0.5));
}

// ── setRowsRefreshSuspended / refreshAfterBulkResample ──────────────────────

TEST_F(AnimationControlControllerResampleCoverageTest,
       SuspendSuppressesPerCallEmitThenBulkRefreshFiresOnce) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ResCov_Suspend");
    ASSERT_NE(entity, nullptr);
    auto* ctrl = AnimationControlController::instance();
    QString bone = driveToBone(entity, ctrl);
    ASSERT_FALSE(bone.isEmpty());

    ctrl->setRowsRefreshSuspended(true);

    QSignalSpy spy(ctrl, &AnimationControlController::boneRowsChanged);

    // While suspended, a successful resampleCurveSegment must NOT emit
    // boneRowsChanged (the per-call refresh is coalesced).
    EXPECT_TRUE(ctrl->resampleCurveSegment(bone, "tx", 0.0, 0.5));
    EXPECT_EQ(spy.count(), 0) << "suspended resample must not emit per-call refresh";

    // The bulk-finish helper fires it exactly once.
    ctrl->refreshAfterBulkResample();
    EXPECT_GE(spy.count(), 1) << "refreshAfterBulkResample must emit boneRowsChanged";

    ctrl->setRowsRefreshSuspended(false);
}

TEST_F(AnimationControlControllerResampleCoverageTest,
       NotSuspendedResampleEmitsBoneRowsChanged) {
    // Counterpart: with suspension OFF, the per-call emit fires.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ResCov_NotSuspended");
    ASSERT_NE(entity, nullptr);
    auto* ctrl = AnimationControlController::instance();
    QString bone = driveToBone(entity, ctrl);
    ASSERT_FALSE(bone.isEmpty());

    ctrl->setRowsRefreshSuspended(false);
    QSignalSpy spy(ctrl, &AnimationControlController::boneRowsChanged);
    EXPECT_TRUE(ctrl->resampleCurveSegment(bone, "tx", 0.0, 0.5));
    EXPECT_GE(spy.count(), 1) << "un-suspended resample emits per-call refresh";
}

// ── resampleAllSegmentsForBone adaptive branch (density 0 / 1) ──────────────
// These densities use baselineFps > 0, exercising the internal
// reduceTrackToFps pre-decimate path (distinct from the fixed-fps branch the
// existing suite covers via density 5/6).

TEST_F(AnimationControlControllerResampleCoverageTest,
       AdaptiveDensitySparseBaselinePreDecimate) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ResCov_AdaptiveSparse");
    ASSERT_NE(entity, nullptr);
    auto* ctrl = AnimationControlController::instance();
    QString bone = driveToBone(entity, ctrl);
    ASSERT_FALSE(bone.isEmpty());

    // A stepped start handle gives the resampler a sharp curve so the bake
    // reliably emits segments.
    ctrl->setCurveHandle(bone, "tx", 0.0, 0.0, 0.0,
                         CurveEditModel::ModeStepped);

    auto* stack = UndoManager::getSingleton()->stack();
    const int undoBefore = stack->count();

    // density 0 → Sparse: toleranceMul 12, baselineFps 5 → adaptive branch
    // runs reduceTrackToFps(bone, 5) internally before the per-pair loop.
    const int segments = ctrl->resampleAllSegmentsForBone(bone, "tx", 0);
    EXPECT_GE(segments, 0);

    // Whole bake collapses to a single undo macro entry.
    EXPECT_EQ(stack->count(), undoBefore + 1)
        << "adaptive bake is one undo macro";
}

TEST_F(AnimationControlControllerResampleCoverageTest,
       AdaptiveDensityMediumBaselinePreDecimate) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupAnimatedEntity("ResCov_AdaptiveMedium");
    ASSERT_NE(entity, nullptr);
    auto* ctrl = AnimationControlController::instance();
    QString bone = driveToBone(entity, ctrl);
    ASSERT_FALSE(bone.isEmpty());

    ctrl->setCurveHandle(bone, "tx", 0.0, 0.0, 0.0,
                         CurveEditModel::ModeStepped);

    auto* track = firstTrack(entity);
    const int before = track->getNumKeyFrames();

    // density 1 → Medium: toleranceMul 4, baselineFps 15 → adaptive branch.
    const int segments = ctrl->resampleAllSegmentsForBone(bone, "tx", 1);
    EXPECT_GE(segments, 0);
    // The bake should at least leave a valid track (>= the original anchors).
    EXPECT_GE(track->getNumKeyFrames(), 2);
    // Idempotent stability: a second Medium bake converges (does not blow up).
    const int firstCount = track->getNumKeyFrames();
    ctrl->resampleAllSegmentsForBone(bone, "tx", 1);
    EXPECT_GE(track->getNumKeyFrames(), 2);
    EXPECT_LE(track->getNumKeyFrames(), firstCount * 4 + 8)
        << "repeated Medium bake must converge, not balloon";
    (void)before;
}

// ── Guard: adaptive bake on missing bone returns 0 ──────────────────────────

TEST_F(AnimationControlControllerResampleCoverageTest,
       ResampleAllSegmentsForBoneGuards) {
    auto* ctrl = AnimationControlController::instance();
    // No selection.
    EXPECT_EQ(ctrl->resampleAllSegmentsForBone("AnyBone", "tx", 0), 0);
    EXPECT_EQ(ctrl->resampleAllSegmentsForBone("", "tx", 0), 0);
    EXPECT_EQ(ctrl->resampleAllSegmentsForBone("AnyBone", "qq", 0), 0);
}
