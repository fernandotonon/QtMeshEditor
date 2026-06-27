#include <gtest/gtest.h>

#include <QSignalSpy>

#include "UVEditorController.h"
#include "EditableMesh.h"
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

// Performance gate (#762): while INACTIVE (UV dock hidden), selection / entity
// signals must NOT rebuild the cache — they only mark it dirty. setActive(true)
// then rebuilds lazily. This is what stops import from paying per-entity UV
// reads + island computation when the UV editor is closed.
TEST_F(UVEditorControllerTest, InactiveControllerDefersRebuildUntilActivated)
{
    auto mesh = createInMemoryTriangleMesh("UVEditor_gate_tri");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UVEditor_gate_node");
    auto* entity = sceneMgr->createEntity("UVEditor_gate_entity", mesh);
    node->attachObject(entity);

    UVEditorController* ctrl = UVEditorController::instance();
    ctrl->setActive(false);              // dock hidden

    // A selection change while inactive must not build the mesh cache (the
    // expensive path) — it only marks dirty.
    SelectionSet::getSingleton()->selectOne(entity);
    EXPECT_FALSE(ctrl->hasMesh()) << "inactive controller rebuilt on selection";

    // Activating rebuilds lazily from the pending-dirty state.
    ctrl->setActive(true);
    EXPECT_TRUE(ctrl->hasMesh()) << "setActive(true) did not flush the deferred rebuild";
    EXPECT_EQ(ctrl->triangles().size(), 1);

    ctrl->setActive(false);              // leave clean for other tests
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
