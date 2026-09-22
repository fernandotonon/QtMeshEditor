// LatticeController + LatticeCommands tests.
//
// The pure deformation math is covered in LatticeDeformer_test.cpp; these
// exercise the Ogre adapter: a live session on a real entity (rest capture,
// live deform + GPU write-back, apply/cancel, undo of the bake), plus the
// no-scene error branches of the commands.

#include <gtest/gtest.h>

#include "LatticeController.h"
#include "LatticeDeformer.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "UndoManager.h"
#include "EditableMesh.h"
#include "TestHelpers.h"
#include "commands/LatticeCommands.h"

#include <OgreEntity.h>
#include <OgreMeshManager.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>


namespace {

std::vector<Ogre::Vector3> readPositions(Ogre::Entity* entity)
{
    EditableMesh mesh;
    std::vector<Ogre::Vector3> out;
    if (!mesh.loadFromEntity(entity)) return out;
    for (const auto& sm : mesh.subMeshes())
        for (const auto& v : sm.vertices) out.push_back(v.position);
    return out;
}

void expectNear(const Ogre::Vector3& a, const Ogre::Vector3& b, float eps = 1e-4f)
{
    EXPECT_NEAR(a.x, b.x, eps);
    EXPECT_NEAR(a.y, b.y, eps);
    EXPECT_NEAR(a.z, b.z, eps);
}

struct LatticeSceneFixture {
    Ogre::SceneManager* scene = nullptr;
    Ogre::MeshPtr mesh;
    Ogre::Entity* entity = nullptr;
    Ogre::SceneNode* node = nullptr;

    bool setup(const QString& tag)
    {
        if (!tryInitOgre()) return false;
        auto* mgr = Manager::getSingleton();
        if (!mgr) return false;
        scene = mgr->getSceneMgr();
        if (!scene) return false;
        mesh = createInMemoryTriangleMesh(("Lattice_Mesh_" + tag).toStdString());
        entity = scene->createEntity(("Lattice_Entity_" + tag).toStdString(), mesh->getName());
        // NAMED node: Manager::getEntities() skips unnamed ("forbidden") nodes,
        // so an unnamed fixture node would hide the entity from MCP-style lookups.
        node = scene->getRootSceneNode()->createChildSceneNode(("Lattice_Node_" + tag).toStdString());
        node->attachObject(entity);
        SelectionSet::getSingleton()->clear();
        SelectionSet::getSingleton()->append(entity);
        return true;
    }

    void teardown()
    {
        if (auto* lat = LatticeController::instance(); lat->sessionActive()) lat->cancelSession();
        SelectionSet::getSingleton()->clear();
        if (scene && entity) {
            node->detachObject(entity);
            scene->destroyEntity(entity);
            scene->getRootSceneNode()->removeAndDestroyChild(node);
        }
        if (mesh) Ogre::MeshManager::getSingleton().remove(mesh->getHandle());
        entity = nullptr; node = nullptr; mesh.reset();
    }
};

class LatticeControllerFixture : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(tryInitOgre()) << "Ogre init / render window required"; }
    void TearDown() override { m_fix.teardown(); }
    LatticeSceneFixture m_fix;
};

} // namespace

// ---------------------------------------------------------------------------
// Command error branches (no scene needed)
// ---------------------------------------------------------------------------

TEST(LatticeCommands, WritePositionsToMissingEntityFailsWithReason)
{
    QString err;
    EXPECT_FALSE(LatticeCmd::writePositionsToEntity("__no_such_lattice_entity__", {}, nullptr, true, &err));
    EXPECT_FALSE(err.isEmpty());
}

TEST(LatticeCommands, ApplyCommandSkipsFirstRedoWhenAlreadyApplied)
{
    LatticeApplyCommand cmd("__no_such_lattice_entity__", {}, {}, {}, QJsonObject{}, /*alreadyApplied=*/true);
    cmd.redo(); // skipped — must NOT try to resolve the bogus entity
    EXPECT_TRUE(cmd.ok());
    cmd.undo(); // now it does, and the entity is missing
    EXPECT_FALSE(cmd.ok());
    EXPECT_FALSE(cmd.error().isEmpty());
    EXPECT_EQ(cmd.text(), QStringLiteral("Apply Lattice Deform"));
}

