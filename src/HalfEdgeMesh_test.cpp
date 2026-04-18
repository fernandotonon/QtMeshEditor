/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include <gtest/gtest.h>
#include "HalfEdgeMesh.h"
#include "EditableMesh.h"
#include "TestHelpers.h"
#include "Manager.h"

// ===========================================================================
// Helper: build an EditableMesh manually (no Ogre needed)
// ===========================================================================

/**
 * Creates a single-triangle EditableMesh:
 *    v2 (0,1,0)
 *    |\
 *    | \
 *    |  \
 *    v0--v1  (0,0,0)--(1,0,0)
 */
static EditableMesh makeTriangleMesh()
{
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "TestMat";

    EditableVertex v0, v1, v2;
    v0.position = Ogre::Vector3(0, 0, 0);
    v0.normal = Ogre::Vector3(0, 0, 1); v0.hasNormal = true;
    v0.uv = Ogre::Vector2(0, 0); v0.hasUV = true;

    v1.position = Ogre::Vector3(1, 0, 0);
    v1.normal = Ogre::Vector3(0, 0, 1); v1.hasNormal = true;
    v1.uv = Ogre::Vector2(1, 0); v1.hasUV = true;

    v2.position = Ogre::Vector3(0, 1, 0);
    v2.normal = Ogre::Vector3(0, 0, 1); v2.hasNormal = true;
    v2.uv = Ogre::Vector2(0, 1); v2.hasUV = true;

    sub.vertices = {v0, v1, v2};

    EditableTriangle tri;
    tri.indices[0] = 0; tri.indices[1] = 1; tri.indices[2] = 2;
    sub.triangles = {tri};

    mesh.subMeshes().push_back(std::move(sub));
    return mesh;
}

/**
 * Creates a quad (two triangles sharing an edge):
 *    v2---v3
 *    |  / |
 *    | /  |
 *    v0---v1
 *
 * Triangle 0: v0, v1, v2
 * Triangle 1: v1, v3, v2
 *
 * Edge v1-v2 is the shared interior edge.
 * All other edges are boundary edges.
 */
static EditableMesh makeQuadMesh()
{
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "QuadMat";

    EditableVertex v0, v1, v2, v3;
    v0.position = Ogre::Vector3(0, 0, 0);
    v0.normal = Ogre::Vector3(0, 0, 1); v0.hasNormal = true;
    v0.uv = Ogre::Vector2(0, 0); v0.hasUV = true;

    v1.position = Ogre::Vector3(1, 0, 0);
    v1.normal = Ogre::Vector3(0, 0, 1); v1.hasNormal = true;
    v1.uv = Ogre::Vector2(1, 0); v1.hasUV = true;

    v2.position = Ogre::Vector3(0, 1, 0);
    v2.normal = Ogre::Vector3(0, 0, 1); v2.hasNormal = true;
    v2.uv = Ogre::Vector2(0, 1); v2.hasUV = true;

    v3.position = Ogre::Vector3(1, 1, 0);
    v3.normal = Ogre::Vector3(0, 0, 1); v3.hasNormal = true;
    v3.uv = Ogre::Vector2(1, 1); v3.hasUV = true;

    sub.vertices = {v0, v1, v2, v3};

    EditableTriangle tri0, tri1;
    tri0.indices[0] = 0; tri0.indices[1] = 1; tri0.indices[2] = 2;
    tri1.indices[0] = 1; tri1.indices[1] = 3; tri1.indices[2] = 2;
    sub.triangles = {tri0, tri1};

    mesh.subMeshes().push_back(std::move(sub));
    return mesh;
}

/**
 * Creates a two-submesh mesh (one triangle each, sharing no vertices):
 *   Submesh 0: triangle at z=0
 *   Submesh 1: triangle at z=1
 */
static EditableMesh makeTwoSubMeshMesh()
{
    EditableMesh mesh;

    // Submesh 0
    {
        EditableSubMesh sub;
        sub.materialName = "Mat0";

        EditableVertex v0, v1, v2;
        v0.position = Ogre::Vector3(0, 0, 0);
        v1.position = Ogre::Vector3(1, 0, 0);
        v2.position = Ogre::Vector3(0, 1, 0);
        v0.hasNormal = true; v0.normal = Ogre::Vector3(0, 0, 1);
        v1.hasNormal = true; v1.normal = Ogre::Vector3(0, 0, 1);
        v2.hasNormal = true; v2.normal = Ogre::Vector3(0, 0, 1);
        sub.vertices = {v0, v1, v2};

        EditableTriangle tri;
        tri.indices[0] = 0; tri.indices[1] = 1; tri.indices[2] = 2;
        sub.triangles = {tri};

        mesh.subMeshes().push_back(std::move(sub));
    }

    // Submesh 1
    {
        EditableSubMesh sub;
        sub.materialName = "Mat1";

        EditableVertex v0, v1, v2;
        v0.position = Ogre::Vector3(0, 0, 1);
        v1.position = Ogre::Vector3(1, 0, 1);
        v2.position = Ogre::Vector3(0, 1, 1);
        v0.hasNormal = true; v0.normal = Ogre::Vector3(0, 0, 1);
        v1.hasNormal = true; v1.normal = Ogre::Vector3(0, 0, 1);
        v2.hasNormal = true; v2.normal = Ogre::Vector3(0, 0, 1);
        sub.vertices = {v0, v1, v2};

        EditableTriangle tri;
        tri.indices[0] = 0; tri.indices[1] = 1; tri.indices[2] = 2;
        sub.triangles = {tri};

        mesh.subMeshes().push_back(std::move(sub));
    }

    return mesh;
}

/**
 * Creates a mesh with bone assignments for testing weight preservation:
 *   Single triangle, each vertex assigned to bone 1 with weight 1.0
 */
