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

// ===========================================================================
// Phase 3 Items 6-8: Vertex Transform, Normals, Validation
// ===========================================================================

// ---- Item 7: Flat normals recalculation ----
TEST(EditableMeshStandalone, RecalculateNormalsFlatEmpty) {
    EditableMesh mesh;
    // Should not crash on empty mesh
    mesh.recalculateNormalsFlat();
    EXPECT_EQ(mesh.totalVertexCount(), 0u);
}

TEST_F(EditableMeshTest, RecalculateNormalsFlat) {
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_flat_normals");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_flat_normals_node");
    auto* entity = sceneMgr->createEntity("EditableMesh_flat_normals_ent", meshPtr);
    node->attachObject(entity);

    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));

    // Zero out normals
    editMesh.setVertexNormal(0, 0, Ogre::Vector3::ZERO);
    editMesh.setVertexNormal(0, 1, Ogre::Vector3::ZERO);
    editMesh.setVertexNormal(0, 2, Ogre::Vector3::ZERO);

    // Recalculate flat normals
    editMesh.recalculateNormalsFlat();

    // All vertices in a single flat triangle should have the same face normal
    auto norm0 = editMesh.getVertexNormal(0, 0);
    auto norm1 = editMesh.getVertexNormal(0, 1);
    auto norm2 = editMesh.getVertexNormal(0, 2);

    // Should point in Z direction for XY-plane triangle
    EXPECT_NEAR(std::abs(norm0.z), 1.0f, 0.01f);
    EXPECT_NEAR(std::abs(norm1.z), 1.0f, 0.01f);
    EXPECT_NEAR(std::abs(norm2.z), 1.0f, 0.01f);

    // All normals should be identical
    EXPECT_FLOAT_EQ(norm0.x, norm1.x);
    EXPECT_FLOAT_EQ(norm0.y, norm1.y);
    EXPECT_FLOAT_EQ(norm0.z, norm1.z);

    Manager::getSingleton()->destroySceneNode("EditableMesh_flat_normals_node");
}

// ---- Item 8: Degenerate triangle detection ----
TEST(EditableMeshStandalone, CountDegenerateTrianglesEmpty) {
    EditableMesh mesh;
    EXPECT_EQ(mesh.countDegenerateTriangles(), 0);
}

TEST(EditableMeshStandalone, CountDegenerateTrianglesManual) {
    EditableMesh mesh;
    EditableSubMesh sub;

    // Add 3 vertices forming a valid triangle
    EditableVertex v0, v1, v2;
    v0.position = Ogre::Vector3(0, 0, 0);
    v1.position = Ogre::Vector3(1, 0, 0);
    v2.position = Ogre::Vector3(0, 1, 0);
    sub.vertices.push_back(v0);
    sub.vertices.push_back(v1);
    sub.vertices.push_back(v2);

    // Add a valid triangle
    EditableTriangle tri;
    tri.indices[0] = 0;
    tri.indices[1] = 1;
    tri.indices[2] = 2;
    sub.triangles.push_back(tri);

    // Add a degenerate triangle (all same vertex)
    EditableTriangle degTri;
    degTri.indices[0] = 0;
    degTri.indices[1] = 0;
    degTri.indices[2] = 0;
    sub.triangles.push_back(degTri);

    mesh.subMeshes().push_back(std::move(sub));

    EXPECT_EQ(mesh.countDegenerateTriangles(), 1);
}

TEST(EditableMeshStandalone, RemoveDegenerateTriangles) {
    EditableMesh mesh;
    EditableSubMesh sub;

    EditableVertex v0, v1, v2;
    v0.position = Ogre::Vector3(0, 0, 0);
    v1.position = Ogre::Vector3(1, 0, 0);
    v2.position = Ogre::Vector3(0, 1, 0);
    sub.vertices.push_back(v0);
    sub.vertices.push_back(v1);
    sub.vertices.push_back(v2);

    // Valid triangle
    EditableTriangle tri1;
    tri1.indices[0] = 0;
    tri1.indices[1] = 1;
    tri1.indices[2] = 2;
    sub.triangles.push_back(tri1);

    // Degenerate triangle
    EditableTriangle tri2;
    tri2.indices[0] = 0;
    tri2.indices[1] = 0;
    tri2.indices[2] = 0;
    sub.triangles.push_back(tri2);

    mesh.subMeshes().push_back(std::move(sub));

    EXPECT_EQ(mesh.totalTriangleCount(), 2u);
    int removed = mesh.removeDegenerateTriangles();
    EXPECT_EQ(removed, 1);
    EXPECT_EQ(mesh.totalTriangleCount(), 1u);
    EXPECT_EQ(mesh.countDegenerateTriangles(), 0);
}