TEST(LatticeCommands, GridCommandIsANoOpWithoutASession)
{
    auto* lat = LatticeController::instance();
    ASSERT_FALSE(lat->sessionActive());
    LatticeGridCommand cmd("ghost", 1, QJsonObject{}, QJsonObject{}, "Move");
    cmd.redo();
    cmd.undo(); // nothing to restore into; must not crash
    EXPECT_FALSE(lat->sessionActive());
    lat->abandonSessionFor("ghost"); // also a no-op
}

TEST(LatticeControllerHeadless, SafeDegradationWithoutASession)
{
    auto* lat = LatticeController::instance();
    EXPECT_FALSE(lat->sessionActive());
    EXPECT_FALSE(lat->isDeformed());
    EXPECT_EQ(lat->pointCount(), 0);
    EXPECT_FALSE(lat->applySession());
    lat->cancelSession();
    lat->resetPoints();
    lat->moveSelectedPoints(1, 0, 0);
    EXPECT_FALSE(lat->setPoint(0, 0, 0, 0));
    EXPECT_TRUE(lat->pointPosition(0).isEmpty());
    QString err;
    EXPECT_FALSE(lat->setLatticeJson(QJsonObject{}, &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_FALSE(lat->saveLatticeToFile("/nonexistent/dir/x.json"));
    EXPECT_TRUE(lat->statusIsError());
}

TEST(LatticeControllerHeadless, ResolutionAndInterpolationClampOutsideASession)
{
    auto* lat = LatticeController::instance();
    lat->setResolution(1, 40, 5);
    EXPECT_EQ(lat->resolutionX(), 2);
    EXPECT_EQ(lat->resolutionY(), 16);
    EXPECT_EQ(lat->resolutionZ(), 5);
    lat->setInterpolation(7);
    EXPECT_EQ(lat->interpolation(), 2);
    lat->setInterpolation(1);
    lat->setResolution(3, 3, 3);
}

// ---------------------------------------------------------------------------
// Live session (GL-gated)
// ---------------------------------------------------------------------------

TEST_F(LatticeControllerFixture, BeginCapturesRestAndBoxesTheMesh)
{
    ASSERT_TRUE(m_fix.setup("begin"));
    auto* lat = LatticeController::instance();
    EXPECT_TRUE(lat->hasSelection());
    ASSERT_TRUE(lat->beginSession());
    EXPECT_TRUE(lat->sessionActive());
    EXPECT_EQ(lat->entityName().toStdString(), m_fix.entity->getName());
    EXPECT_EQ(lat->pointCount(), 27);
    EXPECT_FALSE(lat->isDeformed());

    // The rest box encloses every vertex (with padding) — the triangle spans
    // (0,0,0)-(1,1,0), so its z extent is flat and gets a sliver of thickness.
    const auto& g = lat->grid();
    EXPECT_LE(g.origin.x, 0.0f);
    EXPECT_LE(g.origin.y, 0.0f);
    EXPECT_GE(g.origin.x + g.size.x, 1.0f);
    EXPECT_GE(g.origin.y + g.size.y, 1.0f);
    EXPECT_GT(g.size.z, 0.0f);

    lat->cancelSession();
    EXPECT_FALSE(lat->sessionActive());
}

TEST_F(LatticeControllerFixture, MovingAPointDeformsTheGpuMeshAndCancelRestoresIt)
{
    ASSERT_TRUE(m_fix.setup("deform"));
    auto* lat = LatticeController::instance();
    const std::vector<Ogre::Vector3> rest = readPositions(m_fix.entity);
    ASSERT_EQ(rest.size(), 3u);

    lat->setResolution(2, 2, 2);
    lat->setInterpolation(0); // trilinear: moving the +X+Y face is exact for the (1,0,0)/(0,1,0) verts' shares
    ASSERT_TRUE(lat->beginSession());
    // Lift every control point by +2 in Y → the whole mesh translates by +2 Y.
    lat->selectAllPoints();
    EXPECT_EQ(lat->selectedPointCount(), 8);
    lat->moveSelectedPoints(0.0, 2.0, 0.0);
    EXPECT_TRUE(lat->isDeformed());
    lat->flushPendingDeform(); // the coalesced GPU write-back, run now

    const std::vector<Ogre::Vector3> moved = readPositions(m_fix.entity);
    ASSERT_EQ(moved.size(), 3u);
    for (size_t i = 0; i < 3; ++i) expectNear(moved[i], rest[i] + Ogre::Vector3(0, 2, 0));

    // Undo inside the session restores the control points AND the mesh.
    UndoManager::getSingleton()->undo();
    lat->flushPendingDeform();
    EXPECT_FALSE(lat->isDeformed());
    const std::vector<Ogre::Vector3> undone = readPositions(m_fix.entity);
    for (size_t i = 0; i < 3; ++i) expectNear(undone[i], rest[i]);
    UndoManager::getSingleton()->redo();
    lat->flushPendingDeform();
    EXPECT_TRUE(lat->isDeformed());

    lat->cancelSession();
    const std::vector<Ogre::Vector3> restored = readPositions(m_fix.entity);
    for (size_t i = 0; i < 3; ++i) expectNear(restored[i], rest[i]);

    // A command recorded in THAT session must not replay into a new one on
    // the same mesh (its rest shape is different; the old points would bend it).
    ASSERT_TRUE(lat->beginSession());
    UndoManager::getSingleton()->undo(); // the old drag's "before" (rest) — harmless either way
    UndoManager::getSingleton()->redo(); // the old drag's "after": must be ignored
    lat->flushPendingDeform();
    EXPECT_FALSE(lat->isDeformed());
    lat->cancelSession();
}

TEST_F(LatticeControllerFixture, CancelRestoresAuthoredNormalsVerbatim)
{
    ASSERT_TRUE(m_fix.setup("normals"));
    // Author a deliberately "wrong" normal set — a recompute would replace it
    // with the flat triangle normal (0,0,1), so survival proves no recompute.
    {
        EditableMesh mesh;
        ASSERT_TRUE(mesh.loadFromEntity(m_fix.entity));
        for (auto& sm : mesh.subMeshes())
            for (auto& v : sm.vertices) v.normal = Ogre::Vector3(1, 0, 0);
        ASSERT_TRUE(mesh.commitToEntity(m_fix.entity, /*recomputeNormals=*/false));
    }
    auto readNormals = [&]() {
        EditableMesh mesh;
        std::vector<Ogre::Vector3> out;
        if (mesh.loadFromEntity(m_fix.entity))
            for (const auto& sm : mesh.subMeshes()) for (const auto& v : sm.vertices) out.push_back(v.normal);
        return out;
    };
    for (const auto& n : readNormals()) expectNear(n, Ogre::Vector3(1, 0, 0));

    auto* lat = LatticeController::instance();
    lat->setResolution(2, 2, 2);
    ASSERT_TRUE(lat->beginSession());
    lat->flushPendingDeform();                               // adding a lattice alone must not touch normals
    for (const auto& n : readNormals()) expectNear(n, Ogre::Vector3(1, 0, 0));

    lat->selectAllPoints();
    lat->moveSelectedPoints(0, 0, 1);
    lat->flushPendingDeform();                               // a bend recomputes (normals follow the surface)
    lat->cancelSession();
    for (const auto& n : readNormals()) expectNear(n, Ogre::Vector3(1, 0, 0));

    // Same contract through the bake command's undo.
    ASSERT_TRUE(lat->beginSession());
    lat->selectAllPoints();
    lat->moveSelectedPoints(0, 0, 1);
    lat->flushPendingDeform();
    ASSERT_TRUE(lat->applySession());
    UndoManager::getSingleton()->undo();
    for (const auto& n : readNormals()) expectNear(n, Ogre::Vector3(1, 0, 0));
}

TEST_F(LatticeControllerFixture, ResolutionChangeIsUndoableAsAWholeLattice)
{
    ASSERT_TRUE(m_fix.setup("resundo"));
    auto* lat = LatticeController::instance();
    lat->setResolution(2, 2, 2);
    ASSERT_TRUE(lat->beginSession());
    lat->selectAllPoints();
    lat->moveSelectedPoints(0, 0, 3);
    lat->flushPendingDeform();
    EXPECT_TRUE(lat->isDeformed());

    lat->setResolutionX(4);              // drops the bend…
    EXPECT_EQ(lat->pointCount(), 16);
    EXPECT_FALSE(lat->isDeformed());
    UndoManager::getSingleton()->undo(); // …and Ctrl+Z brings the whole 2×2×2 bend back
    lat->flushPendingDeform();
    EXPECT_EQ(lat->pointCount(), 8);
    EXPECT_EQ(lat->resolutionX(), 2);
    EXPECT_TRUE(lat->isDeformed());
    const auto pos = readPositions(m_fix.entity);
    for (const auto& p : pos) EXPECT_NEAR(p.z, 3.0f, 1e-4f);
    lat->cancelSession();
}

TEST_F(LatticeControllerFixture, SetPointsIsOneUndoStep)
{
    ASSERT_TRUE(m_fix.setup("setpoints"));
    auto* lat = LatticeController::instance();
    lat->setResolution(2, 2, 2);
    ASSERT_TRUE(lat->beginSession());
    auto* undo = UndoManager::getSingleton();
    const int before = undo->stack()->index();
    std::vector<Ogre::Vector3> pts = lat->grid().points;
    for (auto& p : pts) p += Ogre::Vector3(0, 0, 2);
    EXPECT_FALSE(lat->setPoints({}));   // wrong size rejected
    EXPECT_TRUE(lat->setPoints(pts));
    EXPECT_EQ(undo->stack()->index(), before + 1);
    EXPECT_TRUE(lat->isDeformed());
    lat->cancelSession();
}

TEST_F(LatticeControllerFixture, ApplyBakesAsOneUndoStep)
{
    ASSERT_TRUE(m_fix.setup("apply"));
    auto* lat = LatticeController::instance();
    const std::vector<Ogre::Vector3> rest = readPositions(m_fix.entity);
    lat->setResolution(2, 2, 2);
    ASSERT_TRUE(lat->beginSession());
    lat->selectAllPoints();
    lat->moveSelectedPoints(1.0, 0.0, 0.0);
    lat->flushPendingDeform();

    auto* undo = UndoManager::getSingleton();
    const int before = undo->stack()->index();
    ASSERT_TRUE(lat->applySession());
    EXPECT_FALSE(lat->sessionActive());
    EXPECT_EQ(undo->stack()->index(), before + 1); // exactly one bake step
    EXPECT_EQ(undo->stack()->text(before), QStringLiteral("Apply Lattice Deform"));

    std::vector<Ogre::Vector3> baked = readPositions(m_fix.entity);
    for (size_t i = 0; i < 3; ++i) expectNear(baked[i], rest[i] + Ogre::Vector3(1, 0, 0));

    undo->undo();
    baked = readPositions(m_fix.entity);
    for (size_t i = 0; i < 3; ++i) expectNear(baked[i], rest[i]);
    undo->redo();
    baked = readPositions(m_fix.entity);
    for (size_t i = 0; i < 3; ++i) expectNear(baked[i], rest[i] + Ogre::Vector3(1, 0, 0));
}

TEST_F(LatticeControllerFixture, ApplyingARestLatticeClosesWithoutAnUndoStep)
{
    ASSERT_TRUE(m_fix.setup("rest"));
    auto* lat = LatticeController::instance();
    ASSERT_TRUE(lat->beginSession());
    auto* undo = UndoManager::getSingleton();
    const int before = undo->stack()->index();
    EXPECT_TRUE(lat->applySession());
    EXPECT_FALSE(lat->sessionActive());
    EXPECT_EQ(undo->stack()->index(), before);
}

TEST_F(LatticeControllerFixture, LatticeJsonRoundTripsThroughTheSession)
{
    ASSERT_TRUE(m_fix.setup("json"));
    auto* lat = LatticeController::instance();
    lat->setResolution(3, 3, 3);
    ASSERT_TRUE(lat->beginSession());
    ASSERT_TRUE(lat->setPoint(26, 5.0, 5.0, 5.0));
    const QJsonObject doc = lat->latticeJson();
    EXPECT_EQ(doc.value("schema").toString(), QString::fromLatin1(Lattice::kJsonSchema));

    lat->resetPoints();
    EXPECT_FALSE(lat->isDeformed());
    QString err;
    ASSERT_TRUE(lat->setLatticeJson(doc, &err)) << err.toStdString();
    EXPECT_TRUE(lat->isDeformed());
    const QVariantList p = lat->pointPosition(26);
    ASSERT_EQ(p.size(), 3);
    EXPECT_NEAR(p[0].toDouble(), 5.0, 1e-5);
    lat->cancelSession();
}

TEST_F(LatticeControllerFixture, ApplyCommandUndoAbandonsALiveSessionOnTheSameMesh)
{
    // Drag → Apply → Add Lattice again → Ctrl+Z (undo the bake): the second
    // session's rest snapshot no longer describes the mesh, so it must close.
    ASSERT_TRUE(m_fix.setup("abandon"));
    auto* lat = LatticeController::instance();
    lat->setResolution(2, 2, 2);
    ASSERT_TRUE(lat->beginSession());
    lat->selectAllPoints();
    lat->moveSelectedPoints(1, 0, 0);
    lat->flushPendingDeform();
    ASSERT_TRUE(lat->applySession());

    ASSERT_TRUE(lat->beginSession());
    UndoManager::getSingleton()->undo(); // LatticeApplyCommand::undo
    EXPECT_FALSE(lat->sessionActive());
    const auto pos = readPositions(m_fix.entity);
    expectNear(pos[1], Ogre::Vector3(1, 0, 0)); // rest restored, not the session's stale shape
}

TEST_F(LatticeControllerFixture, ResolutionChangeRebuildsARestLattice)
{
    ASSERT_TRUE(m_fix.setup("res"));
    auto* lat = LatticeController::instance();
    lat->setResolution(2, 2, 2);
    ASSERT_TRUE(lat->beginSession());
    lat->selectAllPoints();
    lat->moveSelectedPoints(0, 0, 3);
    EXPECT_TRUE(lat->isDeformed());
    lat->setResolutionX(4);
    EXPECT_EQ(lat->pointCount(), 4 * 2 * 2);
    EXPECT_FALSE(lat->isDeformed());
    EXPECT_EQ(lat->selectedPointCount(), 0);
    lat->cancelSession();
}

TEST_F(LatticeControllerFixture, ReplacingTheEntityUnderItsNodeEndsTheSessionSafely)
{
    // SplitMeshCommand-style swap: the entity is destroyed and a NEW one with
    // the same name is attached to the same node. The session must notice
    // (pointer/mesh identity) and close instead of touching freed memory.
    ASSERT_TRUE(m_fix.setup("swap"));
    auto* lat = LatticeController::instance();
    ASSERT_TRUE(lat->beginSession());
    lat->selectAllPoints();
    lat->moveSelectedPoints(0, 1, 0);
    lat->flushPendingDeform();

    const std::string name = m_fix.entity->getName();
    m_fix.node->detachObject(m_fix.entity);
    m_fix.scene->destroyEntity(m_fix.entity);
    Ogre::MeshPtr other = createInMemoryTriangleMesh("Lattice_Mesh_swap_other");
    m_fix.entity = m_fix.scene->createEntity(name, other->getName());
    m_fix.node->attachObject(m_fix.entity);

    // Any entry point resolves the entity first and drops the stale session.
    EXPECT_EQ(lat->hitTestPoint(nullptr, QPoint(0, 0)), -1);
    lat->moveSelectedPoints(0, 1, 0);
    EXPECT_FALSE(lat->sessionActive());
    EXPECT_TRUE(lat->statusIsError());
    EXPECT_FALSE(lat->applySession());

    // The replacement mesh was never written to.
    const auto pos = readPositions(m_fix.entity);
    ASSERT_EQ(pos.size(), 3u);
    expectNear(pos[1], Ogre::Vector3(1, 0, 0));
    Ogre::MeshManager::getSingleton().remove(other->getHandle());
}

TEST_F(LatticeControllerFixture, OneShotDeformWithGridWritesThroughWithoutASession)
{
    ASSERT_TRUE(m_fix.setup("oneshot"));
    const std::vector<Ogre::Vector3> rest = readPositions(m_fix.entity);
    Lattice::Grid g = Lattice::Grid::fromBounds(Ogre::AxisAlignedBox(Ogre::Vector3(0, 0, -1), Ogre::Vector3(1, 1, 1)),
                                                2, 2, 2, 0.0f);
    g.interpolation = Lattice::Interpolation::Linear;
    for (auto& p : g.points) p += Ogre::Vector3(0, 0, 4);
    QString err;
    ASSERT_TRUE(LatticeController::deformEntityWithGrid(m_fix.entity, g, &err)) << err.toStdString();
    const std::vector<Ogre::Vector3> out = readPositions(m_fix.entity);
    for (size_t i = 0; i < 3; ++i) expectNear(out[i], rest[i] + Ogre::Vector3(0, 0, 4));
    EXPECT_FALSE(LatticeController::instance()->sessionActive());
}
