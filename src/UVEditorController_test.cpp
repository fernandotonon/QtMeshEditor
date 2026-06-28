#include <gtest/gtest.h>

#include <QSignalSpy>

#include "UVEditorController.h"
#include "EditableMesh.h"
#include "EditModeController.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "UndoManager.h"

#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreHardwareIndexBuffer.h>
#include <OgreHardwareVertexBuffer.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>

// Two triangles sharing a 3D edge but with a UV seam (duplicated verts).
static Ogre::MeshPtr createSeamedQuadMesh(const std::string& name)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    sub->useSharedVertices = true;
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;

    size_t offset = 0;
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

    // v0,v1,v2 = tri A; v3,v4,v5 = tri B. v1/v2 share positions with v4/v3 but UVs differ.
    const float verts[] = {
        0, 0, 0,   0.0f, 0.0f,
        1, 0, 0,   1.0f, 0.0f,
        0, 1, 0,   0.0f, 1.0f,
        1, 0, 0,   1.0f, 0.2f,
        0, 1, 0,   0.0f, 1.2f,
        1, 1, 0,   1.0f, 1.2f,
    };

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 6, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    vbuf->writeData(0, sizeof(verts), verts);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = 6;

    const uint16_t idx[] = {0, 1, 2, 3, 4, 5};
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 6, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    ibuf->writeData(0, sizeof(idx), idx);
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 6;

    mesh->_setBounds(Ogre::AxisAlignedBox(0, 0, 0, 1, 1, 0));
    mesh->_setBoundingSphereRadius(1.5f);
    mesh->load();
    return mesh;
}

static EditableMesh makeUvQuadMesh()
{
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.vertices = {
        EditableVertex{.position = {0, 0, 0}, .uv = {0, 0}, .hasUV = true},
        EditableVertex{.position = {1, 0, 0}, .uv = {1, 0}, .hasUV = true},
        EditableVertex{.position = {1, 1, 0}, .uv = {1, 1}, .hasUV = true},
        EditableVertex{.position = {0, 1, 0}, .uv = {0, 1}, .hasUV = true},
    };
    EditableFace face;
    face.indices = {0, 1, 2, 3};
    sub.faces.push_back(std::move(face));
    triangulateFaces(sub);
    mesh.subMeshes().push_back(std::move(sub));
    return mesh;
}

class UVEditorControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb required in CI)";
        ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable (Xvfb required in CI)";
        createStandardOgreMaterials();
        UVEditorController::kill();
    }

    void TearDown() override {
        if (auto* undo = UndoManager::getSingleton())
            undo->clear();
        if (auto* edit = EditModeController::instance()) {
            if (edit->isEditModeActive())
                edit->exitEditMode(false);
        }
        UVEditorController::kill();
        Manager::kill();
    }
};

TEST_F(UVEditorControllerTest, SingleTriangleOneIsland)
{
    auto mesh = createInMemoryTriangleMesh("UVEditor_single_tri");
    EditableMesh em;
    ASSERT_TRUE(em.loadFromMesh(mesh));

    const auto result = UVEditorController::computeIslandsFromEditableMesh(em);
    EXPECT_EQ(result.islandCount, 1);
    ASSERT_EQ(result.faceIslandIds.size(), 1u);
    EXPECT_EQ(result.faceIslandIds[0], 0);
}

TEST_F(UVEditorControllerTest, TwoSubmeshesTwoIslands)
{
    auto mesh = createInMemoryMeshSharedVertsPlusLocalSubmesh("UVEditor_two_subs");
    EditableMesh em;
    ASSERT_TRUE(em.loadFromMesh(mesh));

    const auto result = UVEditorController::computeIslandsFromEditableMesh(em);
    EXPECT_EQ(result.islandCount, 2);
}

TEST_F(UVEditorControllerTest, UvSeamSplitsIslands)
{
    auto mesh = createSeamedQuadMesh("UVEditor_seamed_quad");
    EditableMesh em;
    ASSERT_TRUE(em.loadFromMesh(mesh));

    const auto result = UVEditorController::computeIslandsFromEditableMesh(em);
    EXPECT_EQ(result.islandCount, 2);
    ASSERT_EQ(result.faceIslandIds.size(), 2u);
    EXPECT_NE(result.faceIslandIds[0], result.faceIslandIds[1]);
}

