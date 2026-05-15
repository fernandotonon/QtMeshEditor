/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QTextStream>
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
        ASSERT_TRUE(tryInitOgre()) << "Ogre not available (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles()) << "Cannot create hardware buffers (Xvfb/GL required in CI)";
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

// -- weldByPosition / collapseToSingleSubmeshAndWeld ------------------------

namespace {
    EditableVertex makeV(float x, float y, float z) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, z);
        v.normal = Ogre::Vector3(0, 0, 1);
        v.hasNormal = true;
        return v;
    }
    EditableTriangle makeT(unsigned int a, unsigned int b, unsigned int c) {
        EditableTriangle t;
        t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
        return t;
    }
}

TEST(EditableMeshStandalone, VertexColorBrushFalloffHigherExponentWeakerAtEdge)
{
    EditableMesh meshLo;
    EditableMesh meshHi;
    EditableSubMesh subLo;
    EditableSubMesh subHi;
    subLo.vertices.push_back(makeV(0, 0, 0));
    subLo.vertices.push_back(makeV(0.9f, 0, 0));
    subHi.vertices = subLo.vertices;
    meshLo.subMeshes().push_back(std::move(subLo));
    meshHi.subMeshes().push_back(std::move(subHi));

    const Ogre::ColourValue red(1, 0, 0, 1);
    const Ogre::Vector3 center(0, 0, 0);
    ASSERT_TRUE(EditModeController::applyVertexColorBrush(meshLo, center, 1.0f, red, 1.0f, 0.0f));
    ASSERT_TRUE(EditModeController::applyVertexColorBrush(meshHi, center, 1.0f, red, 1.0f, 1.0f));

    const float gLo = meshLo.subMeshes()[0].vertices[1].color.g;
    const float gHi = meshHi.subMeshes()[0].vertices[1].color.g;
    EXPECT_GT(gHi, gLo);
}

// ---- Square (AABB cube) brush shape ----

TEST(EditableMeshStandalone, VertexColorBrushSquareCoversAABBCorners)
{
    // The sphere brush excludes corners of a unit-radius cube
    // (their distance to centre is sqrt(3) ≈ 1.73). The square
    // brush includes them because the test is per-axis |d| <= radius.
    EditableMesh mesh;
    EditableSubMesh sub;
    // Cube corner at (1,1,1) — outside the sphere of radius 1.
    sub.vertices.push_back(makeV(1.0f, 1.0f, 1.0f));
    mesh.subMeshes().push_back(std::move(sub));

    const Ogre::ColourValue red(1, 0, 0, 1);
    const Ogre::Vector3 center(0, 0, 0);
    // Round brush should not reach this corner.
    EXPECT_FALSE(EditModeController::applyVertexColorBrush(
        mesh, center, 1.0f, red, 1.0f, 0.0f, /*square=*/false));

    // Square brush with same radius DOES reach the corner.
    EXPECT_TRUE(EditModeController::applyVertexColorBrush(
        mesh, center, 1.0f, red, 1.0f, 0.0f, /*square=*/true));
    EXPECT_NEAR(mesh.subMeshes()[0].vertices[0].color.r, 1.0f, 1e-3f);
}

TEST(EditableMeshStandalone, VertexColorBrushSquareConstantStrengthNoFalloff)
{
    // Falloff has no effect on the square brush — every vertex
    // inside the AABB gets the same blend weight, regardless of
    // distance to centre.
    EditableMesh meshLo;
    EditableMesh meshHi;
    EditableSubMesh subLo;
    EditableSubMesh subHi;
    // One vertex near centre, one at the cube edge.
    subLo.vertices.push_back(makeV(0.1f, 0.0f, 0.0f));
    subLo.vertices.push_back(makeV(0.9f, 0.0f, 0.0f));
    subHi.vertices = subLo.vertices;
    meshLo.subMeshes().push_back(std::move(subLo));
    meshHi.subMeshes().push_back(std::move(subHi));

    const Ogre::ColourValue red(1, 0, 0, 1);
    const Ogre::Vector3 center(0, 0, 0);
    ASSERT_TRUE(EditModeController::applyVertexColorBrush(
        meshLo, center, 1.0f, red, 1.0f, 0.0f, /*square=*/true));
    ASSERT_TRUE(EditModeController::applyVertexColorBrush(
        meshHi, center, 1.0f, red, 1.0f, 1.0f, /*square=*/true));

    // Both vertices in both runs should converge on red (same blend).
    EXPECT_NEAR(meshLo.subMeshes()[0].vertices[0].color.r, 1.0f, 1e-3f);
    EXPECT_NEAR(meshLo.subMeshes()[0].vertices[1].color.r, 1.0f, 1e-3f);
    EXPECT_NEAR(meshHi.subMeshes()[0].vertices[0].color.r, 1.0f, 1e-3f);
    EXPECT_NEAR(meshHi.subMeshes()[0].vertices[1].color.r, 1.0f, 1e-3f);
}

TEST(EditableMeshStandalone, VertexColorBrushSquareSkipsOutsideAABB)
{
    EditableMesh mesh;
    EditableSubMesh sub;
    // Inside the cube of radius 1, and outside.
    sub.vertices.push_back(makeV(0.0f, 0.0f, 0.0f));   // dead-center
    sub.vertices.push_back(makeV(2.0f, 0.0f, 0.0f));   // outside on +X
    mesh.subMeshes().push_back(std::move(sub));

    const Ogre::ColourValue red(1, 0, 0, 1);
    const Ogre::Vector3 center(0, 0, 0);
    EXPECT_TRUE(EditModeController::applyVertexColorBrush(
        mesh, center, 1.0f, red, 1.0f, 0.5f, /*square=*/true));

    EXPECT_NEAR(mesh.subMeshes()[0].vertices[0].color.r, 1.0f, 1e-3f);
    // The outside vertex still has no color set.
    EXPECT_FALSE(mesh.subMeshes()[0].vertices[1].hasColor);
}

