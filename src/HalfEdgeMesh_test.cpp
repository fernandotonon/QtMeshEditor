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