TEST_F(UVEditorControllerTest, CanonicalQuadFaceOneIsland)
{
    const auto result =
        UVEditorController::computeIslandsFromEditableMesh(makeUvQuadMesh());
    EXPECT_EQ(result.islandCount, 1);
    ASSERT_EQ(result.faceIslandIds.size(), 1u);
    EXPECT_EQ(result.faceIslandIds[0], 0);
}

TEST_F(UVEditorControllerTest, MissingUvChannelClearsLayout)
{
    auto mesh = createInMemoryTriangleMesh("UVEditor_uv0_only");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UVEditor_uv1_node");
    auto* entity = sceneMgr->createEntity("UVEditor_uv1_entity", mesh);
    node->attachObject(entity);

    UVEditorController* ctrl = UVEditorController::instance();
    SelectionSet::getSingleton()->selectOne(entity);

    ctrl->setUvChannel(0);
    ctrl->refresh();
    EXPECT_TRUE(ctrl->hasMesh());
    EXPECT_EQ(ctrl->triangles().size(), 1);

    ctrl->setUvChannel(1);
    ctrl->refresh();
    EXPECT_FALSE(ctrl->hasMesh());
    EXPECT_TRUE(ctrl->triangles().isEmpty());
}

TEST_F(UVEditorControllerTest, ControllerRefreshTracksSelection)
{
    auto mesh = createInMemoryTriangleMesh("UVEditor_ctrl_tri");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UVEditor_node");
    auto* entity = sceneMgr->createEntity("UVEditor_entity", mesh);
    node->attachObject(entity);

    UVEditorController* ctrl = UVEditorController::instance();
    QSignalSpy spy(ctrl, &UVEditorController::meshDataChanged);

    EXPECT_FALSE(ctrl->hasMesh());
    SelectionSet::getSingleton()->selectOne(entity);
    ctrl->refresh();

    EXPECT_GE(spy.count(), 1);
    EXPECT_TRUE(ctrl->hasMesh());
    EXPECT_EQ(ctrl->islandCount(), 1);
    EXPECT_EQ(ctrl->triangles().size(), 1);
}

TEST_F(UVEditorControllerTest, SkipsBackgroundRebuildWhilePanelHidden)
{
    auto mesh = createInMemoryTriangleMesh("UVEditor_hidden_tri");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UVEditor_hidden_node");
    auto* entity = sceneMgr->createEntity("UVEditor_hidden_entity", mesh);
    node->attachObject(entity);

    UVEditorController::kill();
    UVEditorController* ctrl = UVEditorController::instance();
    ctrl->setPanelActive(false);
    QSignalSpy spy(ctrl, &UVEditorController::meshDataChanged);

    SelectionSet::getSingleton()->selectOne(entity);
    EXPECT_EQ(spy.count(), 0);
    EXPECT_FALSE(ctrl->hasMesh());

    spy.clear();
    ctrl->setPanelActive(true);
    EXPECT_GE(spy.count(), 1);
    EXPECT_TRUE(ctrl->hasMesh());
}

TEST_F(UVEditorControllerTest, ShowTextureBackgroundToggle)
{
    UVEditorController* ctrl = UVEditorController::instance();
    QSignalSpy spy(ctrl, &UVEditorController::showTextureBackgroundChanged);

    const bool initial = ctrl->showTextureBackground();
    ctrl->setShowTextureBackground(!initial);
    EXPECT_EQ(ctrl->showTextureBackground(), !initial);
    EXPECT_GE(spy.count(), 1);

    ctrl->setShowTextureBackground(initial);
}

TEST_F(UVEditorControllerTest, FacePickSelectsTriangle)
{
    auto mesh = createInMemoryTriangleMesh("UVEditor_pick_tri");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UVEditor_pick_node");
    auto* entity = sceneMgr->createEntity("UVEditor_pick_entity", mesh);
    node->attachObject(entity);

    UVEditorController* ctrl = UVEditorController::instance();
    SelectionSet::getSingleton()->selectOne(entity);
    ctrl->setSelectionMode(UVEditorController::FaceMode);
    ctrl->refresh();

    ASSERT_TRUE(ctrl->hasMesh());
    ctrl->pickAt(0.25, 0.25, UVEditorController::NoModifier, 0.5);
    EXPECT_EQ(ctrl->selectedFaceCount(), 1);
    EXPECT_EQ(ctrl->selectionFaces().size(), 1);
}