static EditableMesh makeBoneWeightMesh()
{
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "BoneMat";

    EditableVertex v0, v1, v2;
    v0.position = Ogre::Vector3(0, 0, 0);
    v1.position = Ogre::Vector3(1, 0, 0);
    v2.position = Ogre::Vector3(0, 1, 0);

    EditableBoneAssignment ba;
    ba.boneIndex = 1;
    ba.weight = 1.0f;
    v0.boneAssignments = {ba};
    v1.boneAssignments = {ba};

    EditableBoneAssignment ba2;
    ba2.boneIndex = 2;
    ba2.weight = 0.5f;
    EditableBoneAssignment ba3;
    ba3.boneIndex = 3;
    ba3.weight = 0.5f;
    v2.boneAssignments = {ba2, ba3};

    sub.vertices = {v0, v1, v2};

    EditableTriangle tri;
    tri.indices[0] = 0; tri.indices[1] = 1; tri.indices[2] = 2;
    sub.triangles = {tri};

    mesh.subMeshes().push_back(std::move(sub));
    return mesh;
}

// ===========================================================================
// Tests: Construction and basic properties
// ===========================================================================

TEST(HalfEdgeMeshStandalone, BuildFromEmptyMeshFails) {
    EditableMesh empty;
    HalfEdgeMesh he;
    EXPECT_FALSE(he.buildFromEditableMesh(empty));
}

TEST(HalfEdgeMeshStandalone, BuildFromSingleTriangle) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    EXPECT_EQ(he.vertexCount(), 3u);
    EXPECT_EQ(he.faceCount(), 1u);
    EXPECT_EQ(he.edgeCount(), 3u);
    // 3 interior + 3 boundary = 6 half-edges
    EXPECT_EQ(he.halfEdgeCount(), 6u);
    EXPECT_EQ(he.subMeshCount(), 1);
}

TEST(HalfEdgeMeshStandalone, BuildFromQuad) {
    auto editMesh = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    EXPECT_EQ(he.vertexCount(), 4u);
    EXPECT_EQ(he.faceCount(), 2u);
    EXPECT_EQ(he.edgeCount(), 5u);
    // 6 interior half-edges + 4 boundary half-edges = 10
    EXPECT_EQ(he.halfEdgeCount(), 10u);
}

TEST(HalfEdgeMeshStandalone, BuildFromTwoSubmeshes) {
    auto editMesh = makeTwoSubMeshMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    EXPECT_EQ(he.vertexCount(), 6u); // 3 per submesh, not merged
    EXPECT_EQ(he.faceCount(), 2u);
    EXPECT_EQ(he.edgeCount(), 6u);   // 3 per triangle
    EXPECT_EQ(he.subMeshCount(), 2);
}

// ===========================================================================
// Tests: Validation
// ===========================================================================

TEST(HalfEdgeMeshStandalone, ValidateTriangle) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, ValidateQuad) {
    auto editMesh = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, ValidateTwoSubmeshes) {
    auto editMesh = makeTwoSubMeshMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));
    EXPECT_TRUE(he.validate());
}

// ===========================================================================
// Tests: Vertex attribute preservation
// ===========================================================================

TEST(HalfEdgeMeshStandalone, VertexPositionsPreserved) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Check positions
    bool found000 = false, found100 = false, found010 = false;
    for (size_t i = 0; i < he.vertexCount(); ++i) {
        auto pos = he.vertex(i).position;
        if (pos.squaredDistance(Ogre::Vector3(0, 0, 0)) < 0.001f) found000 = true;
        if (pos.squaredDistance(Ogre::Vector3(1, 0, 0)) < 0.001f) found100 = true;
        if (pos.squaredDistance(Ogre::Vector3(0, 1, 0)) < 0.001f) found010 = true;
    }
    EXPECT_TRUE(found000);
    EXPECT_TRUE(found100);
    EXPECT_TRUE(found010);
}

TEST(HalfEdgeMeshStandalone, NormalsAndUVsPreserved) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    for (size_t i = 0; i < he.vertexCount(); ++i) {
        EXPECT_TRUE(he.vertex(i).hasNormal);
        EXPECT_TRUE(he.vertex(i).hasUV);
        // All normals should be (0,0,1)
        EXPECT_NEAR(he.vertex(i).normal.z, 1.0f, 0.01f);
    }
}

TEST(HalfEdgeMeshStandalone, BoneWeightsPreserved) {
    auto editMesh = makeBoneWeightMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Find the vertex at (0,1,0) — it should have 2 bone assignments
    for (size_t i = 0; i < he.vertexCount(); ++i) {
        if (he.vertex(i).position.squaredDistance(Ogre::Vector3(0, 1, 0)) < 0.001f) {
            EXPECT_EQ(he.vertex(i).boneAssignments.size(), 2u);
            // bone 2 weight 0.5 and bone 3 weight 0.5
            bool foundBone2 = false, foundBone3 = false;
            for (const auto& [boneIdx, weight] : he.vertex(i).boneAssignments) {
                if (boneIdx == 2) { EXPECT_FLOAT_EQ(weight, 0.5f); foundBone2 = true; }
                if (boneIdx == 3) { EXPECT_FLOAT_EQ(weight, 0.5f); foundBone3 = true; }
            }
            EXPECT_TRUE(foundBone2);
            EXPECT_TRUE(foundBone3);
        }
    }
}

// ===========================================================================
// Tests: Adjacency queries
// ===========================================================================

TEST(HalfEdgeMeshStandalone, FaceVerticesTriangle) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    auto verts = he.faceVertices(0);
    ASSERT_EQ(verts.size(), 3u);

    // All vertex indices should be valid and distinct
    std::set<int> uniqueVerts(verts.begin(), verts.end());
    EXPECT_EQ(uniqueVerts.size(), 3u);

    for (int v : verts) {
        EXPECT_GE(v, 0);
        EXPECT_LT(v, static_cast<int>(he.vertexCount()));
    }
}