TEST(EditableMeshStandalone, WeldByPositionMergesCoincidentVerts) {
    EditableMesh em;
    EditableSubMesh sub;
    sub.materialName = "M";
    // Two coincident verts and two unique ones; two triangles sharing no edge.
    sub.vertices = {
        makeV(0, 0, 0),
        makeV(1, 0, 0),
        makeV(0, 0, 0), // duplicate of v0
        makeV(0, 1, 0),
    };
    sub.triangles = { makeT(0, 1, 3), makeT(2, 1, 3) };
    em.subMeshes().push_back(std::move(sub));

    em.weldByPosition();

    ASSERT_EQ(em.subMeshes().size(), 1u);
    EXPECT_EQ(em.subMeshes()[0].vertices.size(), 3u);
    // Both triangles should still exist and reference the same merged vertex.
    EXPECT_EQ(em.subMeshes()[0].triangles.size(), 2u);
}

TEST(EditableMeshStandalone, WeldDropsDegenerateTriangles) {
    EditableMesh em;
    EditableSubMesh sub;
    sub.vertices = {
        makeV(0, 0, 0),
        makeV(0, 0, 0), // duplicate
        makeV(1, 0, 0),
    };
    // This triangle references both coincident verts → after weld, two indices
    // collapse and it becomes a degenerate sliver.
    sub.triangles = { makeT(0, 1, 2) };
    em.subMeshes().push_back(std::move(sub));

    em.weldByPosition();
    EXPECT_TRUE(em.subMeshes()[0].triangles.empty());
}

TEST(EditableMeshStandalone, CollapseTwoSubMeshesAndWeldUnifiesVertices) {
    EditableMesh em;
    // Two submeshes that share a common corner vertex (position-wise).
    EditableSubMesh a, b;
    a.materialName = "M";
    a.vertices = { makeV(0, 0, 0), makeV(1, 0, 0), makeV(0, 1, 0) };
    a.triangles = { makeT(0, 1, 2) };
    b.materialName = "M";
    b.vertices = { makeV(0, 0, 0), makeV(0, 0, 1), makeV(1, 0, 0) };
    b.triangles = { makeT(0, 1, 2) };
    em.subMeshes().push_back(std::move(a));
    em.subMeshes().push_back(std::move(b));

    em.collapseToSingleSubmeshAndWeld();

    ASSERT_EQ(em.subMeshes().size(), 1u);
    // 3 from a + 3 from b = 6, but two pairs coincide → 4 unique verts.
    EXPECT_EQ(em.subMeshes()[0].vertices.size(), 4u);
    EXPECT_EQ(em.subMeshes()[0].triangles.size(), 2u);
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
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_triangle_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

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

    // The in-memory triangle mesh uses shared vertices
    EXPECT_TRUE(editMesh.subMeshes()[0].usesSharedVertices);

    // Cleanup
    Manager::getSingleton()->destroySceneNode("EditableMesh_triangle_node");
}

TEST_F(EditableMeshTest, VertexManipulation) {
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_manip");
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_manip_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

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

TEST_F(EditableMeshTest, VertexColorsRoundTripThroughCommitAndResize)
{
    auto meshPtr = createInMemoryTriangleMeshWithVertexColors("EditableMesh_colors");
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_colors_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));

    // Verify initial colors were read.
    const Ogre::ColourValue c0 = editMesh.getVertexColor(0, 0);
    const Ogre::ColourValue c1 = editMesh.getVertexColor(0, 1);
    const Ogre::ColourValue c2 = editMesh.getVertexColor(0, 2);
    EXPECT_NEAR(c0.r, 1.0f, 1e-6f);
    EXPECT_NEAR(c1.g, 1.0f, 1e-6f);
    EXPECT_NEAR(c2.b, 1.0f, 1e-6f);

    // Paint vertex 1 to purple and commit.
    editMesh.setVertexColor(0, 1, Ogre::ColourValue(0.5f, 0.0f, 0.5f, 1.0f));
    ASSERT_TRUE(editMesh.commitToEntity(entity));

    // Resize path should preserve colors too.
    ASSERT_TRUE(editMesh.resizeEntityBuffers(entity));

    EditableMesh reloaded;
    ASSERT_TRUE(reloaded.loadFromEntity(entity));
    const Ogre::ColourValue c0b = reloaded.getVertexColor(0, 0);
    const Ogre::ColourValue c1b = reloaded.getVertexColor(0, 1);
    const Ogre::ColourValue c2b = reloaded.getVertexColor(0, 2);
    // Diffuse is stored as 8-bit per channel on the GPU; mid-tones (e.g. 0.5) round-trip as 127/255.
    constexpr float kByteTol = (1.0f / 255.0f) + 1e-5f;
    EXPECT_NEAR(c0b.r, 1.0f, 1e-4f);
    EXPECT_NEAR(c0b.g, 0.0f, 1e-4f);
    EXPECT_NEAR(c0b.b, 0.0f, 1e-4f);
    EXPECT_NEAR(c1b.r, 0.5f, kByteTol);
    EXPECT_NEAR(c1b.g, 0.0f, 1e-4f);
    EXPECT_NEAR(c1b.b, 0.5f, kByteTol);
    EXPECT_NEAR(c2b.r, 0.0f, 1e-4f);
    EXPECT_NEAR(c2b.g, 0.0f, 1e-4f);
    EXPECT_NEAR(c2b.b, 1.0f, 1e-4f);

    Manager::getSingleton()->destroySceneNode("EditableMesh_colors_node");
}