TEST_F(UVEditorControllerTest, ShiftClickAddsVertexSelection)
{
    auto mesh = createSeamedQuadMesh("UVEditor_multi_pick");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UVEditor_multi_node");
    auto* entity = sceneMgr->createEntity("UVEditor_multi_entity", mesh);
    node->attachObject(entity);

    UVEditorController* ctrl = UVEditorController::instance();
    SelectionSet::getSingleton()->selectOne(entity);
    ctrl->setSelectionMode(UVEditorController::VertexMode);
    ctrl->refresh();
    ASSERT_TRUE(ctrl->hasMesh());

    ctrl->pickAt(0.0, 0.0, UVEditorController::NoModifier, 0.5);
    EXPECT_EQ(ctrl->selectedVertexCount(), 1);

    ctrl->pickAt(1.0, 0.2, UVEditorController::ShiftModifier, 0.5);
    EXPECT_GE(ctrl->selectedVertexCount(), 2);
}

TEST_F(UVEditorControllerTest, BoxSelectFacesTouchesPartialOverlap)
{
    auto mesh = createSeamedQuadMesh("UVEditor_box_pick");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UVEditor_box_node");
    auto* entity = sceneMgr->createEntity("UVEditor_box_entity", mesh);
    node->attachObject(entity);

    UVEditorController* ctrl = UVEditorController::instance();
    SelectionSet::getSingleton()->selectOne(entity);
    ctrl->setSelectionMode(UVEditorController::FaceMode);
    ctrl->refresh();
    ASSERT_TRUE(ctrl->hasMesh());

    ctrl->boxSelect(0.4, 0.0, 1.1, 1.3, UVEditorController::NoModifier);
    EXPECT_GE(ctrl->selectedFaceCount(), 1);
}

TEST_F(UVEditorControllerTest, BoxSelectVerticesRequiresFullEnclosure)
{
    auto mesh = createSeamedQuadMesh("UVEditor_box_vert");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UVEditor_box_vert_node");
    auto* entity = sceneMgr->createEntity("UVEditor_box_vert_entity", mesh);
    node->attachObject(entity);

    UVEditorController* ctrl = UVEditorController::instance();
    SelectionSet::getSingleton()->selectOne(entity);
    ctrl->setSelectionMode(UVEditorController::VertexMode);
    ctrl->refresh();
    ASSERT_TRUE(ctrl->hasMesh());

    ctrl->boxSelect(0.45, 0.45, 0.55, 0.55, UVEditorController::NoModifier);
    EXPECT_EQ(ctrl->selectedVertexCount(), 0);

    ctrl->boxSelect(-0.05, -0.05, 0.15, 0.15, UVEditorController::NoModifier);
    EXPECT_GE(ctrl->selectedVertexCount(), 1);
}

TEST_F(UVEditorControllerTest, SelectionModeEmitsBreadcrumbSignal)
{
    UVEditorController* ctrl = UVEditorController::instance();
    QSignalSpy spy(ctrl, &UVEditorController::selectionModeChanged);
    ctrl->setSelectionMode(UVEditorController::EdgeMode);
    EXPECT_EQ(ctrl->selectionMode(), UVEditorController::EdgeMode);
    EXPECT_GE(spy.count(), 1);
}

TEST_F(UVEditorControllerTest, FacePickMissesEmptySpaceOutsideRadius)
{
    auto mesh = createInMemoryTriangleMesh("UVEditor_pick_miss");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UVEditor_pick_miss_node");
    auto* entity = sceneMgr->createEntity("UVEditor_pick_miss_entity", mesh);
    node->attachObject(entity);

    UVEditorController* ctrl = UVEditorController::instance();
    SelectionSet::getSingleton()->selectOne(entity);
    ctrl->setSelectionMode(UVEditorController::FaceMode);
    ctrl->refresh();
    ASSERT_TRUE(ctrl->hasMesh());

    ctrl->pickAt(5.0, 5.0, UVEditorController::NoModifier, 0.1);
    EXPECT_EQ(ctrl->selectedFaceCount(), 0);
}