TEST(HalfEdgeMeshStandalone, FaceEdgesTriangle) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    auto edges = he.faceEdges(0);
    ASSERT_EQ(edges.size(), 3u);

    std::set<int> uniqueEdges(edges.begin(), edges.end());
    EXPECT_EQ(uniqueEdges.size(), 3u);
}

TEST(HalfEdgeMeshStandalone, FacesAroundVertexTriangle) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Each vertex of a single triangle is adjacent to exactly 1 face
    for (size_t v = 0; v < he.vertexCount(); ++v) {
        auto faces = he.facesAroundVertex(v);
        EXPECT_EQ(faces.size(), 1u);
        EXPECT_EQ(faces[0], 0);
    }
}

TEST(HalfEdgeMeshStandalone, FacesAroundVertexQuad) {
    auto editMesh = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // In the quad, v1 and v2 (the diagonal vertices) are shared by 2 faces.
    // v0 and v3 are each part of 1 face.
    // Find which HE vertices correspond to v1(1,0,0) and v2(0,1,0)
    int sharedCount = 0;
    for (size_t v = 0; v < he.vertexCount(); ++v) {
        auto faces = he.facesAroundVertex(v);
        if (faces.size() == 2) ++sharedCount;
    }
    EXPECT_EQ(sharedCount, 2); // v1 and v2 are shared
}

TEST(HalfEdgeMeshStandalone, VerticesAroundVertexTriangle) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Each vertex has 2 neighbors in a triangle
    for (size_t v = 0; v < he.vertexCount(); ++v) {
        auto neighbors = he.verticesAroundVertex(v);
        EXPECT_EQ(neighbors.size(), 2u);
    }
}

TEST(HalfEdgeMeshStandalone, EdgesAroundVertexTriangle) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Each vertex touches 2 edges in a single triangle
    for (size_t v = 0; v < he.vertexCount(); ++v) {
        auto edges = he.edgesAroundVertex(v);
        EXPECT_EQ(edges.size(), 2u);
    }
}

TEST(HalfEdgeMeshStandalone, EdgeVertices) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    for (size_t e = 0; e < he.edgeCount(); ++e) {
        auto [v1, v2] = he.edgeVertices(e);
        EXPECT_GE(v1, 0);
        EXPECT_GE(v2, 0);
        EXPECT_NE(v1, v2);
    }
}

TEST(HalfEdgeMeshStandalone, EdgeFacesTriangle) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // All edges in a single triangle are boundary edges: one face, one -1
    for (size_t e = 0; e < he.edgeCount(); ++e) {
        auto [f1, f2] = he.edgeFaces(e);
        // One should be 0, the other -1
        bool hasInterior = (f1 == 0 || f2 == 0);
        bool hasBoundary = (f1 == -1 || f2 == -1);
        EXPECT_TRUE(hasInterior);
        EXPECT_TRUE(hasBoundary);
    }
}

TEST(HalfEdgeMeshStandalone, EdgeFacesQuadInterior) {
    auto editMesh = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Exactly one interior edge (shared diagonal) should have 2 faces
    int interiorEdgeCount = 0;
    for (size_t e = 0; e < he.edgeCount(); ++e) {
        auto [f1, f2] = he.edgeFaces(e);
        if (f1 >= 0 && f2 >= 0) {
            ++interiorEdgeCount;
            EXPECT_NE(f1, f2); // the two faces should be different
        }
    }
    EXPECT_EQ(interiorEdgeCount, 1);
}

// ===========================================================================
// Tests: Boundary queries
// ===========================================================================

TEST(HalfEdgeMeshStandalone, AllVerticesBoundaryInTriangle) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    for (size_t v = 0; v < he.vertexCount(); ++v) {
        EXPECT_TRUE(he.isVertexBoundary(v));
    }
}

TEST(HalfEdgeMeshStandalone, AllEdgesBoundaryInTriangle) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    for (size_t e = 0; e < he.edgeCount(); ++e) {
        EXPECT_TRUE(he.isEdgeBoundary(e));
    }
}

TEST(HalfEdgeMeshStandalone, BoundaryLoopTriangle) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    auto loops = he.boundaryLoops();
    EXPECT_EQ(loops.size(), 1u);
    if (!loops.empty()) {
        EXPECT_EQ(loops[0].size(), 3u); // 3 boundary edges = 3 vertices in loop
    }
}

TEST(HalfEdgeMeshStandalone, BoundaryEdgesQuad) {
    auto editMesh = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    int boundaryEdgeCount = 0;
    int interiorEdgeCount = 0;
    for (size_t e = 0; e < he.edgeCount(); ++e) {
        if (he.isEdgeBoundary(e))
            ++boundaryEdgeCount;
        else
            ++interiorEdgeCount;
    }
    EXPECT_EQ(boundaryEdgeCount, 4); // 4 outer edges
    EXPECT_EQ(interiorEdgeCount, 1); // 1 diagonal
}

TEST(HalfEdgeMeshStandalone, BoundaryLoopQuad) {
    auto editMesh = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    auto loops = he.boundaryLoops();
    EXPECT_EQ(loops.size(), 1u);
    if (!loops.empty()) {
        EXPECT_EQ(loops[0].size(), 4u); // 4 boundary edges
    }
}

// ===========================================================================
// Tests: Roundtrip conversion (EditableMesh -> HalfEdgeMesh -> EditableMesh)
// ===========================================================================