TEST_F(EditableMeshTest, VertexColorsCommitDoesNotSwapDiffuseVertexBuffer)
{
    // Regression test: freshly imported meshes may not show paint until a full buffer
    // rebuild if we replace the VES_DIFFUSE vertex buffer binding (VAO caching).
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_colors_no_swap");
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_colors_no_swap_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));
    ASSERT_TRUE(editMesh.ensureVertexColorBuffers(entity));

    Ogre::Mesh* mesh = entity->getMesh().get();
    ASSERT_NE(mesh, nullptr);
    ASSERT_GE(mesh->getNumSubMeshes(), 1u);
    Ogre::SubMesh* sm = mesh->getSubMesh(0);
    ASSERT_NE(sm, nullptr);
    ASSERT_TRUE(!sm->useSharedVertices);
    ASSERT_NE(sm->vertexData, nullptr);
    const auto* elem = sm->vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_DIFFUSE);
    ASSERT_NE(elem, nullptr);
    const unsigned short src = elem->getSource();
    const auto before = sm->vertexData->vertexBufferBinding->getBuffer(src);
    ASSERT_TRUE(!!before);

    // Change one vertex color and commit.
    editMesh.setVertexColor(0, 0, Ogre::ColourValue(0.2f, 0.8f, 0.1f, 1.0f));
    ASSERT_TRUE(editMesh.commitVertexColorsToEntity(entity));

    const auto after = sm->vertexData->vertexBufferBinding->getBuffer(src);
    ASSERT_TRUE(!!after);
    EXPECT_EQ(before.get(), after.get());

    Manager::getSingleton()->destroySceneNode("EditableMesh_colors_no_swap_node");
}

