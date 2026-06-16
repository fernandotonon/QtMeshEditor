#include "SkinWeights.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonArray>
#include <QThread>

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSubMesh.h>
#include <OgreSkeleton.h>

#include <atomic>
#include <string>

#include "Manager.h"
#include "TestHelpers.h"

// Coverage suite for the Ogre-backed SkinWeights paths
// (computeAndApply / the anonymous-namespace applyToEntity).
//
// The existing SkinWeights_test.cpp only drives the pure-data
// computeWeights() free function and hand-built report serialization;
// it never reaches the live-entity commit path. This suite builds a
// real skinned Ogre entity (createAnimatedTestEntity — shared vertex
// data, Root+Child skeleton, bone 1 weighted on all 3 verts) and a
// skeleton-less entity (createInMemoryTriangleMesh) to exercise:
//   - the null-entity / no-skeleton guards
//   - bind-pose bone-segment build (reset, _getDerivedPosition,
//     child-tail averaging, leaf head==tail fallback)
//   - the shared-vertex-data commit branch (mesh-level
//     get/clear/add/_compileBoneAssignments)
//   - replaceExisting = true (clear then re-add)
//   - replaceExisting = false (merge mode, alreadyWeighted skip)
//   - skipUnweightedBones (sparse bones[] + boneIdxToHandle remap)
//   - report totals aggregation + reportToJson/reportToText on a live
//     populated report.
//
// Distinct file name + distinct suite name (SkinWeightsOgreCoverageTest)
// so there is no ODR / duplicate-registration clash with the existing
// SkinWeightsTest suite.

namespace {

// Monotonic counter so each test uses a unique mesh / skeleton /
// entity name and we never collide in the Ogre ResourceManager across
// repeated runs within one process.
std::atomic<int> g_skinCoverageCounter{0};

std::string uniqueName(const char* prefix)
{
    return std::string(prefix) + "_swcov_"
         + std::to_string(g_skinCoverageCounter.fetch_add(1));
}

} // namespace

class SkinWeightsOgreCoverageTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        // The live commit path needs a real Ogre scene + GL context for
        // hardware buffer locking. Fail loudly (never GTEST_SKIP) per
        // project convention.
        ASSERT_TRUE(tryInitOgre())
            << "Ogre init failed — invalid CI/runtime environment";
        ASSERT_TRUE(canLoadMeshFiles())
            << "no GL context for hardware buffers";
        createStandardOgreMaterials();
    }
};

// ─── Guard: null entity ───────────────────────────────────────────────────────

TEST_F(SkinWeightsOgreCoverageTest, NullEntityReportsError)
{
    SkinWeightsReport report = SkinWeights::computeAndApply(nullptr);
    EXPECT_FALSE(report.applied);
    EXPECT_EQ(report.error, QStringLiteral("null entity"));
    EXPECT_TRUE(report.submeshes.isEmpty());
}

// ─── Guard: entity with no skeleton ───────────────────────────────────────────

TEST_F(SkinWeightsOgreCoverageTest, NoSkeletonReportsError)
{
    const std::string meshName = uniqueName("noskel");
    Ogre::MeshPtr mesh = createInMemoryTriangleMesh(meshName);
    ASSERT_TRUE(mesh);

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    const std::string entName = uniqueName("noskel_ent");
    Ogre::Entity* ent = sceneMgr->createEntity(entName, mesh);
    ASSERT_NE(ent, nullptr);

    SkinWeightsReport report = SkinWeights::computeAndApply(ent);
    EXPECT_FALSE(report.applied);
    EXPECT_TRUE(report.error.contains(QStringLiteral("no skeleton")))
        << "error was: " << report.error.toStdString();

    sceneMgr->destroyEntity(ent);
}

// ─── Full success path on a real skinned entity ────────────────────────────────

TEST_F(SkinWeightsOgreCoverageTest, ComputeAndApplySucceedsOnSkinnedEntity)
{
    const std::string name = uniqueName("hero");
    Ogre::Entity* ent = createAnimatedTestEntity(name);
    ASSERT_NE(ent, nullptr);

    // Default options: replaceExisting=true, InverseDistance.
    SkinWeightsReport report = SkinWeights::computeAndApply(ent);

    EXPECT_TRUE(report.applied) << "error: " << report.error.toStdString();
    EXPECT_TRUE(report.error.isEmpty());

    // mesh is "<name>_mesh", skeleton "<name>_skel".
    EXPECT_EQ(report.meshName, QString::fromStdString(name + "_mesh"));
    EXPECT_EQ(report.skeletonName, QString::fromStdString(name + "_skel"));

    // Root + Child = 2 bones.
    EXPECT_EQ(report.totalBones, 2);

    // 3 shared verts processed; committed assignments > 0.
    EXPECT_EQ(report.totalVerticesProcessed, 3);
    EXPECT_GT(report.totalAssignmentsAfter, 0);

    // Shared-vertex-data branch produces a single report entry with
    // submeshIndex == -1 (mesh-level shared data).
    ASSERT_EQ(report.submeshes.size(), 1);
    EXPECT_EQ(report.submeshes[0].submeshIndex, -1);
    EXPECT_EQ(report.submeshes[0].verticesProcessed, 3);
    EXPECT_GT(report.submeshes[0].boneAssignmentsAfter, 0);
}

