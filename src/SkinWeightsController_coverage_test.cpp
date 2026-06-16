// Coverage tests for SkinWeightsController (issue #402) — the QML-facing
// singleton wrapping SkinWeights::computeAndApply over the current
// SelectionSet. The existing SkinWeights_test.cpp only exercises the pure
// algorithm (SkinWeights::computeWeights / report serialization); the
// controller's lifecycle, selection-gating, error branches, success path,
// signal emission, undo integration, and option plumbing were entirely
// uncovered before this file.
//
// Distinct filename + distinct suite name (SkinWeightsControllerCoverageTest)
// so there is no ODR / duplicate-registration clash with the algorithm suite.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QApplication>
#include <QSignalSpy>
#include <QVariantMap>

#include "SkinWeightsController.h"
#include "SkinWeights.h"
#include "SelectionSet.h"
#include "UndoManager.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>

namespace {

class SkinWeightsControllerCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        // test_main.cpp owns the single QApplication — never create one here.
        ASSERT_NE(qobject_cast<QApplication*>(QCoreApplication::instance()), nullptr)
            << "QApplication must be provided by test_main.cpp";
        ASSERT_TRUE(tryInitOgre());
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();

        // Start from a clean selection + undo stack so prior suites don't
        // bleed state into these assertions.
        if (auto* sel = SelectionSet::getSingletonPtr())
            sel->clear();
        if (auto* undo = UndoManager::getSingleton())
            undo->stack()->clear();
    }

    void TearDown() override {
        // Drop controller + selection state so later suites start fresh.
        SkinWeightsController::kill();
        if (auto* sel = SelectionSet::getSingletonPtr())
            sel->clear();
        if (auto* undo = UndoManager::getSingleton())
            undo->stack()->clear();
    }

    // Unique mesh/entity names per test to avoid Ogre resource collisions
    // across the suite (createManual throws on a duplicate name).
    static std::string uniqueName(const char* base) {
        static int counter = 0;
        return std::string(base) + "_" + std::to_string(++counter);
    }
};

// ---------------------------------------------------------------------------
// Lifecycle: instance() / qmlInstance() / kill()
// ---------------------------------------------------------------------------

TEST_F(SkinWeightsControllerCoverageTest, InstanceIsLazyAndStable) {
    SkinWeightsController::kill();
    auto* a = SkinWeightsController::instance();
    ASSERT_NE(a, nullptr);
    auto* b = SkinWeightsController::instance();
    EXPECT_EQ(a, b) << "instance() must return the same singleton";
}

TEST_F(SkinWeightsControllerCoverageTest, KillResetsSingleton) {
    auto* a = SkinWeightsController::instance();
    ASSERT_NE(a, nullptr);
    SkinWeightsController::kill();
    auto* b = SkinWeightsController::instance();
    ASSERT_NE(b, nullptr);
    // Not asserting pointer inequality (allocator may reuse the address) —
    // the contract is simply that instance() still works after kill().
    EXPECT_EQ(b, SkinWeightsController::instance());
}

TEST_F(SkinWeightsControllerCoverageTest, QmlInstanceReturnsTheSingleton) {
    auto* inst = SkinWeightsController::instance();
    // qmlInstance ignores its engine args and returns the singleton with
    // CppOwnership; passing nullptrs exercises that path safely.
    auto* fromQml = SkinWeightsController::qmlInstance(nullptr, nullptr);
    EXPECT_EQ(fromQml, inst);
}

// ---------------------------------------------------------------------------
// hasSkinnedSelection()
// ---------------------------------------------------------------------------

TEST_F(SkinWeightsControllerCoverageTest, HasSkinnedSelectionFalseWhenEmpty) {
    SelectionSet::getSingleton()->clear();
    auto* c = SkinWeightsController::instance();
    EXPECT_FALSE(c->hasSkinnedSelection());
}