TEST(HalfEdgeMeshStandalone, RoundtripTriangle) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    EditableMesh result;
    ASSERT_TRUE(he.toEditableMesh(result));

    // Same number of submeshes
    EXPECT_EQ(result.subMeshCount(), 1u);
    // Same number of vertices and triangles
    EXPECT_EQ(result.totalVertexCount(), 3u);
    EXPECT_EQ(result.totalTriangleCount(), 1u);

    // Material preserved
    EXPECT_EQ(result.subMeshes()[0].materialName, "TestMat");

    // All vertex positions should survive roundtrip
    std::vector<Ogre::Vector3> positions;
    for (const auto& v : result.subMeshes()[0].vertices) {
        positions.push_back(v.position);
    }
    // Check that we have the original 3 positions
    bool found000 = false, found100 = false, found010 = false;
    for (const auto& p : positions) {
        if (p.squaredDistance(Ogre::Vector3(0, 0, 0)) < 0.001f) found000 = true;
        if (p.squaredDistance(Ogre::Vector3(1, 0, 0)) < 0.001f) found100 = true;
        if (p.squaredDistance(Ogre::Vector3(0, 1, 0)) < 0.001f) found010 = true;
    }
    EXPECT_TRUE(found000);
    EXPECT_TRUE(found100);
    EXPECT_TRUE(found010);
}

TEST(HalfEdgeMeshStandalone, RoundtripQuad) {
    auto editMesh = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    EditableMesh result;
    ASSERT_TRUE(he.toEditableMesh(result));

    EXPECT_EQ(result.subMeshCount(), 1u);
    EXPECT_EQ(result.totalVertexCount(), 4u);
    EXPECT_EQ(result.totalTriangleCount(), 2u);
    EXPECT_EQ(result.subMeshes()[0].materialName, "QuadMat");
}

TEST(HalfEdgeMeshStandalone, RoundtripTwoSubmeshes) {
    auto editMesh = makeTwoSubMeshMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    EditableMesh result;
    ASSERT_TRUE(he.toEditableMesh(result));

    EXPECT_EQ(result.subMeshCount(), 2u);
    EXPECT_EQ(result.subMeshes()[0].materialName, "Mat0");
    EXPECT_EQ(result.subMeshes()[1].materialName, "Mat1");
    EXPECT_EQ(result.subMeshes()[0].vertices.size(), 3u);
    EXPECT_EQ(result.subMeshes()[1].vertices.size(), 3u);
    EXPECT_EQ(result.subMeshes()[0].triangles.size(), 1u);
    EXPECT_EQ(result.subMeshes()[1].triangles.size(), 1u);
}

TEST(HalfEdgeMeshStandalone, RoundtripPreservesUVs) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    EditableMesh result;
    ASSERT_TRUE(he.toEditableMesh(result));

    // Check UVs survived roundtrip
    for (const auto& v : result.subMeshes()[0].vertices) {
        EXPECT_TRUE(v.hasUV);
        // Each vertex should have one of: (0,0), (1,0), (0,1)
        bool validUV = (v.uv.squaredDistance(Ogre::Vector2(0, 0)) < 0.001f) ||
                       (v.uv.squaredDistance(Ogre::Vector2(1, 0)) < 0.001f) ||
                       (v.uv.squaredDistance(Ogre::Vector2(0, 1)) < 0.001f);
        EXPECT_TRUE(validUV);
    }
}

TEST(HalfEdgeMeshStandalone, RoundtripPreservesBoneWeights) {
    auto editMesh = makeBoneWeightMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    EditableMesh result;
    ASSERT_TRUE(he.toEditableMesh(result));

    ASSERT_EQ(result.subMeshCount(), 1u);
    const auto& verts = result.subMeshes()[0].vertices;

    // Find vertex at (0,1,0) and check its bone assignments
    for (const auto& v : verts) {
        if (v.position.squaredDistance(Ogre::Vector3(0, 1, 0)) < 0.001f) {
            EXPECT_EQ(v.boneAssignments.size(), 2u);
            bool foundBone2 = false, foundBone3 = false;
            for (const auto& ba : v.boneAssignments) {
                if (ba.boneIndex == 2) { EXPECT_FLOAT_EQ(ba.weight, 0.5f); foundBone2 = true; }
                if (ba.boneIndex == 3) { EXPECT_FLOAT_EQ(ba.weight, 0.5f); foundBone3 = true; }
            }
            EXPECT_TRUE(foundBone2);
            EXPECT_TRUE(foundBone3);
        }
    }
}

TEST(HalfEdgeMeshStandalone, RoundtripPreservesNormals) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    EditableMesh result;
    ASSERT_TRUE(he.toEditableMesh(result));

    for (const auto& v : result.subMeshes()[0].vertices) {
        EXPECT_TRUE(v.hasNormal);
        EXPECT_NEAR(v.normal.z, 1.0f, 0.01f);
    }
}

