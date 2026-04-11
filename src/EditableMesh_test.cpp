/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include <gtest/gtest.h>
#include "EditableMesh.h"
#include "EditModeController.h"
#include "TestHelpers.h"
#include "Manager.h"
#include "SelectionSet.h"

// ===========================================================================
// EditableMesh unit tests
// ===========================================================================

class EditableMeshTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!tryInitOgre()) {
            GTEST_SKIP() << "Ogre not available";
            return;
        }
        if (!canLoadMeshFiles()) {
            GTEST_SKIP() << "Cannot create hardware buffers";
            return;
        }
        createStandardOgreMaterials();
    }
};

// These tests don't need Ogre, so use standalone TEST instead of TEST_F
TEST(EditableMeshStandalone, LoadFromEntityNullReturnsFailure) {
    EditableMesh mesh;
    EXPECT_FALSE(mesh.loadFromEntity(nullptr));
}

TEST(EditableMeshStandalone, CommitToEntityNullReturnsFailure) {
    EditableMesh mesh;
    EXPECT_FALSE(mesh.commitToEntity(nullptr));
}

TEST(EditableMeshStandalone, CalculateBoundsEmpty) {
    EditableMesh editMesh;
    auto bounds = editMesh.calculateBounds();
    EXPECT_TRUE(bounds.isNull());
}

TEST(EditableMeshStandalone, InitialState) {
    EditableMesh mesh;
    EXPECT_EQ(mesh.subMeshCount(), 0u);
    EXPECT_EQ(mesh.totalVertexCount(), 0u);
    EXPECT_EQ(mesh.totalTriangleCount(), 0u);
    EXPECT_TRUE(mesh.subMeshes().empty());
}

TEST(EditableMeshStandalone, RecalculateNormalsNoSubmeshes) {
    EditableMesh mesh;
    // Should not crash on empty mesh
    mesh.recalculateNormals();
    EXPECT_EQ(mesh.totalVertexCount(), 0u);
}

TEST(EditableMeshStandalone, OutOfBoundsAccessReturnsZero) {
    EditableMesh mesh;
    auto pos = mesh.getVertexPosition(0, 0);
    EXPECT_FLOAT_EQ(pos.x, 0.0f);
    EXPECT_FLOAT_EQ(pos.y, 0.0f);
    EXPECT_FLOAT_EQ(pos.z, 0.0f);

    auto norm = mesh.getVertexNormal(99, 0);
    EXPECT_FLOAT_EQ(norm.x, 0.0f);

    auto uv = mesh.getVertexUV(0, 999);
    EXPECT_FLOAT_EQ(uv.x, 0.0f);
}

TEST_F(EditableMeshTest, LoadFromEntityTriangleMesh) {
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_triangle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_triangle_node");
    auto* entity = sceneMgr->createEntity("EditableMesh_triangle_ent", meshPtr);
    node->attachObject(entity);

    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));

    // The triangle mesh has 1 submesh with 3 vertices and 1 triangle
    EXPECT_EQ(editMesh.subMeshCount(), 1u);
    EXPECT_EQ(editMesh.totalVertexCount(), 3u);
    EXPECT_EQ(editMesh.totalTriangleCount(), 1u);

    // Check vertex positions match what we put in createInMemoryTriangleMesh
    auto pos0 = editMesh.getVertexPosition(0, 0);
    auto pos1 = editMesh.getVertexPosition(0, 1);
    auto pos2 = editMesh.getVertexPosition(0, 2);

    EXPECT_FLOAT_EQ(pos0.x, 0.0f);
    EXPECT_FLOAT_EQ(pos0.y, 0.0f);
    EXPECT_FLOAT_EQ(pos0.z, 0.0f);

    EXPECT_FLOAT_EQ(pos1.x, 1.0f);
    EXPECT_FLOAT_EQ(pos1.y, 0.0f);
    EXPECT_FLOAT_EQ(pos1.z, 0.0f);

    EXPECT_FLOAT_EQ(pos2.x, 0.0f);
    EXPECT_FLOAT_EQ(pos2.y, 1.0f);
    EXPECT_FLOAT_EQ(pos2.z, 0.0f);

    // Check normals were read
    auto norm0 = editMesh.getVertexNormal(0, 0);
    EXPECT_FLOAT_EQ(norm0.x, 0.0f);
    EXPECT_FLOAT_EQ(norm0.y, 0.0f);
    EXPECT_FLOAT_EQ(norm0.z, 1.0f);

    // Check UVs
    auto uv0 = editMesh.getVertexUV(0, 0);
    EXPECT_FLOAT_EQ(uv0.x, 0.0f);
    EXPECT_FLOAT_EQ(uv0.y, 0.0f);

    auto uv1 = editMesh.getVertexUV(0, 1);
    EXPECT_FLOAT_EQ(uv1.x, 1.0f);
    EXPECT_FLOAT_EQ(uv1.y, 0.0f);

    // Check material name (default for shared vertices)
    EXPECT_FALSE(editMesh.subMeshes()[0].materialName.empty() &&
                 editMesh.subMeshes()[0].usesSharedVertices);

    // Cleanup
    Manager::getSingleton()->destroySceneNode("EditableMesh_triangle_node");
}