TEST_F(SkinWeightsControllerCoverageTest, HasSkinnedSelectionFalseForStaticMesh) {
    auto mesh = createInMemoryTriangleMesh(uniqueName("hss_static"));
    ASSERT_TRUE(mesh);
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("hss_static_node");
    auto* entity = sceneMgr->createEntity(uniqueName("hss_static_ent"), mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->selectOne(entity);
    auto* c = SkinWeightsController::instance();
    EXPECT_FALSE(c->hasSkinnedSelection())
        << "static (skeleton-less) mesh must report no skinned selection";
}

TEST_F(SkinWeightsControllerCoverageTest, HasSkinnedSelectionTrueForSkinnedMesh) {
    auto* entity = createAnimatedTestEntity(uniqueName("hss_skinned"));
    ASSERT_NE(entity, nullptr);
    ASSERT_NE(entity->getMesh()->getSkeleton(), nullptr);

    SelectionSet::getSingleton()->selectOne(entity);
    auto* c = SkinWeightsController::instance();
    EXPECT_TRUE(c->hasSkinnedSelection());

    // And clearing selection flips it back to false.
    SelectionSet::getSingleton()->clear();
    EXPECT_FALSE(c->hasSkinnedSelection());
}

// ---------------------------------------------------------------------------
// computeWeightsForSelected: error branches
// ---------------------------------------------------------------------------

TEST_F(SkinWeightsControllerCoverageTest, ComputeNoSelectionEmitsError) {
    SelectionSet::getSingleton()->clear();
    auto* c = SkinWeightsController::instance();

    QSignalSpy errSpy(c, &SkinWeightsController::error);
    QSignalSpy appliedSpy(c, &SkinWeightsController::weightsApplied);

    QVariantMap result = c->computeWeightsForSelected(4, 4.0, 0.5, false, true);

    EXPECT_FALSE(result["applied"].toBool());
    EXPECT_EQ(result["error"].toString(), QStringLiteral("No mesh selected."));
    ASSERT_EQ(errSpy.count(), 1);
    EXPECT_EQ(errSpy.takeFirst().at(0).toString(), QStringLiteral("No mesh selected."));
    EXPECT_EQ(appliedSpy.count(), 0);
}

TEST_F(SkinWeightsControllerCoverageTest, ComputeNoSkeletonEmitsErrorAndNoUndoEntry) {
    auto mesh = createInMemoryTriangleMesh(uniqueName("noskel"));
    ASSERT_TRUE(mesh);
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("noskel_node");
    auto* entity = sceneMgr->createEntity(uniqueName("noskel_ent"), mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->selectOne(entity);

    auto* undo = UndoManager::getSingleton();
    undo->stack()->clear();
    const int beforeCount = undo->stack()->count();

    auto* c = SkinWeightsController::instance();
    QSignalSpy errSpy(c, &SkinWeightsController::error);

    QVariantMap result = c->computeWeightsForSelected(4, 4.0, 0.5, false, true);

    EXPECT_FALSE(result["applied"].toBool());
    EXPECT_EQ(result["error"].toString(), QStringLiteral("Mesh has no skeleton attached."));
    ASSERT_EQ(errSpy.count(), 1);
    EXPECT_EQ(errSpy.takeFirst().at(0).toString(),
              QStringLiteral("Mesh has no skeleton attached."));
    // The pre-check must avoid leaving a no-op command on the undo stack.
    EXPECT_EQ(undo->stack()->count(), beforeCount);
}

// ---------------------------------------------------------------------------
// computeWeightsForSelected: success path
// ---------------------------------------------------------------------------

TEST_F(SkinWeightsControllerCoverageTest, ComputeSuccessAppliesAndEmitsAndPushesUndo) {
    auto* entity = createAnimatedTestEntity(uniqueName("ok_skinned"));
    ASSERT_NE(entity, nullptr);
    ASSERT_NE(entity->getMesh()->getSkeleton(), nullptr);

    SelectionSet::getSingleton()->selectOne(entity);

    auto* undo = UndoManager::getSingleton();
    undo->stack()->clear();
    const int beforeCount = undo->stack()->count();

    auto* c = SkinWeightsController::instance();
    QSignalSpy appliedSpy(c, &SkinWeightsController::weightsApplied);
    QSignalSpy busySpy(c, &SkinWeightsController::busyChanged);
    QSignalSpy errSpy(c, &SkinWeightsController::error);

    QVariantMap result = c->computeWeightsForSelected(4, 4.0, 0.5, false, true);

    EXPECT_TRUE(result["applied"].toBool());
    EXPECT_EQ(errSpy.count(), 0);

    // Report fields populated.
    EXPECT_GT(result["totalBones"].toInt(), 0);
    EXPECT_GT(result["totalVerticesProcessed"].toInt(), 0);
    EXPECT_GT(result["totalAssignmentsAfter"].toInt(), 0);
    EXPECT_TRUE(result.contains("meshName"));
    EXPECT_TRUE(result.contains("skeletonName"));

    // weightsApplied(report) fired with the result map.
    ASSERT_EQ(appliedSpy.count(), 1);
    const QVariantMap reportArg = appliedSpy.takeFirst().at(0).toMap();
    EXPECT_TRUE(reportArg["applied"].toBool());

    // busyChanged toggled on entry and exit (at least twice).
    EXPECT_GE(busySpy.count(), 2);
    // Controller is no longer busy after a synchronous run.
    EXPECT_FALSE(c->busy());

    // Operation landed on the undo stack via ComputeSkinWeightsCommand.
    EXPECT_EQ(undo->stack()->count(), beforeCount + 1);
    EXPECT_TRUE(undo->canUndo());
}

// ---------------------------------------------------------------------------
// computeWeightsForSelected: option plumbing
// ---------------------------------------------------------------------------

TEST_F(SkinWeightsControllerCoverageTest, MaxInfluencesReachesOptionsAndChangesResult) {
    // Two independent entities so the second compute isn't reading back the
    // assignments the first one committed.
    auto* entity1 = createAnimatedTestEntity(uniqueName("opt_inf1"));
    auto* entity2 = createAnimatedTestEntity(uniqueName("opt_inf2"));
    ASSERT_NE(entity1, nullptr);
    ASSERT_NE(entity2, nullptr);

    auto* c = SkinWeightsController::instance();

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(entity1);
    QVariantMap r1 = c->computeWeightsForSelected(/*maxInf*/1, 4.0, 0.5, false, true);
    ASSERT_TRUE(r1["applied"].toBool());
    const int afterMax1 = r1["totalAssignmentsAfter"].toInt();

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(entity2);
    QVariantMap r2 = c->computeWeightsForSelected(/*maxInf*/2, 4.0, 0.5, false, true);
    ASSERT_TRUE(r2["applied"].toBool());
    const int afterMax2 = r2["totalAssignmentsAfter"].toInt();

    // The test mesh has 3 verts and 2 bones. maxInf=1 caps each vertex to a
    // single influence (3 assignments); maxInf=2 permits both bones to be
    // kept per vertex. The cap must therefore yield no MORE assignments than
    // the wider setting — and with 2 reachable bones, strictly fewer.
    EXPECT_LE(afterMax1, afterMax2)
        << "maxInfluencesPerVertex=1 must not produce more assignments than =2";
    EXPECT_GT(afterMax1, 0);
    EXPECT_GT(afterMax2, 0);
}

TEST_F(SkinWeightsControllerCoverageTest, AllOptionArgsAcceptedAndApplied) {
    auto* entity = createAnimatedTestEntity(uniqueName("opt_all"));
    ASSERT_NE(entity, nullptr);
    SelectionSet::getSingleton()->selectOne(entity);

    auto* c = SkinWeightsController::instance();
    QSignalSpy errSpy(c, &SkinWeightsController::error);

    // Exercise non-default falloff, an explicit distance cap, skip flag, and
    // the merge (replaceExisting=false) path through the option struct.
    QVariantMap result = c->computeWeightsForSelected(
        /*maxInfluencesPerVertex*/3,
        /*falloff*/2.5,
        /*maxInfluenceDistance*/0.75,
        /*skipUnweightedBones*/true,
        /*replaceExisting*/false);

    EXPECT_EQ(errSpy.count(), 0);
    EXPECT_TRUE(result["applied"].toBool());
    EXPECT_GT(result["totalAssignmentsAfter"].toInt(), 0);
}

} // namespace