// ─── Bind-pose bone-segment build (Root→Child segment, Child leaf) ─────────────

TEST_F(SkinWeightsOgreCoverageTest, BindPoseSegmentBuildProducesValidWeights)
{
    // createInMemorySkeletonMesh: Root at (0,0,0) with Child at (0,1,0)
    // (Root gets a real head→tail segment), Child is a leaf (head==tail
    // → point distance fallback). Drive it through a fresh entity.
    const std::string meshName = uniqueName("bindpose");
    Ogre::MeshPtr mesh = createInMemorySkeletonMesh(meshName);
    ASSERT_TRUE(mesh);

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    const std::string entName = uniqueName("bindpose_ent");
    Ogre::Entity* ent = sceneMgr->createEntity(entName, mesh);
    ASSERT_NE(ent, nullptr);

    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = 2;
    opts.maxInfluenceDistance   = 0;  // consider both bones
    SkinWeightsReport report = SkinWeights::computeAndApply(ent, opts);

    EXPECT_TRUE(report.applied) << "error: " << report.error.toStdString();
    EXPECT_EQ(report.totalBones, 2);
    EXPECT_GT(report.totalAssignmentsAfter, 0);

    // Every committed assignment must reference a real bone handle
    // (0 = Root, 1 = Child) and a real vertex index (0..2).
    const auto& ba = mesh->getBoneAssignments();
    ASSERT_FALSE(ba.empty());
    for (const auto& kv : ba) {
        EXPECT_LT(kv.second.boneIndex, 2u);
        EXPECT_LT(kv.second.vertexIndex, 3u);
        EXPECT_GT(kv.second.weight, 0.0f);
    }

    sceneMgr->destroyEntity(ent);
}

// ─── replaceExisting = true clears then re-adds ───────────────────────────────

TEST_F(SkinWeightsOgreCoverageTest, ReplaceExistingClearsAndRecomputes)
{
    const std::string name = uniqueName("replace");
    Ogre::Entity* ent = createAnimatedTestEntity(name);
    ASSERT_NE(ent, nullptr);
    Ogre::MeshPtr mesh = ent->getMesh();

    // Helper mesh ships 3 pre-existing assignments (one per vertex on
    // bone 1).
    const int before = static_cast<int>(mesh->getBoneAssignments().size());
    EXPECT_EQ(before, 3);

    SkinWeightsOptions opts;
    opts.replaceExisting = true;
    opts.maxInfluenceDistance = 0;
    SkinWeightsReport report = SkinWeights::computeAndApply(ent, opts);

    EXPECT_TRUE(report.applied) << "error: " << report.error.toStdString();
    ASSERT_EQ(report.submeshes.size(), 1);
    EXPECT_EQ(report.submeshes[0].boneAssignmentsBefore, 3);
    EXPECT_GT(report.submeshes[0].boneAssignmentsAfter, 0);
    EXPECT_GT(static_cast<int>(mesh->getBoneAssignments().size()), 0);
}

// ─── replaceExisting = false merge mode (alreadyWeighted skip) ────────────────

TEST_F(SkinWeightsOgreCoverageTest, MergeModePreservesExistingAssignments)
{
    const std::string name = uniqueName("merge");
    Ogre::Entity* ent = createAnimatedTestEntity(name);
    ASSERT_NE(ent, nullptr);
    Ogre::MeshPtr mesh = ent->getMesh();

    // All 3 verts are pre-weighted in the helper, so merge mode should
    // skip every vertex (alreadyWeighted) and leave the count unchanged.
    const int before = static_cast<int>(mesh->getBoneAssignments().size());
    EXPECT_EQ(before, 3);

    SkinWeightsOptions opts;
    opts.replaceExisting = false;     // merge — fill only unweighted verts
    opts.maxInfluenceDistance = 0;
    SkinWeightsReport report = SkinWeights::computeAndApply(ent, opts);

    EXPECT_TRUE(report.applied) << "error: " << report.error.toStdString();
    ASSERT_EQ(report.submeshes.size(), 1);
    EXPECT_EQ(report.submeshes[0].boneAssignmentsBefore, 3);

    // Every vertex was already weighted → nothing new appended; the
    // existing single-influence (bone 1) assignments survive unchanged.
    const auto& ba = mesh->getBoneAssignments();
    EXPECT_EQ(static_cast<int>(ba.size()), 3);
    for (const auto& kv : ba) {
        EXPECT_EQ(kv.second.boneIndex, 1u);
        EXPECT_FLOAT_EQ(kv.second.weight, 1.0f);
    }
}

// ─── skipUnweightedBones builds a sparse bone list + handle remap ─────────────