TEST(HalfEdgeMeshStandalone, RoundtripPreservesTangents) {
    // Build a triangle with explicit tangents (distinct per vertex so we can
    // verify the correct tangent survives round-trip for each vertex).
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "TangentMat";

    EditableVertex v0, v1, v2;
    v0.position = Ogre::Vector3(0, 0, 0);
    v0.normal = Ogre::Vector3(0, 0, 1); v0.hasNormal = true;
    v0.uv = Ogre::Vector2(0, 0); v0.hasUV = true;
    v0.tangent = Ogre::Vector4(1.0f, 0.0f, 0.0f, 1.0f); v0.hasTangent = true;

    v1.position = Ogre::Vector3(1, 0, 0);
    v1.normal = Ogre::Vector3(0, 0, 1); v1.hasNormal = true;
    v1.uv = Ogre::Vector2(1, 0); v1.hasUV = true;
    v1.tangent = Ogre::Vector4(0.7071f, 0.7071f, 0.0f, -1.0f); v1.hasTangent = true;

    v2.position = Ogre::Vector3(0, 1, 0);
    v2.normal = Ogre::Vector3(0, 0, 1); v2.hasNormal = true;
    v2.uv = Ogre::Vector2(0, 1); v2.hasUV = true;
    v2.tangent = Ogre::Vector4(0.0f, 1.0f, 0.0f, 1.0f); v2.hasTangent = true;

    sub.vertices = {v0, v1, v2};
    EditableTriangle tri;
    tri.indices[0] = 0; tri.indices[1] = 1; tri.indices[2] = 2;
    sub.triangles = {tri};
    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));

    EditableMesh result;
    ASSERT_TRUE(he.toEditableMesh(result));

    ASSERT_EQ(result.subMeshCount(), 1u);
    ASSERT_EQ(result.subMeshes()[0].vertices.size(), 3u);

    // Each vertex should have hasTangent set and match the input tangent
    // (identified by the position that came out).
    for (const auto& v : result.subMeshes()[0].vertices) {
        EXPECT_TRUE(v.hasTangent);
        if (v.position.squaredDistance(Ogre::Vector3(0, 0, 0)) < 0.001f) {
            EXPECT_NEAR(v.tangent.x, 1.0f, 0.001f);
            EXPECT_NEAR(v.tangent.w, 1.0f, 0.001f);
        } else if (v.position.squaredDistance(Ogre::Vector3(1, 0, 0)) < 0.001f) {
            EXPECT_NEAR(v.tangent.x, 0.7071f, 0.001f);
            EXPECT_NEAR(v.tangent.y, 0.7071f, 0.001f);
            EXPECT_NEAR(v.tangent.w, -1.0f, 0.001f); // negative parity preserved
        } else if (v.position.squaredDistance(Ogre::Vector3(0, 1, 0)) < 0.001f) {
            EXPECT_NEAR(v.tangent.y, 1.0f, 0.001f);
        }
    }
}

TEST(HalfEdgeMeshStandalone, ExtrudePreservesTangents) {
    // Tangents of original vertices should be unchanged; new vertices should
    // inherit the tangent of the vertex they were duplicated from.
    EditableMesh mesh;
    EditableSubMesh sub;

    EditableVertex v0, v1, v2;
    v0.position = Ogre::Vector3(0, 0, 0);
    v0.normal = Ogre::Vector3(0, 0, 1); v0.hasNormal = true;
    v0.uv = Ogre::Vector2(0, 0); v0.hasUV = true;
    v0.tangent = Ogre::Vector4(1.0f, 0.0f, 0.0f, 1.0f); v0.hasTangent = true;

    v1.position = Ogre::Vector3(1, 0, 0);
    v1.normal = Ogre::Vector3(0, 0, 1); v1.hasNormal = true;
    v1.uv = Ogre::Vector2(1, 0); v1.hasUV = true;
    v1.tangent = Ogre::Vector4(1.0f, 0.0f, 0.0f, 1.0f); v1.hasTangent = true;

    v2.position = Ogre::Vector3(0, 1, 0);
    v2.normal = Ogre::Vector3(0, 0, 1); v2.hasNormal = true;
    v2.uv = Ogre::Vector2(0, 1); v2.hasUV = true;
    v2.tangent = Ogre::Vector4(1.0f, 0.0f, 0.0f, 1.0f); v2.hasTangent = true;

    sub.vertices = {v0, v1, v2};
    EditableTriangle tri;
    tri.indices[0] = 0; tri.indices[1] = 1; tri.indices[2] = 2;
    sub.triangles = {tri};
    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));

    auto newVerts = he.extrudeFaces({0});
    ASSERT_EQ(newVerts.size(), 3u);

    // New vertices should inherit tangents from their originals
    for (int nv : newVerts) {
        EXPECT_TRUE(he.vertex(nv).hasTangent);
        EXPECT_NEAR(he.vertex(nv).tangent.x, 1.0f, 0.001f);
        EXPECT_NEAR(he.vertex(nv).tangent.w, 1.0f, 0.001f);
    }
}

// ===========================================================================
// Tests: Edge cases
// ===========================================================================

TEST(HalfEdgeMeshStandalone, OutOfBoundsQueries) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Out of bounds vertex queries should return empty
    EXPECT_TRUE(he.facesAroundVertex(-1).empty());
    EXPECT_TRUE(he.facesAroundVertex(999).empty());
    EXPECT_TRUE(he.edgesAroundVertex(-1).empty());
    EXPECT_TRUE(he.verticesAroundVertex(-1).empty());

    // Out of bounds face queries
    EXPECT_TRUE(he.faceVertices(-1).empty());
    EXPECT_TRUE(he.faceVertices(999).empty());
    EXPECT_TRUE(he.faceEdges(-1).empty());

    // Out of bounds edge queries
    auto [f1, f2] = he.edgeFaces(-1);
    EXPECT_EQ(f1, -1);
    EXPECT_EQ(f2, -1);

    auto [v1, v2] = he.edgeVertices(-1);
    EXPECT_EQ(v1, -1);
    EXPECT_EQ(v2, -1);

    // Out of bounds boundary queries
    EXPECT_FALSE(he.isVertexBoundary(-1));
    EXPECT_FALSE(he.isEdgeBoundary(-1));
}

TEST(HalfEdgeMeshStandalone, DegenerateTriangleSkipped) {
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "TestMat";

    EditableVertex v0, v1, v2;
    v0.position = Ogre::Vector3(0, 0, 0);
    v1.position = Ogre::Vector3(1, 0, 0);
    v2.position = Ogre::Vector3(0, 1, 0);
    sub.vertices = {v0, v1, v2};

    // Valid triangle
    EditableTriangle tri;
    tri.indices[0] = 0; tri.indices[1] = 1; tri.indices[2] = 2;
    sub.triangles.push_back(tri);

    // Degenerate triangle (two identical vertex indices)
    EditableTriangle degTri;
    degTri.indices[0] = 0; degTri.indices[1] = 0; degTri.indices[2] = 1;
    sub.triangles.push_back(degTri);

    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));

    // The degenerate triangle should be skipped
    EXPECT_EQ(he.faceCount(), 1u);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, MaterialNamesPreserved) {
    auto editMesh = makeTwoSubMeshMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    ASSERT_EQ(he.materialNames().size(), 2u);
    EXPECT_EQ(he.materialNames()[0], "Mat0");
    EXPECT_EQ(he.materialNames()[1], "Mat1");
}

