#include <gtest/gtest.h>

#include <QSignalSpy>

#include "Manager.h"
#include "MorphAnimationManager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "UndoManager.h"
#include "commands/MorphCommands.h"

#include <OgreAnimation.h>
#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgrePose.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubMesh.h>

#include <vector>

namespace {

// Build a tiny mesh with two named morph targets attached as Ogre::Pose
// + per-pose VAT_POSE animations, mirroring what `MeshProcessor` does
// at import time. Same shape the manager is contracted against, so
// the test exercises the public API end-to-end without dragging in
// Assimp.
Ogre::MeshPtr createMorphTestMesh(const std::string& name)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    sub->useSharedVertices = false;
    sub->vertexData = new Ogre::VertexData();
    auto* decl = sub->vertexData->vertexDeclaration;
    size_t off = 0;
    decl->addElement(0, off, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    off += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, off, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    float verts[] = {
        0,0,0,  0,0,1,
        1,0,0,  0,0,1,
        0,1,0,  0,0,1,
    };
    vbuf->writeData(0, sizeof(verts), verts);
    sub->vertexData->vertexBufferBinding->setBinding(0, vbuf);
    sub->vertexData->vertexCount = 3;

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = {0, 1, 2};
    ibuf->writeData(0, sizeof(idx), idx);
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 3;

    mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,2,2,2));
    mesh->_setBoundingSphereRadius(2.0f);
    mesh->load();

    // Two named poses on submesh 1.
    {
        Ogre::Pose* p = mesh->createPose(/*target=*/1, "JawOpen");
        p->addVertex(0, Ogre::Vector3(0, -0.1f, 0));
    }
    {
        Ogre::Pose* p = mesh->createPose(/*target=*/1, "Smile");
        p->addVertex(1, Ogre::Vector3(0.05f, 0.02f, 0));
        p->addVertex(2, Ogre::Vector3(-0.05f, 0.02f, 0));
    }

    // One Animation per pose, mirroring MeshProcessor's pattern.
    const auto& poses = mesh->getPoseList();
    for (unsigned short pi = 0; pi < poses.size(); ++pi) {
        Ogre::Animation* a = mesh->createAnimation(poses[pi]->getName(), 0.0f);
        auto* track = a->createVertexTrack(poses[pi]->getTarget(), Ogre::VAT_POSE);
        auto* kf = track->createVertexPoseKeyFrame(0.0f);
        kf->addPoseReference(pi, 1.0f);
    }

    return mesh;
}

}  // namespace

// =============================================================================
// Standalone (no Ogre)
// =============================================================================

TEST(MorphAnimationManagerStandalone, InstanceIsSingleton) {
    auto* a = MorphAnimationManager::instance();
    auto* b = MorphAnimationManager::instance();
    EXPECT_EQ(a, b);
    EXPECT_NE(a, nullptr);
}

TEST(MorphAnimationManagerStandalone, NullEntityReturnsEmptyAndZero) {
    auto* m = MorphAnimationManager::instance();
    EXPECT_TRUE(m->morphTargetsFor(nullptr).isEmpty());
    EXPECT_FLOAT_EQ(m->weight(nullptr, QStringLiteral("JawOpen")), 0.0f);
    EXPECT_FALSE(m->setWeight(nullptr, QStringLiteral("JawOpen"), 0.5f));
}

TEST(MorphAnimationManagerStandalone, EmptyNameRejected) {
    auto* m = MorphAnimationManager::instance();
    EXPECT_FALSE(m->setWeight(nullptr, QString(), 0.5f));
}

// =============================================================================
// Scene fixture — tests need a real Ogre entity with poses.
// =============================================================================

class MorphAnimationManagerSceneTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        ASSERT_TRUE(canLoadMeshFiles());
        if (auto* sel = SelectionSet::getSingleton()) sel->clear();
    }
    void TearDown() override
    {
        if (auto* sel = SelectionSet::getSingleton()) sel->clear();
        if (auto* mgr = Manager::getSingletonPtr()) {
            if (auto* scene = mgr->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
        }
    }
};

TEST_F(MorphAnimationManagerSceneTest, ListsMorphTargetsInPoseOrder) {
    auto mesh = createMorphTestMesh("Morph_ListOrder");
    auto* scene = Manager::getSingleton()->getSceneMgr();
    auto* entity = scene->createEntity("Morph_ListOrderEnt", mesh->getName());
    auto* node = scene->getRootSceneNode()->createChildSceneNode();
    node->attachObject(entity);

    auto* m = MorphAnimationManager::instance();
    QStringList names = m->morphTargetsFor(entity);
    ASSERT_EQ(names.size(), 2);
    EXPECT_EQ(names[0], QStringLiteral("JawOpen"));
    EXPECT_EQ(names[1], QStringLiteral("Smile"));
}