TEST_F(EditableMeshTest, VertexManipulation) {
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_manip");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_manip_node");
    auto* entity = sceneMgr->createEntity("EditableMesh_manip_ent", meshPtr);
    node->attachObject(entity);

    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));

    // Move vertex 0
    Ogre::Vector3 newPos(5.0f, 5.0f, 5.0f);
    editMesh.setVertexPosition(0, 0, newPos);
    auto readBack = editMesh.getVertexPosition(0, 0);
    EXPECT_FLOAT_EQ(readBack.x, 5.0f);
    EXPECT_FLOAT_EQ(readBack.y, 5.0f);
    EXPECT_FLOAT_EQ(readBack.z, 5.0f);

    // Out-of-bounds access should not crash and return zero
    auto oob = editMesh.getVertexPosition(99, 0);
    EXPECT_FLOAT_EQ(oob.x, 0.0f);

    auto oob2 = editMesh.getVertexPosition(0, 999);
    EXPECT_FLOAT_EQ(oob2.x, 0.0f);

    Manager::getSingleton()->destroySceneNode("EditableMesh_manip_node");
}

TEST_F(EditableMeshTest, RecalculateNormals) {
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_normals");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_normals_node");
    auto* entity = sceneMgr->createEntity("EditableMesh_normals_ent", meshPtr);
    node->attachObject(entity);

    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));

    // Zero out normals first
    editMesh.setVertexNormal(0, 0, Ogre::Vector3::ZERO);
    editMesh.setVertexNormal(0, 1, Ogre::Vector3::ZERO);
    editMesh.setVertexNormal(0, 2, Ogre::Vector3::ZERO);

    // Recalculate — the triangle lies in the XY plane, so normals should point in Z
    editMesh.recalculateNormals();

    auto norm0 = editMesh.getVertexNormal(0, 0);
    auto norm1 = editMesh.getVertexNormal(0, 1);
    auto norm2 = editMesh.getVertexNormal(0, 2);

    // All three vertices should have the same normal (flat shading of single triangle)
    EXPECT_NEAR(norm0.x, 0.0f, 0.01f);
    EXPECT_NEAR(norm0.y, 0.0f, 0.01f);
    EXPECT_NEAR(std::abs(norm0.z), 1.0f, 0.01f);

    EXPECT_NEAR(norm1.x, 0.0f, 0.01f);
    EXPECT_NEAR(norm1.y, 0.0f, 0.01f);
    EXPECT_NEAR(std::abs(norm1.z), 1.0f, 0.01f);

    EXPECT_NEAR(norm2.x, 0.0f, 0.01f);
    EXPECT_NEAR(norm2.y, 0.0f, 0.01f);
    EXPECT_NEAR(std::abs(norm2.z), 1.0f, 0.01f);

    Manager::getSingleton()->destroySceneNode("EditableMesh_normals_node");
}