TEST_F(EditableMeshTest, RecalculateNormals) {
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_normals");
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_normals_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

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
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_bounds_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

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
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_commit_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

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
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_skel_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

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
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_uv_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

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
        ASSERT_TRUE(tryInitOgre()) << "Ogre not available (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles()) << "Cannot create hardware buffers (Xvfb/GL required in CI)";
        createStandardOgreMaterials();

        // Make tests deterministic even if the user has persisted settings.
        auto* ctrl = EditModeController::instance();
        ctrl->setSoftSelectionEnabled(false);
        ctrl->setSoftSelectionRadius(2.0);
        ctrl->setSoftSelectionFalloff(0);
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
    auto* node = Manager::getSingleton()->addSceneNode("EditMode_enter_exit_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

    // Select the node
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    EXPECT_TRUE(ctrl->canEnterEditMode());

    // Enter edit mode
    EXPECT_TRUE(ctrl->enterEditMode());
    EXPECT_TRUE(ctrl->isEditModeActive());
    EXPECT_EQ(ctrl->modeLabel(), "Edit Mode (Vertex)");
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
    auto* node = Manager::getSingleton()->addSceneNode("EditMode_toggle_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

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
    auto* node1 = Manager::getSingleton()->addSceneNode("EditMode_auto_exit1_node");
    auto* entity1 = Manager::getSingleton()->createEntity(node1, meshPtr1);

    auto meshPtr2 = createInMemoryTriangleMesh("EditMode_auto_exit2");
    auto* node2 = Manager::getSingleton()->addSceneNode("EditMode_auto_exit2_node");
    Manager::getSingleton()->createEntity(node2, meshPtr2);

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
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_flat_normals_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

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
    auto* node = Manager::getSingleton()->addSceneNode("EditMode_centroid_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

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
    auto* node = Manager::getSingleton()->addSceneNode("EditMode_translate_vert_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

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
    auto* node = Manager::getSingleton()->addSceneNode("EditMode_snapshot_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

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
    auto* node = Manager::getSingleton()->addSceneNode("EditMode_validate_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

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
    auto* node = Manager::getSingleton()->addSceneNode("EditMode_soft_weights_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

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

// ===========================================================================
// EditableFace + n-gon helpers (chunk 1) and triangulation sync (chunk 2)
// ===========================================================================

TEST(EditableMeshStandalone, SyncTriangulationFanTriangulatesQuadFaces) {
    EditableMesh mesh;
    EditableSubMesh sub;
    EditableVertex v;
    sub.vertices = {v, v, v, v};
    EditableFace f;
    f.indices = {0, 1, 2, 3};
    sub.faces.push_back(std::move(f));
    // triangles deliberately stale (empty); syncTriangulation must
    // populate it from faces.
    mesh.subMeshes().push_back(std::move(sub));

    syncTriangulation(mesh.subMeshes());
    ASSERT_EQ(mesh.subMeshes()[0].triangles.size(), 2u)
        << "quad must yield 2 fan triangles after syncTriangulation";
}

TEST(EditableMeshStandalone, SyncTriangulationLeavesTriOnlySubmeshAlone) {
    EditableMesh mesh;
    EditableSubMesh sub;
    EditableVertex v;
    sub.vertices = {v, v, v};
    EditableTriangle t;
    t.indices[0] = 0; t.indices[1] = 1; t.indices[2] = 2;
    sub.triangles.push_back(t);
    // faces intentionally empty — triangle-only legacy submesh.
    mesh.subMeshes().push_back(std::move(sub));

    syncTriangulation(mesh.subMeshes());
    EXPECT_EQ(mesh.subMeshes()[0].triangles.size(), 1u);
    EXPECT_TRUE(mesh.subMeshes()[0].faces.empty());
}

TEST(EditableMeshStandalone, TotalFaceCountFallsBackToTriangleCountForLegacy) {
    EditableMesh mesh;
    EditableSubMesh sub;
    EditableVertex v;
    sub.vertices = {v, v, v, v, v};
    EditableTriangle t;
    t.indices[0] = 0; t.indices[1] = 1; t.indices[2] = 2;
    sub.triangles.push_back(t);
    t.indices[0] = 0; t.indices[1] = 2; t.indices[2] = 3;
    sub.triangles.push_back(t);
    t.indices[0] = 0; t.indices[1] = 3; t.indices[2] = 4;
    sub.triangles.push_back(t);
    mesh.subMeshes().push_back(std::move(sub));

    EXPECT_EQ(totalFaceCount(mesh.subMeshes()), 3u)
        << "legacy submesh: face count == triangle count";
}

TEST(EditableMeshStandalone, TotalFaceCountReportsNGonsWhenPresent) {
    EditableMesh mesh;
    EditableSubMesh sub;
    EditableVertex v;
    sub.vertices = {v, v, v, v, v};
    EditableFace pent;
    pent.indices = {0, 1, 2, 3, 4};
    sub.faces.push_back(std::move(pent));
    triangulateFaces(sub); // produces 3 fan tris
    mesh.subMeshes().push_back(std::move(sub));

    EXPECT_EQ(totalFaceCount(mesh.subMeshes()), 1u)
        << "pentagon: 1 face (n-gon canonical), not 3 (triangle mirror)";
    EXPECT_EQ(mesh.totalTriangleCount(), 3u);
}

TEST(EditableMeshStandalone, RecalculateNormalsResyncsTrianglesFromFaces) {
    // Build a quad mesh where `triangles` is intentionally stale —
    // recalculateNormals should triangulate from `faces` first so the
    // resulting normals reflect the live geometry, not the stale tris.
    EditableMesh mesh;
    EditableSubMesh sub;
    auto mkV = [](float x, float y, float z) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, z);
        v.hasNormal = true;
        v.normal = Ogre::Vector3::UNIT_Z;
        return v;
    };
    // Quad in the XY plane with normal +Z.
    sub.vertices = {
        mkV(0, 0, 0), mkV(1, 0, 0), mkV(1, 1, 0), mkV(0, 1, 0),
    };
    EditableFace f;
    f.indices = {0, 1, 2, 3};
    sub.faces.push_back(std::move(f));
    // triangles starts empty; recalculateNormals must sync it before
    // accumulating normals.
    mesh.subMeshes().push_back(std::move(sub));

    mesh.recalculateNormals();

    // After recalc, triangles must be the fan triangulation (2 tris).
    EXPECT_EQ(mesh.subMeshes()[0].triangles.size(), 2u);

    // Every vertex should end up with normal == +Z (within tolerance).
    for (const auto& v : mesh.subMeshes()[0].vertices) {
        EXPECT_NEAR(v.normal.z, 1.0f, 1e-4f);
        EXPECT_NEAR(v.normal.x, 0.0f, 1e-4f);
        EXPECT_NEAR(v.normal.y, 0.0f, 1e-4f);
    }
}

// ===========================================================================
// loadFromAssimpFile (chunk 3) — n-gon-aware re-import
// ===========================================================================

namespace {
// Write a minimal OBJ to a temp file with the given face line. Returns
// the path on success, empty on failure. The OBJ format keeps quads
// intact through Assimp's reader when aiProcess_Triangulate is off.
QString writeObj(const QString& baseName,
                 const QString& vertexLines,
                 const QString& faceLines)
{
    const QString path = QDir::tempPath() + "/" + baseName + ".obj";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    QTextStream out(&f);
    out << "# auto-generated by EditableMesh_test\n";
    out << vertexLines;
    out << faceLines;
    f.close();
    return path;
}
} // namespace

TEST(EditableMeshStandalone, LoadFromAssimpFileEmptyPathFails) {
    EditableMesh mesh;
    EXPECT_FALSE(mesh.loadFromAssimpFile(""));
    EXPECT_EQ(mesh.subMeshCount(), 0u);
}

TEST(EditableMeshStandalone, LoadFromAssimpFileMissingFileFails) {
    EditableMesh mesh;
    EXPECT_FALSE(mesh.loadFromAssimpFile(
        "/this/path/does/not/exist.obj"));
}

TEST(EditableMeshStandalone, LoadFromAssimpFilePreservesQuadFromObj) {
    // OBJ with a single quad face on 4 vertices. Without
    // aiProcess_Triangulate, Assimp should yield aiMesh::mFaces[0]
    // with mNumIndices == 4, which loadFromAssimpFile records as a
    // single 4-vertex EditableFace.
    const QString path = writeObj("editmesh_quad",
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n",
        "f 1 2 3 4\n");
    ASSERT_FALSE(path.isEmpty());

    EditableMesh mesh;
    ASSERT_TRUE(mesh.loadFromAssimpFile(path.toStdString()));
    QFile::remove(path);

    ASSERT_EQ(mesh.subMeshCount(), 1u);
    const auto& sub = mesh.subMeshes()[0];
    EXPECT_EQ(sub.vertices.size(), 4u);
    ASSERT_EQ(sub.faces.size(), 1u)
        << "OBJ quad must round-trip as a single 4-vertex EditableFace";
    EXPECT_EQ(sub.faces[0].indices.size(), 4u);
    // triangles is the fan-triangulation (chunk 1 invariant)
    EXPECT_EQ(sub.triangles.size(), 2u);
}

TEST(EditableMeshStandalone, LoadFromAssimpFileTriangleOnlyLeavesFacesEmpty) {
    // Triangle-only OBJ should follow the chunk-1 invariant: faces
    // empty, triangles canonical.
    const QString path = writeObj("editmesh_tri",
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n",
        "f 1 2 3\n");
    ASSERT_FALSE(path.isEmpty());

    EditableMesh mesh;
    ASSERT_TRUE(mesh.loadFromAssimpFile(path.toStdString()));
    QFile::remove(path);

    ASSERT_EQ(mesh.subMeshCount(), 1u);
    const auto& sub = mesh.subMeshes()[0];
    EXPECT_EQ(sub.vertices.size(), 3u);
    EXPECT_TRUE(sub.faces.empty())
        << "triangle-only mesh keeps faces empty (legacy invariant)";
    EXPECT_EQ(sub.triangles.size(), 1u);
}

TEST(EditableMeshStandalone, LoadFromAssimpFileMixedTriAndQuadKeepsBoth) {
    // OBJ with one tri and one quad — the submesh should be quad-aware
    // (faces non-empty) and triangles should mirror the fan.
    const QString path = writeObj("editmesh_mix",
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 2 0 0\nv 2 1 0\nv 1 1 0\n",
        "f 1 2 3\nf 2 4 5 6\n");
    ASSERT_FALSE(path.isEmpty());

    EditableMesh mesh;
    ASSERT_TRUE(mesh.loadFromAssimpFile(path.toStdString()));
    QFile::remove(path);

    ASSERT_EQ(mesh.subMeshCount(), 1u);
    const auto& sub = mesh.subMeshes()[0];
    EXPECT_EQ(sub.vertices.size(), 6u);
    ASSERT_EQ(sub.faces.size(), 2u);
    // Tri = 3 vertices, quad = 4.
    bool sawTri = false, sawQuad = false;
    for (const auto& f : sub.faces) {
        if (f.indices.size() == 3) sawTri = true;
        if (f.indices.size() == 4) sawQuad = true;
    }
    EXPECT_TRUE(sawTri);
    EXPECT_TRUE(sawQuad);
    // 1 tri + 2 fan tris from the quad = 3 entries.
    EXPECT_EQ(sub.triangles.size(), 3u);
}

TEST(EditableMeshStandalone, LoadFromAssimpFileAppliesZUpBakeWhenRequested) {
    // Regression for the quads follow-up: when the source asset was
    // declared Z-up (FBX UpAxis = 2), MeshProcessor bakes a +90° X
    // rotation into the rendered Ogre buffers. loadFromAssimpFile must
    // apply the SAME rotation when isZup=true so the editable mesh
    // lives in the same basis. Without this, vertex overlays would
    // appear rotated 90° on Z-up assets.
    //
    // OBJ ignores UpAxis metadata, so passing isZup=true here triggers
    // the rotation deterministically regardless of file format.
    const QString path = writeObj("editmesh_zup",
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n",  // y=1 vertex at index 2
        "f 1 2 3\n");
    ASSERT_FALSE(path.isEmpty());

    EditableMesh ymesh;
    ASSERT_TRUE(ymesh.loadFromAssimpFile(
        path.toStdString(), /*convertLH=*/false, /*isZup=*/false));
    EditableMesh zmesh;
    ASSERT_TRUE(zmesh.loadFromAssimpFile(
        path.toStdString(), /*convertLH=*/false, /*isZup=*/true));
    QFile::remove(path);

    ASSERT_EQ(ymesh.subMeshes()[0].vertices.size(), 3u);
    ASSERT_EQ(zmesh.subMeshes()[0].vertices.size(), 3u);

    // The vertex at OBJ index 3 is (0,1,0) — Y-up. After +90° X bake
    // (R_x90 * (0,1,0) = (0,0,1)) it should land on the Z axis.
    const auto& y = ymesh.subMeshes()[0].vertices[2].position;
    const auto& z = zmesh.subMeshes()[0].vertices[2].position;
    EXPECT_NEAR(y.x, 0.0f, 1e-5f);
    EXPECT_NEAR(y.y, 1.0f, 1e-5f);
    EXPECT_NEAR(y.z, 0.0f, 1e-5f);
    EXPECT_NEAR(z.x, 0.0f, 1e-5f);
    EXPECT_NEAR(z.y, 0.0f, 1e-5f);
    EXPECT_NEAR(z.z, 1.0f, 1e-5f);
}

TEST(EditableMeshStandalone, LoadFromAssimpFileSkipsBonesNotInSkeleton) {
    // Regression for the quads follow-up bone-handle bug. Without a
    // skeleton lookup, this path emitted aiBone mesh-local indices as
    // if they were Ogre bone handles. With skeletonForBoneHandles=null
    // the legacy behaviour (mesh-local indices) is preserved for
    // unskinned meshes; with a non-null skeleton, only resolvable
    // bones contribute assignments. This standalone test exercises
    // the unskinned-mesh path (OBJ has no bones), confirming the new
    // signature compiles and behaves identically when there are no
    // bones — the skinned-mesh skeleton-lookup case is exercised in
    // the EditModeController integration tests where a real skeleton
    // is available.
    const QString path = writeObj("editmesh_nobone",
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n",
        "f 1 2 3\n");
    ASSERT_FALSE(path.isEmpty());

    EditableMesh m1, m2;
    ASSERT_TRUE(m1.loadFromAssimpFile(path.toStdString()));
    ASSERT_TRUE(m2.loadFromAssimpFile(
        path.toStdString(), /*convertLH=*/true, /*isZup=*/false,
        /*skeletonForBoneHandles=*/nullptr));
    QFile::remove(path);

    // Both should produce identical output for an unskinned mesh.
    ASSERT_EQ(m1.subMeshCount(), 1u);
    ASSERT_EQ(m2.subMeshCount(), 1u);
    EXPECT_EQ(m1.subMeshes()[0].vertices.size(),
              m2.subMeshes()[0].vertices.size());
    for (const auto& v : m1.subMeshes()[0].vertices) {
        EXPECT_TRUE(v.boneAssignments.empty());
    }
}

TEST(EditableMeshStandalone, LoadFromAssimpFileReplacesPreviousContents) {
    // Loading into a non-empty EditableMesh should replace the
    // existing submeshes — like buildFromEditableMesh does.
    EditableMesh mesh;
    EditableSubMesh stale;
    EditableVertex v;
    stale.vertices = {v, v, v};
    EditableTriangle t;
    t.indices[0] = 0; t.indices[1] = 1; t.indices[2] = 2;
    stale.triangles.push_back(t);
    mesh.subMeshes().push_back(std::move(stale));
    ASSERT_EQ(mesh.subMeshCount(), 1u);

    const QString path = writeObj("editmesh_replace",
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n",
        "f 1 2 3 4\n");
    ASSERT_FALSE(path.isEmpty());
    ASSERT_TRUE(mesh.loadFromAssimpFile(path.toStdString()));
    QFile::remove(path);

    ASSERT_EQ(mesh.subMeshCount(), 1u);
    EXPECT_EQ(mesh.subMeshes()[0].vertices.size(), 4u);
}

// ===========================================================================
// commitToEntity / resizeEntityBuffers wipe qtme.source_path (chunk 4)
// ===========================================================================

TEST_F(EditableMeshTest, CommitToEntityClearsCachedSourcePath) {
    // Simulate: a freshly-imported asset has the source path tag
    // attached. Commit-to-entity (a same-vertex-count edit) must clear
    // the tag so the next enterEditMode falls back to the legacy
    // loadFromEntity path instead of re-importing and discarding the
    // user's edit.
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_commit_clear_path");
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_commit_clear_path_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

    meshPtr->getUserObjectBindings().setUserAny(
        "qtme.source_path", Ogre::Any(std::string("/some/path.fbx")));
    ASSERT_TRUE(meshPtr->getUserObjectBindings().getUserAny(
        "qtme.source_path").has_value());

    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));
    EXPECT_TRUE(editMesh.commitToEntity(entity));

    EXPECT_FALSE(meshPtr->getUserObjectBindings().getUserAny(
        "qtme.source_path").has_value())
        << "commitToEntity must wipe the cached source path so subsequent "
           "enterEditMode calls don't re-import and lose the edit";

    Manager::getSingleton()->destroySceneNode(
        "EditableMesh_commit_clear_path_node");
}

TEST_F(EditableMeshTest, ResizeEntityBuffersClearsCachedSourcePath) {
    // Same rationale as above for the topology-edit path.
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_resize_clear_path");
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_resize_clear_path_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

    meshPtr->getUserObjectBindings().setUserAny(
        "qtme.source_path", Ogre::Any(std::string("/some/path.fbx")));
    ASSERT_TRUE(meshPtr->getUserObjectBindings().getUserAny(
        "qtme.source_path").has_value());

    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));
    EXPECT_TRUE(editMesh.resizeEntityBuffers(entity));

    EXPECT_FALSE(meshPtr->getUserObjectBindings().getUserAny(
        "qtme.source_path").has_value())
        << "resizeEntityBuffers must wipe the cached source path";

    Manager::getSingleton()->destroySceneNode(
        "EditableMesh_resize_clear_path_node");
}

// ===========================================================================
// qtme.faces.<i> — n-gon face cache for exporters (chunk 6)
// ===========================================================================

TEST_F(EditableMeshTest, WriteAndReadNgonFacesRoundTrip) {
    // Pure helper round-trip: write per-submesh n-gon faces onto a
    // live Ogre::Mesh's UserObjectBindings, read them back, verify
    // payload survives unchanged.
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_ngon_roundtrip");

    std::vector<EditableSubMesh> subs(1);
    EditableFace quad;
    quad.indices = {0, 1, 2, 3};
    EditableFace tri;
    tri.indices = {2, 3, 4};
    subs[0].faces = {quad, tri};
    subs[0].vertices.resize(5); // 5 dummy verts

    writeNgonFacesToMesh(meshPtr.get(), subs);

    std::vector<std::vector<unsigned int>> readBack;
    EXPECT_TRUE(readNgonFacesFromMesh(meshPtr.get(), 0, readBack));
    ASSERT_EQ(readBack.size(), 2u);
    EXPECT_EQ(readBack[0], (std::vector<unsigned int>{0, 1, 2, 3}));
    EXPECT_EQ(readBack[1], (std::vector<unsigned int>{2, 3, 4}));
}

TEST_F(EditableMeshTest, WriteNgonFacesErasesBindingForTriOnlySubmesh) {
    // When the canonical state flips back to triangle-only (sub.faces
    // empty), any prior qtme.faces.<i> binding must be cleared so a
    // later export doesn't read stale n-gon data.
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_ngon_erase");

    std::vector<EditableSubMesh> subs(1);
    EditableFace quad;
    quad.indices = {0, 1, 2, 3};
    subs[0].faces = {quad};
    subs[0].vertices.resize(4);
    writeNgonFacesToMesh(meshPtr.get(), subs);

    // Binding present.
    std::vector<std::vector<unsigned int>> readBack;
    EXPECT_TRUE(readNgonFacesFromMesh(meshPtr.get(), 0, readBack));

    // Now flip the submesh to tri-only and write again.
    subs[0].faces.clear();
    writeNgonFacesToMesh(meshPtr.get(), subs);

    EXPECT_FALSE(readNgonFacesFromMesh(meshPtr.get(), 0, readBack));
    EXPECT_TRUE(readBack.empty());
}

TEST_F(EditableMeshTest, ReadNgonFacesReturnsFalseWhenAbsent) {
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_ngon_absent");
    std::vector<std::vector<unsigned int>> readBack;
    EXPECT_FALSE(readNgonFacesFromMesh(meshPtr.get(), 0, readBack));
    EXPECT_FALSE(readNgonFacesFromMesh(meshPtr.get(), 99, readBack));
}

TEST_F(EditableMeshTest, CommitToEntityRefreshesNgonFacesBinding) {
    // After a non-topology edit (commitToEntity), the n-gon face cache
    // must reflect the live EditableSubMesh::faces so a subsequent
    // export still sees the n-gon structure even though the
    // qtme.source_path tag is gone.
    auto meshPtr = createInMemoryTriangleMesh("EditableMesh_ngon_commit");
    auto* node = Manager::getSingleton()->addSceneNode("EditableMesh_ngon_commit_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));

    // Promote the legacy tri submesh into n-gon canonical form so
    // commit has something to write.
    auto& subs = editMesh.subMeshes();
    ASSERT_EQ(subs.size(), 1u);
    promoteTrianglesToFaces(subs[0]);
    ASSERT_FALSE(subs[0].faces.empty());

    EXPECT_TRUE(editMesh.commitToEntity(entity));

    std::vector<std::vector<unsigned int>> readBack;
    EXPECT_TRUE(readNgonFacesFromMesh(meshPtr.get(), 0, readBack));
    EXPECT_EQ(readBack.size(), subs[0].faces.size());

    Manager::getSingleton()->destroySceneNode("EditableMesh_ngon_commit_node");
}

// ===========================================================================
// faceIndexForTriangle (chunk 4b) — n-gon-aware selection mapping
// ===========================================================================

TEST(EditableMeshStandalone, FaceIndexForTriangleLegacyTriangleSubmeshIsIdentity) {
    EditableSubMesh sub;
    EditableVertex v;
    sub.vertices = {v, v, v, v};
    EditableTriangle t1, t2;
    t1.indices[0] = 0; t1.indices[1] = 1; t1.indices[2] = 2;
    t2.indices[0] = 0; t2.indices[1] = 2; t2.indices[2] = 3;
    sub.triangles = {t1, t2};
    // faces deliberately empty — legacy triangle-only mode.

    size_t firstTri = 99, count = 99;
    EXPECT_EQ(faceIndexForTriangle(sub, 0, &firstTri, &count), -1);
    EXPECT_EQ(firstTri, 0u);
    EXPECT_EQ(count, 1u);

    EXPECT_EQ(faceIndexForTriangle(sub, 1, &firstTri, &count), -1);
    EXPECT_EQ(firstTri, 1u);
    EXPECT_EQ(count, 1u);
}

TEST(EditableMeshStandalone, FaceIndexForTriangleQuadMapsBothTrianglesToSameFace) {
    EditableSubMesh sub;
    EditableVertex v;
    sub.vertices = {v, v, v, v};
    EditableFace f;
    f.indices = {0, 1, 2, 3};
    sub.faces.push_back(std::move(f));
    triangulateFaces(sub); // produces 2 fan triangles

    ASSERT_EQ(sub.triangles.size(), 2u);

    // Both fan triangles map to face 0.
    size_t firstTri = 99, count = 99;
    EXPECT_EQ(faceIndexForTriangle(sub, 0, &firstTri, &count), 0);
    EXPECT_EQ(firstTri, 0u);
    EXPECT_EQ(count, 2u) << "quad's owning face spans 2 triangles";

    EXPECT_EQ(faceIndexForTriangle(sub, 1, &firstTri, &count), 0);
    EXPECT_EQ(firstTri, 0u);
    EXPECT_EQ(count, 2u);
}

TEST(EditableMeshStandalone, FaceIndexForTriangleMixedTriQuadMapsCorrectly) {
    EditableSubMesh sub;
    EditableVertex v;
    sub.vertices = {v, v, v, v, v, v, v};
    EditableFace tri;
    tri.indices = {0, 1, 2};
    EditableFace quad;
    quad.indices = {3, 4, 5, 6};
    sub.faces.push_back(std::move(tri));
    sub.faces.push_back(std::move(quad));
    triangulateFaces(sub); // 1 + 2 = 3 fan triangles

    ASSERT_EQ(sub.triangles.size(), 3u);

    // Triangle 0 → face 0 (the lone triangle).
    size_t firstTri = 99, count = 99;
    EXPECT_EQ(faceIndexForTriangle(sub, 0, &firstTri, &count), 0);
    EXPECT_EQ(firstTri, 0u);
    EXPECT_EQ(count, 1u);

    // Triangles 1 + 2 → face 1 (the quad).
    EXPECT_EQ(faceIndexForTriangle(sub, 1, &firstTri, &count), 1);
    EXPECT_EQ(firstTri, 1u);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(faceIndexForTriangle(sub, 2, &firstTri, &count), 1);
    EXPECT_EQ(firstTri, 1u);
    EXPECT_EQ(count, 2u);
}

TEST(EditableMeshStandalone, FaceIndexForTriangleOutOfRangeIsBenign) {
    EditableSubMesh sub;
    EditableVertex v;
    sub.vertices = {v, v, v, v};
    EditableFace f;
    f.indices = {0, 1, 2, 3};
    sub.faces.push_back(std::move(f));
    triangulateFaces(sub);

    // Out-of-range triangle index returns -1 with a defensive
    // single-triangle fallback.
    size_t firstTri = 0, count = 0;
    EXPECT_EQ(faceIndexForTriangle(sub, 99, &firstTri, &count), -1);
    EXPECT_EQ(firstTri, 99u);
    EXPECT_EQ(count, 1u);
}

TEST(EditableMeshStandalone, FaceIndexForTriangleAcceptsNullOutPointers) {
    EditableSubMesh sub;
    EditableVertex v;
    sub.vertices = {v, v, v, v};
    EditableFace f;
    f.indices = {0, 1, 2, 3};
    sub.faces.push_back(std::move(f));
    triangulateFaces(sub);

    // Caller can pass nullptr for either output if they don't care.
    EXPECT_EQ(faceIndexForTriangle(sub, 0, nullptr, nullptr), 0);
    EXPECT_EQ(faceIndexForTriangle(sub, 1, nullptr, nullptr), 0);
}

TEST(EditableMeshStandalone, FaceIndexForTriangleSkipsInvalidFaces) {
    // Regression for chunk-4b drift bug: a face with consecutive
    // duplicate indices passes `n >= 3` but fails isValid(), so it
    // produces zero triangles in `triangles`. faceIndexForTriangle
    // must skip it the same way triangulateFaces does — otherwise the
    // mapping for triangles after it points to the wrong source face.
    EditableSubMesh sub;
    EditableVertex v;
    sub.vertices = {v, v, v, v, v, v};
    EditableFace bad;        // 4 indices but [0]==[1] — !isValid()
    bad.indices = {0, 0, 1, 2};
    EditableFace good;       // valid quad → 2 fan triangles
    good.indices = {2, 3, 4, 5};
    sub.faces.push_back(std::move(bad));
    sub.faces.push_back(std::move(good));
    triangulateFaces(sub);

    ASSERT_EQ(sub.triangles.size(), 2u);
    size_t firstTri = 0, count = 0;
    // Both triangles must map to face index 1 (the good face),
    // not face 0 (which contributed nothing to `triangles`).
    EXPECT_EQ(faceIndexForTriangle(sub, 0, &firstTri, &count), 1);
    EXPECT_EQ(firstTri, 0u);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(faceIndexForTriangle(sub, 1, &firstTri, &count), 1);
    EXPECT_EQ(firstTri, 0u);
    EXPECT_EQ(count, 2u);
}

// ============================================================================
// mergeCoplanarTrianglesToQuads
// ============================================================================

namespace {
EditableVertex mkPosV(float x, float y, float z) {
    EditableVertex v;
    v.position = Ogre::Vector3(x, y, z);
    v.normal = Ogre::Vector3::UNIT_Z;
    v.hasNormal = true;
    return v;
}
} // namespace

TEST(EditableMeshStandalone, MergeCoplanarTrianglesProducesQuad) {
    // Two triangles forming a planar unit quad on the XY plane.
    EditableSubMesh sub;
    sub.vertices = {
        mkPosV(0, 0, 0), mkPosV(1, 0, 0), mkPosV(1, 1, 0), mkPosV(0, 1, 0),
    };
    EditableTriangle t1{}, t2{};
    t1.indices[0] = 0; t1.indices[1] = 1; t1.indices[2] = 2;
    t2.indices[0] = 0; t2.indices[1] = 2; t2.indices[2] = 3;
    sub.triangles = {t1, t2};

    const int merged = mergeCoplanarTrianglesToQuads(sub);
    EXPECT_EQ(merged, 1);
    ASSERT_EQ(sub.faces.size(), 1u);
    EXPECT_EQ(sub.faces[0].indices.size(), 4u);
    // triangulation mirror is rebuilt
    EXPECT_EQ(sub.triangles.size(), 2u);
}

TEST(EditableMeshStandalone, MergeCoplanarLeavesNonCoplanarAsTris) {
    // Two triangles sharing edge (0,1) but bent at 90°.
    EditableSubMesh sub;
    sub.vertices = {
        mkPosV(0, 0, 0), mkPosV(1, 0, 0), mkPosV(0, 1, 0), mkPosV(0, 0, 1),
    };
    EditableTriangle t1{}, t2{};
    t1.indices[0] = 0; t1.indices[1] = 1; t1.indices[2] = 2;
    // Second tri lifted off the XY plane along Z — 90° dihedral.
    t2.indices[0] = 1; t2.indices[1] = 0; t2.indices[2] = 3;
    sub.triangles = {t1, t2};

    const int merged = mergeCoplanarTrianglesToQuads(sub, 1.0f);
    EXPECT_EQ(merged, 0);
    ASSERT_EQ(sub.faces.size(), 2u);
    EXPECT_EQ(sub.faces[0].indices.size(), 3u);
    EXPECT_EQ(sub.faces[1].indices.size(), 3u);
}

TEST(EditableMeshStandalone, MergeCoplanarHandlesTriangulatedCube) {
    // Standard 8-vert cube triangulated as 12 tris (2 per face).
    // mergeCoplanarTrianglesToQuads should reconstruct 6 quads.
    EditableSubMesh sub;
    sub.vertices = {
        mkPosV(-1,-1,-1), mkPosV( 1,-1,-1), mkPosV( 1, 1,-1), mkPosV(-1, 1,-1),
        mkPosV(-1,-1, 1), mkPosV( 1,-1, 1), mkPosV( 1, 1, 1), mkPosV(-1, 1, 1),
    };
    auto T = [](unsigned a, unsigned b, unsigned c) {
        EditableTriangle t{}; t.indices[0]=a; t.indices[1]=b; t.indices[2]=c;
        return t;
    };
    // Each face split along a single diagonal — winding outward.
    sub.triangles = {
        T(0,2,1), T(0,3,2),    // back  (-Z)
        T(4,5,6), T(4,6,7),    // front (+Z)
        T(0,1,5), T(0,5,4),    // bottom (-Y)
        T(2,3,7), T(2,7,6),    // top    (+Y)
        T(0,4,7), T(0,7,3),    // left   (-X)
        T(1,2,6), T(1,6,5),    // right  (+X)
    };

    const int merged = mergeCoplanarTrianglesToQuads(sub, 1.0f);
    EXPECT_EQ(merged, 6);
    EXPECT_EQ(sub.faces.size(), 6u);
    for (const auto& f : sub.faces) {
        EXPECT_EQ(f.indices.size(), 4u);
    }
    // 6 quads × 2 fan tris = 12 — same as input.
    EXPECT_EQ(sub.triangles.size(), 12u);
}

TEST(EditableMeshStandalone, MergeCoplanarRespectsAngleThreshold) {
    // Two triangles bent by ~5° dihedral. With strict threshold (1°),
    // they don't merge; with loose threshold (10°), they do.
    EditableSubMesh sub;
    sub.vertices = {
        mkPosV(0, 0, 0),
        mkPosV(1, 0, 0),
        mkPosV(1, 1, 0),
        // 4th vertex tilted up in z by tan(5°) ≈ 0.0875
        mkPosV(0, 1, 0.0875f),
    };
    EditableTriangle t1{}, t2{};
    t1.indices[0] = 0; t1.indices[1] = 1; t1.indices[2] = 2;
    t2.indices[0] = 0; t2.indices[1] = 2; t2.indices[2] = 3;
    sub.triangles = {t1, t2};

    EditableSubMesh strict = sub;
    EXPECT_EQ(mergeCoplanarTrianglesToQuads(strict, 1.0f), 0);
    EXPECT_EQ(strict.faces.size(), 2u);

    EditableSubMesh loose = sub;
    EXPECT_EQ(mergeCoplanarTrianglesToQuads(loose, 10.0f), 1);
    EXPECT_EQ(loose.faces.size(), 1u);
}

TEST(EditableMeshStandalone, MergeCoplanarEmptySubMesh) {
    EditableSubMesh sub;
    EXPECT_EQ(mergeCoplanarTrianglesToQuads(sub), 0);
    EXPECT_TRUE(sub.faces.empty());
    EXPECT_TRUE(sub.triangles.empty());
}
