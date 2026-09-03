/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — SkinWeightController tests (Skel Slice D, issue #558)

The brush/op MATHS is covered pure-data in WeightPaintOps_test. These cases
cover the CONTROLLER contract: setter clamping, safe degradation with no
session/bone, bone locking by name, and — the ones that matter — that a weight
edit actually reaches the mesh's VertexBoneAssignment lists and round-trips
through undo.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>

#include "SkinWeightController.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "WeightPaintOps.h"

#include <OgreMesh.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMeshManager.h>
#include <OgreSubMesh.h>

#include <QApplication>

// --- settings / degradation (no Ogre scene needed) -------------------------

TEST(SkinWeightControllerTest, SettersClampAndPersist) {
    auto* c = SkinWeightController::instance();
    ASSERT_NE(c, nullptr);

    c->setBrushMode(1);
    EXPECT_EQ(c->brushMode(), 1);
    c->setBrushMode(99);
    EXPECT_EQ(c->brushMode(), 2) << "out-of-range mode must clamp into the enum";
    c->setBrushMode(-5);
    EXPECT_EQ(c->brushMode(), 0);

    c->setMaxInfluences(4);
    EXPECT_EQ(c->maxInfluences(), 4);
    c->setMaxInfluences(99);
    EXPECT_LE(c->maxInfluences(), 8) << "a row holds at most 8 influences";
    c->setMaxInfluences(0);
    EXPECT_GE(c->maxInfluences(), 1);
}

TEST(SkinWeightControllerTest, OpsDegradeSafelyWithoutASession) {
    auto* c = SkinWeightController::instance();
    ASSERT_NE(c, nullptr);
    // ESTABLISH the precondition rather than assuming it: ensureSession() finds
    // its entity through the SelectionSet, which an earlier test in the same
    // process may have left populated (BoneWeightOverlay_test clears it for the
    // same reason). Without this the ops legitimately succeed and this test
    // fails only when run after a scene test — a false alarm, not a bug.
    if (auto* sel = SelectionSet::getSingletonPtr()) sel->clear();
    c->setWeightPaintEnabled(false);

    // Every Q_INVOKABLE is reachable from QML at any time, so each must refuse
    // cleanly rather than dereferencing a null entity/mesh.
    EXPECT_FALSE(c->normalizeAll());
    EXPECT_FALSE(c->smoothAll(2));
    EXPECT_FALSE(c->limitInfluencesAll(4));
    EXPECT_FALSE(c->mirrorAll(0, 0.001));
    EXPECT_FALSE(c->fillConnectedAtHover(0));
    EXPECT_FALSE(c->setVertexWeight(0, QStringLiteral("Nope"), 0.5));
    EXPECT_TRUE(c->vertexWeights(0).isEmpty());
    EXPECT_TRUE(c->vertexWeights(-1).isEmpty());
    EXPECT_FALSE(c->strokeActive());
}

TEST(SkinWeightControllerTest, BoneLocksAreTrackedByName) {
    auto* c = SkinWeightController::instance();
    ASSERT_NE(c, nullptr);
    // Names, not handles: a lock must survive a skeleton rebind, which
    // renumbers handles.
    EXPECT_FALSE(c->isBoneLocked(QStringLiteral("Child")));
    c->setBoneLocked(QStringLiteral("Child"), true);
    EXPECT_TRUE(c->isBoneLocked(QStringLiteral("Child")));
    EXPECT_TRUE(c->lockedBoneNames().contains(QStringLiteral("Child")));

    c->setBoneLocked(QStringLiteral("Child"), false);
    EXPECT_FALSE(c->isBoneLocked(QStringLiteral("Child")));
    EXPECT_FALSE(c->lockedBoneNames().contains(QStringLiteral("Child")));

    // An empty name is not a bone.
    c->setBoneLocked(QString(), true);
    EXPECT_FALSE(c->lockedBoneNames().contains(QString()));
}

// --- write-back + undo (needs Ogre) ---------------------------------------

class SkinWeightControllerSceneTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_NE(qobject_cast<QApplication*>(QCoreApplication::instance()), nullptr);
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed — invalid test environment";
        ASSERT_TRUE(canLoadMeshFiles()) << "no GL context";
        createStandardOgreMaterials();
    }
    void TearDown() override
    {
        SkinWeightController::instance()->setWeightPaintEnabled(false);
    }
    static std::string uniqueName(const char* prefix)
    {
        static int counter = 0;
        return std::string(prefix) + std::to_string(++counter);
    }
};

TEST_F(SkinWeightControllerSceneTest, SnapshotRoundTripsBoneAssignments) {
    // The undo path depends on capture/restore being exact. The fixture weights
    // all three verts 1.0 to bone handle 1.
    Ogre::MeshPtr mesh = createInMemorySkeletonMesh(uniqueName("swcSnap"));
    ASSERT_TRUE(mesh);
    const size_t before = mesh->getBoneAssignments().size();
    ASSERT_GT(before, 0u);

    const auto snap = SkinWeightController::captureSnapshot(mesh.get());
    ASSERT_FALSE(snap.empty());

    // Wipe, then restore.
    mesh->clearBoneAssignments();
    EXPECT_EQ(mesh->getBoneAssignments().size(), 0u);
    SkinWeightController::restoreSnapshot(mesh.get(), snap);
    EXPECT_EQ(mesh->getBoneAssignments().size(), before)
        << "restore must reinstate every assignment";

    // And the values, not just the count.
    for (const auto& kv : mesh->getBoneAssignments()) {
        EXPECT_EQ(kv.second.boneIndex, 1);
        EXPECT_NEAR(kv.second.weight, 1.0f, 1e-6f);
    }
}