TEST_F(SkinWeightsOgreCoverageTest, SkipUnweightedBonesRemapsHandles)
{
    const std::string name = uniqueName("skipbones");
    Ogre::Entity* ent = createAnimatedTestEntity(name);
    ASSERT_NE(ent, nullptr);
    Ogre::MeshPtr mesh = ent->getMesh();

    // Only bone 1 (Child) carries weights in the helper mesh, so with
    // skipUnweightedBones the segment list collapses to just bone 1 and
    // the boneIdxToHandle remap must translate every committed
    // assignment back to handle 1.
    SkinWeightsOptions opts;
    opts.skipUnweightedBones = true;
    opts.maxInfluenceDistance = 0;
    SkinWeightsReport report = SkinWeights::computeAndApply(ent, opts);

    EXPECT_TRUE(report.applied) << "error: " << report.error.toStdString();
    EXPECT_EQ(report.totalBones, 2);   // total reflects skeleton, not filtered set
    EXPECT_GT(report.totalAssignmentsAfter, 0);

    const auto& ba = mesh->getBoneAssignments();
    ASSERT_FALSE(ba.empty());
    for (const auto& kv : ba) {
        // The only weighted bone is handle 1; the remap must map back to
        // it (not the sparse-array index 0).
        EXPECT_EQ(kv.second.boneIndex, 1u)
            << "boneIdxToHandle remap did not translate sparse index → real handle";
    }
}

// ─── verticesWithMaxInfluences accounting + totals aggregation ────────────────

TEST_F(SkinWeightsOgreCoverageTest, MaxInfluenceAccountingAndTotals)
{
    const std::string name = uniqueName("maxinfl");
    Ogre::Entity* ent = createAnimatedTestEntity(name);
    ASSERT_NE(ent, nullptr);

    // Only 2 bones exist; request 2 influences/vertex so every vertex
    // that gets both bones lands at maxK and increments
    // verticesWithMaxInfluences.
    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = 2;
    opts.maxInfluenceDistance   = 0;  // no cap → both bones in range
    SkinWeightsReport report = SkinWeights::computeAndApply(ent, opts);

    EXPECT_TRUE(report.applied) << "error: " << report.error.toStdString();
    ASSERT_EQ(report.submeshes.size(), 1);

    const SkinWeightsSubmeshReport& sub = report.submeshes[0];
    // verticesWithMaxInfluences is a subset of the verts processed.
    EXPECT_GE(sub.verticesWithMaxInfluences, 0);
    EXPECT_LE(sub.verticesWithMaxInfluences, sub.verticesProcessed);

    // Report totals must equal the single shared-data submesh entry's
    // contribution (aggregation correctness).
    EXPECT_EQ(report.totalVerticesProcessed, sub.verticesProcessed);
    EXPECT_EQ(report.totalAssignmentsBefore, sub.boneAssignmentsBefore);
    EXPECT_EQ(report.totalAssignmentsAfter, sub.boneAssignmentsAfter);
    EXPECT_GT(report.totalAssignmentsAfter, 0);
}

// ─── Live populated report feeds reportToJson / reportToText ──────────────────

TEST_F(SkinWeightsOgreCoverageTest, LiveReportSerializesToJsonAndText)
{
    const std::string name = uniqueName("serialize");
    Ogre::Entity* ent = createAnimatedTestEntity(name);
    ASSERT_NE(ent, nullptr);

    SkinWeightsOptions opts;
    opts.maxInfluenceDistance = 0;
    SkinWeightsReport report = SkinWeights::computeAndApply(ent, opts);
    ASSERT_TRUE(report.applied) << "error: " << report.error.toStdString();

    // JSON — populated (applied=true) branch on a real result.
    const QJsonObject json = SkinWeights::reportToJson(report);
    EXPECT_TRUE(json["applied"].toBool());
    EXPECT_EQ(json["meshName"].toString(),
              QString::fromStdString(name + "_mesh"));
    EXPECT_EQ(json["skeletonName"].toString(),
              QString::fromStdString(name + "_skel"));
    EXPECT_EQ(json["totalBones"].toInt(), 2);
    EXPECT_EQ(json["totalVerticesProcessed"].toInt(),
              report.totalVerticesProcessed);
    EXPECT_GT(json["totalAssignmentsAfter"].toInt(), 0);
    // applied=true → no error key.
    EXPECT_FALSE(json.contains("error"));
    ASSERT_TRUE(json["submeshes"].isArray());
    const QJsonArray subs = json["submeshes"].toArray();
    ASSERT_EQ(subs.size(), 1);
    EXPECT_EQ(subs[0].toObject()["submeshIndex"].toInt(), -1);

    // Text — same live result.
    const QString txt = SkinWeights::reportToText(report);
    EXPECT_TRUE(txt.contains(QString::fromStdString(name + "_mesh")));
    EXPECT_TRUE(txt.contains(QString::fromStdString(name + "_skel")));
    EXPECT_TRUE(txt.contains(QStringLiteral("Skin Weights")));
    EXPECT_FALSE(txt.contains(QStringLiteral("Error:")));
}