TEST_F(MorphAnimationManagerSceneTest, WeightDefaultIsZeroBeforeSet) {
    auto mesh = createMorphTestMesh("Morph_DefaultZero");
    auto* scene = Manager::getSingleton()->getSceneMgr();
    auto* entity = scene->createEntity("Morph_DefaultZeroEnt", mesh->getName());
    auto* node = scene->getRootSceneNode()->createChildSceneNode();
    node->attachObject(entity);

    auto* m = MorphAnimationManager::instance();
    EXPECT_FLOAT_EQ(m->weight(entity, QStringLiteral("JawOpen")), 0.0f);
    EXPECT_FLOAT_EQ(m->weight(entity, QStringLiteral("Smile")), 0.0f);
}

TEST_F(MorphAnimationManagerSceneTest, SetWeightStoresClampedValue) {
    auto mesh = createMorphTestMesh("Morph_SetClamp");
    auto* scene = Manager::getSingleton()->getSceneMgr();
    auto* entity = scene->createEntity("Morph_SetClampEnt", mesh->getName());
    auto* node = scene->getRootSceneNode()->createChildSceneNode();
    node->attachObject(entity);

    auto* m = MorphAnimationManager::instance();
    EXPECT_TRUE(m->setWeight(entity, QStringLiteral("JawOpen"), 0.75f));
    EXPECT_NEAR(m->weight(entity, QStringLiteral("JawOpen")), 0.75f, 1e-5f);
    // Clamp to [0..1].
    EXPECT_TRUE(m->setWeight(entity, QStringLiteral("Smile"), 2.0f));
    EXPECT_FLOAT_EQ(m->weight(entity, QStringLiteral("Smile")), 1.0f);
    EXPECT_TRUE(m->setWeight(entity, QStringLiteral("Smile"), -0.5f));
    EXPECT_FLOAT_EQ(m->weight(entity, QStringLiteral("Smile")), 0.0f);
}

TEST_F(MorphAnimationManagerSceneTest, SetWeightUnknownNameReturnsFalse) {
    auto mesh = createMorphTestMesh("Morph_Unknown");
    auto* scene = Manager::getSingleton()->getSceneMgr();
    auto* entity = scene->createEntity("Morph_UnknownEnt", mesh->getName());
    auto* node = scene->getRootSceneNode()->createChildSceneNode();
    node->attachObject(entity);

    auto* m = MorphAnimationManager::instance();
    EXPECT_FALSE(m->setWeight(entity, QStringLiteral("NotARealShape"), 0.5f));
}

TEST_F(MorphAnimationManagerSceneTest, SetWeightEmitsSignalOnRealChange) {
    auto mesh = createMorphTestMesh("Morph_Signal");
    auto* scene = Manager::getSingleton()->getSceneMgr();
    auto* entity = scene->createEntity("Morph_SignalEnt", mesh->getName());
    auto* node = scene->getRootSceneNode()->createChildSceneNode();
    node->attachObject(entity);

    auto* m = MorphAnimationManager::instance();
    QSignalSpy spy(m, &MorphAnimationManager::morphWeightChanged);

    // First write emits.
    EXPECT_TRUE(m->setWeight(entity, QStringLiteral("JawOpen"), 0.5f));
    const int afterFirst = spy.count();
    EXPECT_GE(afterFirst, 1);

    // Idempotent write must NOT emit again — same value, same enabled state.
    EXPECT_TRUE(m->setWeight(entity, QStringLiteral("JawOpen"), 0.5f));
    EXPECT_EQ(spy.count(), afterFirst);

    // Different value DOES emit.
    EXPECT_TRUE(m->setWeight(entity, QStringLiteral("JawOpen"), 0.7f));
    EXPECT_GT(spy.count(), afterFirst);
}

TEST_F(MorphAnimationManagerSceneTest, EmptyNameRejectedForValidEntity) {
    // Empty-name rejection is also exercised standalone with a null
    // entity, but a passing test there could hide a regression where
    // empty-name handling becomes entity-dependent. Re-test with a
    // valid live entity to lock the contract.
    auto mesh = createMorphTestMesh("Morph_EmptyName");
    auto* scene = Manager::getSingleton()->getSceneMgr();
    auto* entity = scene->createEntity("Morph_EmptyNameEnt", mesh->getName());
    auto* node = scene->getRootSceneNode()->createChildSceneNode();
    node->attachObject(entity);

    auto* m = MorphAnimationManager::instance();
    EXPECT_FALSE(m->setWeight(entity, QString(), 0.5f));
    EXPECT_FLOAT_EQ(m->weight(entity, QString()), 0.0f);
}