TEST_F(UVEditorControllerTest, ContextIslandsHighlightFromEditSelection)
{
    auto mesh = createInMemoryTriangleMesh("UVEditor_ctx_island");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UVEditor_ctx_node");
    auto* entity = sceneMgr->createEntity("UVEditor_ctx_entity", mesh);
    node->attachObject(entity);

    UVEditorController* ctrl = UVEditorController::instance();
    ctrl->setPanelActive(true);
    SelectionSet::getSingleton()->selectOne(entity);
    ctrl->refresh();
    ASSERT_TRUE(ctrl->hasMesh());

    auto* edit = EditModeController::instance();
    ASSERT_TRUE(edit->enterEditMode());
    ctrl->refresh();

    edit->setSelectionMode(EditModeController::FaceMode);
    edit->selectFace(0);
    EXPECT_EQ(ctrl->selectedFaceCount(), 0);
    EXPECT_FALSE(ctrl->contextIslandFaces().isEmpty());
}

TEST_F(UVEditorControllerTest, FacePickWorksInEditMode)
{
    auto mesh = createInMemoryTriangleMesh("UVEditor_edit_pick");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UVEditor_edit_pick_node");
    auto* entity = sceneMgr->createEntity("UVEditor_edit_pick_entity", mesh);
    node->attachObject(entity);

    UVEditorController* ctrl = UVEditorController::instance();
    SelectionSet::getSingleton()->selectOne(entity);
    ctrl->setSelectionMode(UVEditorController::FaceMode);
    ctrl->refresh();
    ASSERT_TRUE(ctrl->hasMesh());

    auto* edit = EditModeController::instance();
    ASSERT_TRUE(edit->enterEditMode());
    ctrl->refresh();
    ASSERT_TRUE(ctrl->hasMesh());
    ASSERT_GE(ctrl->triangles().size(), 1);

    ctrl->pickAt(0.25, 0.25, UVEditorController::NoModifier, 0.5);
    EXPECT_GE(ctrl->selectedFaceCount(), 1);
}

static Ogre::Vector2 readEntityUv0(Ogre::Entity* entity, int localVert)
{
    const Ogre::SubMesh* sub = entity->getMesh()->getSubMesh(0);
    const Ogre::VertexData* vd = sub->useSharedVertices
        ? entity->getMesh()->sharedVertexData
        : sub->vertexData;
    const auto* elem = vd->vertexDeclaration->findElementBySemantic(
        Ogre::VES_TEXTURE_COORDINATES, 0);
    auto vbuf = vd->vertexBufferBinding->getBuffer(elem->getSource());
    const size_t stride = vbuf->getVertexSize();
    auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    float* p = nullptr;
    elem->baseVertexPointerToElement(base + static_cast<size_t>(localVert) * stride, &p);
    const Ogre::Vector2 uv(p[0], p[1]);
    vbuf->unlock();
    return uv;
}

TEST_F(UVEditorControllerTest, MoveSelectionUpdatesGpuAndUndoRoundTrip)
{
    auto mesh = createInMemoryTriangleMesh("UVEditor_move_undo");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UVEditor_move_node");
    auto* entity = sceneMgr->createEntity("UVEditor_move_entity", mesh);
    node->attachObject(entity);

    UVEditorController* ctrl = UVEditorController::instance();
    ctrl->setPanelActive(true);
    SelectionSet::getSingleton()->selectOne(entity);
    ctrl->setSelectionMode(UVEditorController::FaceMode);
    ctrl->refresh();
    ASSERT_TRUE(ctrl->hasMesh());

    ctrl->pickAt(0.2, 0.2, UVEditorController::NoModifier, 0.5);
    ASSERT_GT(ctrl->selectedFaceCount(), 0);

    const Ogre::Vector2 before = readEntityUv0(entity, 0);
    ctrl->setTransformMode(UVEditorController::MoveTransform);
    ASSERT_TRUE(ctrl->applyNumericTransform(0.25));

    const Ogre::Vector2 after = readEntityUv0(entity, 0);
    EXPECT_NEAR(after.x, before.x + 0.25f, 1e-4f);

    UndoManager::getSingleton()->undo();
    const Ogre::Vector2 restored = readEntityUv0(entity, 0);
    EXPECT_NEAR(restored.x, before.x, 1e-4f);
    EXPECT_NEAR(restored.y, before.y, 1e-4f);

    UndoManager::getSingleton()->redo();
    const Ogre::Vector2 redone = readEntityUv0(entity, 0);
    EXPECT_NEAR(redone.x, after.x, 1e-4f);
}