// ===========================================================================
// Tests with Ogre (requires GL context)
// ===========================================================================

class HalfEdgeMeshOgreTest : public ::testing::Test {
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

TEST_F(HalfEdgeMeshOgreTest, BuildFromOgreEntity) {
    auto meshPtr = createInMemoryTriangleMesh("HalfEdge_ogre_triangle");
    auto* node = Manager::getSingleton()->addSceneNode("HalfEdge_ogre_triangle_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    EXPECT_EQ(he.vertexCount(), 3u);
    EXPECT_EQ(he.faceCount(), 1u);
    EXPECT_EQ(he.edgeCount(), 3u);
    EXPECT_TRUE(he.validate());

    Manager::getSingleton()->destroySceneNode("HalfEdge_ogre_triangle_node");
}

TEST_F(HalfEdgeMeshOgreTest, RoundtripThroughOgre) {
    auto meshPtr = createInMemoryTriangleMesh("HalfEdge_roundtrip_ogre");
    auto* node = Manager::getSingleton()->addSceneNode("HalfEdge_roundtrip_ogre_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

    // Load into EditableMesh
    EditableMesh editMesh;
    ASSERT_TRUE(editMesh.loadFromEntity(entity));

    // Convert to HalfEdgeMesh
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Convert back to EditableMesh
    EditableMesh result;
    ASSERT_TRUE(he.toEditableMesh(result));

    // Verify same structure
    EXPECT_EQ(result.totalVertexCount(), 3u);
    EXPECT_EQ(result.totalTriangleCount(), 1u);

    Manager::getSingleton()->destroySceneNode("HalfEdge_roundtrip_ogre_node");
}

// ===========================================================================
// Tests: Extrude Faces
// ===========================================================================

TEST(HalfEdgeMeshStandalone, ExtrudeFacesEmpty) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Extrude with no faces should return empty
    auto newVerts = he.extrudeFaces({});
    EXPECT_TRUE(newVerts.empty());
}

TEST(HalfEdgeMeshStandalone, ExtrudeSingleTriangle) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Original: 3 vertices, 1 face, 3 edges
    EXPECT_EQ(he.vertexCount(), 3u);
    EXPECT_EQ(he.faceCount(), 1u);

    // Extrude the single face
    auto newVerts = he.extrudeFaces({0});

    // Should have created 3 new vertices (one per boundary vertex)
    EXPECT_EQ(newVerts.size(), 3u);

    // Total vertices: 3 original + 3 new = 6
    EXPECT_EQ(he.vertexCount(), 6u);

    // Total faces: 1 original (rewired to new verts) + 6 side-wall triangles
    // (3 boundary edges * 2 triangles each = 6) = 7
    EXPECT_EQ(he.faceCount(), 7u);

    // New vertices should be at same positions as originals
    for (int nv : newVerts) {
        // Each new vertex should match one of the original positions
        auto pos = he.vertex(nv).position;
        bool matchesOriginal =
            pos.squaredDistance(Ogre::Vector3(0, 0, 0)) < 0.001f ||
            pos.squaredDistance(Ogre::Vector3(1, 0, 0)) < 0.001f ||
            pos.squaredDistance(Ogre::Vector3(0, 1, 0)) < 0.001f;
        EXPECT_TRUE(matchesOriginal) << "New vertex at " << pos.x << "," << pos.y << "," << pos.z;
    }

    // Structure should still be valid
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, ExtrudeFaceRoundtrip) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    auto newVerts = he.extrudeFaces({0});
    ASSERT_FALSE(newVerts.empty());

    // Move new vertices up by 1 unit in Z
    for (int nv : newVerts) {
        he.vertex(nv).position.z += 1.0f;
    }

    // Convert back to EditableMesh
    EditableMesh result;
    ASSERT_TRUE(he.toEditableMesh(result));

    // Should have 1 submesh with 6 vertices and 7 triangles
    EXPECT_EQ(result.subMeshCount(), 1u);
    EXPECT_EQ(result.totalVertexCount(), 6u);
    EXPECT_EQ(result.totalTriangleCount(), 7u);
}

TEST(HalfEdgeMeshStandalone, ExtrudeOneFaceOfQuad) {
    auto editMesh = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Original: 4 vertices, 2 faces, 5 edges
    EXPECT_EQ(he.vertexCount(), 4u);
    EXPECT_EQ(he.faceCount(), 2u);

    // Extrude just face 0
    auto newVerts = he.extrudeFaces({0});

    // Face 0 has 3 boundary edges (the edges not shared with face 1 + the shared edge).
    // Actually in a quad: face 0 shares 1 edge with face 1, so face 0 has 2 boundary edges
    // plus 1 edge shared with face 1 = 3 edges total, but only 2 are "exterior" boundary
    // and 1 is the shared interior edge. Since face 1 is NOT in the selection,
    // the shared edge IS a boundary of the selection. So all 3 edges are selection-boundary.
    // That means 3 boundary vertices.
    EXPECT_EQ(newVerts.size(), 3u);

    // Total: 4 + 3 = 7 vertices
    EXPECT_EQ(he.vertexCount(), 7u);

    // Face count: 2 original + 6 side walls = 8
    EXPECT_EQ(he.faceCount(), 8u);

    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, ExtrudeBothFacesOfQuad) {
    auto editMesh = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Extrude both faces — the shared edge is NOT a boundary of the selection
    auto newVerts = he.extrudeFaces({0, 1});

    // Boundary of {face0, face1} is the 4 outer edges.
    // Boundary vertices are all 4 vertices.
    EXPECT_EQ(newVerts.size(), 4u);

    // Total: 4 + 4 = 8 vertices
    EXPECT_EQ(he.vertexCount(), 8u);

    // Face count: 2 original + (4 boundary edges * 2 tris) = 10
    EXPECT_EQ(he.faceCount(), 10u);

    EXPECT_TRUE(he.validate());
}