TEST_F(MorphAnimationManagerSceneTest, SelectionDrivenAccessorsResolveFirstEntity) {
    auto mesh = createMorphTestMesh("Morph_Sel");
    auto* scene = Manager::getSingleton()->getSceneMgr();
    auto* entity = scene->createEntity("Morph_SelEnt", mesh->getName());
    auto* node = scene->getRootSceneNode()->createChildSceneNode();
    node->attachObject(entity);

    auto* sel = SelectionSet::getSingleton();
    ASSERT_NE(sel, nullptr);
    sel->append(entity);

    auto* m = MorphAnimationManager::instance();
    EXPECT_EQ(m->morphTargetsForSelection().size(), 2);
    EXPECT_TRUE(m->setWeightForSelection(QStringLiteral("Smile"), 0.6));
    EXPECT_NEAR(m->weightForSelection(QStringLiteral("Smile")), 0.6, 1e-4);
}

TEST_F(MorphAnimationManagerSceneTest, NoSelectionGivesEmptyList) {
    auto* m = MorphAnimationManager::instance();
    EXPECT_TRUE(m->morphTargetsForSelection().isEmpty());
    EXPECT_DOUBLE_EQ(m->weightForSelection(QStringLiteral("X")), 0.0);
    EXPECT_FALSE(m->setWeightForSelection(QStringLiteral("X"), 0.5));
}

// =============================================================================
// Authoring (slice A3) — exercises the three commands directly because
// the manager wrappers' `addMorphTargetFromCurrentEdit` path needs a
// live EditModeController, which is heavy to bootstrap headless. The
// undo / redo path is what we really care about — the wrappers are
// thin selection + name validation in front of the same commands.
// =============================================================================

// Helper: produce a 1-vertex offset slice on submesh 1.
static MorphPoseSlice makeSlice(unsigned short submesh, unsigned int vi, float x)
{
    MorphPoseSlice s;
    s.submeshHandle = submesh;
    s.offsets[vi] = Ogre::Vector3f(x, 0, 0);
    return s;
}

TEST_F(MorphAnimationManagerSceneTest, AddMorphTargetCommandCreatesPoseAndAnimation) {
    auto mesh = createMorphTestMesh("Morph_AddCmd");
    auto* scene = Manager::getSingleton()->getSceneMgr();
    auto* entity = scene->createEntity("Morph_AddCmdEnt", mesh->getName());
    auto* node = scene->getRootSceneNode()->createChildSceneNode();
    node->attachObject(entity);

    std::vector<MorphPoseSlice> slices{makeSlice(1, 0, 0.25f)};
    AddMorphTargetCommand cmd(entity, QStringLiteral("Frown"), slices);
    cmd.redo();

    EXPECT_TRUE(mesh->hasAnimation("Frown"));
    bool found = false;
    for (const auto* p : mesh->getPoseList())
        if (p && p->getName() == "Frown") { found = true; break; }
    EXPECT_TRUE(found);

    // Undo strips both pose and animation back out.
    cmd.undo();
    EXPECT_FALSE(mesh->hasAnimation("Frown"));
    for (const auto* p : mesh->getPoseList())
        EXPECT_NE(p->getName(), "Frown");
}

TEST_F(MorphAnimationManagerSceneTest, DeleteMorphTargetCommandRoundTrips) {
    auto mesh = createMorphTestMesh("Morph_DelCmd");
    auto* scene = Manager::getSingleton()->getSceneMgr();
    auto* entity = scene->createEntity("Morph_DelCmdEnt", mesh->getName());
    auto* node = scene->getRootSceneNode()->createChildSceneNode();
    node->attachObject(entity);

    // Sanity — fixture seeded the mesh with "JawOpen" + "Smile".
    ASSERT_TRUE(mesh->hasAnimation("JawOpen"));
    const size_t posesBefore = mesh->getPoseCount();

    DeleteMorphTargetCommand cmd(entity, QStringLiteral("JawOpen"));
    cmd.redo();

    EXPECT_FALSE(mesh->hasAnimation("JawOpen"));
    EXPECT_EQ(mesh->getPoseCount(), posesBefore - 1);

    cmd.undo();
    EXPECT_TRUE(mesh->hasAnimation("JawOpen"));
    EXPECT_EQ(mesh->getPoseCount(), posesBefore);
}