TEST_F(UVEditorControllerTest, MirrorXCommandIsUndoable)
{
    auto mesh = createInMemoryTriangleMesh("UVEditor_mirror_undo");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UVEditor_mirror_node");
    auto* entity = sceneMgr->createEntity("UVEditor_mirror_entity", mesh);
    node->attachObject(entity);

    UVEditorController* ctrl = UVEditorController::instance();
    ctrl->setPanelActive(true);
    SelectionSet::getSingleton()->selectOne(entity);
    ctrl->setSelectionMode(UVEditorController::FaceMode);
    ctrl->refresh();
    ctrl->pickAt(0.2, 0.2, UVEditorController::NoModifier, 0.5);
    ASSERT_GT(ctrl->selectedFaceCount(), 0);

    const Ogre::Vector2 before0 = readEntityUv0(entity, 0);
    const Ogre::Vector2 before1 = readEntityUv0(entity, 1);
    ctrl->mirrorSelectionX();
    const Ogre::Vector2 mirrored0 = readEntityUv0(entity, 0);
    const Ogre::Vector2 mirrored1 = readEntityUv0(entity, 1);
    EXPECT_NE(mirrored0.x, before0.x);
    EXPECT_NE(mirrored1.x, before1.x);

    UndoManager::getSingleton()->undo();
    const Ogre::Vector2 restored0 = readEntityUv0(entity, 0);
    const Ogre::Vector2 restored1 = readEntityUv0(entity, 1);
    EXPECT_NEAR(restored0.x, before0.x, 1e-4f);
    EXPECT_NEAR(restored0.y, before0.y, 1e-4f);
    EXPECT_NEAR(restored1.x, before1.x, 1e-4f);
    EXPECT_NEAR(restored1.y, before1.y, 1e-4f);
}

static Ogre::MeshPtr createTwoSharedSubmeshTriangleMesh(const std::string& name)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    size_t offset = 0;
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    static constexpr std::array<float, 24> verts{{
        0,0,0,   0,0,1,  0.0f,0.0f,
        1,0,0,   0,0,1,  1.0f,0.0f,
        0,1,0,   0,0,1,  0.0f,1.0f,
    }};
    vbuf->writeData(0, verts.size() * sizeof(float), verts.data());
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = 3;

    for (int sub = 0; sub < 2; ++sub) {
        auto* sm = mesh->createSubMesh();
        auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
            Ogre::HardwareIndexBuffer::IT_16BIT, 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        static constexpr std::array<uint16_t, 3> idx{{0, 1, 2}};
        ibuf->writeData(0, idx.size() * sizeof(uint16_t), idx.data());
        sm->useSharedVertices = true;
        sm->indexData->indexBuffer = ibuf;
        sm->indexData->indexCount = 3;
    }

    mesh->_setBounds(Ogre::AxisAlignedBox(-1, -1, -1, 1, 1, 1));
    mesh->_setBoundingSphereRadius(2.0);
    mesh->load();
    return mesh;
}

TEST_F(UVEditorControllerTest, SharedPoolCommitUsesLaterSubmeshCopy)
{
    auto mesh = createTwoSharedSubmeshTriangleMesh("UVEditor_two_shared");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UVEditor_two_shared_node");
    auto* entity = sceneMgr->createEntity("UVEditor_two_shared_entity", mesh);
    node->attachObject(entity);

    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));
    ASSERT_GE(editMesh.subMeshes().size(), 2u);
    editMesh.setVertexUV(1, 0, Ogre::Vector2(0.42f, 0.17f));
    ASSERT_TRUE(editMesh.commitUvsToEntity(entity, 0));

    const Ogre::Vector2 gpu = readEntityUv0(entity, 0);
    EXPECT_NEAR(gpu.x, 0.42f, 1e-4f);
    EXPECT_NEAR(gpu.y, 0.17f, 1e-4f);
}
