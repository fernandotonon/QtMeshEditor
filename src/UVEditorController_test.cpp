#include <gtest/gtest.h>

#include <QSignalSpy>

#include "UVEditorController.h"
#include "EditableMesh.h"
#include "EditModeController.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

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