TEST_F(MorphAnimationManagerSceneTest, RenameMorphTargetCommandRoundTrips) {
    auto mesh = createMorphTestMesh("Morph_RenameCmd");
    auto* scene = Manager::getSingleton()->getSceneMgr();
    auto* entity = scene->createEntity("Morph_RenameCmdEnt", mesh->getName());
    auto* node = scene->getRootSceneNode()->createChildSceneNode();
    node->attachObject(entity);

    RenameMorphTargetCommand cmd(entity,
                                  QStringLiteral("Smile"),
                                  QStringLiteral("Grin"));
    cmd.redo();
    EXPECT_FALSE(mesh->hasAnimation("Smile"));
    EXPECT_TRUE(mesh->hasAnimation("Grin"));

    cmd.undo();
    EXPECT_TRUE(mesh->hasAnimation("Smile"));
    EXPECT_FALSE(mesh->hasAnimation("Grin"));
}

TEST_F(MorphAnimationManagerSceneTest, RenameRejectsCollisionAndIdempotentName) {
    auto mesh = createMorphTestMesh("Morph_RenameReject");
    auto* scene = Manager::getSingleton()->getSceneMgr();
    auto* entity = scene->createEntity("Morph_RenameRejectEnt", mesh->getName());
    auto* node = scene->getRootSceneNode()->createChildSceneNode();
    node->attachObject(entity);

    auto* sel = SelectionSet::getSingleton();
    sel->append(entity);

    auto* m = MorphAnimationManager::instance();
    // Renaming to an existing name is a no-op (would otherwise produce
    // two unrelated poses sharing a name).
    EXPECT_FALSE(m->renameMorphTarget(QStringLiteral("JawOpen"),
                                       QStringLiteral("Smile")));
    // Renaming to the same name is a no-op.
    EXPECT_FALSE(m->renameMorphTarget(QStringLiteral("JawOpen"),
                                       QStringLiteral("JawOpen")));
    // Empty / whitespace new name is a no-op.
    EXPECT_FALSE(m->renameMorphTarget(QStringLiteral("JawOpen"),
                                       QStringLiteral("   ")));
}

TEST_F(MorphAnimationManagerSceneTest, DeleteUnknownTargetIsRejected) {
    auto mesh = createMorphTestMesh("Morph_DelUnknown");
    auto* scene = Manager::getSingleton()->getSceneMgr();
    auto* entity = scene->createEntity("Morph_DelUnknownEnt", mesh->getName());
    auto* node = scene->getRootSceneNode()->createChildSceneNode();
    node->attachObject(entity);

    auto* sel = SelectionSet::getSingleton();
    sel->append(entity);

    auto* m = MorphAnimationManager::instance();
    EXPECT_FALSE(m->deleteMorphTarget(QStringLiteral("NotARealShape")));
    EXPECT_FALSE(m->deleteMorphTarget(QString()));
    // Real one should succeed via undo manager.
    EXPECT_TRUE(m->deleteMorphTarget(QStringLiteral("Smile")));
    EXPECT_FALSE(mesh->hasAnimation("Smile"));
}

TEST_F(MorphAnimationManagerSceneTest, AddMorphTargetFromEditNoEditableMeshReturnsFalse) {
    auto mesh = createMorphTestMesh("Morph_AddNoEdit");
    auto* scene = Manager::getSingleton()->getSceneMgr();
    auto* entity = scene->createEntity("Morph_AddNoEditEnt", mesh->getName());
    auto* node = scene->getRootSceneNode()->createChildSceneNode();
    node->attachObject(entity);

    auto* sel = SelectionSet::getSingleton();
    sel->append(entity);

    auto* m = MorphAnimationManager::instance();
    // Not in edit mode → no EditableMesh available → no-op false.
    EXPECT_FALSE(m->addMorphTargetFromCurrentEdit(QStringLiteral("FromEdit")));
    EXPECT_FALSE(mesh->hasAnimation("FromEdit"));

    // Empty / whitespace name rejected even before edit-mode check.
    EXPECT_FALSE(m->addMorphTargetFromCurrentEdit(QString()));
    EXPECT_FALSE(m->addMorphTargetFromCurrentEdit(QStringLiteral("  ")));
}

TEST_F(MorphAnimationManagerSceneTest, AddMorphTargetFromEditNoSelectionReturnsFalse) {
    auto* m = MorphAnimationManager::instance();
    EXPECT_FALSE(m->addMorphTargetFromCurrentEdit(QStringLiteral("X")));
    EXPECT_FALSE(m->renameMorphTarget(QStringLiteral("A"), QStringLiteral("B")));
    EXPECT_FALSE(m->deleteMorphTarget(QStringLiteral("X")));
}