// ---- Item 6: Soft selection ----
TEST_F(EditModeControllerTest, SoftSelectionDefaults) {
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->softSelectionEnabled());
    EXPECT_DOUBLE_EQ(ctrl->softSelectionRadius(), 2.0);
    EXPECT_EQ(ctrl->softSelectionFalloff(), 0);
}

TEST_F(EditModeControllerTest, SoftSelectionSetters) {
    auto* ctrl = EditModeController::instance();

    ctrl->setSoftSelectionEnabled(true);
    EXPECT_TRUE(ctrl->softSelectionEnabled());

    ctrl->setSoftSelectionRadius(5.0);
    EXPECT_DOUBLE_EQ(ctrl->softSelectionRadius(), 5.0);

    ctrl->setSoftSelectionFalloff(1);
    EXPECT_EQ(ctrl->softSelectionFalloff(), 1);

    // Reset
    ctrl->setSoftSelectionEnabled(false);
    ctrl->setSoftSelectionRadius(2.0);
    ctrl->setSoftSelectionFalloff(0);
}

TEST_F(EditModeControllerTest, SoftSelectionWeightsWithoutSelection) {
    auto* ctrl = EditModeController::instance();
    // No edit mode, so weights should be empty
    auto weights = ctrl->getSoftSelectionWeights();
    EXPECT_TRUE(weights.empty());
}

TEST_F(EditModeControllerTest, VertexCentroid) {
    auto meshPtr = createInMemoryTriangleMesh("EditMode_centroid");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditMode_centroid_node");
    auto* entity = sceneMgr->createEntity("EditMode_centroid_ent", meshPtr);
    node->attachObject(entity);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    // Select first two vertices: (0,0,0) and (1,0,0)
    ctrl->selectVertex(0, false);
    ctrl->selectVertex(1, true);

    auto centroid = ctrl->getSelectedVerticesCentroid();
    EXPECT_NEAR(centroid.x, 0.5f, 0.01f);
    EXPECT_NEAR(centroid.y, 0.0f, 0.01f);
    EXPECT_NEAR(centroid.z, 0.0f, 0.01f);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditMode_centroid_node");
}

TEST_F(EditModeControllerTest, TranslateSelectedVertices) {
    auto meshPtr = createInMemoryTriangleMesh("EditMode_translate_vert");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditMode_translate_vert_node");
    auto* entity = sceneMgr->createEntity("EditMode_translate_vert_ent", meshPtr);
    node->attachObject(entity);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    // Select vertex 0 at (0,0,0)
    ctrl->selectVertex(0, false);

    // Translate by (1,2,3)
    ctrl->translateSelectedVertices(Ogre::Vector3(1.0f, 2.0f, 3.0f));

    // Check the vertex moved
    auto pos = ctrl->currentMesh()->getVertexPosition(0, 0);
    EXPECT_NEAR(pos.x, 1.0f, 0.01f);
    EXPECT_NEAR(pos.y, 2.0f, 0.01f);
    EXPECT_NEAR(pos.z, 3.0f, 0.01f);

    // Other vertices should not have moved
    auto pos1 = ctrl->currentMesh()->getVertexPosition(0, 1);
    EXPECT_NEAR(pos1.x, 1.0f, 0.01f);
    EXPECT_NEAR(pos1.y, 0.0f, 0.01f);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditMode_translate_vert_node");
}