TEST_F(EditableMeshTest, CalculateBoundsTriangle) {
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_bounds");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_bounds_node");
    auto* entity = sceneMgr->createEntity("EditableMesh_bounds_ent", meshPtr);
    node->attachObject(entity);

    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));

    auto bounds = editMesh.calculateBounds();
    EXPECT_FALSE(bounds.isNull());

    // Triangle has vertices at (0,0,0), (1,0,0), (0,1,0)
    auto minPt = bounds.getMinimum();
    auto maxPt = bounds.getMaximum();
    EXPECT_FLOAT_EQ(minPt.x, 0.0f);
    EXPECT_FLOAT_EQ(minPt.y, 0.0f);
    EXPECT_FLOAT_EQ(minPt.z, 0.0f);
    EXPECT_FLOAT_EQ(maxPt.x, 1.0f);
    EXPECT_FLOAT_EQ(maxPt.y, 1.0f);
    EXPECT_FLOAT_EQ(maxPt.z, 0.0f);

    Manager::getSingleton()->destroySceneNode("EditableMesh_bounds_node");
}

TEST_F(EditableMeshTest, CommitToEntity) {
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_commit");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_commit_node");
    auto* entity = sceneMgr->createEntity("EditableMesh_commit_ent", meshPtr);
    node->attachObject(entity);

    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));

    // Move vertex 0 to a new position
    editMesh.setVertexPosition(0, 0, Ogre::Vector3(10.0f, 20.0f, 30.0f));

    // Commit changes
    ASSERT_TRUE(editMesh.commitToEntity(entity));

    // Re-read from the entity to verify the GPU buffers were updated
    EditableMesh verifyMesh;
    ASSERT_TRUE(verifyMesh.loadFromEntity(entity));

    auto pos0 = verifyMesh.getVertexPosition(0, 0);
    EXPECT_FLOAT_EQ(pos0.x, 10.0f);
    EXPECT_FLOAT_EQ(pos0.y, 20.0f);
    EXPECT_FLOAT_EQ(pos0.z, 30.0f);

    Manager::getSingleton()->destroySceneNode("EditableMesh_commit_node");
}

TEST_F(EditableMeshTest, LoadSkeletonMeshWithBoneAssignments) {
    auto meshPtr = createInMemorySkeletonMesh("EditableMesh_skel");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_skel_node");
    auto* entity = sceneMgr->createEntity("EditableMesh_skel_ent", meshPtr);
    node->attachObject(entity);

    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));

    EXPECT_EQ(editMesh.subMeshCount(), 1u);
    EXPECT_EQ(editMesh.totalVertexCount(), 3u);

    // Check bone assignments were loaded
    const auto& vertices = editMesh.subMeshes()[0].vertices;
    for (const auto& v : vertices) {
        EXPECT_FALSE(v.boneAssignments.empty());
        // Each vertex should be assigned to bone 1 with weight 1.0
        EXPECT_EQ(v.boneAssignments[0].boneIndex, 1);
        EXPECT_FLOAT_EQ(v.boneAssignments[0].weight, 1.0f);
    }

    Manager::getSingleton()->destroySceneNode("EditableMesh_skel_node");
}

TEST_F(EditableMeshTest, UVManipulation) {
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_uv");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_uv_node");
    auto* entity = sceneMgr->createEntity("EditableMesh_uv_ent", meshPtr);
    node->attachObject(entity);

    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));

    // Modify UV
    editMesh.setVertexUV(0, 0, Ogre::Vector2(0.5f, 0.75f));
    auto uv = editMesh.getVertexUV(0, 0);
    EXPECT_FLOAT_EQ(uv.x, 0.5f);
    EXPECT_FLOAT_EQ(uv.y, 0.75f);

    Manager::getSingleton()->destroySceneNode("EditableMesh_uv_node");
}

// ===========================================================================
// EditModeController unit tests
// ===========================================================================

class EditModeControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!tryInitOgre()) {
            GTEST_SKIP() << "Ogre not available";
            return;
        }
        if (!canLoadMeshFiles()) {
            GTEST_SKIP() << "Cannot create hardware buffers";
            return;
        }
        createStandardOgreMaterials();
    }
};