TEST_F(SkinWeightControllerSceneTest, SnapshotOfASharedVertexMeshUsesTheMeshLevelOwner) {
    // The fixture's submesh uses shared vertices, so the assignments live on the
    // MESH, not the submesh. Capturing the wrong owner would silently snapshot
    // an empty list and make undo a no-op.
    Ogre::MeshPtr mesh = createInMemorySkeletonMesh(uniqueName("swcShared"));
    ASSERT_TRUE(mesh);
    ASSERT_GT(mesh->getNumSubMeshes(), 0);
    ASSERT_TRUE(mesh->getSubMesh(0)->useSharedVertices);

    const auto snap = SkinWeightController::captureSnapshot(mesh.get());
    ASSERT_EQ(snap.size(), 1u) << "one owner: the shared mesh-level list";
    EXPECT_EQ(snap[0].submeshIndex, -1) << "-1 marks the mesh-level owner";
    EXPECT_GT(snap[0].assignments.size(), 0u);
}

TEST_F(SkinWeightControllerSceneTest, RestoreIsIdempotent) {
    Ogre::MeshPtr mesh = createInMemorySkeletonMesh(uniqueName("swcIdem"));
    ASSERT_TRUE(mesh);
    const auto snap = SkinWeightController::captureSnapshot(mesh.get());
    const size_t n = mesh->getBoneAssignments().size();

    // Undo/redo can replay the same snapshot repeatedly; it must not accumulate
    // duplicate assignments each time.
    SkinWeightController::restoreSnapshot(mesh.get(), snap);
    SkinWeightController::restoreSnapshot(mesh.get(), snap);
    EXPECT_EQ(mesh->getBoneAssignments().size(), n)
        << "repeated restores must not duplicate assignments";
}

TEST_F(SkinWeightControllerSceneTest, RestoreToleratesAMissingSubmesh) {
    // A snapshot can outlive the submesh it described (mesh rebuilt between undo
    // steps). Restore must bounds-check rather than crash.
    Ogre::MeshPtr mesh = createInMemorySkeletonMesh(uniqueName("swcMissing"));
    ASSERT_TRUE(mesh);
    std::vector<SkinWeightController::OwnerSnapshot> bogus;
    SkinWeightController::OwnerSnapshot o;
    o.submeshIndex = 99;                 // does not exist
    bogus.push_back(o);
    SkinWeightController::restoreSnapshot(mesh.get(), bogus);   // must not crash
    SkinWeightController::restoreSnapshot(nullptr, bogus);      // nor on a null mesh
    SUCCEED();
}

// A mesh can retain a non-null sharedVertexData while every submesh uses
// PRIVATE vertex data. The shared block is then not an owner, and every walk
// over owners must agree on that: SkinEvaluate::extract gates on it, so a
// write-back that included the unused shared block would consume the first
// private submesh's rows as the shared owner, advance its base, and then read
// shifted/exhausted rows for every later owner — clearing valid assignments
// across the mesh. Reported in review on PR #973.
//
// captureSnapshot shares the gate with flushToMesh, so asserting on the
// snapshot's owner list pins the walk without needing a live paint session.
TEST_F(SkinWeightControllerSceneTest, UnusedSharedVertexDataIsNotAnOwner) {
    const std::string name = uniqueName("swcUnusedShared");
    Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_TRUE(mesh);

    // A shared block exists...
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    mesh->sharedVertexData->vertexCount = 3;
    auto sharedBuf = Ogre::HardwareBufferManager::getSingleton()
        .createVertexBuffer(decl->getVertexSize(0), 3,
                            Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, sharedBuf);

    // ...but the only submesh uses its OWN vertex data.
    auto* sub = mesh->createSubMesh();
    sub->useSharedVertices = false;
    sub->vertexData = new Ogre::VertexData();
    auto* sdecl = sub->vertexData->vertexDeclaration;
    sdecl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    sub->vertexData->vertexCount = 3;
    auto subBuf = Ogre::HardwareBufferManager::getSingleton()
        .createVertexBuffer(sdecl->getVertexSize(0), 3,
                            Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    sub->vertexData->vertexBufferBinding->setBinding(0, subBuf);

    Ogre::VertexBoneAssignment vba;
    vba.vertexIndex = 0; vba.boneIndex = 1; vba.weight = 1.0f;
    sub->addBoneAssignment(vba);

    const auto snap = SkinWeightController::captureSnapshot(mesh.get());
    ASSERT_EQ(snap.size(), 1u)
        << "an unused shared block must NOT appear as an owner";
    EXPECT_EQ(snap[0].submeshIndex, 0)
        << "the single owner is the private submesh, not the mesh-level list";

    Ogre::MeshManager::getSingleton().remove(mesh);
}