TEST_F(EditModeControllerTest, SnapshotAndRestore) {
    auto meshPtr = createInMemoryTriangleMesh("EditMode_snapshot");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditMode_snapshot_node");
    auto* entity = sceneMgr->createEntity("EditMode_snapshot_ent", meshPtr);
    node->attachObject(entity);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    ctrl->selectVertex(0, false);

    // Snapshot original positions
    auto snapshot = ctrl->snapshotVertexPositions();
    EXPECT_FALSE(snapshot.empty());

    // Modify vertex
    ctrl->translateSelectedVertices(Ogre::Vector3(5.0f, 5.0f, 5.0f));

    // Restore
    ctrl->restoreVertexPositions(snapshot);

    auto pos = ctrl->currentMesh()->getVertexPosition(0, 0);
    EXPECT_NEAR(pos.x, 0.0f, 0.01f);
    EXPECT_NEAR(pos.y, 0.0f, 0.01f);
    EXPECT_NEAR(pos.z, 0.0f, 0.01f);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditMode_snapshot_node");
}

// ---- Item 7: Normals mode ----
TEST_F(EditModeControllerTest, NormalsMode) {
    auto* ctrl = EditModeController::instance();
    EXPECT_EQ(ctrl->normalsMode(), 0); // default smooth

    ctrl->setNormalsMode(1); // flat
    EXPECT_EQ(ctrl->normalsMode(), 1);

    ctrl->setNormalsMode(0); // reset
    EXPECT_EQ(ctrl->normalsMode(), 0);

    // Invalid mode should be rejected
    ctrl->setNormalsMode(5);
    EXPECT_EQ(ctrl->normalsMode(), 0);
}

// ---- Item 8: Mesh validation ----
TEST_F(EditModeControllerTest, ValidateMesh) {
    auto meshPtr = createInMemoryTriangleMesh("EditMode_validate");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditMode_validate_node");
    auto* entity = sceneMgr->createEntity("EditMode_validate_ent", meshPtr);
    node->attachObject(entity);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    // Initially the triangle should be valid
    ctrl->validateMesh();
    EXPECT_EQ(ctrl->degenerateTriangleCount(), 0);
    EXPECT_FALSE(ctrl->hasValidationWarnings());

    // Move a vertex to create a degenerate triangle
    // Set vertex 1 to same position as vertex 0
    ctrl->currentMesh()->setVertexPosition(0, 1, Ogre::Vector3(0.0f, 0.0f, 0.0f));
    ctrl->validateMesh();
    EXPECT_GT(ctrl->degenerateTriangleCount(), 0);
    EXPECT_TRUE(ctrl->hasValidationWarnings());

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditMode_validate_node");
}

TEST_F(EditModeControllerTest, SoftSelectionWeightsInEditMode) {
    auto meshPtr = createInMemoryTriangleMesh("EditMode_soft_weights");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditMode_soft_weights_node");
    auto* entity = sceneMgr->createEntity("EditMode_soft_weights_ent", meshPtr);
    node->attachObject(entity);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    // Select vertex 0
    ctrl->selectVertex(0, false);

    // Without soft selection, only selected vertex gets weight
    auto weights = ctrl->getSoftSelectionWeights();
    EXPECT_EQ(weights.size(), 1u);
    EXPECT_FLOAT_EQ(weights[0], 1.0f);

    // Enable soft selection with large radius to include all vertices
    ctrl->setSoftSelectionEnabled(true);
    ctrl->setSoftSelectionRadius(10.0);
    weights = ctrl->getSoftSelectionWeights();
    // Should include at least the selected vertex and nearby vertices
    EXPECT_GE(weights.size(), 1u);
    EXPECT_FLOAT_EQ(weights[0], 1.0f);

    // Check that nearby vertices have weight < 1.0 but > 0
    for (const auto& [gi, w] : weights) {
        if (gi != 0) {
            EXPECT_GT(w, 0.0f);
            EXPECT_LE(w, 1.0f);
        }
    }

    // Reset
    ctrl->setSoftSelectionEnabled(false);
    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditMode_soft_weights_node");
}