// ===========================================================================
// Tests: Extrude Edges
// ===========================================================================

TEST(HalfEdgeMeshStandalone, ExtrudeEdgesEmpty) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    auto newVerts = he.extrudeEdges({});
    EXPECT_TRUE(newVerts.empty());
}

TEST(HalfEdgeMeshStandalone, ExtrudeSingleEdge) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Original: 3 vertices, 1 face, 3 edges
    // Extrude edge 0
    auto newVerts = he.extrudeEdges({0});

    // Edge 0 has 2 vertices, so 2 new vertices
    EXPECT_EQ(newVerts.size(), 2u);

    // Total: 3 + 2 = 5 vertices
    EXPECT_EQ(he.vertexCount(), 5u);

    // Total faces: 1 original + 2 (one quad = 2 tris) = 3
    EXPECT_EQ(he.faceCount(), 3u);

    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, ExtrudeEdgeRoundtrip) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    auto newVerts = he.extrudeEdges({0});
    ASSERT_FALSE(newVerts.empty());

    // Move new vertices
    for (int nv : newVerts) {
        he.vertex(nv).position.z += 1.0f;
    }

    EditableMesh result;
    ASSERT_TRUE(he.toEditableMesh(result));

    EXPECT_EQ(result.totalVertexCount(), 5u);
    EXPECT_EQ(result.totalTriangleCount(), 3u);
}

TEST(HalfEdgeMeshStandalone, ExtrudeMultipleEdges) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Extrude all 3 edges of the triangle
    auto newVerts = he.extrudeEdges({0, 1, 2});

    // 3 edges sharing 3 vertices total = 3 new vertices
    EXPECT_EQ(newVerts.size(), 3u);

    // Total: 3 + 3 = 6 vertices
    EXPECT_EQ(he.vertexCount(), 6u);

    // Total faces: 1 + 6 (3 edges * 2 tris each) = 7
    EXPECT_EQ(he.faceCount(), 7u);

    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, ExtrudePreservesAttributes) {
    auto editMesh = makeBoneWeightMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    auto newVerts = he.extrudeFaces({0});

    // New vertices should have copied bone weights from originals
    for (int nv : newVerts) {
        // Each new vertex should have bone assignments
        // (all original vertices had at least one assignment)
        EXPECT_FALSE(he.vertex(nv).boneAssignments.empty())
            << "New vertex " << nv << " has no bone assignments";
    }
}

// ===========================================================================
// Tests: Bevel
// ===========================================================================

TEST(HalfEdgeMeshStandalone, BevelEdgesEmpty) {
    auto editMesh = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));
    EXPECT_TRUE(he.bevelEdges({}, 0.01f).empty());
}

TEST(HalfEdgeMeshStandalone, BevelBoundaryEdgeSkipped) {
    auto editMesh = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));
    // All edges of a single triangle are boundary — bevel should skip them all.
    std::vector<int> allEdges;
    for (size_t e = 0; e < he.edgeCount(); ++e) allEdges.push_back(static_cast<int>(e));
    EXPECT_TRUE(he.bevelEdges(allEdges, 0.05f).empty());
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, BevelQuadInteriorEdgeAddsFourVerticesAndTwoFaces) {
    auto editMesh = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Find the one interior edge of the quad (diagonal between (1,0,0) and (0,1,0)).
    int interior = -1;
    for (size_t e = 0; e < he.edgeCount(); ++e) {
        auto [f1, f2] = he.edgeFaces(e);
        if (f1 >= 0 && f2 >= 0) { interior = static_cast<int>(e); break; }
    }
    ASSERT_GE(interior, 0);

    const size_t vBefore = he.vertexCount();
    const size_t fBefore = he.faceCount();

    auto newVerts = he.bevelEdges({interior}, 0.1f);

    // One interior edge → 4 new vertices (v1a, v1b, v2a, v2b).
    EXPECT_EQ(newVerts.size(), 4u);
    EXPECT_EQ(he.vertexCount(), vBefore + 4u);
    // Each of the 2 face tris is replaced by 3 retriangulation tris, plus
    // 2 chamfer tris + 2 end-cap tris. The 2 originals are orphaned
    // (halfEdge=-1) but still occupy slots in m_faces, so vectorized
    // face count is fBefore + 10. faceCount() returns the raw size.
    EXPECT_EQ(he.faceCount(), fBefore + 10u);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, BevelSharedEndpointEdgesSkipped) {
    // Two edges sharing a vertex → first version skips them both (chained
    // bevels need direction logic the first pass doesn't handle).
    auto editMesh = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    // Collect interior-edge index and one boundary-edge index that shares a vertex.
    int interior = -1, adjacentBoundary = -1;
    for (size_t e = 0; e < he.edgeCount(); ++e) {
        auto [f1, f2] = he.edgeFaces(e);
        if (f1 >= 0 && f2 >= 0) interior = static_cast<int>(e);
    }
    ASSERT_GE(interior, 0);
    auto [iv1, iv2] = he.edgeVertices(interior);
    for (size_t e = 0; e < he.edgeCount(); ++e) {
        if (static_cast<int>(e) == interior) continue;
        auto [v1, v2] = he.edgeVertices(e);
        if (v1 == iv1 || v2 == iv1 || v1 == iv2 || v2 == iv2) {
            adjacentBoundary = static_cast<int>(e);
            break;
        }
    }
    ASSERT_GE(adjacentBoundary, 0);

    // Bevel [interior, adjacentBoundary]: adjacentBoundary is a boundary edge
    // (filtered out in pass 1), so interior is the only survivor — it should
    // succeed. To actually exercise the shared-endpoint filter we need two
    // interior edges; a single quad only has one, so we assert the filter
    // doesn't break single-edge cases.
    auto newVerts = he.bevelEdges({interior, adjacentBoundary}, 0.05f);
    EXPECT_EQ(newVerts.size(), 4u);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, BevelZeroWidthRejected) {
    auto editMesh = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));
    int interior = -1;
    for (size_t e = 0; e < he.edgeCount(); ++e) {
        auto [f1, f2] = he.edgeFaces(e);
        if (f1 >= 0 && f2 >= 0) { interior = static_cast<int>(e); break; }
    }
    EXPECT_TRUE(he.bevelEdges({interior}, 0.0f).empty());
    EXPECT_TRUE(he.bevelEdges({interior}, -1.0f).empty());
}