TEST_F(EditModeControllerTest, InitialState) {
    // Ensure edit mode is not active on startup
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->isEditModeActive());
    EXPECT_EQ(ctrl->modeLabel(), "Object Mode");
    EXPECT_EQ(ctrl->vertexCount(), 0);
    EXPECT_EQ(ctrl->triangleCount(), 0);
    EXPECT_EQ(ctrl->subMeshCount(), 0);
    EXPECT_EQ(ctrl->currentMesh(), nullptr);
    EXPECT_EQ(ctrl->editEntity(), nullptr);
}

TEST_F(EditModeControllerTest, CannotEnterWithoutSelection) {
    auto* ctrl = EditModeController::instance();
    SelectionSet::getSingleton()->clear();

    EXPECT_FALSE(ctrl->canEnterEditMode());
    EXPECT_FALSE(ctrl->enterEditMode());
    EXPECT_FALSE(ctrl->isEditModeActive());
}

TEST_F(EditModeControllerTest, EnterAndExitEditMode) {
    auto meshPtr = createInMemoryTriangleMesh("EditMode_enter_exit");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditMode_enter_exit_node");
    auto* entity = sceneMgr->createEntity("EditMode_enter_exit_ent", meshPtr);
    node->attachObject(entity);

    // Select the node
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    EXPECT_TRUE(ctrl->canEnterEditMode());

    // Enter edit mode
    EXPECT_TRUE(ctrl->enterEditMode());
    EXPECT_TRUE(ctrl->isEditModeActive());
    EXPECT_EQ(ctrl->modeLabel(), "Edit Mode");
    EXPECT_NE(ctrl->currentMesh(), nullptr);
    EXPECT_EQ(ctrl->editEntity(), entity);
    EXPECT_EQ(ctrl->vertexCount(), 3);
    EXPECT_EQ(ctrl->triangleCount(), 1);
    EXPECT_EQ(ctrl->subMeshCount(), 1);

    // Exit edit mode
    ctrl->exitEditMode(true);
    EXPECT_FALSE(ctrl->isEditModeActive());
    EXPECT_EQ(ctrl->modeLabel(), "Object Mode");
    EXPECT_EQ(ctrl->currentMesh(), nullptr);
    EXPECT_EQ(ctrl->editEntity(), nullptr);
    EXPECT_EQ(ctrl->vertexCount(), 0);

    Manager::getSingleton()->destroySceneNode("EditMode_enter_exit_node");
}

TEST_F(EditModeControllerTest, ToggleEditMode) {
    auto meshPtr = createInMemoryTriangleMesh("EditMode_toggle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditMode_toggle_node");
    auto* entity = sceneMgr->createEntity("EditMode_toggle_ent", meshPtr);
    node->attachObject(entity);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();

    // Toggle on
    ctrl->toggleEditMode();
    EXPECT_TRUE(ctrl->isEditModeActive());

    // Toggle off
    ctrl->toggleEditMode();
    EXPECT_FALSE(ctrl->isEditModeActive());

    Manager::getSingleton()->destroySceneNode("EditMode_toggle_node");
}

TEST_F(EditModeControllerTest, AutoExitOnSelectionChange) {
    auto meshPtr1 = createInMemoryTriangleMesh("EditMode_auto_exit1");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node1 = Manager::getSingleton()->addSceneNode("EditMode_auto_exit1_node");
    auto* entity1 = sceneMgr->createEntity("EditMode_auto_exit1_ent", meshPtr1);
    node1->attachObject(entity1);

    auto meshPtr2 = createInMemoryTriangleMesh("EditMode_auto_exit2");
    auto* node2 = Manager::getSingleton()->addSceneNode("EditMode_auto_exit2_node");
    auto* entity2 = sceneMgr->createEntity("EditMode_auto_exit2_ent", meshPtr2);
    node2->attachObject(entity2);

    // Select first node and enter edit mode
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node1);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    EXPECT_TRUE(ctrl->isEditModeActive());

    // Change selection to second node — should auto-exit edit mode
    SelectionSet::getSingleton()->selectOne(node2);
    EXPECT_FALSE(ctrl->isEditModeActive());

    Manager::getSingleton()->destroySceneNode("EditMode_auto_exit1_node");
    Manager::getSingleton()->destroySceneNode("EditMode_auto_exit2_node");
}