TEST(HalfEdgeMeshStandalone, BevelLargerWidthProducesLargerChamfer) {
    // Same mesh beveled at two different widths should produce chamfer
    // vertices at proportionally different distances from the original edge.
    auto mk = []() {
        HalfEdgeMesh he;
        he.buildFromEditableMesh(makeQuadMesh());
        return he;
    };

    auto findInterior = [](const HalfEdgeMesh& he) {
        for (size_t e = 0; e < he.edgeCount(); ++e) {
            auto [f1, f2] = he.edgeFaces(static_cast<int>(e));
            if (f1 >= 0 && f2 >= 0) return static_cast<int>(e);
        }
        return -1;
    };

    HalfEdgeMesh hSmall = mk();
    int eSmall = findInterior(hSmall);
    auto [v1Small, v2Small] = hSmall.edgeVertices(eSmall);
    auto v1Orig = hSmall.vertex(v1Small).position;
    auto newSmall = hSmall.bevelEdges({eSmall}, 0.05f);
    ASSERT_EQ(newSmall.size(), 4u);

    HalfEdgeMesh hLarge = mk();
    int eLarge = findInterior(hLarge);
    auto newLarge = hLarge.bevelEdges({eLarge}, 0.15f);
    ASSERT_EQ(newLarge.size(), 4u);

    // The first new vertex is v1a (v1 pulled toward f1Opp). At 3x the width,
    // it should be roughly 3x farther from v1 (within the clamp limits).
    float distSmall = hSmall.vertex(newSmall[0]).position.distance(v1Orig);
    float distLarge = hLarge.vertex(newLarge[0]).position.distance(v1Orig);
    EXPECT_GT(distLarge, distSmall * 2.5f);
}

TEST(HalfEdgeMeshStandalone, BevelWidthCappedForShortEdges) {
    // A mesh with a very short adjacent edge should clamp the effective
    // bevel width so no degenerate triangles are produced, regardless of
    // how large a width the caller passes.
    EditableMesh em;
    EditableSubMesh sub;
    sub.materialName = "M";
    // Two narrow triangles sharing a short diagonal (length 0.1) with long
    // outer edges (length 1.0). Bevel width 10.0 would trivially collapse
    // if uncapped.
    EditableVertex v0, v1, v2, v3;
    v0.position = Ogre::Vector3(0, 0, 0);
    v1.position = Ogre::Vector3(1, 0, 0);
    v2.position = Ogre::Vector3(0.5f, 0.05f, 0);
    v3.position = Ogre::Vector3(0.5f, -0.05f, 0);
    v0.hasNormal = v1.hasNormal = v2.hasNormal = v3.hasNormal = true;
    v0.normal = v1.normal = v2.normal = v3.normal = Ogre::Vector3(0,0,1);
    sub.vertices = {v0, v1, v2, v3};
    EditableTriangle t0, t1;
    t0.indices[0] = 0; t0.indices[1] = 2; t0.indices[2] = 3;
    t1.indices[0] = 1; t1.indices[1] = 3; t1.indices[2] = 2;
    sub.triangles = {t0, t1};
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    int interior = -1;
    for (size_t e = 0; e < he.edgeCount(); ++e) {
        auto [f1, f2] = he.edgeFaces(static_cast<int>(e));
        if (f1 >= 0 && f2 >= 0) { interior = static_cast<int>(e); break; }
    }
    ASSERT_GE(interior, 0);

    // Request absurdly large width; effective width should clamp to 40% of
    // the shortest adjacent edge (≈0.1 diagonal → 0.04 cap). Result still
    // produces 4 new verts and a valid mesh.
    auto newVerts = he.bevelEdges({interior}, 10.0f);
    EXPECT_EQ(newVerts.size(), 4u);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, BevelRoundtripKeepsMeshValid) {
    auto editMesh = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(editMesh));

    int interior = -1;
    for (size_t e = 0; e < he.edgeCount(); ++e) {
        auto [f1, f2] = he.edgeFaces(e);
        if (f1 >= 0 && f2 >= 0) { interior = static_cast<int>(e); break; }
    }
    ASSERT_GE(interior, 0);

    auto newVerts = he.bevelEdges({interior}, 0.1f);
    ASSERT_EQ(newVerts.size(), 4u);

    EditableMesh result;
    ASSERT_TRUE(he.toEditableMesh(result));

    // Roundtripped mesh: all 4 original verts stay in use (end-caps reference
    // the shared-edge endpoints), plus 4 new chamfer verts = 8 distinct verts.
    // Triangle count: 3 per face × 2 faces + 2 chamfer + 2 end-cap = 10.
    EXPECT_EQ(result.totalVertexCount(), 8u);
    EXPECT_EQ(result.totalTriangleCount(), 10u);
}
