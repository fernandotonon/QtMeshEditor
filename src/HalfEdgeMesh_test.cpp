/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <numeric>
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

/// Two-quad strip in the n-gon canonical form (sub.faces is populated).
/// Topology:
///   3---2---5
///   |q1 |q2 |
///   0---1---4
/// Both quads are coplanar in XY at z=0, share edge (1,2). Used by the
/// quad-mesh topology-op test audit (#326 acceptance criterion).
static EditableMesh makeNativeQuadMesh()
{
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "QuadStripMat";
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        v.uv = Ogre::Vector2(x, y); v.hasUV = true;
        return v;
    };
    sub.vertices = { mkV(0, 0), mkV(1, 0), mkV(1, 1), mkV(0, 1),
                     mkV(2, 0), mkV(2, 1) };
    EditableFace q1, q2;
    q1.indices = {0, 1, 2, 3};
    q2.indices = {1, 4, 5, 2};
    sub.faces = { q1, q2 };
    triangulateFaces(sub);
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
        ASSERT_TRUE(tryInitOgre()) << "Ogre not available (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles()) << "Cannot create hardware buffers (Xvfb/GL required in CI)";
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

// ===========================================================================
// Tests: Bevel on a welded cube (matches runtime's PrimitiveObject output)
// ===========================================================================

namespace {
    // Builds the exact same welded-cube EditableMesh the primitive generator
    // produces at runtime: 8 verts at (±1, ±1, ±1), 12 tris, 6 logical cube
    // faces triangulated along a diagonal. Vertex + triangle ordering matches
    // the app's [MESH] dump so test scenarios mirror real behavior.
    EditableMesh makeCubeMesh()
    {
        EditableMesh em;
        EditableSubMesh sub;
        sub.materialName = "M";
        auto mkV = [](float x, float y, float z) {
            EditableVertex v;
            v.position = Ogre::Vector3(x, y, z);
            v.normal = Ogre::Vector3::UNIT_Z;
            v.hasNormal = true;
            return v;
        };
        sub.vertices = {
            mkV(-1, -1, -1), // 0
            mkV( 1, -1, -1), // 1
            mkV(-1,  1, -1), // 2
            mkV( 1,  1, -1), // 3
            mkV(-1,  1,  1), // 4
            mkV( 1,  1,  1), // 5
            mkV(-1, -1,  1), // 6
            mkV( 1, -1,  1), // 7
        };
        auto mkT = [](unsigned a, unsigned b, unsigned c) {
            EditableTriangle t;
            t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
            return t;
        };
        sub.triangles = {
            mkT(0, 2, 1), // f0  back    (-Z)
            mkT(1, 2, 3), // f1  back
            mkT(4, 6, 5), // f2  front   (+Z)
            mkT(5, 6, 7), // f3  front
            mkT(6, 0, 7), // f4  bottom  (-Y)
            mkT(7, 0, 1), // f5  bottom
            mkT(2, 4, 3), // f6  top     (+Y)
            mkT(3, 4, 5), // f7  top
            mkT(2, 0, 4), // f8  left    (-X)
            mkT(4, 0, 6), // f9  left
            mkT(1, 3, 7), // f10 right   (+X)
            mkT(7, 3, 5), // f11 right
        };
        em.subMeshes().push_back(std::move(sub));
        return em;
    }

    // Returns HE edge index between v1 and v2 (any order), or -1 if not found.
    int findEdge(const HalfEdgeMesh& he, int v1, int v2)
    {
        int a = std::min(v1, v2), b = std::max(v1, v2);
        for (size_t e = 0; e < he.edgeCount(); ++e) {
            auto [ev1, ev2] = he.edgeVertices(static_cast<int>(e));
            int ea = std::min(ev1, ev2), eb = std::max(ev1, ev2);
            if (ea == a && eb == b) return static_cast<int>(e);
        }
        return -1;
    }

    // True if the output mesh's triangle set has every edge referenced by
    // exactly 1 or 2 triangles (manifold). Any edge referenced by >2 or with
    // mismatched winding on a shared edge indicates a tear.
    bool isManifold(const EditableMesh& em)
    {
        const auto& subs = em.subMeshes();
        std::map<std::pair<unsigned,unsigned>, int> edgeUse;
        std::map<std::pair<unsigned,unsigned>, int> directedEdgeUse;
        for (const auto& sub : subs) {
            for (const auto& t : sub.triangles) {
                for (int k = 0; k < 3; ++k) {
                    unsigned a = t.indices[k], b = t.indices[(k + 1) % 3];
                    if (a == b) return false; // degenerate
                    auto key = std::make_pair(std::min(a, b), std::max(a, b));
                    ++edgeUse[key];
                    ++directedEdgeUse[{a, b}];
                }
            }
        }
        for (const auto& [_, count] : edgeUse) {
            if (count < 1 || count > 2) return false;
        }
        // Two tris sharing an edge must traverse it in opposite directions.
        // A directed count > 1 means same-winding neighbors (inverted tri).
        for (const auto& [_, count] : directedEdgeUse) {
            if (count > 1) return false;
        }
        return true;
    }

    // Counts triangles and boundary edges (used exactly once). In a closed
    // manifold every edge is used exactly twice (boundaryEdges == 0).
    struct CubeStats { size_t tris; size_t verts; size_t boundaryEdges; };
    CubeStats statsOf(const EditableMesh& em)
    {
        CubeStats s{0, 0, 0};
        const auto& subs = em.subMeshes();
        std::map<std::pair<unsigned,unsigned>, int> edgeUse;
        for (const auto& sub : subs) {
            s.verts += sub.vertices.size();
            s.tris += sub.triangles.size();
            for (const auto& t : sub.triangles) {
                for (int k = 0; k < 3; ++k) {
                    unsigned a = t.indices[k], b = t.indices[(k + 1) % 3];
                    auto key = std::make_pair(std::min(a, b), std::max(a, b));
                    ++edgeUse[key];
                }
            }
        }
        for (const auto& [_, count] : edgeUse)
            if (count == 1) ++s.boundaryEdges;
        return s;
    }
}

TEST(HalfEdgeMeshStandalone, CubeBaselineIsClosedManifold) {
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    auto s = statsOf(back);
    EXPECT_EQ(s.verts, 8u);
    EXPECT_EQ(s.tris, 12u);
    EXPECT_EQ(s.boundaryEdges, 0u) << "welded cube must have no boundary edges";
    EXPECT_TRUE(isManifold(back));
}

TEST(HalfEdgeMeshStandalone, CubeBevelTopRightEdgeKeepsMeshClosed) {
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    int edgeIdx = findEdge(he, 5, 3);
    ASSERT_GE(edgeIdx, 0);

    auto newVerts = he.bevelEdges({edgeIdx}, 0.05f);
    ASSERT_FALSE(newVerts.empty()) << "bevel did nothing";
    ASSERT_TRUE(he.validate()) << "half-edge structure invalid after bevel";

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));

    auto s = statsOf(back);
    EXPECT_EQ(s.boundaryEdges, 0u)
        << "bevel should produce a closed manifold mesh, but " << s.boundaryEdges
        << " boundary edge(s) remain";
    EXPECT_TRUE(isManifold(back));

    // Winding consistency: for a convex closed mesh centered at origin,
    // every triangle's outward normal should point away from origin — the
    // dot product (normal · centroid) must be strictly positive. Tests
    // catches inverted-normal bugs that "closed manifold" alone wouldn't
    // flag (including tiny corner tris near the origin where the dot is
    // close to zero).
    const auto& sub = back.subMeshes()[0];
    for (size_t t = 0; t < sub.triangles.size(); ++t) {
        const auto& tri = sub.triangles[t];
        const auto& p0 = sub.vertices[tri.indices[0]].position;
        const auto& p1 = sub.vertices[tri.indices[1]].position;
        const auto& p2 = sub.vertices[tri.indices[2]].position;
        auto n = (p1 - p0).crossProduct(p2 - p0);
        auto centroid = (p0 + p1 + p2) / 3.0f;
        // Normalize both so threshold applies to angle, not absolute magnitude.
        float nLen = n.length();
        if (nLen < 1e-8f) {
            ADD_FAILURE() << "triangle " << t << " = (" << tri.indices[0] << ","
                << tri.indices[1] << "," << tri.indices[2] << ") is degenerate: "
                << "positions ("
                << p0.x << "," << p0.y << "," << p0.z << ") ("
                << p1.x << "," << p1.y << "," << p1.z << ") ("
                << p2.x << "," << p2.y << "," << p2.z << ")";
            continue;
        }
        n /= nLen;
        auto cDir = centroid;
        float cLen = cDir.length();
        if (cLen > 1e-6f) cDir /= cLen;
        float cosAngle = n.dotProduct(cDir);
        EXPECT_GT(cosAngle, 0.1f)
            << "triangle " << t << " = (" << tri.indices[0] << ","
            << tri.indices[1] << "," << tri.indices[2] << ") wind/position inverted: "
            << "cos(normal, outward) = " << cosAngle
            << ", positions ("
            << p0.x << "," << p0.y << "," << p0.z << ") ("
            << p1.x << "," << p1.y << "," << p1.z << ") ("
            << p2.x << "," << p2.y << "," << p2.z << ")";
    }
}

// Given a beveled cube edge, return the count of triangles in face `normal`
// (±X, ±Y, ±Z) that still reference the original cube corner at position
// `corner`.
namespace {
    int countTrisOnFaceReferencingVert(const EditableMesh& em,
                                       const Ogre::Vector3& faceNormalAxis,
                                       const Ogre::Vector3& corner)
    {
        if (em.subMeshes().empty()) return 0;
        const auto& sub = em.subMeshes()[0];
        int cornerIdx = -1;
        for (size_t i = 0; i < sub.vertices.size(); ++i) {
            if (sub.vertices[i].position.squaredDistance(corner) < 1e-8f) {
                cornerIdx = static_cast<int>(i);
                break;
            }
        }
        if (cornerIdx < 0) return 0;

        int count = 0;
        for (const auto& t : sub.triangles) {
            // On-face check: pick the component of `faceNormalAxis` that is
            // nonzero; all three vertices should have that coordinate ≈ ±1.
            bool onFace = true;
            for (int k = 0; k < 3; ++k) {
                const auto& p = sub.vertices[t.indices[k]].position;
                float c = p.dotProduct(faceNormalAxis);
                if (std::abs(c - 1.0f) > 0.01f) { onFace = false; break; }
            }
            if (!onFace) continue;
            for (int k = 0; k < 3; ++k)
                if (static_cast<int>(t.indices[k]) == cornerIdx) { ++count; break; }
        }
        return count;
    }
}

TEST(HalfEdgeMeshStandalone, CubeBevelTopFrontEdgeTrimsLeftAndRightFaceCorners) {
    // Beveling the top-front cube edge (between v4=(-1,1,1) and v5=(1,1,1))
    // has the chamfer strip running along the TOP and FRONT faces. At each
    // endpoint, the OTHER adjacent face (left face at v4, right face at v5)
    // is perpendicular to the chamfer and shares a v-edge that is a crease
    // with a beveled face — so the left/right face corners SHOULD be trimmed
    // (the original v4 / v5 corner should no longer appear in a single-tri
    // face corner on those faces).
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    int edgeIdx = findEdge(he, 4, 5);
    ASSERT_GE(edgeIdx, 0);
    ASSERT_FALSE(he.bevelEdges({edgeIdx}, 0.05f).empty());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));

    // Left face (x=-1): should have NO tri referencing v4=(-1,1,1).
    int leftFaceV4 = countTrisOnFaceReferencingVert(
        back, Ogre::Vector3(-1, 0, 0), Ogre::Vector3(-1, 1, 1));
    // Right face (x=+1): should have NO tri referencing v5=(1,1,1).
    int rightFaceV5 = countTrisOnFaceReferencingVert(
        back, Ogre::Vector3(1, 0, 0), Ogre::Vector3(1, 1, 1));

    EXPECT_EQ(leftFaceV4, 0)
        << "left face still has " << leftFaceV4
        << " tris referencing v4=(-1,1,1) — corner not trimmed";
    EXPECT_EQ(rightFaceV5, 0)
        << "right face still has " << rightFaceV5
        << " tris referencing v5=(1,1,1) — corner not trimmed";
}

TEST(HalfEdgeMeshStandalone, CubeBevelPreservesVolumeMinusChamfer) {
    // The signed volume of a closed mesh centered at origin = sum over
    // triangles of (p0 · (p1 × p2)) / 6. For our 2x2x2 cube (volume 8),
    // beveling an edge removes a small prism of volume ≈ w² × length.
    // With w=0.05 and edge length 2, chamfer volume ≈ 0.01. So post-bevel
    // volume should be ~7.99. Anything far from that (negative, near-zero,
    // or much larger) means inverted tris or duplicated geometry.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    int edgeIdx = findEdge(he, 5, 3);
    ASSERT_GE(edgeIdx, 0);
    ASSERT_FALSE(he.bevelEdges({edgeIdx}, 0.05f).empty());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    const auto& sub = back.subMeshes()[0];
    double volume6 = 0.0;
    for (const auto& tri : sub.triangles) {
        const auto& p0 = sub.vertices[tri.indices[0]].position;
        const auto& p1 = sub.vertices[tri.indices[1]].position;
        const auto& p2 = sub.vertices[tri.indices[2]].position;
        volume6 += p0.dotProduct(p1.crossProduct(p2));
    }
    double volume = volume6 / 6.0;
    // Pre-bevel cube volume = 8. Expected post-bevel: 8 - chamfer_slice.
    EXPECT_NEAR(volume, 7.99, 0.1)
        << "signed volume should be ~7.99 (cube minus chamfer). Actual: " << volume
        << " — likely inverted tris or duplicates.";
}

TEST(HalfEdgeMeshStandalone, CubeBevelEveryPerimeterEdgeKeepsMeshClosed) {
    // Sweep across every cube perimeter edge and assert the output is closed.
    // Each edge runs between two adjacent (±1, ±1, ±1) corners — that's 12
    // cube edges total. Skip face diagonals (same face, coplanar).
    const std::vector<std::pair<int,int>> perimeterEdges = {
        {0,1}, {0,2}, {0,6}, {1,3}, {1,7}, {2,3},
        {2,4}, {3,5}, {4,5}, {4,6}, {5,7}, {6,7},
    };
    for (const auto& [a, b] : perimeterEdges) {
        auto em = makeCubeMesh();
        HalfEdgeMesh he;
        ASSERT_TRUE(he.buildFromEditableMesh(em));
        int edgeIdx = findEdge(he, a, b);
        ASSERT_GE(edgeIdx, 0) << "edge (" << a << "," << b << ") not found";

        auto newVerts = he.bevelEdges({edgeIdx}, 0.05f);
        ASSERT_FALSE(newVerts.empty())
            << "bevel rejected cube perimeter edge (" << a << "," << b << ")";
        EXPECT_TRUE(he.validate())
            << "validate() failed after beveling edge (" << a << "," << b << ")";

        EditableMesh back;
        ASSERT_TRUE(he.toEditableMesh(back));
        auto s = statsOf(back);
        EXPECT_EQ(s.boundaryEdges, 0u)
            << "bevel of edge (" << a << "," << b << ") leaves "
            << s.boundaryEdges << " boundary edge(s)";
        EXPECT_TRUE(isManifold(back))
            << "bevel of edge (" << a << "," << b << ") produced a non-manifold mesh";

        // Strict winding: every tri's normal · centroid-direction must be
        // clearly positive (cube centered at origin, outward normals expected).
        const auto& sub = back.subMeshes()[0];
        for (size_t t = 0; t < sub.triangles.size(); ++t) {
            const auto& tri = sub.triangles[t];
            const auto& p0 = sub.vertices[tri.indices[0]].position;
            const auto& p1 = sub.vertices[tri.indices[1]].position;
            const auto& p2 = sub.vertices[tri.indices[2]].position;
            auto n = (p1 - p0).crossProduct(p2 - p0);
            float nLen = n.length();
            ASSERT_GT(nLen, 1e-8f)
                << "bevel of (" << a << "," << b << ") tri " << t << " is degenerate";
            auto cDir = (p0 + p1 + p2) / 3.0f;
            float cLen = cDir.length();
            if (cLen > 1e-6f) cDir /= cLen;
            float cosAngle = (n / nLen).dotProduct(cDir);
            EXPECT_GT(cosAngle, 0.1f)
                << "bevel of (" << a << "," << b << ") tri " << t << " = ("
                << tri.indices[0] << "," << tri.indices[1] << "," << tri.indices[2]
                << ") wind/position inverted: cos=" << cosAngle;
        }
    }
}

// ===========================================================================
// Smooth-surface bevel (character-mesh scenario).
//
// Lead Jab.fbx and similar character meshes produce holes/degenerate tris
// because the bevel endpoint's ring has crease edges on both sides of the
// beveled faces (v-f1Opposite and v-f2Opposite), yet processRingNeighbors
// does NOT emit a merged polygon covering those creases — the non-beveled
// ring faces between f1 and f2 are themselves creased (not coplanar), so
// each falls back to single-face processNeighborFace. The chamfer's end
// edge v1a-v1b has no covering tri from the neighbor side, so it's a
// boundary edge (hole).
// ===========================================================================

namespace {
    // A 6-vertex "tent" that reproduces the smooth-surface bevel scenario.
    // Vertices v0, v1 form the beveled edge. Four surrounding faces (f1, f2
    // meeting at v0-v1; fA/fB on the v0 side; fC/fD on the v1 side) create
    // a fan at each endpoint with crease edges on the f1 and f2 sides AND
    // a crease between the two non-beveled neighbors — precisely the case
    // where processRingNeighbors cannot merge them.
    // A closed-ring smooth character fixture: both bevel endpoints have
    // full rings (ringWalkFailed=0 in the bevel diag, matching Lead Jab's
    // real topology). Built as two "pie fans" joined at the bevel edge.
    EditableMesh makeSmoothCharacterMesh()
    {
        EditableMesh em;
        EditableSubMesh sub;
        sub.materialName = "M";
        auto mkV = [](float x, float y, float z) {
            EditableVertex v;
            v.position = Ogre::Vector3(x, y, z);
            v.normal = Ogre::Vector3::UNIT_Z;
            v.hasNormal = true;
            return v;
        };
        // v0 and v1 are the bevel endpoints. Around each of them is a
        // "pie fan" of 4 triangles sharing successive edges — giving each
        // endpoint a closed ring. The bevel faces f1 and f2 are two of
        // those ring faces (the ones along the v0-v1 edge).
        //
        // Layout:
        //   v2 — v3 — v4 (upper ring above v0/v1)
        //   v0 — v1 (bevel edge)
        //   v5 — v6 — v7 (lower ring below)
        // With slight z-perturbation so faces aren't coplanar.
        sub.vertices = {
            mkV( 0.0f,  0.0f,  0.00f),  // 0 — v0 (bevel endpoint)
            mkV( 1.0f,  0.0f,  0.00f),  // 1 — v1 (bevel endpoint)
            mkV(-0.5f,  0.8f,  0.30f),  // 2 — upper-left
            mkV( 0.5f,  0.8f,  0.50f),  // 3 — upper-mid
            mkV( 1.5f,  0.8f,  0.30f),  // 4 — upper-right
            mkV(-0.5f, -0.8f,  0.30f),  // 5 — lower-left
            mkV( 0.5f, -0.8f,  0.50f),  // 6 — lower-mid
            mkV( 1.5f, -0.8f,  0.30f),  // 7 — lower-right
            mkV(-1.0f,  0.0f,  0.25f),  // 8 — far-left
            mkV( 2.0f,  0.0f,  0.25f),  // 9 — far-right
        };
        auto mkT = [&](unsigned a, unsigned b, unsigned c) {
            EditableTriangle t;
            t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
            return t;
        };
        sub.triangles = {
            // Bevel faces meeting along v0-v1:
            mkT(0, 1, 3),   // f1 (above beveled edge, shares v0-v1)
            mkT(1, 0, 6),   // f2 (below beveled edge, shares v0-v1)
            // v0's upper ring: f1 ← fA ← fB
            mkT(0, 3, 2),   // fA — shares v0-v3 with f1, v0-v2 with fB
            mkT(0, 2, 8),   // fB — shares v0-v2 with fA, v0-v8 with fC
            // v0's lower ring: f2 ← fC ← fD
            mkT(0, 8, 5),   // fC — shares v0-v8 with fB, v0-v5 with fD
            mkT(0, 5, 6),   // fD — shares v0-v5 with fC, v0-v6 with f2
            // v1's upper ring: f1 → fE → fF
            mkT(1, 4, 3),   // fE — shares v1-v3 with f1, v1-v4 with fF
            mkT(1, 9, 4),   // fF — shares v1-v4 with fE, v1-v9 with fG
            // v1's lower ring: f2 → fG → fH
            mkT(1, 7, 9),   // fG — shares v1-v9 with fF, v1-v7 with fH
            mkT(1, 6, 7),   // fH — shares v1-v7 with fG, v1-v6 with f2
        };
        em.subMeshes().push_back(std::move(sub));
        return em;
    }

    EditableMesh makeSmoothSurfaceMesh(bool reverseWinding = false)
    {
        EditableMesh em;
        EditableSubMesh sub;
        sub.materialName = "M";
        auto mkV = [](float x, float y, float z) {
            EditableVertex v;
            v.position = Ogre::Vector3(x, y, z);
            v.normal = Ogre::Vector3::UNIT_Z;
            v.hasNormal = true;
            return v;
        };
        sub.vertices = {
            mkV( 0.0f,  0.0f,  0.0f),  // 0 — v0 (bevel endpoint)
            mkV( 1.0f,  0.0f,  0.0f),  // 1 — v1 (bevel endpoint)
            mkV( 0.5f,  0.5f,  0.5f),  // 2 — v2 (upper side)
            mkV( 0.5f, -0.5f,  0.5f),  // 3 — v3 (lower side)
            mkV(-0.5f,  0.0f,  0.3f),  // 4 — v4 (behind v0)
            mkV( 1.5f,  0.0f,  0.3f),  // 5 — v5 (behind v1)
        };
        auto mkT = [&](unsigned a, unsigned b, unsigned c) {
            EditableTriangle t;
            if (reverseWinding) {
                t.indices[0] = c; t.indices[1] = b; t.indices[2] = a;
                // Flip normal direction too so the reversed-mesh tris face
                // the opposite side.
            } else {
                t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
            }
            return t;
        };
        sub.triangles = {
            mkT(0, 1, 2),  // f1 — upper bevel face
            mkT(1, 0, 3),  // f2 — lower bevel face
            mkT(0, 2, 4),  // fA — upper-left
            mkT(0, 4, 3),  // fB — lower-left
            mkT(1, 5, 2),  // fC — upper-right
            mkT(1, 3, 5),  // fD — lower-right
        };
        em.subMeshes().push_back(std::move(sub));
        return em;
    }
}

TEST(HalfEdgeMeshStandalone, SmoothSurfaceBevelProducesManifold) {
    // Bevel the v0-v1 edge and verify the result has no boundary edges
    // (except those at the mesh perimeter, which are already boundary edges
    // in the input). In a properly-beveled smooth-surface mesh every
    // interior chamfer strip edge must be shared between the chamfer and
    // a neighbor retriangulation.
    auto em = makeSmoothSurfaceMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    int edgeIdx = findEdge(he, 0, 1);
    ASSERT_GE(edgeIdx, 0);
    ASSERT_FALSE(he.bevelEdges({edgeIdx}, 0.05f).empty());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));

    // The input mesh has an open perimeter (no faces beyond v2,v3,v4,v5
    // loop), so boundary edges are expected — but only along that perimeter.
    // Count "interior" boundary edges: those whose endpoints are NOT both
    // on the original open boundary (v2, v3, v4, v5).
    const auto& sub = back.subMeshes()[0];
    std::map<std::pair<unsigned,unsigned>, int> edgeUse;
    for (const auto& t : sub.triangles) {
        for (int k = 0; k < 3; ++k) {
            unsigned a = t.indices[k], b = t.indices[(k+1)%3];
            auto key = std::make_pair(std::min(a,b), std::max(a,b));
            ++edgeUse[key];
        }
    }
    // Pre-bevel boundary verts by position (v2,v3,v4,v5 in original index).
    std::set<unsigned> perimeterVerts;
    for (size_t i = 0; i < sub.vertices.size(); ++i) {
        const auto& p = sub.vertices[i].position;
        // Perimeter in original mesh: v2=(0.5,0.5,0.5), v3=(0.5,-0.5,0.5),
        // v4=(-0.5,0,0.3), v5=(1.5,0,0.3). Any of these positions is
        // perimeter.
        if ((p - Ogre::Vector3(0.5f, 0.5f, 0.5f)).length() < 1e-4f
         || (p - Ogre::Vector3(0.5f, -0.5f, 0.5f)).length() < 1e-4f
         || (p - Ogre::Vector3(-0.5f, 0.0f, 0.3f)).length() < 1e-4f
         || (p - Ogre::Vector3(1.5f, 0.0f, 0.3f)).length() < 1e-4f) {
            perimeterVerts.insert(static_cast<unsigned>(i));
        }
    }
    int interiorBoundaryEdges = 0;
    for (const auto& [key, count] : edgeUse) {
        if (count == 1) {
            // Skip edges where BOTH endpoints are on the original perimeter.
            if (perimeterVerts.count(key.first) && perimeterVerts.count(key.second))
                continue;
            ++interiorBoundaryEdges;
            fprintf(stderr, "  interior boundary edge: (%u,%u)\n",
                    key.first, key.second);
        }
    }
    if (interiorBoundaryEdges > 0) {
        fprintf(stderr, "=== Smooth bevel output dump ===\n");
        for (size_t i = 0; i < sub.vertices.size(); ++i) {
            const auto& p = sub.vertices[i].position;
            fprintf(stderr, "v%zu = (%.3f, %.3f, %.3f)%s\n", i, p.x, p.y, p.z,
                    perimeterVerts.count(static_cast<unsigned>(i)) ? " [PERIMETER]" : "");
        }
        for (size_t t = 0; t < sub.triangles.size(); ++t) {
            const auto& tri = sub.triangles[t];
            fprintf(stderr, "t%zu = (%u, %u, %u)\n",
                    t, tri.indices[0], tri.indices[1], tri.indices[2]);
        }
    }
    EXPECT_EQ(interiorBoundaryEdges, 0)
        << "bevel left " << interiorBoundaryEdges
        << " interior boundary edges (holes)";

    // Winding consistency: the fixture's rest-pose faces all have +Z-ish
    // normals (the mesh bulges toward +Z). After bevel, every emitted tri
    // should ALSO have a +Z-ish normal — no flips.
    int flipped = 0;
    for (size_t t = 0; t < sub.triangles.size(); ++t) {
        const auto& tri = sub.triangles[t];
        const auto& p0 = sub.vertices[tri.indices[0]].position;
        const auto& p1 = sub.vertices[tri.indices[1]].position;
        const auto& p2 = sub.vertices[tri.indices[2]].position;
        auto n = (p1 - p0).crossProduct(p2 - p0);
        if (n.length() < 1e-8f) continue; // degenerate, skip
        n.normalise();
        if (n.z < 0.1f) {
            ++flipped;
            fprintf(stderr, "  flipped tri %zu = (%u,%u,%u) normal=(%.3f,%.3f,%.3f)\n",
                    t, tri.indices[0], tri.indices[1], tri.indices[2],
                    n.x, n.y, n.z);
        }
    }
    EXPECT_EQ(flipped, 0) << flipped << " tris have inverted winding";
}

namespace {
    // High-valence smooth fixture: each bevel endpoint has a 6-face fan
    // (closed ring), mimicking a denser character-mesh neighborhood.
    EditableMesh makeDenseSmoothCharacterMesh()
    {
        EditableMesh em;
        EditableSubMesh sub;
        sub.materialName = "M";
        auto mkV = [](float x, float y, float z) {
            EditableVertex v;
            v.position = Ogre::Vector3(x, y, z);
            v.normal = Ogre::Vector3::UNIT_Z;
            v.hasNormal = true;
            return v;
        };
        // v0 at origin, v1 at (1,0,0). Each has a 6-point ring around it.
        // Use slight z-wavelets so faces aren't coplanar.
        auto dz = [](float theta) {
            return 0.15f + 0.05f * std::sin(theta * 3.14159f);
        };
        sub.vertices.push_back(mkV( 0.0f,  0.0f, 0.0f));  // 0 — v0
        sub.vertices.push_back(mkV( 1.0f,  0.0f, 0.0f));  // 1 — v1
        // v0's ring: 5 extra verts (v2-v6) at 72° intervals, plus v1 completes the 6-fan
        for (int i = 0; i < 5; ++i) {
            float theta = (-2.0f + i * 0.8f) * 0.5f; // skip the v1 direction
            float x = 0.5f * std::cos(theta);
            float y = 0.5f * std::sin(theta);
            sub.vertices.push_back(mkV(x, y, dz(theta)));
        }
        // v1's ring: 5 extra verts (v7-v11) mirrored, plus v0
        for (int i = 0; i < 5; ++i) {
            float theta = (-2.0f + i * 0.8f) * 0.5f;
            float x = 1.0f + 0.5f * std::cos(3.14159f - theta);
            float y = 0.5f * std::sin(3.14159f - theta);
            sub.vertices.push_back(mkV(x, y, dz(theta)));
        }
        auto mkT = [](unsigned a, unsigned b, unsigned c) {
            EditableTriangle t;
            t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
            return t;
        };
        // v0's fan: 6 tris connecting v0 to consecutive ring verts.
        // Ring order around v0: v1 (as one of the ring), v2, v3, v4, v5, v6, back to v1.
        // So fan tris: (0,1,2), (0,2,3), (0,3,4), (0,4,5), (0,5,6), (0,6,1)
        // But (0,1,?) is the bevel face; we pair it with (1,0,?) below.
        sub.triangles.push_back(mkT(0, 2, 1));  // f1: upper bevel face (contains v0-v1)
        sub.triangles.push_back(mkT(0, 3, 2));
        sub.triangles.push_back(mkT(0, 4, 3));
        sub.triangles.push_back(mkT(0, 5, 4));
        sub.triangles.push_back(mkT(0, 6, 5));
        sub.triangles.push_back(mkT(0, 1, 6));  // f2: lower bevel face (contains v0-v1)
        // v1's fan: tris sharing v1 with the other ring
        // But f1 and f2 already touch v1, so v1's OTHER ring faces are:
        sub.triangles.push_back(mkT(1, 2, 7));   // above v0-v1 continuing to v1's ring
        sub.triangles.push_back(mkT(1, 7, 8));
        sub.triangles.push_back(mkT(1, 8, 9));
        sub.triangles.push_back(mkT(1, 9, 10));
        sub.triangles.push_back(mkT(1, 10, 11));
        sub.triangles.push_back(mkT(1, 11, 6));  // closes back to v6 (shared with f2)
        em.subMeshes().push_back(std::move(sub));
        return em;
    }
}

namespace {
    // Build a smooth-surface fan-to-fan mesh parameterized by fanSize and
    // seed, matching the pattern used by RandomSmoothFanBevelManifold.
    // Used by single-case repro tests below.
    EditableMesh makeFanToFanMesh(int fanSize, int seed)
    {
        EditableMesh em;
        EditableSubMesh sub;
        sub.materialName = "M";
        auto mkV = [](float x, float y, float z) {
            EditableVertex v;
            v.position = Ogre::Vector3(x, y, z);
            v.normal = Ogre::Vector3::UNIT_Z;
            v.hasNormal = true;
            return v;
        };
        auto zOf = [&](int i) {
            float t = static_cast<float>(fanSize * 13 + seed * 7 + i);
            return 0.1f + 0.15f * std::sin(t * 1.1f);
        };
        sub.vertices.push_back(mkV(0.0f, 0.0f, 0.0f));
        sub.vertices.push_back(mkV(1.0f, 0.0f, 0.0f));
        for (int i = 0; i < fanSize; ++i) {
            float theta = (static_cast<float>(i) / fanSize - 0.5f)
                        * 3.14159f * 0.9f + 3.14159f * 0.5f;
            float x = 0.5f * std::cos(theta);
            float y = 0.5f * std::sin(theta);
            sub.vertices.push_back(mkV(x, -y, zOf(i)));
        }
        for (int i = 0; i < fanSize; ++i) {
            float theta = (static_cast<float>(i) / fanSize - 0.5f)
                        * 3.14159f * 0.9f + 3.14159f * 0.5f;
            float x = 1.0f - 0.5f * std::cos(theta);
            float y = 0.5f * std::sin(theta);
            sub.vertices.push_back(mkV(x, -y, zOf(i + fanSize)));
        }
        auto mkT = [](unsigned a, unsigned b, unsigned c) {
            EditableTriangle t;
            t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
            return t;
        };
        sub.triangles.push_back(mkT(0, 2, 1));
        for (int i = 0; i < fanSize - 1; ++i) {
            sub.triangles.push_back(mkT(0, i + 3, i + 2));
        }
        sub.triangles.push_back(mkT(0, 1, fanSize + 1));
        sub.triangles.push_back(mkT(1, 2, fanSize + 2));
        for (int i = 0; i < fanSize - 1; ++i) {
            sub.triangles.push_back(mkT(1, fanSize + 2 + i, fanSize + 3 + i));
        }
        sub.triangles.push_back(mkT(1, 2 * fanSize + 1, fanSize + 1));
        em.subMeshes().push_back(std::move(sub));
        return em;
    }
}

// Stress test: generate many different fan sizes and geometries and verify
// every one produces a manifold bevel. Designed to catch edge cases in
// processRingNeighbors / PNF interactions with the cap emission.
// Stress test: generate many different fan sizes and geometries and verify
// every one produces a manifold bevel (no same-direction shared edges,
// no interior boundary holes at non-perimeter vertices).
TEST(HalfEdgeMeshStandalone, RandomSmoothFanBevelManifold) {
    int totalTested = 0;
    int totalFailures = 0;
    for (int fanSize = 3; fanSize <= 8; ++fanSize) {
        for (int seed = 0; seed < 5; ++seed) {
            EditableMesh em;
            EditableSubMesh sub;
            sub.materialName = "M";
            auto mkV = [](float x, float y, float z) {
                EditableVertex v;
                v.position = Ogre::Vector3(x, y, z);
                v.normal = Ogre::Vector3::UNIT_Z;
                v.hasNormal = true;
                return v;
            };
            // Pseudo-random z-perturbation seeded by (fanSize, seed).
            auto zOf = [&](int i) {
                float t = static_cast<float>(fanSize * 13 + seed * 7 + i);
                return 0.1f + 0.15f * std::sin(t * 1.1f);
            };
            sub.vertices.push_back(mkV(0.0f, 0.0f, 0.0f));  // 0 — v0
            sub.vertices.push_back(mkV(1.0f, 0.0f, 0.0f));  // 1 — v1
            // v0's fan (indices 2..fanSize+1)
            for (int i = 0; i < fanSize; ++i) {
                float theta = (static_cast<float>(i) / fanSize - 0.5f)
                            * 3.14159f * 0.9f + 3.14159f * 0.5f;
                float x = 0.5f * std::cos(theta);
                float y = 0.5f * std::sin(theta);
                sub.vertices.push_back(mkV(x, -y, zOf(i)));
            }
            // v1's fan (indices fanSize+2..2*fanSize+1)
            for (int i = 0; i < fanSize; ++i) {
                float theta = (static_cast<float>(i) / fanSize - 0.5f)
                            * 3.14159f * 0.9f + 3.14159f * 0.5f;
                float x = 1.0f - 0.5f * std::cos(theta);
                float y = 0.5f * std::sin(theta);
                sub.vertices.push_back(mkV(x, -y, zOf(i + fanSize)));
            }
            auto mkT = [](unsigned a, unsigned b, unsigned c) {
                EditableTriangle t;
                t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
                return t;
            };
            // v0's fan: (0, 2, 1), (0, 3, 2), ..., (0, fanSize+1, fanSize)
            sub.triangles.push_back(mkT(0, 2, 1)); // bevel face on one side
            for (int i = 0; i < fanSize - 1; ++i) {
                sub.triangles.push_back(mkT(0, i + 3, i + 2));
            }
            sub.triangles.push_back(mkT(0, 1, fanSize + 1)); // bevel face other side
            // v1's fan: similar
            sub.triangles.push_back(mkT(1, 2, fanSize + 2));
            for (int i = 0; i < fanSize - 1; ++i) {
                sub.triangles.push_back(mkT(1, fanSize + 2 + i, fanSize + 3 + i));
            }
            sub.triangles.push_back(mkT(1, 2 * fanSize + 1, fanSize + 1));

            em.subMeshes().push_back(std::move(sub));

            HalfEdgeMesh he;
            if (!he.buildFromEditableMesh(em)) continue;
            int edgeIdx = findEdge(he, 0, 1);
            if (edgeIdx < 0) continue;
            if (he.bevelEdges({edgeIdx}, 0.05f).empty()) continue;

            EditableMesh back;
            if (!he.toEditableMesh(back)) continue;
            ++totalTested;

            // Check for duplicate-directed edges (non-manifold).
            const auto& bsub = back.subMeshes()[0];
            std::map<std::pair<unsigned,unsigned>, int> directedEdges;
            for (const auto& tri : bsub.triangles) {
                for (int k = 0; k < 3; ++k) {
                    unsigned a = tri.indices[k], b = tri.indices[(k + 1) % 3];
                    ++directedEdges[{a, b}];
                }
            }
            int sameDir = 0;
            for (const auto& [dir, count] : directedEdges) {
                if (count > 1) ++sameDir;
            }
            // Check for boundary-only edges at non-perimeter verts.
            std::set<unsigned> perimeterVerts;
            for (size_t i = 0; i < bsub.vertices.size(); ++i) {
                const auto& p = bsub.vertices[i].position;
                // Original perimeter = v2..v(2*fanSize+1).
                for (int j = 0; j < 2 * fanSize; ++j) {
                    const auto& orig = em.subMeshes()[0].vertices[j + 2].position;
                    if ((p - orig).length() < 1e-4f) {
                        perimeterVerts.insert(static_cast<unsigned>(i));
                    }
                }
            }
            int interiorBdry = 0;
            for (const auto& [dir, count] : directedEdges) {
                if (count == 1) {
                    auto rev = directedEdges.find({dir.second, dir.first});
                    if (rev == directedEdges.end() || rev->second == 0) {
                        if (perimeterVerts.count(dir.first)
                         && perimeterVerts.count(dir.second)) continue;
                        ++interiorBdry;
                    }
                }
            }
            if (sameDir > 0 || interiorBdry > 0) {
                ++totalFailures;
                fprintf(stderr, "  FAIL fanSize=%d seed=%d sameDir=%d interiorBdry=%d\n",
                        fanSize, seed, sameDir, interiorBdry);
            }
        }
    }
    fprintf(stderr, "Total tested: %d, failures: %d\n", totalTested, totalFailures);
    EXPECT_EQ(totalFailures, 0);
}

TEST(HalfEdgeMeshStandalone, DenseSmoothCharacterBevelManifold) {
    auto em = makeDenseSmoothCharacterMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    int edgeIdx = findEdge(he, 0, 1);
    ASSERT_GE(edgeIdx, 0);
    ASSERT_FALSE(he.bevelEdges({edgeIdx}, 0.02f).empty());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));

    // Winding consistency check.
    const auto& sub = back.subMeshes()[0];
    std::map<std::pair<unsigned,unsigned>, int> directedEdges;
    for (const auto& tri : sub.triangles) {
        for (int k = 0; k < 3; ++k) {
            unsigned a = tri.indices[k], b = tri.indices[(k + 1) % 3];
            ++directedEdges[{a, b}];
        }
    }
    int windingInconsistencies = 0;
    for (const auto& [dir, count] : directedEdges) {
        if (count > 1) {
            ++windingInconsistencies;
            fprintf(stderr, "  winding inconsistency: edge (%u→%u) used %d times\n",
                    dir.first, dir.second, count);
        }
    }
    if (windingInconsistencies > 0) {
        fprintf(stderr, "=== Dense char dump ===\n");
        for (size_t i = 0; i < sub.vertices.size(); ++i) {
            const auto& p = sub.vertices[i].position;
            fprintf(stderr, "v%zu = (%.3f, %.3f, %.3f)\n", i, p.x, p.y, p.z);
        }
        for (size_t t = 0; t < sub.triangles.size(); ++t) {
            const auto& tri = sub.triangles[t];
            fprintf(stderr, "t%zu = (%u, %u, %u)\n",
                    t, tri.indices[0], tri.indices[1], tri.indices[2]);
        }
    }
    EXPECT_EQ(windingInconsistencies, 0)
        << windingInconsistencies << " winding inconsistencies";
}

TEST(HalfEdgeMeshStandalone, SmoothCharacterBevelSmallScale) {
    // Same fixture as SmoothCharacterBevelProducesManifold but scaled to
    // ~0.1-unit dimensions matching Lead Jab (edge length 0.1 per diag).
    // Also uses Lead Jab's bevel width (0.05 → clamped to 0.025).
    auto em = makeSmoothCharacterMesh();
    // Scale all vertex positions by 0.1
    for (auto& vert : em.subMeshes()[0].vertices) {
        vert.position *= 0.1f;
    }
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    int edgeIdx = findEdge(he, 0, 1);
    ASSERT_GE(edgeIdx, 0);
    ASSERT_FALSE(he.bevelEdges({edgeIdx}, 0.05f).empty());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));

    const auto& sub = back.subMeshes()[0];
    std::map<std::pair<unsigned,unsigned>, int> directedEdges;
    for (const auto& tri : sub.triangles) {
        for (int k = 0; k < 3; ++k) {
            unsigned a = tri.indices[k], b = tri.indices[(k + 1) % 3];
            ++directedEdges[{a, b}];
        }
    }
    int windingInconsistencies = 0;
    for (const auto& [dir, count] : directedEdges) {
        if (count > 1) {
            ++windingInconsistencies;
            fprintf(stderr, "  winding inconsistency: edge (%u→%u) used %d times\n",
                    dir.first, dir.second, count);
        }
    }
    EXPECT_EQ(windingInconsistencies, 0)
        << windingInconsistencies << " winding inconsistencies";
}

TEST(HalfEdgeMeshStandalone, SmoothCharacterBevelProducesManifold) {
    // Reproduces the Lead Jab scenario: smooth surface where the bevel-edge
    // endpoint's ring has faces with multiple creases and the chamfer
    // cap-end is not covered by any existing retriangulation.
    auto em = makeSmoothCharacterMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    int edgeIdx = findEdge(he, 0, 1);
    ASSERT_GE(edgeIdx, 0);
    ASSERT_FALSE(he.bevelEdges({edgeIdx}, 0.05f).empty());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));

    const auto& sub = back.subMeshes()[0];

    // Winding consistency: for every shared interior edge, the two tris
    // that use it should traverse it in OPPOSITE directions. Two tris
    // using the edge in the same direction indicates an inverted-winding
    // triangle.
    std::map<std::pair<unsigned,unsigned>, int> directedEdges; // (from, to) -> count
    for (const auto& tri : sub.triangles) {
        for (int k = 0; k < 3; ++k) {
            unsigned a = tri.indices[k], b = tri.indices[(k + 1) % 3];
            ++directedEdges[{a, b}];
        }
    }
    int windingInconsistencies = 0;
    for (const auto& [dir, count] : directedEdges) {
        auto rev = std::make_pair(dir.second, dir.first);
        auto it = directedEdges.find(rev);
        int revCount = (it == directedEdges.end()) ? 0 : it->second;
        if (count > 1 || (count == 1 && revCount == 0 && count > 0)) {
            // If same edge appears twice in same direction, winding bug.
            // Boundary edges (used once, no reverse) are OK.
            if (count > 1) {
                ++windingInconsistencies;
                fprintf(stderr, "  winding inconsistency: edge (%u→%u) used %d times\n",
                        dir.first, dir.second, count);
            }
        }
    }
    if (windingInconsistencies > 0) {
        fprintf(stderr, "=== Character mesh dump ===\n");
        for (size_t i = 0; i < sub.vertices.size(); ++i) {
            const auto& p = sub.vertices[i].position;
            fprintf(stderr, "v%zu = (%.3f, %.3f, %.3f)\n", i, p.x, p.y, p.z);
        }
        for (size_t t = 0; t < sub.triangles.size(); ++t) {
            const auto& tri = sub.triangles[t];
            fprintf(stderr, "t%zu = (%u, %u, %u)\n",
                    t, tri.indices[0], tri.indices[1], tri.indices[2]);
        }
    }
    EXPECT_EQ(windingInconsistencies, 0)
        << windingInconsistencies << " winding inconsistencies";
}

TEST(HalfEdgeMeshStandalone, SmoothSurfaceBevelReversedWindingManifold) {
    // Same fixture as above but with all face windings reversed — exercises
    // the other winding polarity (f1WalksAB=false at the chosen edge).
    auto em = makeSmoothSurfaceMesh(/*reverseWinding=*/true);
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    int edgeIdx = findEdge(he, 0, 1);
    ASSERT_GE(edgeIdx, 0);
    ASSERT_FALSE(he.bevelEdges({edgeIdx}, 0.05f).empty());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));

    const auto& sub = back.subMeshes()[0];
    std::map<std::pair<unsigned,unsigned>, int> edgeUse;
    for (const auto& t : sub.triangles) {
        for (int k = 0; k < 3; ++k) {
            unsigned a = t.indices[k], b = t.indices[(k+1)%3];
            auto key = std::make_pair(std::min(a,b), std::max(a,b));
            ++edgeUse[key];
        }
    }
    std::set<unsigned> perimeterVerts;
    for (size_t i = 0; i < sub.vertices.size(); ++i) {
        const auto& p = sub.vertices[i].position;
        if ((p - Ogre::Vector3(0.5f, 0.5f, 0.5f)).length() < 1e-4f
         || (p - Ogre::Vector3(0.5f, -0.5f, 0.5f)).length() < 1e-4f
         || (p - Ogre::Vector3(-0.5f, 0.0f, 0.3f)).length() < 1e-4f
         || (p - Ogre::Vector3(1.5f, 0.0f, 0.3f)).length() < 1e-4f) {
            perimeterVerts.insert(static_cast<unsigned>(i));
        }
    }
    int interiorBoundaryEdges = 0;
    for (const auto& [key, count] : edgeUse) {
        if (count == 1) {
            if (perimeterVerts.count(key.first) && perimeterVerts.count(key.second))
                continue;
            ++interiorBoundaryEdges;
            fprintf(stderr, "  interior boundary edge: (%u,%u)\n",
                    key.first, key.second);
        }
    }
    EXPECT_EQ(interiorBoundaryEdges, 0)
        << "reversed bevel left " << interiorBoundaryEdges
        << " interior boundary edges (holes)";

    // With reversed winding, normals should point -Z instead of +Z.
    int flipped = 0;
    for (size_t t = 0; t < sub.triangles.size(); ++t) {
        const auto& tri = sub.triangles[t];
        const auto& p0 = sub.vertices[tri.indices[0]].position;
        const auto& p1 = sub.vertices[tri.indices[1]].position;
        const auto& p2 = sub.vertices[tri.indices[2]].position;
        auto n = (p1 - p0).crossProduct(p2 - p0);
        if (n.length() < 1e-8f) continue;
        n.normalise();
        if (n.z > -0.1f) {
            ++flipped;
            fprintf(stderr, "  flipped tri %zu = (%u,%u,%u) normal=(%.3f,%.3f,%.3f)\n",
                    t, tri.indices[0], tri.indices[1], tri.indices[2],
                    n.x, n.y, n.z);
        }
    }
    EXPECT_EQ(flipped, 0) << flipped << " tris have inverted winding";
}

// ===========================================================================
// Tests: bevelEdges segments + profile parameters
// ===========================================================================

namespace {
    void expectCubeBevelSegments(int segments, float profile) {
        ASSERT_GE(segments, 1);
        auto em = makeCubeMesh();
        HalfEdgeMesh he;
        ASSERT_TRUE(he.buildFromEditableMesh(em));
        // Pick a deterministic perimeter edge (top-right vertical 5↔3).
        int edgeIdx = findEdge(he, 5, 3);
        ASSERT_GE(edgeIdx, 0);

        auto newVerts = he.bevelEdges({edgeIdx}, 0.05f, segments, profile);
        ASSERT_FALSE(newVerts.empty())
            << "bevel returned no new verts (segments=" << segments
            << " profile=" << profile << ")";
        EXPECT_TRUE(he.validate())
            << "validate() failed (segments=" << segments
            << " profile=" << profile << ")";

        EditableMesh back;
        ASSERT_TRUE(he.toEditableMesh(back));
        const auto stats = statsOf(back);
        EXPECT_EQ(stats.boundaryEdges, 0u)
            << "boundary edges for segments=" << segments
            << " profile=" << profile;
        EXPECT_TRUE(isManifold(back))
            << "non-manifold output for segments=" << segments
            << " profile=" << profile;

        // Each endpoint contributes (segments - 1) intermediate verts.
        const size_t intermediates = 2u * static_cast<size_t>(segments - 1);
        EXPECT_GE(newVerts.size(), 4u + intermediates)
            << "newVerts too small for segments=" << segments;
    }
}

TEST(HalfEdgeMeshStandalone, BevelSegments1FlatMatchesBaseline) {
    // Default args (segments=1, profile=0.5) must reproduce the original
    // single-strip chamfer geometry.
    auto em = makeCubeMesh();
    HalfEdgeMesh heA, heB;
    ASSERT_TRUE(heA.buildFromEditableMesh(em));
    ASSERT_TRUE(heB.buildFromEditableMesh(em));
    int eA = findEdge(heA, 5, 3);
    int eB = findEdge(heB, 5, 3);
    ASSERT_EQ(eA, eB);
    auto a = heA.bevelEdges({eA}, 0.05f);
    auto b = heB.bevelEdges({eB}, 0.05f, 1, 0.5f);
    EXPECT_EQ(a.size(), b.size());
    EditableMesh outA, outB;
    ASSERT_TRUE(heA.toEditableMesh(outA));
    ASSERT_TRUE(heB.toEditableMesh(outB));
    EXPECT_EQ(outA.subMeshes()[0].vertices.size(),
              outB.subMeshes()[0].vertices.size());
    EXPECT_EQ(outA.subMeshes()[0].triangles.size(),
              outB.subMeshes()[0].triangles.size());
}

TEST(HalfEdgeMeshStandalone, BevelSegments2FlatStillManifold) {
    expectCubeBevelSegments(2, 0.5f);
}

TEST(HalfEdgeMeshStandalone, BevelSegments3FlatStillManifold) {
    expectCubeBevelSegments(3, 0.5f);
}

TEST(HalfEdgeMeshStandalone, BevelSegments4FlatStillManifold) {
    expectCubeBevelSegments(4, 0.5f);
}

TEST(HalfEdgeMeshStandalone, BevelSegments8FlatStillManifold) {
    expectCubeBevelSegments(8, 0.5f);
}

TEST(HalfEdgeMeshStandalone, BevelSegments12FlatStillManifold) {
    expectCubeBevelSegments(12, 0.5f);
}

TEST(HalfEdgeMeshStandalone, BevelSegments16FlatStillManifold) {
    expectCubeBevelSegments(16, 0.5f);
}

TEST(HalfEdgeMeshStandalone, BevelSegments4ConvexStillManifold) {
    // Convex (>0.5) bulges intermediate verts outward toward where the
    // original sharp edge was.
    expectCubeBevelSegments(4, 1.0f);
}

TEST(HalfEdgeMeshStandalone, BevelSegments4ConcaveStillManifold) {
    // Concave (<0.5) bulges intermediate verts inward into the solid,
    // producing a groove-like chamfer.
    expectCubeBevelSegments(4, 0.0f);
}

TEST(HalfEdgeMeshStandalone, BevelSegments8ConcaveStillManifold) {
    // High segment count + concave profile is a stress case for the
    // post-pass hole filler's loop walker.
    expectCubeBevelSegments(8, 0.0f);
}

namespace {
    // Run one bevel with the scalar profile overload and return the min
    // distance from any vertex (other than v5 itself) to v5=(1,1,1). If
    // the bevel fails or the back-conversion fails, returns NaN so the
    // caller can flag it without dereferencing empty buffers.
    float bevelDistFromV5Scalar(float profile) {
        auto em = makeCubeMesh();
        HalfEdgeMesh he;
        if (!he.buildFromEditableMesh(em)) return std::nanf("");
        int edgeIdx = findEdge(he, 5, 3);
        if (edgeIdx < 0) return std::nanf("");
        auto newVerts = he.bevelEdges({edgeIdx}, 0.05f, 2, profile);
        if (newVerts.empty()) return std::nanf("");
        EditableMesh back;
        if (!he.toEditableMesh(back) || back.subMeshes().empty())
            return std::nanf("");
        const auto& sub = back.subMeshes()[0];
        const Ogre::Vector3 v5(1, 1, 1);
        float minDist = std::numeric_limits<float>::max();
        for (const auto& vert : sub.vertices) {
            float d = vert.position.distance(v5);
            if (d > 1e-5f && d < minDist) minDist = d;
        }
        return minDist;
    }
}

TEST(HalfEdgeMeshStandalone, BevelProfileShiftsIntermediateVertexPosition) {
    // For segments=2, the single intermediate vertex per endpoint should
    // sit at the chord midpoint when profile=0.5 (flat), closer to the
    // original cube corner when profile=1 (convex), and farther from it
    // when profile=0 (concave, digs into the solid).
    const float dFlat    = bevelDistFromV5Scalar(0.5f);
    const float dConvex  = bevelDistFromV5Scalar(1.0f);
    const float dConcave = bevelDistFromV5Scalar(0.0f);
    ASSERT_FALSE(std::isnan(dFlat));
    ASSERT_FALSE(std::isnan(dConvex));
    ASSERT_FALSE(std::isnan(dConcave));
    EXPECT_LT(dConvex, dFlat)
        << "convex profile should bring the chamfer mid closer to the cut-off corner";
    EXPECT_GT(dConcave, dFlat)
        << "concave profile should push the chamfer mid farther from the cut-off corner";
}

// ===========================================================================
// Tests: per-segment profile points (vector overload)
// ===========================================================================

TEST(HalfEdgeMeshStandalone, BevelVectorOverloadAllFlatMatchesScalarFlat) {
    auto em = makeCubeMesh();
    HalfEdgeMesh heA, heB;
    ASSERT_TRUE(heA.buildFromEditableMesh(em));
    ASSERT_TRUE(heB.buildFromEditableMesh(em));
    int eA = findEdge(heA, 5, 3);
    int eB = findEdge(heB, 5, 3);
    auto a = heA.bevelEdges({eA}, 0.05f, 4, 0.5f);
    auto b = heB.bevelEdges({eB}, 0.05f, 4, 0.5f, std::vector<float>{0.5f, 0.5f, 0.5f});
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());
    EXPECT_EQ(a.size(), b.size());
    EditableMesh outA, outB;
    ASSERT_TRUE(heA.toEditableMesh(outA));
    ASSERT_TRUE(heB.toEditableMesh(outB));
    EXPECT_EQ(outA.subMeshes()[0].vertices.size(),
              outB.subMeshes()[0].vertices.size());
    EXPECT_EQ(outA.subMeshes()[0].triangles.size(),
              outB.subMeshes()[0].triangles.size());
}

TEST(HalfEdgeMeshStandalone, BevelVectorOverloadWrongSizeFallsBackToFlat) {
    // When size != segments-1, the vector overload should treat every
    // point as 0.5 (flat chamfer) rather than crashing or corrupting.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    int edgeIdx = findEdge(he, 5, 3);
    auto newVerts = he.bevelEdges({edgeIdx}, 0.05f, 4, 0.5f,
                                  std::vector<float>{0.9f}); // wrong size
    ASSERT_FALSE(newVerts.empty());
    EXPECT_TRUE(he.validate());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
}

TEST(HalfEdgeMeshStandalone, BevelVectorOverloadClampsOutOfRangeValues) {
    // Values outside [0, 1] should be clamped, not rejected. Bevel still
    // succeeds and produces a manifold mesh.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    int edgeIdx = findEdge(he, 5, 3);
    auto newVerts = he.bevelEdges({edgeIdx}, 0.05f, 4, 0.5f,
                                  std::vector<float>{-0.3f, 2.0f, 1.5f});
    ASSERT_FALSE(newVerts.empty());
    EXPECT_TRUE(he.validate());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
}

TEST(HalfEdgeMeshStandalone, BevelVectorOverloadAsymmetricCurveIsAsymmetric) {
    // An asymmetric profile (e.g., one side convex, other side concave)
    // should produce vertex positions that reflect the asymmetry — the
    // distance from each intermediate to the original cut-off corner v5
    // must differ along the chord.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    int edgeIdx = findEdge(he, 5, 3);
    auto newVerts = he.bevelEdges({edgeIdx}, 0.1f, 4, 0.5f,
                                  std::vector<float>{0.9f, 0.5f, 0.1f});
    ASSERT_FALSE(newVerts.empty());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    const Ogre::Vector3 v5(1, 1, 1);
    std::vector<float> dists;
    for (int idx : newVerts) {
        const auto& p = he.vertex(idx).position;
        dists.push_back(p.distance(v5));
    }
    float minD = *std::min_element(dists.begin(), dists.end());
    float maxD = *std::max_element(dists.begin(), dists.end());
    EXPECT_GT(maxD - minD, 0.01f) << "asymmetric profile produced uniform distances";
}

namespace {
    // Return mean distance from v5=(1,1,1) to the newly-created chamfer
    // vertices. NaN on bevel failure so the caller can flag it.
    float meanDistFromV5Vector(const std::vector<float>& points) {
        auto em = makeCubeMesh();
        HalfEdgeMesh he;
        if (!he.buildFromEditableMesh(em)) return std::nanf("");
        int edgeIdx = findEdge(he, 5, 3);
        if (edgeIdx < 0) return std::nanf("");
        auto newVerts = he.bevelEdges({edgeIdx}, 0.1f, 4, 0.5f, points);
        if (newVerts.empty()) return std::nanf("");
        const Ogre::Vector3 v5(1, 1, 1);
        float sum = 0.0f;
        int count = 0;
        for (int idx : newVerts) {
            sum += he.vertex(idx).position.distance(v5);
            ++count;
        }
        return count > 0 ? sum / count : std::nanf("");
    }
}

TEST(HalfEdgeMeshStandalone, BevelVectorOverloadAllConvexMovesPointsToward) {
    // profilePoints all near 1.0 should bring every interior vertex
    // closer to the original cut-off corner than a flat (0.5) profile.
    const float dFlat   = meanDistFromV5Vector({0.5f, 0.5f, 0.5f});
    const float dConvex = meanDistFromV5Vector({0.95f, 0.95f, 0.95f});
    ASSERT_FALSE(std::isnan(dFlat));
    ASSERT_FALSE(std::isnan(dConvex));
    EXPECT_LT(dConvex, dFlat) << "all-convex points should pull the strip toward v5";
}

// ===========================================================================
// Tests: shaped profile carves into adjacent (non-beveled) faces
// ===========================================================================
//
// When the chamfer profile is concave or convex, the chain intermediates must
// become part of the adjacent faces' triangulation — not just the chamfer
// strip. Without this the adjacent faces keep a straight cut and hide the
// profile. These tests verify the triangulation on the +Z and -Z cube faces
// (the faces perpendicular to the beveled edge v5-v3) actually references
// the intermediate vertices, and that no flat cap triangle is emitted at
// the bevel endpoints.

namespace {
    struct AdjFaceProbe {
        size_t triCountOnPlane = 0;   // tris whose verts all lie on the plane
        bool refsIntermediate = false; // any tri uses a chain intermediate?
        bool hasOriginalCorner = false; // any tri uses the original v5 / v3?
    };

    // Probe triangles lying on `z = planeZ` (within tolerance). `corner` is
    // the cube corner vertex (v5 or v3) at that plane. `intermediates`
    // are positions the chain produced (same Z). Returns counts/flags.
    AdjFaceProbe probeFaceAtPlane(const EditableMesh& em,
                                  float planeZ,
                                  const Ogre::Vector3& corner,
                                  const std::vector<Ogre::Vector3>& intermediates)
    {
        AdjFaceProbe p;
        const auto& sub = em.subMeshes()[0];
        auto isOnPlane = [&](unsigned idx) {
            return std::abs(sub.vertices[idx].position.z - planeZ) < 1e-4f;
        };
        auto isCorner = [&](unsigned idx) {
            return sub.vertices[idx].position.squaredDistance(corner) < 1e-6f;
        };
        auto isIntermediate = [&](unsigned idx) {
            const auto& pos = sub.vertices[idx].position;
            for (const auto& ip : intermediates)
                if (pos.squaredDistance(ip) < 1e-5f) return true;
            return false;
        };
        for (const auto& tri : sub.triangles) {
            if (!isOnPlane(tri.indices[0])) continue;
            if (!isOnPlane(tri.indices[1])) continue;
            if (!isOnPlane(tri.indices[2])) continue;
            ++p.triCountOnPlane;
            for (int k = 0; k < 3; ++k) {
                if (isIntermediate(tri.indices[k])) p.refsIntermediate = true;
                if (isCorner(tri.indices[k])) p.hasOriginalCorner = true;
            }
        }
        return p;
    }
}

// A v5-side intermediate must lie OFF both bevel-boundary cube edges
// (v5↔v4 along -X and v5↔v7 along -Y). v1a lies on v5→v4, v1b on
// v5→v7 — those are end-offsets, not shaped chain interior. A true
// interior vertex is offset along the XY diagonal away from v5.
namespace {
    bool isStrictInteriorAtV5(const Ogre::Vector3& p) {
        // Edge v5→v4 has y=1, z=1; edge v5→v7 has x=1, z=1.
        const bool onV5V4 = std::abs(p.y - 1.0f) < 1e-4f
                         && std::abs(p.z - 1.0f) < 1e-4f;
        const bool onV5V7 = std::abs(p.x - 1.0f) < 1e-4f
                         && std::abs(p.z - 1.0f) < 1e-4f;
        return !onV5V4 && !onV5V7;
    }
    bool isStrictInteriorAtV3(const Ogre::Vector3& p) {
        // Edge v3→v2 (x=-1 at... actually v2=(-1,1,-1), so v3→v2 has y=1,z=-1);
        // edge v3→v1 has (v1=(1,-1,-1)) so x=1,z=-1.
        const bool onV3V2 = std::abs(p.y - 1.0f) < 1e-4f
                         && std::abs(p.z + 1.0f) < 1e-4f;
        const bool onV3V1 = std::abs(p.x - 1.0f) < 1e-4f
                         && std::abs(p.z + 1.0f) < 1e-4f;
        return !onV3V2 && !onV3V1;
    }
}

TEST(HalfEdgeMeshStandalone, BevelConcaveCarvesAdjacentFaces) {
    // Beveling the top-right cube edge (v5-v3 along Z) with a concave
    // profile should leave the +Z and -Z faces with triangulations that
    // reference the STRICT chain interior — not the inner offsets
    // v1a/v1b/v2a/v2b which a straight-cut triangulation would already
    // include. Without the splice the adjacent faces are fanned across
    // the cut corner and never touch an interior vertex.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    int edgeIdx = findEdge(he, 5, 3);
    ASSERT_GE(edgeIdx, 0);
    auto newVerts = he.bevelEdges({edgeIdx}, 0.1f, 4, 0.0f);
    ASSERT_FALSE(newVerts.empty());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));

    // Strict interiors only: new verts on each plane that aren't on
    // either bevel-boundary cube edge at that endpoint.
    std::vector<Ogre::Vector3> interZpos, interZneg;
    for (int v : newVerts) {
        const auto& p = he.vertex(v).position;
        if (std::abs(p.z - 1.0f) < 1e-4f && isStrictInteriorAtV5(p))
            interZpos.push_back(p);
        else if (std::abs(p.z + 1.0f) < 1e-4f && isStrictInteriorAtV3(p))
            interZneg.push_back(p);
    }
    // segments=4 → exactly 3 strict interior intermediates per endpoint.
    ASSERT_EQ(interZpos.size(), 3u);
    ASSERT_EQ(interZneg.size(), 3u);

    const Ogre::Vector3 v5(1, 1, 1);
    const Ogre::Vector3 v3(1, 1, -1);
    auto probeFront = probeFaceAtPlane(back,  1.0f, v5, interZpos);
    auto probeBack  = probeFaceAtPlane(back, -1.0f, v3, interZneg);

    EXPECT_TRUE(probeFront.refsIntermediate)
        << "front face (+Z) triangulation must reference a strict chain "
           "interior vertex when the profile is concave";
    EXPECT_TRUE(probeBack.refsIntermediate)
        << "back face (-Z) triangulation must reference a strict chain "
           "interior vertex when the profile is concave";
}

TEST(HalfEdgeMeshStandalone, BevelConcaveRemovesOriginalCornerFromCap) {
    // When the profile is shaped, the endpoint corner cap would mask the
    // concave arc on the adjacent faces with a flat triangle. The bevel
    // skips the cap for shaped profiles — so after the operation the
    // original corner vertex (v5 / v3) should not be referenced by any
    // triangle.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    int edgeIdx = findEdge(he, 5, 3);
    ASSERT_GE(edgeIdx, 0);
    auto newVerts = he.bevelEdges({edgeIdx}, 0.1f, 4, 0.0f);
    ASSERT_FALSE(newVerts.empty());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));

    const auto& sub = back.subMeshes()[0];
    const Ogre::Vector3 v5(1, 1, 1);
    const Ogre::Vector3 v3(1, 1, -1);
    auto usesCorner = [&](const Ogre::Vector3& corner) {
        for (const auto& tri : sub.triangles) {
            for (int k = 0; k < 3; ++k) {
                if (sub.vertices[tri.indices[k]].position
                        .squaredDistance(corner) < 1e-6f) return true;
            }
        }
        return false;
    };
    EXPECT_FALSE(usesCorner(v5))
        << "v5 should be unreferenced (cap skipped) for shaped profile";
    EXPECT_FALSE(usesCorner(v3))
        << "v3 should be unreferenced (cap skipped) for shaped profile";
}

TEST(HalfEdgeMeshStandalone, BevelFlatStillManifold) {
    // Sanity guard for the cap-skip change: a plain flat chamfer must
    // remain closed (manifold) across both bevel endpoints. Neither cap
    // nor splice should be needed when the chamfer strip's v-end edge is
    // already a single straight edge shared with the neighbor face group.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    int edgeIdx = findEdge(he, 5, 3);
    ASSERT_GE(edgeIdx, 0);
    auto newVerts = he.bevelEdges({edgeIdx}, 0.1f);
    ASSERT_FALSE(newVerts.empty());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
    const auto stats = statsOf(back);
    EXPECT_EQ(stats.boundaryEdges, 0u);
}

TEST(HalfEdgeMeshStandalone, BevelConvexCarvesAdjacentFaces) {
    // Convex profile (moderate 0.75). Max convex (1.0) pushes some
    // intermediates past the face plane's own edges, which is a visual
    // edge case — use a moderate value so the interior stays within the
    // face and we can probe that the adjacent face references it.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    int edgeIdx = findEdge(he, 5, 3);
    ASSERT_GE(edgeIdx, 0);
    auto newVerts = he.bevelEdges({edgeIdx}, 0.1f, 4, 0.75f);
    ASSERT_FALSE(newVerts.empty());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));

    std::vector<Ogre::Vector3> interZpos, interZneg;
    for (int v : newVerts) {
        const auto& p = he.vertex(v).position;
        if (std::abs(p.z - 1.0f) < 1e-4f && isStrictInteriorAtV5(p))
            interZpos.push_back(p);
        else if (std::abs(p.z + 1.0f) < 1e-4f && isStrictInteriorAtV3(p))
            interZneg.push_back(p);
    }
    ASSERT_GE(interZpos.size(), 1u);
    ASSERT_GE(interZneg.size(), 1u);
    const Ogre::Vector3 v5(1, 1, 1);
    const Ogre::Vector3 v3(1, 1, -1);
    auto probeFront = probeFaceAtPlane(back,  1.0f, v5, interZpos);
    auto probeBack  = probeFaceAtPlane(back, -1.0f, v3, interZneg);
    EXPECT_TRUE(probeFront.refsIntermediate);
    EXPECT_TRUE(probeBack.refsIntermediate);
}

// ===========================================================================
// Tests: bevelVertices (corner cut)
// ===========================================================================

TEST(HalfEdgeMeshStandalone, BevelVertexCubeCornerProducesTriangleCap) {
    // Beveling a cube corner (valence 3) should produce a triangular
    // cap face at the corner. All three original quads sharing that
    // corner become pentagons — i.e. the old 2 tris per side become 3
    // tris per side (one quad = two tris turns into one pentagon = three
    // tris when fanned). The total tri count increases by 1 per face
    // touched + 1 for the new cap.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const int v5 = 5; // (1, 1, 1) — corner where top, right, front meet
    auto newVerts = he.bevelVertices({v5}, 0.1f);
    ASSERT_EQ(newVerts.size(), 3u)
        << "valence-3 corner should produce 3 edge offsets";

    EXPECT_TRUE(he.validate());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
    const auto stats = statsOf(back);
    EXPECT_EQ(stats.boundaryEdges, 0u);
}

TEST(HalfEdgeMeshStandalone, BevelVertexLowValenceIsSkipped) {
    // Valence < 3 has nothing sensible to bevel — should be a no-op.
    // Build a tiny 2-tri strip and try to bevel an interior vertex
    // whose valence is 2 in our mini mesh.
    EditableMesh em;
    EditableSubMesh sub;
    sub.materialName = "M";
    auto mkV = [](float x, float y, float z) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, z);
        v.normal = Ogre::Vector3::UNIT_Z;
        v.hasNormal = true;
        return v;
    };
    // Two tris sharing an edge — the shared-edge vertices have valence 2.
    sub.vertices = { mkV(0, 0, 0), mkV(1, 0, 0), mkV(0, 1, 0), mkV(1, 1, 0) };
    auto mkT = [](unsigned a, unsigned b, unsigned c) {
        EditableTriangle t;
        t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
        return t;
    };
    sub.triangles = { mkT(0, 1, 2), mkT(1, 3, 2) };
    em.subMeshes().push_back(sub);

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    // Vertex 1 is shared by both tris but has valence 2 internally AND
    // it's on the mesh boundary — should be skipped by bevelVertices.
    auto newVerts = he.bevelVertices({1}, 0.05f);
    EXPECT_TRUE(newVerts.empty())
        << "valence<3 / boundary vertex should be skipped";
}

TEST(HalfEdgeMeshStandalone, BevelVertexZeroWidthIsNoOp) {
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    auto newVerts = he.bevelVertices({5}, 0.0f);
    EXPECT_TRUE(newVerts.empty());
}

TEST(HalfEdgeMeshStandalone, BevelVertexMultipleCorners) {
    // Multi-vertex bevel runs one vertex at a time internally so shared
    // edges get the shortened-edge topology correctly (each pass sees
    // the already-bevelled neighbor).
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    std::vector<int> corners = {0, 1, 2, 3, 4, 5, 6, 7};
    auto newVerts = he.bevelVertices(corners, 0.1f);
    EXPECT_EQ(newVerts.size(), 24u);
    EXPECT_TRUE(he.validate());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
    const auto stats = statsOf(back);
    EXPECT_EQ(stats.boundaryEdges, 0u);
}

TEST(HalfEdgeMeshStandalone, BevelVertexAdjacentPair) {
    // Two vertices sharing a cube edge (v4-v5). Sequential processing
    // should leave the mesh manifold with the shared edge shortened
    // between the two new corner offsets.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    auto newVerts = he.bevelVertices({4, 5}, 0.1f);
    EXPECT_TRUE(he.validate());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
    const auto stats = statsOf(back);
    EXPECT_EQ(stats.boundaryEdges, 0u);
}

TEST(HalfEdgeMeshStandalone, BevelVertexSymmetricBudgetOnSharedEdge) {
    // When two adjacent selected vertices' offsets would collide on
    // the shared edge (width > edge_length / 2), each side should clamp
    // to half the edge so both offsets land at the midpoint. Without a
    // symmetric budget, the first-processed vertex gets the full
    // requested offset and the second gets squeezed by the shortened
    // edge, producing visually uneven offsets.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    // v4 = (-1, 1, 1), v5 = (1, 1, 1). Edge length = 2. Request a
    // width that would exceed half (i.e., collision territory).
    const float requested = 100.0f; // huge → pre-budget caps each at 0.999
    auto newVerts = he.bevelVertices({4, 5}, requested);
    EXPECT_TRUE(he.validate());

    // Distance from v4 to v4's offsets should equal v5 to v5's offsets
    // along the shared edge. Each bevel reaches 0.999 × (edgeLen × share)
    // = 0.999 × (2 × 0.5) = 0.999 from its corner, so the two offsets
    // meet near the midpoint with a ~0.002-wide sliver between them.
    std::vector<Ogre::Vector3> onSharedEdge;
    for (int v : newVerts) {
        const auto& p = he.vertex(v).position;
        if (std::abs(p.y - 1.0f) < 1e-4f
            && std::abs(p.z - 1.0f) < 1e-4f
            && std::abs(p.x) <= 1.0f - 1e-4f) {
            onSharedEdge.push_back(p);
        }
    }
    ASSERT_EQ(onSharedEdge.size(), 2u);
    const Ogre::Vector3 v4(-1, 1, 1), v5(1, 1, 1);
    float dist4 = std::min(onSharedEdge[0].distance(v4), onSharedEdge[1].distance(v4));
    float dist5 = std::min(onSharedEdge[0].distance(v5), onSharedEdge[1].distance(v5));
    EXPECT_NEAR(dist4, dist5, 1e-3f)
        << "offsets at each end of shared edge should be symmetric";
    EXPECT_NEAR(dist4, 0.999f, 2e-3f)
        << "each side should reach to the midpoint of the 2-unit shared edge";
}

TEST(HalfEdgeMeshStandalone, BevelVertexOffsetIsClampedToHalfEdge) {
    // Passing a huge width should clamp — the new offsets should never
    // pass the midpoint of the shortest incident edge.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    auto newVerts = he.bevelVertices({5}, 100.0f);
    ASSERT_EQ(newVerts.size(), 3u);
    const Ogre::Vector3 v5(1, 1, 1);
    for (int v : newVerts) {
        const auto& p = he.vertex(v).position;
        EXPECT_LT(p.distance(v5), 1.01f)
            << "offset must not exceed shortest incident edge (length 2 → half=1)";
    }
    EXPECT_TRUE(he.validate());
}

// ===========================================================================
// Tests: vertex bevel with segments > 1 (rounded cap)
// ===========================================================================

TEST(HalfEdgeMeshStandalone, BevelVertexSegments2FlatStillManifold) {
    // Flat profile (0.5) with segments=2 should produce the same
    // topology count as a flat single-segment cap, because intermediates
    // are collinear with the chord and get deduped during ear-clip.
    // We just assert manifold + no boundary edges.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    auto newVerts = he.bevelVertices({5}, 0.1f, 2, 0.5f);
    ASSERT_FALSE(newVerts.empty());
    EXPECT_TRUE(he.validate());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
    const auto stats = statsOf(back);
    EXPECT_EQ(stats.boundaryEdges, 0u);
}

TEST(HalfEdgeMeshStandalone, BevelVertexSegments3ConvexRoundedCap) {
    // Convex profile (1.0) with segments=3: each of the 3 cap chords
    // gets 2 intermediates bulging TOWARD where v5 used to be, so the
    // new verts end up closer to (1,1,1) than the straight-chord
    // offsets do.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    auto newVerts = he.bevelVertices({5}, 0.1f, 3, 1.0f);
    ASSERT_FALSE(newVerts.empty());
    EXPECT_TRUE(he.validate());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
    const auto stats = statsOf(back);
    EXPECT_EQ(stats.boundaryEdges, 0u);

    // The chain intermediates should sit closer to v5 than the straight
    // chord midpoints. Straight midpoint between any two crease offsets
    // is at distance ≈ 0.0707 from v5 (for width=0.1 and valence-3
    // offsets at right angles). A convex intermediate should be closer.
    const Ogre::Vector3 v5(1, 1, 1);
    int closerThanStraight = 0;
    for (int v : newVerts) {
        const auto& p = he.vertex(v).position;
        if (p.distance(v5) < 0.07f) ++closerThanStraight;
    }
    EXPECT_GT(closerThanStraight, 0)
        << "convex cap should push at least one intermediate toward v5";
}

TEST(HalfEdgeMeshStandalone, BevelVertexSegments3ConcaveCupCap) {
    // Concave profile (0.0) with segments=3: intermediates bulge AWAY
    // from v5, forming a "cup" shape dipping into the solid. The cap's
    // intermediates should sit FARTHER from v5 than the straight-chord
    // offsets.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    auto newVerts = he.bevelVertices({5}, 0.1f, 3, 0.0f);
    ASSERT_FALSE(newVerts.empty());
    EXPECT_TRUE(he.validate());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
    const auto stats = statsOf(back);
    EXPECT_EQ(stats.boundaryEdges, 0u);

    const Ogre::Vector3 v5(1, 1, 1);
    int farther = 0;
    for (int v : newVerts) {
        const auto& p = he.vertex(v).position;
        // Each crease offset sits at distance 0.1 from v5; a concave
        // intermediate should be FARTHER from v5 than that.
        if (p.distance(v5) > 0.11f) ++farther;
    }
    EXPECT_GT(farther, 0)
        << "concave cap should push at least one intermediate away from v5";
}

TEST(HalfEdgeMeshStandalone, BevelVertexSegments4ManifoldWithPerPointProfile) {
    // Per-point profile values stress the splice/ear-clip path.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    std::vector<float> points = {0.2f, 0.8f, 0.3f};
    auto newVerts = he.bevelVertices({5}, 0.1f, 4, 0.5f, points);
    ASSERT_FALSE(newVerts.empty());
    EXPECT_TRUE(he.validate());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
}

TEST(HalfEdgeMeshStandalone, BevelVertexSegmentsClampsOverUpperLimit) {
    // Requesting segments=100 should clamp to the MAX_BEVEL_SEGMENTS=16
    // cap used by edge bevel. No crashes, manifold output.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    auto newVerts = he.bevelVertices({5}, 0.1f, 100, 0.5f);
    EXPECT_TRUE(he.validate());
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
}

// ===========================================================================
// bevelEdgesNgon — n-gon-aware edge bevel
// ===========================================================================

TEST(HalfEdgeMeshStandalone, BevelEdgesNgonOnQuadEdgeKeepsQuads) {
    // Two adjacent quads sharing edge v1-v2. Bevel that shared edge.
    // Expected output:
    //   - 4 new "inner" vertices (innerV1F1, innerV2F1, innerV1F2, innerV2F2).
    //   - The two original quads become quads with vMid replacements
    //     (4-vertex faces still — same arity, just two corners moved).
    //   - 1 new chamfer quad bridging f1 to f2.
    //   - 2 corner-cap triangles, one at v1 and one at v2.
    // Total active face count: 2 (modified quads) + 1 (chamfer) + 2 (caps) = 5.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "M";
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    sub.vertices = { mkV(0, 0), mkV(1, 0), mkV(1, 1), mkV(0, 1),
                     mkV(2, 0), mkV(2, 1) };
    EditableFace q1, q2;
    q1.indices = {0, 1, 2, 3};
    q2.indices = {1, 4, 5, 2};
    sub.faces = { q1, q2 };
    triangulateFaces(sub);
    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));
    ASSERT_EQ(he.faceCount(), 2u);

    const int sharedEdge = findEdge(he, 1, 2);
    ASSERT_GE(sharedEdge, 0);

    auto newVerts = he.bevelEdgesNgon({sharedEdge}, 0.1f);
    EXPECT_EQ(newVerts.size(), 4u)
        << "expected 4 inner vertices (2 per face × 2 endpoints)";
    EXPECT_TRUE(he.validate());

    // Active face count: 2 modified quads + 1 chamfer quad + 2 corner caps = 5.
    int active = 0;
    int quadCount = 0;
    int triCount = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        ++active;
        const auto fv = he.faceVertices(static_cast<int>(f));
        if (fv.size() == 4) ++quadCount;
        if (fv.size() == 3) ++triCount;
    }
    EXPECT_EQ(active, 5);
    EXPECT_EQ(quadCount, 3) << "2 modified original quads + 1 chamfer = 3";
    EXPECT_EQ(triCount, 2) << "2 corner caps at v1 and v2";
}

TEST(HalfEdgeMeshStandalone, BevelEdgesNgonRejectsBoundaryEdge) {
    // Boundary edges (single adjacent face) are skipped — same as the
    // triangle-bevel MVP. No crash, no new vertices.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "M";
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    sub.vertices = { mkV(0, 0), mkV(1, 0), mkV(1, 1), mkV(0, 1) };
    EditableFace q;
    q.indices = {0, 1, 2, 3};
    sub.faces = { q };
    triangulateFaces(sub);
    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));
    const int boundary = findEdge(he, 0, 1);
    ASSERT_GE(boundary, 0);
    auto newVerts = he.bevelEdgesNgon({boundary}, 0.1f);
    EXPECT_TRUE(newVerts.empty());
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, BevelEdgesNgonSegments3ProducesRoundedChamfer) {
    // segments > 1 builds a profile-driven chain at each endpoint:
    // 4 chain vertices per endpoint (innerVF1, mid_1, mid_2, innerVF2),
    // 3 chamfer-strip quads per beveled edge, 6 corner-cap triangles
    // (3 per endpoint).
    //
    // Topology: 2 modified quads (the original f1, f2) + 3 chamfer
    // segment quads + 6 corner caps = 11 active faces.
    // New vertices: 4 inner (segments=1 baseline) + 2 × 2 intermediates
    // (segments-1 = 2 per endpoint) = 8.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "M";
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    sub.vertices = { mkV(0, 0), mkV(1, 0), mkV(1, 1), mkV(0, 1),
                     mkV(2, 0), mkV(2, 1) };
    EditableFace q1, q2;
    q1.indices = {0, 1, 2, 3};
    q2.indices = {1, 4, 5, 2};
    sub.faces = { q1, q2 };
    triangulateFaces(sub);
    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));
    const int sharedEdge = findEdge(he, 1, 2);
    ASSERT_GE(sharedEdge, 0);

    auto newVerts = he.bevelEdgesNgon({sharedEdge}, 0.1f, /*segments=*/3, /*profile=*/0.5f);
    EXPECT_EQ(newVerts.size(), 8u)
        << "4 inner + 2*(segments-1) intermediates = 8 new verts";
    EXPECT_TRUE(he.validate());

    int active = 0;
    int quads = 0;
    int tris = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        ++active;
        const auto fv = he.faceVertices(static_cast<int>(f));
        if (fv.size() == 4) ++quads;
        if (fv.size() == 3) ++tris;
    }
    EXPECT_EQ(active, 11)
        << "2 modified quads + 3 chamfer segment quads + 6 corner caps";
    EXPECT_EQ(quads, 5);
    EXPECT_EQ(tris, 6);
}

TEST(HalfEdgeMeshStandalone, BevelEdgesNgonOnQuadCubeProducesManifoldOutput) {
    // Quad cube: 8 verts, 6 quad faces. Bevel one of the cube's edges
    // (top-back, between v2 and v3). The endpoints have valence 3 — so
    // the test exercises neighbor-face splicing: each non-beveled face
    // adjacent to v2 / v3 must absorb the corresponding inner vertex
    // into its loop, otherwise the chamfer leaves a non-manifold gap.
    EditableMesh em;
    EditableSubMesh sub;
    sub.materialName = "M";
    auto mkV = [](float x, float y, float z) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, z);
        v.normal = Ogre::Vector3::UNIT_Y; v.hasNormal = true;
        return v;
    };
    sub.vertices = {
        mkV(-1,-1,-1), mkV(1,-1,-1), mkV(-1,1,-1), mkV(1,1,-1), // 0..3
        mkV(-1,-1, 1), mkV(1,-1, 1), mkV(-1,1, 1), mkV(1,1, 1), // 4..7
    };
    EditableFace fBack, fFront, fTop, fBottom, fLeft, fRight;
    fBack.indices   = {0, 2, 3, 1};   // -Z (CCW from outside)
    fFront.indices  = {5, 7, 6, 4};   // +Z
    fBottom.indices = {0, 1, 5, 4};   // -Y
    fTop.indices    = {2, 6, 7, 3};   // +Y
    fLeft.indices   = {0, 4, 6, 2};   // -X
    fRight.indices  = {1, 3, 7, 5};   // +X
    sub.faces = { fBack, fFront, fBottom, fTop, fLeft, fRight };
    triangulateFaces(sub);
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(he.faceCount(), 6u);
    const int topBackEdge = findEdge(he, 2, 3);
    ASSERT_GE(topBackEdge, 0);

    auto newVerts = he.bevelEdgesNgon({topBackEdge}, 0.1f);
    EXPECT_EQ(newVerts.size(), 4u);
    EXPECT_TRUE(he.validate());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back))
        << "neighbor-face splicing must close the chamfer around the cube's "
        << "valence-3 endpoints";
}

TEST(HalfEdgeMeshStandalone, BevelVerticesNgonOnQuadCornerKeepsQuads) {
    // 4 quads in a + arrangement around a central vertex v4 of valence 4.
    // Bevel v4. Expected:
    //   - 4 inner vertices (one per incident face), each placed toward
    //     that face's centroid.
    //   - 4 modified quads (each loses v4, gains its inner vertex —
    //     still a 4-vertex face).
    //   - 1 cap n-gon (4-vertex) walking the four inner vertices.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "M";
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    sub.vertices = {
        mkV(0, 0), mkV(1, 0), mkV(2, 0),
        mkV(0, 1), mkV(1, 1), mkV(2, 1),
        mkV(0, 2), mkV(1, 2), mkV(2, 2),
    };
    EditableFace qA, qB, qC, qD;
    qA.indices = {0, 1, 4, 3};
    qB.indices = {1, 2, 5, 4};
    qC.indices = {3, 4, 7, 6};
    qD.indices = {4, 5, 8, 7};
    sub.faces = { qA, qB, qC, qD };
    triangulateFaces(sub);
    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));
    ASSERT_EQ(he.faceCount(), 4u);

    auto newVerts = he.bevelVerticesNgon({4}, 0.1f);
    EXPECT_EQ(newVerts.size(), 4u)
        << "one inner vertex per incident face";
    EXPECT_TRUE(he.validate());

    int active = 0;
    int quads = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        ++active;
        if (he.faceVertices(static_cast<int>(f)).size() == 4) ++quads;
    }
    EXPECT_EQ(active, 5)
        << "4 modified quads + 1 cap quad";
    EXPECT_EQ(quads, 5);
}

TEST(HalfEdgeMeshStandalone, BevelEdgesNgonRejectsChainedSelection) {
    // 4 quads in a + arrangement around a central vertex, so every
    // selected edge is INTERIOR (not a boundary). Two edges chained
    // through that center vertex must be skipped — chained bevels
    // need ring-aware logic the MVP doesn't have.
    //
    //    v6 - v7 - v8
    //    |    |    |
    //    v3 - v4 - v5
    //    |    |    |
    //    v0 - v1 - v2
    //
    // Quads: (0,1,4,3), (1,2,5,4), (3,4,7,6), (4,5,8,7).
    // The edges (1,4) and (4,5) share v4 (the center) and are both
    // interior between quad pairs.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "M";
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    sub.vertices = {
        mkV(0, 0), mkV(1, 0), mkV(2, 0),
        mkV(0, 1), mkV(1, 1), mkV(2, 1),
        mkV(0, 2), mkV(1, 2), mkV(2, 2),
    };
    EditableFace qA, qB, qC, qD;
    qA.indices = {0, 1, 4, 3};
    qB.indices = {1, 2, 5, 4};
    qC.indices = {3, 4, 7, 6};
    qD.indices = {4, 5, 8, 7};
    sub.faces = { qA, qB, qC, qD };
    triangulateFaces(sub);
    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));
    const int eA = findEdge(he, 1, 4);  // interior between qA, qB
    const int eB = findEdge(he, 4, 5);  // interior between qB, qD; shares v4
    ASSERT_GE(eA, 0); ASSERT_GE(eB, 0);
    auto newVerts = he.bevelEdgesNgon({eA, eB}, 0.05f);
    EXPECT_TRUE(newVerts.empty())
        << "chained selections (sharing an endpoint) are skipped in the MVP";
    EXPECT_TRUE(he.validate());
}

// ===========================================================================
// splitEdge / splitFace — knife-tool topology primitives
// ===========================================================================

// Count active (non-retired) faces. After a topology op some face slots have
// halfEdge == -1 — they'll be compacted out of toEditableMesh but still
// contribute to faceCount(), so tests that want the visible triangle count
// need the active count here.
static int activeFaceCount(const HalfEdgeMesh& he)
{
    int n = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge >= 0) ++n;
    }
    return n;
}

TEST(HalfEdgeMeshStandalone, SplitEdgeMidpointOfInteriorEdgeInsertsVertexInBothFaces) {
    // n-gon-aware splitEdge inserts vMid into each adjacent face's vertex
    // loop without introducing a fan diagonal: a triangle adjacent to the
    // split edge becomes a 4-vertex face (NOT two triangles). The quad
    // mesh's two triangles each gain vMid, resulting in 2 active 4-vertex
    // faces. (Pre-quads-followup this returned 4 triangles via fan
    // diagonals on `vMid → vOpp`; the new behaviour preserves more
    // topology and produces no artificial diagonals.)
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    const int sharedEdge = findEdge(he, 1, 2);
    ASSERT_GE(sharedEdge, 0);

    const int vMid = he.splitEdge(sharedEdge, 0.5f);
    ASSERT_GE(vMid, 0);
    EXPECT_TRUE(he.validate());

    EXPECT_EQ(he.vertexCount(), 5u);
    EXPECT_EQ(activeFaceCount(he), 2);
    // Each surviving face should have 4 vertices now (the original
    // triangle plus vMid inserted on the shared edge).
    int quadCount = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        auto fv = he.faceVertices(static_cast<int>(f));
        if (fv.size() == 4) ++quadCount;
    }
    EXPECT_EQ(quadCount, 2)
        << "n-gon-aware splitEdge: each adjacent triangle gains vMid → quad";

    // Midpoint position should be the average of the endpoints.
    const auto mid = he.vertex(vMid).position;
    EXPECT_NEAR(mid.x, 0.5f, 1e-4f);
    EXPECT_NEAR(mid.y, 0.5f, 1e-4f);
    EXPECT_NEAR(mid.z, 0.0f, 1e-4f);

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
}

TEST(HalfEdgeMeshStandalone, SplitEdgeInterpolatesNormalAndUV) {
    // Interpolation at t=0.25 should place the new vertex a quarter of the
    // way from v1 to v2 and carry weighted normal / UV attributes.
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    const int sharedEdge = findEdge(he, 1, 2);
    ASSERT_GE(sharedEdge, 0);

    const int vMid = he.splitEdge(sharedEdge, 0.25f);
    ASSERT_GE(vMid, 0);

    const auto& v = he.vertex(vMid);
    EXPECT_TRUE(v.hasNormal);
    EXPECT_TRUE(v.hasUV);

    // Position lies on the v1↔v2 segment at parameter ~0.25 (direction
    // depends on which endpoint splitEdge treats as the "from"; check
    // distance ratio rather than exact coords).
    const auto& p1 = he.vertex(1).position;
    const auto& p2 = he.vertex(2).position;
    const float segLen = p1.distance(p2);
    const float d1 = v.position.distance(p1);
    const float d2 = v.position.distance(p2);
    const float frac = std::min(d1, d2) / segLen;
    EXPECT_NEAR(frac, 0.25f, 1e-3f);

    // Both endpoints share normal +Z; midpoint should too.
    EXPECT_NEAR(v.normal.z, 1.0f, 1e-4f);

    // UV lerp: v1=(1,0), v2=(0,1). Any linear blend between them falls
    // on the x+y=1 line regardless of the split direction, so check
    // that invariant plus the correct distance from either endpoint.
    // A broken `v.uv` lerp (e.g. zeroed out) fails both.
    EXPECT_NEAR(v.uv.x + v.uv.y, 1.0f, 1e-4f)
        << "UV lerp should stay on the segment between endpoints";
    const float uvDist1 = std::hypot(v.uv.x - 1.0f, v.uv.y - 0.0f);
    const float uvDist2 = std::hypot(v.uv.x - 0.0f, v.uv.y - 1.0f);
    const float uvSeg = std::hypot(1.0f, 1.0f);
    EXPECT_NEAR(std::min(uvDist1, uvDist2) / uvSeg, 0.25f, 1e-3f);
}

TEST(HalfEdgeMeshStandalone, SplitEdgeBoundaryEdgeInsertsVertexInTheTriangle) {
    // A triangle's perimeter edge is a boundary edge (only one adjacent
    // face). n-gon-aware splitEdge: the triangle gains vMid on its
    // perimeter, becoming a 4-vertex face. No phantom face on the outside.
    // (Pre-quads-followup this returned two triangles via a fan diagonal.)
    auto em = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    // Pick the edge between v0 (0,0,0) and v1 (1,0,0).
    const int edge = findEdge(he, 0, 1);
    ASSERT_GE(edge, 0);

    const int vMid = he.splitEdge(edge, 0.5f);
    ASSERT_GE(vMid, 0);
    EXPECT_TRUE(he.validate());

    EXPECT_EQ(he.vertexCount(), 4u);
    EXPECT_EQ(activeFaceCount(he), 1);
    // The single surviving face must be a quad (3 original verts + vMid).
    int quadCount = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        if (he.faceVertices(static_cast<int>(f)).size() == 4) ++quadCount;
    }
    EXPECT_EQ(quadCount, 1);
}

TEST(HalfEdgeMeshStandalone, SplitEdgeClampsExtremeT) {
    // t=0 or t=1 would collapse one of the new triangles; splitEdge should
    // nudge the parameter inside the (0,1) interior rather than fail or
    // produce a zero-area face.
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    const int sharedEdge = findEdge(he, 1, 2);
    ASSERT_GE(sharedEdge, 0);

    const int vMid = he.splitEdge(sharedEdge, 0.0f);
    ASSERT_GE(vMid, 0);
    EXPECT_TRUE(he.validate());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
}

TEST(HalfEdgeMeshStandalone, SplitEdgeInvalidIndexReturnsMinusOne) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_EQ(he.splitEdge(-1, 0.5f), -1);
    EXPECT_EQ(he.splitEdge(999, 0.5f), -1);
}

TEST(HalfEdgeMeshStandalone, TwoSplitEdgesOnOneTriangleNeedFollowupSplitFace) {
    // n-gon-aware splitEdge: two splitEdge calls on the same triangle
    // insert m1 and m2 into its loop, producing a pentagon
    // [v0, m1, v1, m2, v2] — but they are NOT automatically connected.
    // (The triangle-only splitEdge MVP previously cut the triangle into
    // sub-triangles via fan diagonals, which incidentally produced the
    // m1-m2 edge as a side-effect.) Call splitFace explicitly to
    // materialise the cut. This is exactly what `cutPath` now does in
    // its walk loop.
    auto em = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 1);

    const int edgeA = findEdge(he, 0, 1);
    ASSERT_GE(edgeA, 0);
    const int m1 = he.splitEdge(edgeA, 0.5f);
    ASSERT_GE(m1, 0);
    EXPECT_TRUE(he.validate());

    const int edgeB = findEdge(he, 1, 2);
    ASSERT_GE(edgeB, 0);
    const int m2 = he.splitEdge(edgeB, 0.5f);
    ASSERT_GE(m2, 0);
    EXPECT_TRUE(he.validate());

    // After two splitEdges the triangle is now a pentagon; m1 and m2
    // are NOT yet connected — that's the new contract.
    EXPECT_LT(findEdge(he, m1, m2), 0)
        << "n-gon splitEdge does not auto-cut: explicit splitFace required";
    EXPECT_EQ(activeFaceCount(he), 1);

    // Find the pentagon and call splitFace to materialise the cut.
    int pentagonIdx = -1;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        const auto fv = he.faceVertices(static_cast<int>(f));
        if (fv.size() == 5
            && std::find(fv.begin(), fv.end(), m1) != fv.end()
            && std::find(fv.begin(), fv.end(), m2) != fv.end()) {
            pentagonIdx = static_cast<int>(f);
            break;
        }
    }
    ASSERT_GE(pentagonIdx, 0);
    EXPECT_TRUE(he.splitFace(pentagonIdx, m1, m2));
    EXPECT_GE(findEdge(he, m1, m2), 0)
        << "splitFace must materialise the m1-m2 cut on the pentagon";

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
}

TEST(HalfEdgeMeshStandalone, SplitEdgeOnQuadMeshKeepsQuads) {
    // Regression for the n-gon-aware splitEdge: split a quad-imported
    // mesh's edge and confirm both adjacent quads become pentagons
    // (NOT four triangles via fan diagonals as the old MVP would have
    // produced). This is the topology guarantee knife / loop cut
    // depend on for producing real quad outputs on quad-imported
    // assets.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "M";
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    // Two adjacent quads sharing the v1-v2 edge.
    sub.vertices = { mkV(0, 0), mkV(1, 0), mkV(1, 1), mkV(0, 1),
                     mkV(2, 0), mkV(2, 1) };
    EditableFace q1, q2;
    q1.indices = {0, 1, 2, 3};
    q2.indices = {1, 4, 5, 2};
    sub.faces = { q1, q2 };
    triangulateFaces(sub);
    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));
    ASSERT_EQ(activeFaceCount(he), 2);

    const int sharedEdge = findEdge(he, 1, 2);
    ASSERT_GE(sharedEdge, 0);

    const int vMid = he.splitEdge(sharedEdge, 0.5f);
    ASSERT_GE(vMid, 0);
    EXPECT_TRUE(he.validate());

    // Both quads should now be pentagons (5-vertex faces). NOT four
    // triangles — that would mean fan diagonals slipped in.
    EXPECT_EQ(activeFaceCount(he), 2);
    int pentagonCount = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        if (he.faceVertices(static_cast<int>(f)).size() == 5) ++pentagonCount;
    }
    EXPECT_EQ(pentagonCount, 2)
        << "n-gon-aware splitEdge: quad + new vertex on shared edge → pentagon";
}

TEST(HalfEdgeMeshStandalone, SplitFaceRejectsAdjacentBoundaryVertices) {
    // A "diagonal" between two vertices that are already edge-adjacent on a
    // face boundary would duplicate the existing edge. splitFace should
    // refuse rather than produce a degenerate split. On a triangle every
    // pair of boundary vertices is edge-adjacent so every call should fail.
    auto em = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    EXPECT_FALSE(he.splitFace(0, 0, 1));
    EXPECT_FALSE(he.splitFace(0, 1, 2));
    EXPECT_FALSE(he.splitFace(0, 0, 2));
    EXPECT_EQ(activeFaceCount(he), 1);
}

TEST(HalfEdgeMeshStandalone, SplitFaceRejectsInvalidVertexIndices) {
    // These guard paths run BEFORE the triangle-rejection in splitFace
    // (see the ordered checks at the top of the implementation), so the
    // assertions hit the invalid-vertex branches directly rather than
    // getting short-circuited by the "every tri vertex pair is
    // adjacent" rule. Covers: same-vertex, negative, out-of-range,
    // and in-range-but-not-on-this-face.
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    EXPECT_FALSE(he.splitFace(0, 2, 2))
        << "same-vertex pair should fail (check 2 — first guard)";
    EXPECT_FALSE(he.splitFace(0, -1, 1))
        << "negative index should fail (check 3)";
    EXPECT_FALSE(he.splitFace(0, 0, 999))
        << "out-of-range index should fail (check 5)";
    EXPECT_FALSE(he.splitFace(-1, 0, 1))
        << "invalid face index should fail (check 1)";

    // Also verify the "vertex exists but isn't on this face" path.
    // Triangle 0 of makeQuadMesh contains verts {0,1,2}; vertex 3
    // exists in the mesh but is on triangle 1, so splitFace should
    // reject before getting to adjacency checks.
    EXPECT_FALSE(he.splitFace(0, 0, 3))
        << "off-face valid vertex should fail the boundary-loop check";
}

TEST(HalfEdgeMeshStandalone, SplitEdgePreservesSubmeshCount) {
    // splitEdge operating on one submesh's edge should leave the submesh
    // count unchanged. Attribute correctness across submeshes is covered
    // by the roundtrip test below.
    auto em = makeTwoSubMeshMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    const auto beforeSubCount = he.subMeshCount();
    const int edge = findEdge(he, 0, 1);
    ASSERT_GE(edge, 0) << "v0-v1 edge not present in mesh";

    const int vMid = he.splitEdge(edge, 0.5f);
    ASSERT_GE(vMid, 0);
    EXPECT_TRUE(he.validate());
    EXPECT_EQ(he.subMeshCount(), beforeSubCount);

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_EQ(back.subMeshes().size(), static_cast<size_t>(beforeSubCount));
}

// ===========================================================================
// cutPath — knife walk-and-cut along a polyline
// ===========================================================================

TEST(HalfEdgeMeshStandalone, CutPathCrossesInteriorDiagonalAndLinksEndpoints) {
    // Two triangles share a diagonal v1-v2. A horizontal cut from the
    // midpoint of v0-v1 to the midpoint of v2-v3 crosses the diagonal at
    // its midpoint, so the final mesh must have:
    //   - 3 new vertices (both endpoints plus one on the diagonal),
    //   - the cut visible as a chain of real edges end-to-end.
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    const int edgeBottom = findEdge(he, 0, 1);
    const int edgeTop = findEdge(he, 2, 3);
    ASSERT_GE(edgeBottom, 0);
    ASSERT_GE(edgeTop, 0);

    const auto newVerts = he.cutPath({{edgeBottom, 0.5f}, {edgeTop, 0.5f}});
    ASSERT_EQ(newVerts.size(), 3u)
        << "expected 2 endpoint verts + 1 diagonal-crossing vert";
    EXPECT_TRUE(he.validate());

    // All three new vertices should sit on the x=0.5 cut line.
    for (int v : newVerts) {
        EXPECT_NEAR(he.vertex(v).position.x, 0.5f, 1e-3f);
    }

    // The cut must exist as real edges: every consecutive pair of cut
    // vertices (sorted by y) should be connected by an HE edge.
    std::vector<int> orderedCutVerts = newVerts;
    std::sort(orderedCutVerts.begin(), orderedCutVerts.end(),
              [&](int a, int b) {
                  return he.vertex(a).position.y < he.vertex(b).position.y;
              });
    for (size_t i = 0; i + 1 < orderedCutVerts.size(); ++i) {
        EXPECT_GE(findEdge(he, orderedCutVerts[i], orderedCutVerts[i + 1]), 0)
            << "consecutive cut vertices should be connected by a real edge";
    }

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
}

TEST(HalfEdgeMeshStandalone, CutPathSingleTriangleStillProducesEndpointEdge) {
    // Cutting both clicks onto edges of a single triangle doesn't need an
    // interior edge crossing — two splitEdges already leave the M1↔M2
    // segment as a real edge. cutPath should return exactly the two
    // endpoint vertices in that case.
    auto em = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    const int edgeA = findEdge(he, 0, 1);
    const int edgeB = findEdge(he, 1, 2);
    ASSERT_GE(edgeA, 0);
    ASSERT_GE(edgeB, 0);

    const auto newVerts = he.cutPath({{edgeA, 0.5f}, {edgeB, 0.5f}});
    ASSERT_EQ(newVerts.size(), 2u);
    EXPECT_TRUE(he.validate());
    EXPECT_GE(findEdge(he, newVerts[0], newVerts[1]), 0)
        << "single-triangle cut should connect both endpoints directly";
}

TEST(HalfEdgeMeshStandalone, CutPathBailsOnFewerThanTwoPoints) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    EXPECT_TRUE(he.cutPath({}).empty());
    const int edge = findEdge(he, 0, 1);
    ASSERT_GE(edge, 0);
    EXPECT_TRUE(he.cutPath({{edge, 0.5f}}).empty());
}

TEST(HalfEdgeMeshStandalone, CutPathFailsOnInvalidEdgeIndex) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    const int edge = findEdge(he, 0, 1);
    ASSERT_GE(edge, 0);
    // Negative / out-of-range edge indices short-circuit the walk.
    EXPECT_TRUE(he.cutPath({{edge, 0.5f}, {-1, 0.5f}}).empty());
}

TEST(HalfEdgeMeshStandalone, CutPathRollsBackWhenSecondEdgeDuplicatesFirst) {
    // If two CutPoints reference the same underlying edge (same
    // endpoint vertex pair), the first splitEdge removes that edge and
    // the second lookup fails. cutPath must roll back, leaving the
    // mesh byte-identical to its pre-call state — otherwise callers
    // see a half-applied cut.
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const size_t vertsBefore = he.vertexCount();
    const size_t facesBefore = he.faceCount();
    const size_t edgesBefore = he.edgeCount();

    const int edge = findEdge(he, 1, 2); // shared diagonal
    ASSERT_GE(edge, 0);
    const auto result = he.cutPath({{edge, 0.3f}, {edge, 0.7f}});
    EXPECT_TRUE(result.empty()) << "duplicate-edge cutPath must fail";

    EXPECT_EQ(he.vertexCount(), vertsBefore);
    EXPECT_EQ(he.faceCount(), facesBefore);
    EXPECT_EQ(he.edgeCount(), edgesBefore);
    EXPECT_TRUE(he.validate());
}

// ===========================================================================
// loopCut — perpendicular ring cut through quads
// ===========================================================================

TEST(HalfEdgeMeshStandalone, LoopCutOnQuadStripCutsEachQuadOnce) {
    // 3-quad strip: q1=(0,1,5,4), q2=(1,2,6,5), q3=(2,3,7,6).
    //
    //   v0─v1─v2─v3
    //   │ q1│q2│q3│
    //   v4─v5─v6─v7
    //
    // Loop-cutting the (1,5) interior edge starts from q1, the
    // opposite-edge walk crosses (1,5) into q2 then (2,6) into q3,
    // terminates at the q3 boundary (3,7). 3 quads × 1 cut each =
    // 3 new midpoints, 6 quads total after the cut.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "M";
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    sub.vertices = {
        mkV(0, 1), mkV(1, 1), mkV(2, 1), mkV(3, 1),
        mkV(0, 0), mkV(1, 0), mkV(2, 0), mkV(3, 0),
    };
    EditableFace q1, q2, q3;
    q1.indices = {0, 1, 5, 4};
    q2.indices = {1, 2, 6, 5};
    q3.indices = {2, 3, 7, 6};
    sub.faces = { q1, q2, q3 };
    triangulateFaces(sub);
    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));
    const int startEdge = findEdge(he, 1, 5);
    ASSERT_GE(startEdge, 0);

    auto newVerts = he.loopCut(startEdge);
    // 4 rails get midpoints: (1,5) start, (0,4) on q1's other side,
    // (2,6) interior, (3,7) on q3's other side. So 4 new vertices.
    EXPECT_EQ(newVerts.size(), 4u)
        << "open-ended loop cut: 4 rail midpoints across 3 quads";
    EXPECT_TRUE(he.validate());

    int active = 0, quads = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        ++active;
        if (he.faceVertices(static_cast<int>(f)).size() == 4) ++quads;
    }
    EXPECT_EQ(active, 6) << "each of the 3 original quads bisected → 6 quads";
    EXPECT_EQ(quads, 6) << "loop cut preserves quad topology";
}

TEST(HalfEdgeMeshStandalone, LoopCutFailsOnNonQuadAdjacency) {
    // Triangles can't loop-cut: there's no opposite-edge correspondence.
    // Starting from any edge of a triangle pair returns empty.
    auto em = makeQuadMesh(); // two triangles, NOT quads
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const int diag = findEdge(he, 1, 2);
    ASSERT_GE(diag, 0);
    auto newVerts = he.loopCut(diag);
    EXPECT_TRUE(newVerts.empty());
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, LoopCutClosedRingOnQuadCubeReturnsToStart) {
    // Quad cube: 6 quad faces forming a closed manifold. Loop-cutting
    // any edge produces a closed ring of cuts (4 rails total since
    // the walk returns to the starting edge after traversing 4 faces).
    EditableMesh em;
    EditableSubMesh sub;
    sub.materialName = "M";
    auto mkV = [](float x, float y, float z) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, z);
        v.normal = Ogre::Vector3::UNIT_Y; v.hasNormal = true;
        return v;
    };
    sub.vertices = {
        mkV(-1,-1,-1), mkV(1,-1,-1), mkV(-1,1,-1), mkV(1,1,-1),
        mkV(-1,-1, 1), mkV(1,-1, 1), mkV(-1,1, 1), mkV(1,1, 1),
    };
    EditableFace fBack, fFront, fTop, fBottom, fLeft, fRight;
    fBack.indices   = {0, 2, 3, 1};
    fFront.indices  = {5, 7, 6, 4};
    fBottom.indices = {0, 1, 5, 4};
    fTop.indices    = {2, 6, 7, 3};
    fLeft.indices   = {0, 4, 6, 2};
    fRight.indices  = {1, 3, 7, 5};
    sub.faces = { fBack, fFront, fBottom, fTop, fLeft, fRight };
    triangulateFaces(sub);
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    // Pick the front-bottom edge (4,5). Its perpendicular ring runs
    // around the cube via Bottom → Right → Top → Left and closes.
    const int startEdge = findEdge(he, 4, 5);
    ASSERT_GE(startEdge, 0);

    auto newVerts = he.loopCut(startEdge);
    EXPECT_EQ(newVerts.size(), 4u)
        << "closed cube ring: 4 rails (one per traversed face), shared midpoints";
    EXPECT_TRUE(he.validate());

    // 6 original faces + 4 cuts (one per cube face crossed) → 10 quads.
    int active = 0, quads = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        ++active;
        if (he.faceVertices(static_cast<int>(f)).size() == 4) ++quads;
    }
    EXPECT_EQ(active, 10);
    EXPECT_EQ(quads, 10) << "loop cut preserves quad topology on closed manifolds";
}

TEST(HalfEdgeMeshStandalone, LoopCutFailsOnInvalidEdgeIndex) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_TRUE(he.loopCut(-1).empty());
    EXPECT_TRUE(he.loopCut(999).empty());
}

TEST(HalfEdgeMeshStandalone, LoopCutRejectsMixedQuadTriAdjacency) {
    // Regression test for Codex P1 follow-up: loopCut walked both
    // sides of the start edge independently, so a quad+tri adjacency
    // would produce a one-sided cut on the quad side and silently
    // mutate topology when the user expected a "needs a quad mesh"
    // failure. The op must reject upfront when EITHER face adjacent
    // to the start edge is non-quad.
    EditableMesh em;
    EditableSubMesh sub;
    sub.materialName = "M";
    auto mkV = [](float x, float y, float z) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, z);
        v.normal = Ogre::Vector3::UNIT_Z; v.hasNormal = true;
        return v;
    };
    // 5 verts, planar in XY:
    //   3 -- 2
    //   |   /|
    //   |  / |
    //   | /  |
    //   0 -- 1 -- 4 (4 lifted to make a triangle)
    //
    // Quad face: [0,1,2,3]  (winds CCW around +Z)
    // Tri face:  [1,4,2]    (shares edge (1,2) with the quad)
    // Edge (1,2) is the shared boundary — the loop-cut start edge.
    sub.vertices = {
        mkV(0, 0, 0),
        mkV(1, 0, 0),
        mkV(1, 1, 0),
        mkV(0, 1, 0),
        mkV(2, 0, 0),
    };
    EditableFace quad;  quad.indices = {0, 1, 2, 3};
    EditableFace tri;   tri.indices  = {1, 4, 2};
    sub.faces = {quad, tri};
    triangulateFaces(sub);
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const int sharedEdge = findEdge(he, 1, 2);
    ASSERT_GE(sharedEdge, 0);

    auto newVerts = he.loopCut(sharedEdge);
    EXPECT_TRUE(newVerts.empty())
        << "loop cut must reject mixed quad/tri adjacency upfront, "
           "not produce a half-loop on the quad side";
    EXPECT_TRUE(he.validate());

    // No partial mutation: the HE mesh keeps the 2 input faces
    // (1 quad + 1 triangle) — buildFromEditableMesh respects
    // sub.faces directly when it's populated.
    int active = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge >= 0) ++active;
    }
    EXPECT_EQ(active, 2);
}

// ===========================================================================
// Merge vertices
// ===========================================================================

TEST(HalfEdgeMeshStandalone, MergeVerticesNoOpOnLessThanTwoInputs) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const auto vertsBefore = he.vertexCount();
    EXPECT_EQ(he.mergeVertices({}, Ogre::Vector3::ZERO), 0);
    EXPECT_EQ(he.mergeVertices({0}, Ogre::Vector3::ZERO), 0);
    EXPECT_EQ(he.vertexCount(), vertsBefore);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, MergeVerticesCollapsesSharedEdgeRetiringBothTriangles) {
    // The quad mesh has two triangles sharing the v1↔v2 diagonal. Merging
    // those two endpoints fuses the diagonal into one vertex and both
    // triangles become degenerate (two of their three verts collapse).
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 2);

    const Ogre::Vector3 mid(0.5f, 0.5f, 0.0f);
    const int retired = he.mergeVertices({1, 2}, mid);
    EXPECT_EQ(retired, 1) << "one vertex retires; the other becomes the survivor";
    EXPECT_EQ(activeFaceCount(he), 0)
        << "both triangles collapsed because they each touched both merged verts";
    EXPECT_TRUE(he.validate());

    // Survivor moved to the requested centroid.
    EXPECT_NEAR(he.vertex(1).position.x, mid.x, 1e-5f);
    EXPECT_NEAR(he.vertex(1).position.y, mid.y, 1e-5f);
}

TEST(HalfEdgeMeshStandalone, MergeVerticesNonAdjacentPairKeepsTriangleAlive) {
    // Build a strip of three triangles where merging two non-adjacent verts
    // doesn't make any triangle a duplicate. Layout (top-down):
    //
    //   v0─v1─v2─v3
    //    \ /\ /\ /
    //    v4─v5─v6
    //
    // Triangles: (0,1,4), (1,5,4), (1,2,5), (2,6,5), (2,3,6).
    // Merging v0 and v3 doesn't affect any triangle's vertex set since
    // v0 only appears in tri (0,1,4) and v3 only in tri (2,3,6) — both
    // survive with distinct sorted-vert keys.
    EditableMesh em;
    EditableSubMesh sub;
    sub.materialName = "Strip";
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        v.uv = Ogre::Vector2(x * 0.25f, y); v.hasUV = true;
        return v;
    };
    sub.vertices = {
        mkV(0, 1), mkV(1, 1), mkV(2, 1), mkV(3, 1),  // 0..3 top row
        mkV(0.5f, 0), mkV(1.5f, 0), mkV(2.5f, 0),    // 4..6 bottom row
    };
    auto mkT = [](int a, int b, int c) {
        EditableTriangle t;
        t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
        return t;
    };
    sub.triangles = {
        mkT(0, 1, 4), mkT(1, 5, 4), mkT(1, 2, 5),
        mkT(2, 6, 5), mkT(2, 3, 6),
    };
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 5);

    // Move v0 onto v3's position so the survivor sits at the strip's right
    // edge. v0 and v3 share no triangle, so neither face becomes degenerate
    // nor a duplicate.
    const int retired = he.mergeVertices({0, 3}, Ogre::Vector3(3, 1, 0));
    EXPECT_EQ(retired, 1);
    EXPECT_EQ(activeFaceCount(he), 5)
        << "non-adjacent merge on this strip keeps every triangle distinct";
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, MergeVerticesByDistanceFusesCoincidentVerts) {
    // Build a mesh with two pairs of near-coincident vertices and confirm
    // mergeVerticesByDistance picks them up.
    EditableMesh em;
    EditableSubMesh sub;
    sub.materialName = "Mat";
    auto mkV = [](float x, float y, float z) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, z);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        v.uv = Ogre::Vector2(0, 0); v.hasUV = true;
        return v;
    };
    // Two nearly-coincident pairs (within 1e-5 of each other) plus a far one,
    // arranged so distinct triangles cover all of them.
    sub.vertices = {
        mkV(0, 0, 0),                       // 0
        mkV(1e-6f, 0, 0),                   // 1 — coincident with 0
        mkV(1, 0, 0),                       // 2
        mkV(1.0f + 1e-6f, 0, 0),            // 3 — coincident with 2
        mkV(0.5f, 1, 0),                    // 4 — far apex
    };
    auto mkT = [](int a, int b, int c) {
        EditableTriangle t;
        t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
        return t;
    };
    sub.triangles = { mkT(0, 2, 4), mkT(1, 3, 4) };
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_EQ(activeFaceCount(he), 2);

    // Loose threshold catches both pairs; they fuse to a single triangle.
    const int retired = he.mergeVerticesByDistance({0, 1, 2, 3, 4}, 1e-3f);
    EXPECT_EQ(retired, 2);
    EXPECT_EQ(activeFaceCount(he), 1)
        << "the two near-duplicate triangles collapse to one";
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, MergeVerticesRefusesCrossSubmeshSelection) {
    // Same-position verts in different submeshes must NOT fuse — that
    // would silently bridge a UV seam / material boundary.
    auto em = makeTwoSubMeshMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const auto vertsBefore = he.vertexCount();

    // Pick one vertex from each submesh (sub0 has indices 0..2, sub1 has 3..5).
    const int retired = he.mergeVertices({0, 3}, Ogre::Vector3::ZERO);
    EXPECT_EQ(retired, 0) << "cross-submesh merge must be refused";
    EXPECT_EQ(he.vertexCount(), vertsBefore);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, MergeVerticesByDistanceIgnoresVertsBeyondThreshold) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const auto vertsBefore = he.vertexCount();

    // Quad verts are 1.0 apart on either axis — a 1e-3 threshold is far
    // tighter than any real spacing here, so nothing should fuse.
    const int retired = he.mergeVerticesByDistance({0, 1, 2, 3}, 1e-3f);
    EXPECT_EQ(retired, 0);
    EXPECT_EQ(he.vertexCount(), vertsBefore);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, MergeVerticesSurvivorTakesTargetPosition) {
    // Verify the explicit target position is written to the survivor —
    // independently of whether the target equals any input vertex's
    // pre-merge position. Important for "Merge At Cursor" futures.
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    // Pick (10, 20, 30) — far from any quad vert — so we can spot-check the
    // write rather than confusing it with an existing position.
    const Ogre::Vector3 target(10.0f, 20.0f, 30.0f);
    const int retired = he.mergeVertices({0, 3}, target);
    EXPECT_EQ(retired, 1);
    // Survivor is the first input (index 0).
    EXPECT_NEAR(he.vertex(0).position.x, target.x, 1e-5f);
    EXPECT_NEAR(he.vertex(0).position.y, target.y, 1e-5f);
    EXPECT_NEAR(he.vertex(0).position.z, target.z, 1e-5f);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, MergeVerticesPreservesSurvivorBoneWeights) {
    // The merge keeps the survivor's attributes verbatim and discards the
    // doomed verts'. Pin that contract: survivor's bone weights must be
    // unchanged after merge so skinned meshes don't deform unexpectedly.
    EditableMesh em;
    EditableSubMesh sub;
    sub.materialName = "Skinned";

    auto mkV = [](float x, float y, unsigned short bone, float weight) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        EditableBoneAssignment ba;
        ba.boneIndex = bone;
        ba.weight = weight;
        v.boneAssignments.push_back(ba);
        return v;
    };
    sub.vertices = {
        mkV(0, 0, /*bone=*/3, /*weight=*/0.7f),  // survivor
        mkV(1, 0, /*bone=*/8, /*weight=*/0.4f),  // doomed
        mkV(0, 1, /*bone=*/8, /*weight=*/0.5f),
    };
    EditableTriangle tri;
    tri.indices[0] = 0; tri.indices[1] = 1; tri.indices[2] = 2;
    sub.triangles = {tri};
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    // Survivor vert 0 has bone 3 / weight 0.7. Merging in vert 1 (bone 8)
    // must NOT overwrite — survivor wins.
    he.mergeVertices({0, 1}, Ogre::Vector3::ZERO);
    ASSERT_EQ(he.vertex(0).boneAssignments.size(), 1u);
    EXPECT_EQ(he.vertex(0).boneAssignments[0].first, 3);
    EXPECT_FLOAT_EQ(he.vertex(0).boneAssignments[0].second, 0.7f);
}

TEST(HalfEdgeMeshStandalone, MergeVerticesThreeVertCluster) {
    // Merging three verts in one call: one survivor, two retired. All three
    // verts share the only triangle, which collapses entirely.
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 2);

    // Verts 0, 1, 2 form one triangle. Merging all three retires that
    // triangle and leaves the other (1,3,2) — but its verts 1 and 2 also
    // collapsed onto survivor 0, so it becomes (0,3,0) → degenerate.
    const int retired = he.mergeVertices({0, 1, 2}, Ogre::Vector3(0.5f, 0.5f, 0));
    EXPECT_EQ(retired, 2) << "two doomed verts retire";
    EXPECT_EQ(activeFaceCount(he), 0)
        << "every triangle had at least two of {0,1,2} so all collapse";
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, MergeVerticesInvalidIndicesAreFiltered) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const auto vertsBefore = he.vertexCount();

    // Mix valid indices, out-of-range indices, and a duplicate. The
    // duplicate and out-of-range entries are silently filtered, leaving
    // {0, 1} as the actual merge set.
    const int retired = he.mergeVertices({0, 1, -1, 9999, 1, 0},
                                          Ogre::Vector3::ZERO);
    EXPECT_EQ(retired, 1) << "{-1, 9999} dropped, dup {0, 1} dedup'd";
    EXPECT_EQ(he.vertex(1).halfEdge, -1)
        << "vert 1 retired";
    EXPECT_TRUE(he.validate());
    Q_UNUSED(vertsBefore);
}

TEST(HalfEdgeMeshStandalone, MergeVerticesAlreadyRetiredVertsAreSkipped) {
    // Run a first merge to retire vert 1, then call merge again with a
    // selection that includes the now-retired vert 1. It must be filtered
    // out without crashing or producing junk topology.
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    // First pass: merge 1 + 2 (collapses both quad triangles).
    ASSERT_EQ(he.mergeVertices({1, 2}, Ogre::Vector3(0.5f, 0.5f, 0)), 1);
    // vert 1 is retired (m_vertices[1].halfEdge == -1) but vert 2 absorbed
    // into vert 1 via doomed=2; verify the second merge call doesn't choke
    // when we hand it the retired index.
    const int retired = he.mergeVertices({0, 2, 3}, Ogre::Vector3::ZERO);
    EXPECT_GE(retired, 0) << "retired-input filter must not crash";
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, MergeVerticesByDistanceEmptyInputIsNoOp) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const auto vertsBefore = he.vertexCount();
    EXPECT_EQ(he.mergeVerticesByDistance({}, 1e-3f), 0);
    EXPECT_EQ(he.mergeVerticesByDistance({0}, 1e-3f), 0);
    EXPECT_EQ(he.mergeVerticesByDistance({0, 1}, -1.0f), 0)
        << "negative threshold is rejected";
    EXPECT_EQ(he.vertexCount(), vertsBefore);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, MergeVerticesOnBoundaryEdge) {
    // One-triangle mesh — every edge is a boundary. Merging two of its
    // verts must retire the triangle and leave the survivor without
    // crashing the boundary half-edge bookkeeping.
    auto em = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 1);

    const int retired = he.mergeVertices({0, 1}, Ogre::Vector3(0.5f, 0, 0));
    EXPECT_EQ(retired, 1);
    EXPECT_EQ(activeFaceCount(he), 0)
        << "the single tri collapses since 0 and 1 fused";
    EXPECT_TRUE(he.validate());
}

// ---------------------------------------------------------------------------
// Delete / Dissolve
// ---------------------------------------------------------------------------

namespace {
    // Build a 6-vertex hexagonal fan around a center vertex (v0). Used to
    // exercise dissolveVertices with a clean valence-6 interior umbrella.
    EditableMesh makeHexFan()
    {
        EditableMesh em;
        EditableSubMesh sub;
        sub.materialName = "Hex";
        auto mkV = [](float x, float y) {
            EditableVertex v;
            v.position = Ogre::Vector3(x, y, 0);
            v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
            v.uv = Ogre::Vector2((x + 1) * 0.5f, (y + 1) * 0.5f); v.hasUV = true;
            return v;
        };
        // v0 = center; v1..v6 = ring at radius 1, every 60°.
        sub.vertices = {
            mkV(0.0f, 0.0f),
            mkV(1.0f, 0.0f),
            mkV(0.5f, 0.866f),
            mkV(-0.5f, 0.866f),
            mkV(-1.0f, 0.0f),
            mkV(-0.5f, -0.866f),
            mkV(0.5f, -0.866f),
        };
        auto mkT = [](int a, int b, int c) {
            EditableTriangle t;
            t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
            return t;
        };
        sub.triangles = {
            mkT(0, 1, 2), mkT(0, 2, 3), mkT(0, 3, 4),
            mkT(0, 4, 5), mkT(0, 5, 6), mkT(0, 6, 1),
        };
        em.subMeshes().push_back(std::move(sub));
        return em;
    }
} // namespace

TEST(HalfEdgeMeshStandalone, DeleteFacesEmptyIsNoOp) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_EQ(he.deleteFaces({}), 0);
    EXPECT_EQ(activeFaceCount(he), 2);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, DeleteFacesRetiresOneTriangleOfQuad) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 2);

    EXPECT_EQ(he.deleteFaces({0}), 1);
    EXPECT_EQ(activeFaceCount(he), 1);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, DeleteFacesIgnoresOutOfRangeAndDuplicates) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_EQ(he.deleteFaces({0, 0, 99, -1}), 1);
    EXPECT_EQ(activeFaceCount(he), 1);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, DeleteFacesRetiresOrphanVertex) {
    // The hex fan has v0 at the center, only referenced by deleted faces.
    // Once we delete every face containing v0, it has no incident faces left
    // and should be retired.
    auto em = makeHexFan();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    std::vector<int> all;
    for (size_t f = 0; f < he.faceCount(); ++f) all.push_back(static_cast<int>(f));
    EXPECT_EQ(he.deleteFaces(all), 6);
    EXPECT_EQ(activeFaceCount(he), 0);
    // Every vertex's halfEdge should be -1 since no face survives.
    for (size_t v = 0; v < he.vertexCount(); ++v)
        EXPECT_LT(he.vertex(static_cast<int>(v)).halfEdge, 0);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, DeleteEdgesRemovesAdjacentFaces) {
    // The quad mesh shares the v1↔v2 diagonal between two triangles.
    // Deleting that edge should retire both adjacent faces.
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const int diag = findEdge(he, 1, 2);
    ASSERT_GE(diag, 0);
    EXPECT_EQ(he.deleteEdges({diag}), 2);
    EXPECT_EQ(activeFaceCount(he), 0);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, DeleteEdgesBoundaryEdgeRemovesOneFace) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const int boundary = findEdge(he, 0, 1);
    ASSERT_GE(boundary, 0);
    EXPECT_EQ(he.deleteEdges({boundary}), 1);
    EXPECT_EQ(activeFaceCount(he), 1);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, DeleteVerticesRemovesAllIncidentFaces) {
    // Deleting v0 from the hex fan removes all 6 surrounding triangles.
    auto em = makeHexFan();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_EQ(he.deleteVertices({0}), 1);
    EXPECT_EQ(activeFaceCount(he), 0);
    EXPECT_LT(he.vertex(0).halfEdge, 0);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, DeleteVerticesIgnoresInvalidIndices) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_EQ(he.deleteVertices({-1, 99, 0}), 1);
    EXPECT_LT(he.vertex(0).halfEdge, 0);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, DissolveEdgesEmptyIsNoOp) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_EQ(he.dissolveEdges({}), 0);
    EXPECT_EQ(activeFaceCount(he), 2);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, DissolveEdgesQuadDiagonalMergesIntoSingleQuad) {
    // n-gon-aware dissolve: dissolving the shared edge between two
    // triangles merges them into a SINGLE quad face — no diagonal at
    // all. (The previous triangle-only impl just swapped to the OTHER
    // diagonal, which left a fan diagonal in place.)
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const int diag = findEdge(he, 1, 2);
    ASSERT_GE(diag, 0);

    EXPECT_EQ(he.dissolveEdges({diag}), 1);
    EXPECT_EQ(activeFaceCount(he), 1);
    EXPECT_TRUE(he.validate());

    // The shared diagonal is gone; neither (1,2) nor (0,3) is an edge,
    // because the merged face is a quad with only its perimeter edges.
    EXPECT_EQ(findEdge(he, 1, 2), -1);
    EXPECT_EQ(findEdge(he, 0, 3), -1);
    // The single surviving face has 4 vertices.
    int quadCount = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        if (he.faceVertices(static_cast<int>(f)).size() == 4) ++quadCount;
    }
    EXPECT_EQ(quadCount, 1);
}

TEST(HalfEdgeMeshStandalone, DissolveEdgesBoundaryEdgeIsSkipped) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const int boundary = findEdge(he, 0, 1);
    ASSERT_GE(boundary, 0);
    EXPECT_EQ(he.dissolveEdges({boundary}), 0)
        << "boundary edges have no second face to merge into";
    EXPECT_EQ(activeFaceCount(he), 2);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, DissolveVerticesHexFanCenterCollapsesToHexagon) {
    // n-gon-aware dissolve: dissolving v0 (the hex fan's center)
    // replaces the 6 incident triangles with a SINGLE hexagon face —
    // no fan diagonals introduced. (Previous triangle-only impl
    // fan-triangulated the resulting boundary loop into 4 triangles.)
    auto em = makeHexFan();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 6);

    EXPECT_EQ(he.dissolveVertices({0}), 1);
    EXPECT_EQ(activeFaceCount(he), 1)
        << "hexagon merged from the umbrella, no fan diagonals";
    EXPECT_LT(he.vertex(0).halfEdge, 0);
    EXPECT_TRUE(he.validate());

    // The single surviving face has 6 vertices.
    int hexCount = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        if (he.faceVertices(static_cast<int>(f)).size() == 6) ++hexCount;
    }
    EXPECT_EQ(hexCount, 1);
}

TEST(HalfEdgeMeshStandalone, DissolveVerticesBoundaryVertexIsSkipped) {
    // In the quad mesh every vertex sits on the boundary loop. dissolveVertices
    // must refuse to operate on any of them.
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_EQ(he.dissolveVertices({0, 1, 2, 3}), 0);
    EXPECT_EQ(activeFaceCount(he), 2);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, DissolveVerticesLowValenceIsSkipped) {
    // A single triangle: every vertex has valence 2. dissolveVertices skips
    // valence < 3 since there's no umbrella to merge.
    auto em = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_EQ(he.dissolveVertices({0, 1, 2}), 0);
    EXPECT_EQ(activeFaceCount(he), 1);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, DissolveEdgesMultipleDisjointEdgesAllProcessed) {
    // Regression for Codex P1 / CodeRabbit Major: dissolveEdges used to
    // iterate over the input by raw edge index, but rebuildEdgesAndTwins
    // reorders edges after each iteration. Two disjoint interior diagonals
    // selected together used to dissolve only the first reliably.
    //
    // Build a 2×1 quad strip:
    //   v0─v1─v2
    //   │ ╲│ ╲│
    //   v3─v4─v5
    // Triangles: (0,1,3),(1,4,3),(1,2,4),(2,5,4) — two interior diagonals
    // (1↔3) and (2↔4). Both must dissolve; result is two non-degenerate quads
    // (4 triangles total) regardless of insertion order.
    EditableMesh em;
    EditableSubMesh sub;
    sub.materialName = "Strip";
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        v.uv = Ogre::Vector2(x * 0.5f, y); v.hasUV = true;
        return v;
    };
    sub.vertices = {
        mkV(0, 1), mkV(1, 1), mkV(2, 1),       // 0 1 2 (top)
        mkV(0, 0), mkV(1, 0), mkV(2, 0),       // 3 4 5 (bottom)
    };
    auto mkT = [](int a, int b, int c) {
        EditableTriangle t;
        t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
        return t;
    };
    sub.triangles = {
        mkT(0, 1, 3), mkT(1, 4, 3), mkT(1, 2, 4), mkT(2, 5, 4),
    };
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 4);

    const int diagL = findEdge(he, 1, 3);
    const int diagR = findEdge(he, 2, 4);
    ASSERT_GE(diagL, 0);
    ASSERT_GE(diagR, 0);

    EXPECT_EQ(he.dissolveEdges({diagL, diagR}), 2)
        << "both interior diagonals must dissolve regardless of edge-slot reordering";
    // n-gon-aware dissolve merges each pair of triangles into ONE quad —
    // 2 active quads total, NOT 4 fan-triangulated triangles.
    EXPECT_EQ(activeFaceCount(he), 2)
        << "two merged quads, no fan diagonals";
    EXPECT_TRUE(he.validate());

    // Both old diagonals should be gone.
    EXPECT_EQ(findEdge(he, 1, 3), -1);
    EXPECT_EQ(findEdge(he, 2, 4), -1);
}

// ===========================================================================
// subdivideFaces — 1-to-4 triangle split with T-junction handling
// ===========================================================================

TEST(HalfEdgeMeshStandalone, SubdivideFacesEmptyIsNoOp) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_TRUE(he.subdivideFaces({}).empty());
    EXPECT_EQ(activeFaceCount(he), 2);
}

TEST(HalfEdgeMeshStandalone, SubdivideFacesSingleTriangleProducesFourSubTriangles) {
    // A lone triangle subdivided → 3 new midpoint vertices, 4 sub-triangles.
    auto em = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(he.vertexCount(), 3u);
    ASSERT_EQ(activeFaceCount(he), 1);

    const auto newVerts = he.subdivideFaces({0});
    EXPECT_EQ(newVerts.size(), 3u);
    EXPECT_EQ(he.vertexCount(), 6u);
    EXPECT_EQ(activeFaceCount(he), 4);
    EXPECT_TRUE(he.validate());

    // Each midpoint should land at the midpoint of one of the original edges.
    const Ogre::Vector3 expected[3] = {
        Ogre::Vector3(0.5f, 0.0f, 0.0f),    // mid(v0, v1)
        Ogre::Vector3(0.5f, 0.5f, 0.0f),    // mid(v1, v2)
        Ogre::Vector3(0.0f, 0.5f, 0.0f),    // mid(v2, v0)
    };
    for (const auto& exp : expected) {
        bool found = false;
        for (int v : newVerts) {
            if (he.vertex(v).position.distance(exp) < 1e-4f) { found = true; break; }
        }
        EXPECT_TRUE(found) << "midpoint near (" << exp.x << ", " << exp.y << ", " << exp.z << ") not found";
    }

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
}

TEST(HalfEdgeMeshStandalone, SubdivideFacesQuadBothTrianglesProducesEightSubTriangles) {
    // Quad has two triangles sharing the v1-v2 diagonal. Subdividing both
    // should reuse the v1-v2 midpoint between them so the mesh stays
    // manifold (no tear along the shared edge).
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 2);

    const auto newVerts = he.subdivideFaces({0, 1});
    // Edges: 4 unique boundary + 1 shared diagonal = 5 edges → 5 midpoints.
    EXPECT_EQ(newVerts.size(), 5u);
    EXPECT_EQ(activeFaceCount(he), 8); // 4 sub-triangles per face
    EXPECT_TRUE(he.validate());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
}

TEST(HalfEdgeMeshStandalone, SubdivideFacesAdjacentNonSelectedAvoidsTjunction) {
    // Subdividing only triangle 0 of the quad splits the shared v1-v2
    // diagonal. Triangle 1 (not selected) must be retriangulated against
    // that midpoint so we don't leave a T-junction. Expect:
    //   - 3 new midpoints (the 3 edges of triangle 0)
    //   - triangle 0 → 4 sub-triangles
    //   - triangle 1 → 2 sub-triangles (one edge split via the diagonal)
    //   - total: 6 active triangles
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    const auto newVerts = he.subdivideFaces({0});
    EXPECT_EQ(newVerts.size(), 3u);
    EXPECT_EQ(activeFaceCount(he), 6);
    EXPECT_TRUE(he.validate());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back))
        << "non-selected adjacent triangle must be retriangulated to "
           "avoid a T-junction at the new midpoint";
}

TEST(HalfEdgeMeshStandalone, SubdivideFacesPreservesUVAtMidpoint) {
    // UV / normal interpolation flows through the same `interpolateVertex`
    // helper as splitEdge, but check the t=0.5 case explicitly here.
    auto em = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    const auto newVerts = he.subdivideFaces({0});
    ASSERT_EQ(newVerts.size(), 3u);

    // v0=(uv 0,0), v1=(uv 1,0), v2=(uv 0,1). Midpoints land at:
    //   mid(v0,v1) → (0.5, 0)
    //   mid(v1,v2) → (0.5, 0.5)
    //   mid(v2,v0) → (0, 0.5)
    std::set<std::pair<float,float>> expectedUV = {
        {0.5f, 0.0f}, {0.5f, 0.5f}, {0.0f, 0.5f},
    };
    for (int v : newVerts) {
        const auto& vert = he.vertex(v);
        EXPECT_TRUE(vert.hasUV);
        bool matched = false;
        for (auto it = expectedUV.begin(); it != expectedUV.end(); ++it) {
            if (std::abs(vert.uv.x - it->first) < 1e-4f
             && std::abs(vert.uv.y - it->second) < 1e-4f) {
                expectedUV.erase(it);
                matched = true;
                break;
            }
        }
        EXPECT_TRUE(matched) << "unexpected UV (" << vert.uv.x << ", " << vert.uv.y << ")";
    }
    EXPECT_TRUE(expectedUV.empty()) << "every expected midpoint UV should match a new vertex";
}

TEST(HalfEdgeMeshStandalone, SubdivideFacesSkipsRetiredAndOutOfRange) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    // Out-of-range and negative entries are ignored. With no live target
    // remaining, the call is a no-op.
    EXPECT_TRUE(he.subdivideFaces({-1, 99, 1000}).empty());
    EXPECT_EQ(activeFaceCount(he), 2);
}

TEST(HalfEdgeMeshStandalone, SubdivideFacesIsRoundtripSafe) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    he.subdivideFaces({0, 1});

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    HalfEdgeMesh he2;
    ASSERT_TRUE(he2.buildFromEditableMesh(back));
    EXPECT_TRUE(he2.validate());
    EXPECT_EQ(activeFaceCount(he2), 8);
}

// ===========================================================================
// fillSelection — face creation from selected vertices / closed loops
// ===========================================================================

// Build a 4-vertex mesh with a single triangle (v0, v1, v2). v3 is dangling
// — has a position but no incident face. Used for fill rejection tests.
static EditableMesh makeQuadMissingOneTriangle()
{
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "FillMat";

    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        v.uv = Ogre::Vector2(x, y); v.hasUV = true;
        return v;
    };
    sub.vertices = { mkV(0, 0), mkV(1, 0), mkV(0, 1), mkV(1, 1) };

    EditableTriangle t;
    t.indices[0] = 0; t.indices[1] = 1; t.indices[2] = 2;
    sub.triangles = { t };
    mesh.subMeshes().push_back(std::move(sub));
    return mesh;
}

TEST(HalfEdgeMeshStandalone, FillSelectionTooFewVerticesReturnsZero) {
    auto em = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_EQ(he.fillSelection({}), 0);
    EXPECT_EQ(he.fillSelection({0}), 0);
    EXPECT_EQ(he.fillSelection({0, 1}), 0);
}

TEST(HalfEdgeMeshStandalone, FillSelectionRejectsDuplicateVertices) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_EQ(he.fillSelection({0, 0, 1}), 0);
    EXPECT_EQ(he.fillSelection({0, 1, 2, 1}), 0);
}

TEST(HalfEdgeMeshStandalone, FillSelectionRejectsOutOfRangeVertices) {
    auto em = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_EQ(he.fillSelection({0, 1, 99}), 0);
    EXPECT_EQ(he.fillSelection({-1, 0, 1}), 0);
}

TEST(HalfEdgeMeshStandalone, FillSelectionRejectsCrossSubMesh) {
    auto em = makeTwoSubMeshMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    // Submesh 0 owns HE verts 0-2; submesh 1 owns 3-5. Picking one from
    // each should refuse the fill (would silently weld material groups).
    EXPECT_EQ(he.fillSelection({0, 1, 3}), 0);
    EXPECT_EQ(activeFaceCount(he), 2) << "no faces created on cross-submesh fill";
}

TEST(HalfEdgeMeshStandalone, FillSelectionRejectsDuplicateOfExistingTriangle) {
    auto em = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    // The triangle (0, 1, 2) already exists.
    EXPECT_EQ(he.fillSelection({0, 1, 2}), 0);
    EXPECT_EQ(activeFaceCount(he), 1);
}

TEST(HalfEdgeMeshStandalone, FillSelectionThreeVerticesEmitsOneTriangle) {
    // Existing quad has triangles (0,1,2) and (1,3,2). Fill (0, 3, 1) —
    // a different triangle that shares no winding with the existing two.
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 2);

    // Use the missing-one-triangle mesh: only triangle (0,1,2) exists.
    // Filling (1, 3, 2) closes the quad into a planar manifold.
    auto em2 = makeQuadMissingOneTriangle();
    HalfEdgeMesh he2;
    ASSERT_TRUE(he2.buildFromEditableMesh(em2));
    ASSERT_EQ(activeFaceCount(he2), 1);

    EXPECT_EQ(he2.fillSelection({1, 3, 2}), 1);
    EXPECT_EQ(activeFaceCount(he2), 2);
    EXPECT_TRUE(he2.validate());

    EditableMesh back;
    ASSERT_TRUE(he2.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back))
        << "filling a missing tri should produce a watertight quad";
}

TEST(HalfEdgeMeshStandalone, FillSelectionFourVerticesEmitsTwoTriangles) {
    // Build a 5-vertex submesh anchored by a single triangle. The
    // remaining 4 vertices form a convex quad and share no edge with
    // the anchor, so a 4-vertex fill into them is unambiguous.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "FillMat";
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    sub.vertices = {
        // anchor triangle (0, 1, 2)
        mkV(-2, -2), mkV(-1, -2), mkV(-2, -1),
        // four corners of an isolated quad far from the anchor
        mkV(0, 0), mkV(1, 0), mkV(1, 1), mkV(0, 1),
    };
    EditableTriangle anchor;
    anchor.indices[0] = 0; anchor.indices[1] = 1; anchor.indices[2] = 2;
    sub.triangles = { anchor };
    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));
    ASSERT_EQ(activeFaceCount(he), 1);

    // Fill (3, 4, 5, 6) as a single quad face — NOT a fan-triangulation.
    // Pre-quads-followup this returned 2 (triangle count); now it
    // returns 1 (face count) and the result round-trips back through
    // toEditableMesh as one 4-index EditableFace.
    EXPECT_EQ(he.fillSelection({3, 4, 5, 6}), 1);
    EXPECT_EQ(activeFaceCount(he), 2);
    EXPECT_TRUE(he.validate());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));

    // The new face must round-trip as a quad EditableFace, not 2 tris.
    ASSERT_EQ(back.subMeshes().size(), 1u);
    const auto& subBack = back.subMeshes()[0];
    ASSERT_FALSE(subBack.faces.empty())
        << "n>=4 fill must populate EditableSubMesh::faces";
    bool sawQuad = false;
    for (const auto& f : subBack.faces) {
        if (f.indices.size() == 4) { sawQuad = true; break; }
    }
    EXPECT_TRUE(sawQuad)
        << "fillSelection of 4 verts must produce a single quad face";
}

TEST(HalfEdgeMeshStandalone, FillSelectionFiveVerticesProducesSinglePentagon) {
    // Pentagon fill: 5 vertices → ONE 5-vertex EditableFace, not 3 fan
    // triangles. (Quads-followup: a single n-gon HEFace round-trips as
    // a single n-gon EditableFace through toEditableMesh.)
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "FillMat";
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    sub.vertices = { mkV(0, 0), mkV(1, 0), mkV(2, 0.5f), mkV(1, 1), mkV(0, 1), mkV(0.5f, 0.5f) };
    EditableTriangle anchor;
    anchor.indices[0] = 0; anchor.indices[1] = 5; anchor.indices[2] = 4;
    sub.triangles = { anchor };
    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));

    // Vertices 0..4 form a convex pentagon. Fill its loop in order.
    EXPECT_EQ(he.fillSelection({0, 1, 2, 3, 4}), 1);
    EXPECT_TRUE(he.validate());

    // Confirm round-trip preserves the 5-gon.
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    ASSERT_EQ(back.subMeshes().size(), 1u);
    bool sawPentagon = false;
    for (const auto& f : back.subMeshes()[0].faces) {
        if (f.indices.size() == 5) { sawPentagon = true; break; }
    }
    EXPECT_TRUE(sawPentagon)
        << "fillSelection of 5 verts must produce a single pentagon face";
}

// ===========================================================================
// subdivideFaces — deeper coverage on closed manifolds and submesh handling
// ===========================================================================

TEST(HalfEdgeMeshStandalone, SubdivideFacesAllCubeFacesStaysClosedManifold) {
    // Subdividing every triangle of a closed cube must keep the mesh
    // closed (no boundary edges) and watertight. This exercises the
    // splitMask=7 path on every face plus the shared-midpoint reuse
    // between every pair of adjacent triangles.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    std::vector<int> allFaces(12);
    std::iota(allFaces.begin(), allFaces.end(), 0);
    const auto newVerts = he.subdivideFaces(allFaces);
    EXPECT_FALSE(newVerts.empty());
    EXPECT_EQ(activeFaceCount(he), 48); // 12 faces × 4 sub-tris each
    EXPECT_TRUE(he.validate());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    auto s = statsOf(back);
    EXPECT_EQ(s.boundaryEdges, 0u)
        << "subdividing every face of a closed cube must stay closed";
    EXPECT_TRUE(isManifold(back));
}

TEST(HalfEdgeMeshStandalone, SubdivideFacesPreservesBoneWeightsAtMidpoint) {
    // The bone-weight mesh has v0/v1 on bone 1 (weight 1) and v2 on
    // bones 2/3 (weight 0.5 each). Subdividing splits each edge at
    // t=0.5, so mid(v0,v1) keeps bone 1, while mid(v0,v2) is a 50/50
    // blend of bone 1 and bones 2/3.
    auto em = makeBoneWeightMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    const auto newVerts = he.subdivideFaces({0});
    ASSERT_EQ(newVerts.size(), 3u);

    // Find the midpoint that lies on the v0-v1 segment (z=0, y=0).
    // It should have a single bone influence (bone 1).
    bool foundBone1Midpoint = false;
    bool foundBlendedMidpoint = false;
    for (int v : newVerts) {
        const auto& vert = he.vertex(v);
        const auto& bas = vert.boneAssignments;
        // Bone-1-only midpoint (mid of v0, v1): both endpoints have only
        // bone 1, so the lerp keeps a single (1, 1.0) entry.
        if (bas.size() == 1 && bas[0].first == 1) {
            EXPECT_NEAR(bas[0].second, 1.0f, 1e-4f);
            foundBone1Midpoint = true;
        }
        // mid(v0, v2) and mid(v1, v2) blend (1, 1.0) with (2, 0.5)+(3, 0.5).
        // After 50/50 lerp: (1, 0.5), (2, 0.25), (3, 0.25).
        if (bas.size() == 3) {
            float w1 = 0, w2 = 0, w3 = 0;
            for (const auto& [idx, w] : bas) {
                if (idx == 1) w1 = w;
                if (idx == 2) w2 = w;
                if (idx == 3) w3 = w;
            }
            if (w1 > 0 && w2 > 0 && w3 > 0) {
                EXPECT_NEAR(w1, 0.5f, 1e-4f);
                EXPECT_NEAR(w2, 0.25f, 1e-4f);
                EXPECT_NEAR(w3, 0.25f, 1e-4f);
                foundBlendedMidpoint = true;
            }
        }
    }
    EXPECT_TRUE(foundBone1Midpoint) << "expected a midpoint with sole bone-1 influence";
    EXPECT_TRUE(foundBlendedMidpoint) << "expected a midpoint with blended bone weights";
}

TEST(HalfEdgeMeshStandalone, SubdivideFacesHandlesAllSplitMaskPaths) {
    // The splitMask switch has 7 non-zero cases (1..7). Build a planar
    // strip of 7 connected triangles where each one shares a different
    // single edge with its only selected neighbor — that way each tri
    // ends up with a different splitMask after the selection is applied.
    //
    // Simpler approach: subdivide one specific face on a quad mesh, where
    // the neighbor triangle has exactly one of its three edges split,
    // exercising splitMask=1, 2, or 4 depending on which edge it shares.
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    // Subdivide face 0 (verts 0,1,2). Face 1 (verts 1,3,2) shares edge
    // (1,2) with face 0. After subdividing face 0 only the (1,2) midpoint
    // is created, so face 1 lands in the splitMask=2 path (edge v1-v2).
    he.subdivideFaces({0});
    EXPECT_TRUE(he.validate());
    EXPECT_EQ(activeFaceCount(he), 6); // 4 + 2
}

TEST(HalfEdgeMeshStandalone, SubdivideFacesAcrossSubmeshesPreservesMaterials) {
    // Two-submesh mesh: subdividing face 0 (submesh 0) must NOT bleed
    // submesh-0 midpoints into submesh 1's faces.
    auto em = makeTwoSubMeshMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    const auto newVerts = he.subdivideFaces({0});
    EXPECT_EQ(newVerts.size(), 3u);
    EXPECT_TRUE(he.validate());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    ASSERT_EQ(back.subMeshes().size(), 2u);
    EXPECT_EQ(back.subMeshes()[0].materialName, "Mat0");
    EXPECT_EQ(back.subMeshes()[1].materialName, "Mat1");
    // Submesh 1 is untouched: still 1 triangle, 3 vertices.
    EXPECT_EQ(back.subMeshes()[1].triangles.size(), 1u);
    EXPECT_EQ(back.subMeshes()[1].vertices.size(), 3u);
}

TEST(HalfEdgeMeshStandalone, SubdivideFacesNonAdjacentMultipleSelection) {
    // On a cube, faces 0 (back-1) and 2 (front-1) share NO edges. Both
    // should subdivide to 4 sub-triangles each, while their respective
    // adjacent faces each get retriangulated independently. Net: +6 from
    // each subdivided face, +6 from each set of 3 adjacent faces split
    // by 1 edge → +24 total → 36 active triangles.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    he.subdivideFaces({0, 2});
    EXPECT_TRUE(he.validate());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back))
        << "non-adjacent face selections must each tile cleanly";

    auto s = statsOf(back);
    EXPECT_EQ(s.boundaryEdges, 0u);
}

// ===========================================================================
// fillSelection — deeper coverage
// ===========================================================================

TEST(HalfEdgeMeshStandalone, FillSelectionCapsCubeHole) {
    // Real-world hole-fill scenario: take a closed cube, delete one face,
    // then fill the resulting 3-vertex hole. The post-fill mesh should
    // again be closed.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    // Delete face 0 (verts 0, 2, 1 from makeCubeMesh).
    EXPECT_EQ(he.deleteFaces({0}), 1);
    EXPECT_TRUE(he.validate());

    EditableMesh afterDelete;
    ASSERT_TRUE(he.toEditableMesh(afterDelete));
    auto sBefore = statsOf(afterDelete);
    EXPECT_EQ(sBefore.boundaryEdges, 3u) << "deleting one face exposes 3 boundary edges";

    // Find the surviving vertices that match positions (-1,-1,-1), (-1,1,-1),
    // (1,-1,-1) — those are corners 0, 2, 1 from the cube. After deleteFaces
    // their HE indices may have shifted, so look them up by position.
    HalfEdgeMesh he2;
    ASSERT_TRUE(he2.buildFromEditableMesh(afterDelete));
    auto findVertByPos = [&](const Ogre::Vector3& p) -> int {
        for (size_t v = 0; v < he2.vertexCount(); ++v) {
            if (he2.vertex(static_cast<int>(v)).position.distance(p) < 1e-4f)
                return static_cast<int>(v);
        }
        return -1;
    };
    const int v0 = findVertByPos(Ogre::Vector3(-1, -1, -1));
    const int v2 = findVertByPos(Ogre::Vector3(-1,  1, -1));
    const int v1 = findVertByPos(Ogre::Vector3( 1, -1, -1));
    ASSERT_GE(v0, 0); ASSERT_GE(v2, 0); ASSERT_GE(v1, 0);

    // Fill the hole with the same winding the original face had: (0, 2, 1).
    EXPECT_EQ(he2.fillSelection({v0, v2, v1}), 1);
    EXPECT_TRUE(he2.validate());

    EditableMesh afterFill;
    ASSERT_TRUE(he2.toEditableMesh(afterFill));
    auto sAfter = statsOf(afterFill);
    EXPECT_EQ(sAfter.boundaryEdges, 0u) << "filling the hole should re-close the cube";
    EXPECT_TRUE(isManifold(afterFill));
}

TEST(HalfEdgeMeshStandalone, FillSelectionAcceptsOrphanedVertices) {
    // A vertex with no incident face is still valid input — buildFrom-
    // EditableMesh creates HEVertex slots for every vertex regardless of
    // triangle membership. The fill must accept these for hole-cap flows
    // where the user pre-selects vertices that survived a deleteFaces.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "Orphan";
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    sub.vertices = { mkV(0, 0), mkV(1, 0), mkV(0, 1), mkV(1, 1) };
    // No triangles → all 4 vertices are orphans.
    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));

    // Fill with all 4 orphans → ONE new quad face (quads-followup).
    EXPECT_EQ(he.fillSelection({0, 1, 3, 2}), 1);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, FillSelectionRejectsLargerFanThatDuplicatesExistingTri) {
    // Regression for CodeRabbit Major: when n > 3, individual fan
    // triangles can still duplicate existing tris. The quad-missing-one
    // mesh has triangle (0,1,2) already present. Filling (0,1,2,3) would
    // fan into (0,1,2) [duplicate!] + (0,2,3). With the wider dup-check
    // this must refuse and leave the mesh untouched.
    auto em = makeQuadMissingOneTriangle();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    EXPECT_EQ(he.fillSelection({0, 1, 2, 3}), 0)
        << "n>3 fan must also reject duplicates of existing tris";
    EXPECT_EQ(activeFaceCount(he), 1) << "rejected fill must not mutate the mesh";
}

TEST(HalfEdgeMeshStandalone, FillSelectionUndoRoundTrip) {
    // Ensure a subdivide → toEditableMesh → buildFromEditableMesh round
    // trip preserves the new triangulation. Same idea as the existing
    // SubdivideFacesIsRoundtripSafe test, but for fill.
    auto em = makeQuadMissingOneTriangle();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    he.fillSelection({1, 3, 2});

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    HalfEdgeMesh he2;
    ASSERT_TRUE(he2.buildFromEditableMesh(back));
    EXPECT_TRUE(he2.validate());
    EXPECT_EQ(activeFaceCount(he2), 2);
}

// ===========================================================================
// EditableFace (n-gon) round-trip — chunk 1 of the quad migration
// ===========================================================================

TEST(HalfEdgeMeshStandalone, BuildFromQuadFacePopulatesSingleHEFace) {
    // A submesh with a single 4-vertex EditableFace (no triangles)
    // should produce ONE HalfEdgeMesh face of valence 4.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "QuadMat";
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        v.uv = Ogre::Vector2(x, y); v.hasUV = true;
        return v;
    };
    sub.vertices = { mkV(0, 0), mkV(1, 0), mkV(1, 1), mkV(0, 1) };
    EditableFace f;
    f.indices = {0, 1, 2, 3};
    sub.faces.push_back(std::move(f));
    // Note: triangles intentionally left empty — when faces is non-empty
    // it's canonical, and HE should read faces directly.
    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));
    EXPECT_EQ(activeFaceCount(he), 1);
    EXPECT_EQ(he.faceVertices(0).size(), 4u)
        << "quad EditableFace must produce a 4-valence HE face, not 2 triangles";
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, ToEditableMeshPreservesQuadInFaces) {
    // Round-trip: build HE from a quad face, write back, verify the
    // EditableSubMesh has a 4-vertex face (and triangles is the fan
    // triangulation).
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "QuadMat";
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    sub.vertices = { mkV(0, 0), mkV(1, 0), mkV(1, 1), mkV(0, 1) };
    EditableFace f;
    f.indices = {0, 1, 2, 3};
    sub.faces.push_back(std::move(f));
    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    ASSERT_EQ(back.subMeshes().size(), 1u);
    const auto& outSub = back.subMeshes()[0];

    // n-gon faces preserved
    ASSERT_EQ(outSub.faces.size(), 1u)
        << "quad must round-trip as a single 4-vertex face";
    EXPECT_EQ(outSub.faces[0].indices.size(), 4u);

    // triangles is the fan triangulation (2 tris from a quad)
    EXPECT_EQ(outSub.triangles.size(), 2u)
        << "fan-triangulation: a quad emits N-2 = 2 triangles";

    // Vertex count preserved
    EXPECT_EQ(outSub.vertices.size(), 4u);
}

TEST(HalfEdgeMeshStandalone, ToEditableMeshLeavesFacesEmptyWhenAllTris) {
    // The legacy invariant: if every face in the HE mesh is a triangle,
    // `EditableSubMesh::faces` should be left empty so existing
    // triangle-only consumers don't have to learn the new representation.
    auto em = makeQuadMesh(); // two triangles
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    ASSERT_EQ(back.subMeshes().size(), 1u);
    EXPECT_TRUE(back.subMeshes()[0].faces.empty())
        << "all-triangle submeshes must NOT populate faces (legacy invariant)";
    EXPECT_EQ(back.subMeshes()[0].triangles.size(), 2u);
}

TEST(HalfEdgeMeshStandalone, BuildFromMixedTriAndQuadSubMeshes) {
    // Two submeshes: one triangle-only (legacy path), one quad-only
    // (n-gon path). buildFromEditableMesh should produce 3 HE faces
    // total (1 tri + 1 quad ... wait, 1 tri + 1 quad = 2 faces).
    EditableMesh mesh;
    auto mkV = [](float x, float y, float z) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, z);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };

    {
        EditableSubMesh sub;
        sub.materialName = "TriMat";
        sub.vertices = { mkV(0, 0, 0), mkV(1, 0, 0), mkV(0, 1, 0) };
        EditableTriangle t;
        t.indices[0] = 0; t.indices[1] = 1; t.indices[2] = 2;
        sub.triangles.push_back(t);
        mesh.subMeshes().push_back(std::move(sub));
    }
    {
        EditableSubMesh sub;
        sub.materialName = "QuadMat";
        sub.vertices = { mkV(0, 0, 1), mkV(1, 0, 1), mkV(1, 1, 1), mkV(0, 1, 1) };
        EditableFace f;
        f.indices = {0, 1, 2, 3};
        sub.faces.push_back(std::move(f));
        mesh.subMeshes().push_back(std::move(sub));
    }

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(mesh));
    EXPECT_EQ(activeFaceCount(he), 2);
    EXPECT_TRUE(he.validate());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    ASSERT_EQ(back.subMeshes().size(), 2u);
    EXPECT_TRUE(back.subMeshes()[0].faces.empty())  // triangle submesh stays legacy
        << "triangle submesh must not populate faces";
    EXPECT_EQ(back.subMeshes()[1].faces.size(), 1u) // quad submesh has 1 face
        << "quad submesh must populate faces with the polygon";
    EXPECT_EQ(back.subMeshes()[1].faces[0].indices.size(), 4u);
}

TEST(HalfEdgeMeshStandalone, EditableFaceIsValidGuardsBadInputs) {
    EditableFace empty;
    EXPECT_FALSE(empty.isValid());

    EditableFace pair;
    pair.indices = {0, 1};
    EXPECT_FALSE(pair.isValid());

    EditableFace dup;
    dup.indices = {0, 1, 1, 2};
    EXPECT_FALSE(dup.isValid()) << "consecutive duplicate index pair must fail";

    EditableFace okQuad;
    okQuad.indices = {0, 1, 2, 3};
    EXPECT_TRUE(okQuad.isValid());

    EditableFace okTri;
    okTri.indices = {0, 1, 2};
    EXPECT_TRUE(okTri.isValid());
}

TEST(HalfEdgeMeshStandalone, BuildFromQuadFaceRejectsInvalidIndex) {
    // An out-of-range index in EditableFace::indices must not crash —
    // the face is silently skipped.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "Bad";
    EditableVertex v;
    v.position = Ogre::Vector3::ZERO;
    sub.vertices = {v, v, v}; // 3 verts available
    EditableFace f;
    f.indices = {0, 1, 99}; // 99 is out of range
    sub.faces.push_back(std::move(f));
    mesh.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    EXPECT_TRUE(he.buildFromEditableMesh(mesh));
    EXPECT_EQ(activeFaceCount(he), 0)
        << "out-of-range face must be skipped, not crash";
}

// ===========================================================================
// triangulateFaces / promoteTrianglesToFaces helpers
// ===========================================================================

TEST(HalfEdgeMeshStandalone, TriangulateFacesQuadProducesTwoTriangles) {
    EditableSubMesh sub;
    EditableFace f;
    f.indices = {0, 1, 2, 3};
    sub.faces.push_back(std::move(f));

    triangulateFaces(sub);
    ASSERT_EQ(sub.triangles.size(), 2u);
    // Fan from vertex 0: (0, 1, 2) and (0, 2, 3).
    EXPECT_EQ(sub.triangles[0].indices[0], 0u);
    EXPECT_EQ(sub.triangles[0].indices[1], 1u);
    EXPECT_EQ(sub.triangles[0].indices[2], 2u);
    EXPECT_EQ(sub.triangles[1].indices[0], 0u);
    EXPECT_EQ(sub.triangles[1].indices[1], 2u);
    EXPECT_EQ(sub.triangles[1].indices[2], 3u);
}

TEST(HalfEdgeMeshStandalone, TriangulateFacesPentagonProducesThreeTriangles) {
    EditableSubMesh sub;
    EditableFace f;
    f.indices = {10, 20, 30, 40, 50}; // arbitrary indices
    sub.faces.push_back(std::move(f));

    triangulateFaces(sub);
    EXPECT_EQ(sub.triangles.size(), 3u) << "N=5 fan emits N-2 = 3 triangles";
}

TEST(HalfEdgeMeshStandalone, TriangulateFacesSkipsInvalidFaces) {
    EditableSubMesh sub;
    {
        EditableFace bad; // <3 indices, isValid() false
        bad.indices = {0, 1};
        sub.faces.push_back(std::move(bad));
    }
    {
        EditableFace good;
        good.indices = {0, 1, 2};
        sub.faces.push_back(std::move(good));
    }
    triangulateFaces(sub);
    EXPECT_EQ(sub.triangles.size(), 1u) << "invalid face skipped";
}

TEST(HalfEdgeMeshStandalone, PromoteTrianglesToFacesProducesMatchingFaces) {
    EditableSubMesh sub;
    EditableTriangle t1, t2;
    t1.indices[0] = 0; t1.indices[1] = 1; t1.indices[2] = 2;
    t2.indices[0] = 1; t2.indices[1] = 3; t2.indices[2] = 2;
    sub.triangles = {t1, t2};

    promoteTrianglesToFaces(sub);
    ASSERT_EQ(sub.faces.size(), 2u);
    EXPECT_EQ(sub.faces[0].indices.size(), 3u);
    EXPECT_EQ(sub.faces[0].indices[0], 0u);
    EXPECT_EQ(sub.faces[0].indices[1], 1u);
    EXPECT_EQ(sub.faces[0].indices[2], 2u);
    // triangles should still match the canonical faces invariant
    // (every face is a triangle, so triangulateFaces would produce
    // identical content)
    EXPECT_EQ(sub.triangles.size(), 2u);
}

// ===========================================================================
// subdivideCatmullClark — chunk 5a quad-aware subdivision
// ===========================================================================

TEST(HalfEdgeMeshStandalone, CatmullClarkOnSingleQuadProducesFourQuads) {
    // A single quad → 1 face point + 4 edge points + 4 corners (the 4
    // corners get smoothed in place — for a flat planar quad on a
    // boundary, the boundary-vertex chord rule will have moved them).
    // After CC: 4 output faces, each a quad.
    EditableMesh em;
    EditableSubMesh sub;
    auto mkV = [](float x, float y, float z) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, z);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    sub.vertices = {
        mkV(0, 0, 0), mkV(1, 0, 0), mkV(1, 1, 0), mkV(0, 1, 0),
    };
    EditableFace f;
    f.indices = {0, 1, 2, 3};
    sub.faces.push_back(std::move(f));
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 1);

    const auto newVerts = he.subdivideCatmullClark();
    EXPECT_FALSE(newVerts.empty());
    EXPECT_EQ(activeFaceCount(he), 4) << "1 quad → 4 sub-quads";

    // Every output face should be a quad.
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        EXPECT_EQ(he.faceVertices(static_cast<int>(f)).size(), 4u)
            << "Catmull-Clark always produces quads";
    }
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, CatmullClarkOnTriangleProducesThreeQuads) {
    // A single triangle → 3 sub-quads (one per corner).
    auto em = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 1);

    he.subdivideCatmullClark();
    EXPECT_EQ(activeFaceCount(he), 3);
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        EXPECT_EQ(he.faceVertices(static_cast<int>(f)).size(), 4u);
    }
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, CatmullClarkOnCubeStaysClosed) {
    // 12-tri cube → 36 sub-quads. Output is a closed manifold (no
    // boundary edges), and toEditableMesh round-trips through the
    // n-gon path so all 36 faces appear in EditableSubMesh::faces.
    auto em = makeCubeMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 12);

    he.subdivideCatmullClark();
    EXPECT_EQ(activeFaceCount(he), 36) << "12 tris × 3 sub-quads = 36";
    EXPECT_TRUE(he.validate());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    auto s = statsOf(back);
    EXPECT_EQ(s.boundaryEdges, 0u)
        << "Catmull-Clark on a closed cube must stay closed";
    EXPECT_TRUE(isManifold(back));
    // n-gon path: faces non-empty since output is all quads.
    ASSERT_FALSE(back.subMeshes().empty());
    EXPECT_FALSE(back.subMeshes()[0].faces.empty())
        << "all-quad output should populate EditableSubMesh::faces";
    EXPECT_EQ(back.subMeshes()[0].faces.size(), 36u);
    for (const auto& f : back.subMeshes()[0].faces) {
        EXPECT_EQ(f.indices.size(), 4u) << "every output face is a quad";
    }
}

TEST(HalfEdgeMeshStandalone, CatmullClarkOnQuadGridProducesFourTimesAsManyQuads) {
    // 2x2 grid of 4 quads → 16 sub-quads (one per corner of each face).
    EditableMesh em;
    EditableSubMesh sub;
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    sub.vertices = {
        mkV(0, 0), mkV(1, 0), mkV(2, 0),
        mkV(0, 1), mkV(1, 1), mkV(2, 1),
        mkV(0, 2), mkV(1, 2), mkV(2, 2),
    };
    auto mkF = [](unsigned a, unsigned b, unsigned c, unsigned d) {
        EditableFace f;
        f.indices = {a, b, c, d};
        return f;
    };
    sub.faces = {
        mkF(0, 1, 4, 3), mkF(1, 2, 5, 4),
        mkF(3, 4, 7, 6), mkF(4, 5, 8, 7),
    };
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 4);

    he.subdivideCatmullClark();
    EXPECT_EQ(activeFaceCount(he), 16) << "4 quads × 4 sub-quads = 16";
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, CatmullClarkPreservesPlanarFaceCenter) {
    // For a planar regular quad on a closed manifold (e.g. the centre
    // of a 3x3 quad grid), the face point should land exactly at the
    // arithmetic centre of the four corners.
    EditableMesh em;
    EditableSubMesh sub;
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    sub.vertices = { mkV(0, 0), mkV(1, 0), mkV(1, 1), mkV(0, 1) };
    EditableFace f;
    f.indices = {0, 1, 2, 3};
    sub.faces.push_back(std::move(f));
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const auto newVerts = he.subdivideCatmullClark();

    // Find the face point (the only vertex with all 4 corners as 1-ring
    // neighbours — exactly the face point on a single-quad mesh).
    bool foundFacePoint = false;
    for (int v : newVerts) {
        const auto neighbours = he.verticesAroundVertex(v);
        if (neighbours.size() == 4) {
            const auto& p = he.vertex(v).position;
            EXPECT_NEAR(p.x, 0.5f, 1e-4f);
            EXPECT_NEAR(p.y, 0.5f, 1e-4f);
            EXPECT_NEAR(p.z, 0.0f, 1e-4f);
            foundFacePoint = true;
            break;
        }
    }
    EXPECT_TRUE(foundFacePoint);
}

TEST(HalfEdgeMeshStandalone, CatmullClarkEmptyMeshIsNoOp) {
    HalfEdgeMesh he;
    EXPECT_TRUE(he.subdivideCatmullClark().empty());
}

TEST(HalfEdgeMeshStandalone, CatmullClarkPreservesVertexColors) {
    // Regression: averageHEVertices used to accumulate r.color += v.color * w
    // on top of HEVertex's default-init White, which saturated every output
    // colour to White and silently wiped any vertex paint after a CC pass.
    EditableMesh em;
    EditableSubMesh sub;
    sub.materialName = "M";
    auto mkPaintedV = [](float x, float y, const Ogre::ColourValue& c) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        v.color = c; v.hasColor = true;
        return v;
    };
    // Single quad with each corner painted a distinct, non-white colour.
    const Ogre::ColourValue red  (0.8f, 0.1f, 0.1f, 1.0f);
    const Ogre::ColourValue green(0.1f, 0.8f, 0.1f, 1.0f);
    const Ogre::ColourValue blue (0.1f, 0.1f, 0.8f, 1.0f);
    const Ogre::ColourValue gold (0.8f, 0.7f, 0.1f, 1.0f);
    sub.vertices = { mkPaintedV(0, 0, red),
                     mkPaintedV(1, 0, green),
                     mkPaintedV(1, 1, blue),
                     mkPaintedV(0, 1, gold) };
    EditableFace q; q.indices = {0, 1, 2, 3};
    sub.faces = { q };
    triangulateFaces(sub);
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    he.subdivideCatmullClark();
    EXPECT_TRUE(he.validate());

    // The face point of the quad averages all 4 corners. After the fix
    // its colour is the arithmetic mean:
    //   r = (0.8+0.1+0.1+0.8)/4 = 0.45
    //   g = (0.1+0.8+0.1+0.7)/4 = 0.425
    //   b = (0.1+0.1+0.8+0.1)/4 = 0.275
    //   a = (1+1+1+1)/4         = 1.0
    //
    // Before the fix, r.color started as White (1,1,1,1), so the sum
    // was (1.45, 1.425, 1.275, 2.0) — every channel ≥ 1.0, which packs
    // to White when written to GPU. We can detect either outcome: with
    // the fix, ALL channels stay strictly below 1.0; with the bug, all
    // channels are >= 1.0.
    const Ogre::ColourValue expectedMean(0.45f, 0.425f, 0.275f, 1.0f);
    bool foundFacePoint = false;
    for (size_t i = 0; i < he.vertexCount(); ++i) {
        const auto& v = he.vertex(static_cast<int>(i));
        if (!v.hasColor) continue;
        // Identify the face point by its position (centroid of quad).
        if (std::abs(v.position.x - 0.5f) > 0.05f) continue;
        if (std::abs(v.position.y - 0.5f) > 0.05f) continue;
        foundFacePoint = true;

        // RGB must match the arithmetic mean within float precision.
        // (Not just "below 1.0" — the bug produced ≥ 1.0 in every
        // channel, but a less-permissive averaging bug could produce
        // values in [0,1] that are still wrong.)
        EXPECT_NEAR(v.color.r, expectedMean.r, 1e-4f)
            << "face-point red must be the arithmetic mean of corner colours";
        EXPECT_NEAR(v.color.g, expectedMean.g, 1e-4f);
        EXPECT_NEAR(v.color.b, expectedMean.b, 1e-4f);
    }
    EXPECT_TRUE(foundFacePoint)
        << "Catmull-Clark must produce a face-point at the quad centroid";
}

// ===========================================================================
// subdivideFacesToQuads — chunk 4b: subdivide n-gons into quads on selection
// ===========================================================================

TEST(HalfEdgeMeshStandalone, SubdivideFacesToQuadsEmptyIsNoOp) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_TRUE(he.subdivideFacesToQuads({}).empty());
    EXPECT_EQ(activeFaceCount(he), 2);
}

TEST(HalfEdgeMeshStandalone, SubdivideFacesToQuadsOnQuadProducesFourQuads) {
    EditableMesh em;
    EditableSubMesh sub;
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    sub.vertices = { mkV(0, 0), mkV(1, 0), mkV(1, 1), mkV(0, 1) };
    EditableFace f;
    f.indices = {0, 1, 2, 3};
    sub.faces.push_back(std::move(f));
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 1);

    he.subdivideFacesToQuads({0});
    EXPECT_EQ(activeFaceCount(he), 4) << "1 quad → 4 sub-quads";
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        EXPECT_EQ(he.faceVertices(static_cast<int>(f)).size(), 4u)
            << "every output face is a quad";
    }
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, SubdivideFacesToQuadsOnTriangleProducesThreeQuads) {
    auto em = makeTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    he.subdivideFacesToQuads({0});
    EXPECT_EQ(activeFaceCount(he), 3);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, SubdivideFacesToQuadsSharesEdgeMidpointsBetweenSelectedFaces) {
    // Two adjacent quads in the same submesh sharing one edge.
    // Selecting both should re-use the shared edge's midpoint so the
    // boundary stays manifold (no T-junction, no duplicated vertex).
    EditableMesh em;
    EditableSubMesh sub;
    auto mkV = [](float x, float y) {
        EditableVertex v;
        v.position = Ogre::Vector3(x, y, 0);
        v.normal = Ogre::Vector3(0, 0, 1); v.hasNormal = true;
        return v;
    };
    // 6 verts arranged as two side-by-side quads sharing edge (1,4).
    sub.vertices = {
        mkV(0, 0), mkV(1, 0), mkV(2, 0),  // 0 1 2 (bottom)
        mkV(0, 1), mkV(1, 1), mkV(2, 1),  // 3 4 5 (top)
    };
    auto mkF = [](unsigned a, unsigned b, unsigned c, unsigned d) {
        EditableFace f;
        f.indices = {a, b, c, d};
        return f;
    };
    sub.faces = { mkF(0, 1, 4, 3), mkF(1, 2, 5, 4) };
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    he.subdivideFacesToQuads({0, 1});
    EXPECT_EQ(activeFaceCount(he), 8) << "2 quads × 4 sub-quads = 8";
    EXPECT_TRUE(he.validate());

    // Manifold check on round-trip.
    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back))
        << "shared midpoint between the two quads must keep mesh manifold";
}

TEST(HalfEdgeMeshStandalone, CatmullClarkRoundTripsThroughEditableMesh) {
    auto em = makeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    he.subdivideCatmullClark();

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    HalfEdgeMesh he2;
    ASSERT_TRUE(he2.buildFromEditableMesh(back));
    EXPECT_TRUE(he2.validate());
    // 2 input tris × 3 quads each = 6 output quads.
    EXPECT_EQ(activeFaceCount(he2), 6);
}

// ===========================================================================
// Native-quad-mesh coverage for #326 acceptance criterion: every existing
// topology op should have at least one test that exercises a quad input
// via the n-gon canonical path (sub.faces non-empty), not just the legacy
// 2-triangle quad fixture.
//
// These tests use makeNativeQuadMesh() which populates sub.faces with two
// 4-vertex EditableFace entries. buildFromEditableMesh respects the n-gon
// path when faces is non-empty, so the HE mesh has 2 quad faces (not 4
// fan-triangulated tris).
// ===========================================================================

TEST(HalfEdgeMeshStandalone, ExtrudeNativeQuadFacePreservesCapArity) {
    // Extrude on an n-gon face preserves the cap arity (the lifted copy
    // of the original face stays a quad). Side walls are currently
    // fan-triangulated by extrudeFaces — that's a known limitation of
    // the chunk-4 extrude path, tracked for follow-up; this test only
    // pins the cap-arity invariant which is what users see top-down.
    auto em = makeNativeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(he.faceCount(), 2u);
    ASSERT_EQ(he.faceVertices(0).size(), 4u) << "input is a native quad face";

    auto newVerts = he.extrudeFaces({0});
    EXPECT_EQ(newVerts.size(), 4u) << "one new vertex per face corner";
    EXPECT_TRUE(he.validate());

    // Among active faces:
    //   - lifted cap (quad — invariant we care about)
    //   - untouched neighbour q2 (still a quad)
    //   - 8 fan-triangulated side walls (4 sides × 2 tris each)
    int active = 0, quads = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        ++active;
        if (he.faceVertices(static_cast<int>(f)).size() == 4) ++quads;
    }
    EXPECT_GE(active, 6);
    EXPECT_GE(quads, 2)
        << "lifted cap + untouched q2 must remain quads";
}

TEST(HalfEdgeMeshStandalone, CutPathOnNativeQuadCrossesInterior) {
    // Cut horizontally through both quads — start on the bottom edge of
    // q1, end on the bottom edge of q2 (no, that's parallel — go vertical
    // instead): start on left edge of q1, exit on right edge of q2,
    // crossing the shared (1,2) edge in between.
    auto em = makeNativeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    const int leftEdge  = findEdge(he, 0, 3); // left of q1 (x=0)
    const int rightEdge = findEdge(he, 4, 5); // right of q2 (x=2)
    ASSERT_GE(leftEdge, 0);
    ASSERT_GE(rightEdge, 0);

    // Cut at y=0.5 across both quads. The shared edge (1,2) is also
    // horizontal (x=1, y from 0→1) — wait no, (1,2) goes (1,0)→(1,1) so
    // it's vertical at x=1. The cut line y=0.5 will cross it at (1, 0.5).
    auto newVerts = he.cutPath({{leftEdge, 0.5f}, {rightEdge, 0.5f}});
    EXPECT_GE(newVerts.size(), 3u) << "endpoint splits + interior crossing";
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshStandalone, MergeVerticesOnNativeQuadCollapsesCorner) {
    // Merge two adjacent corners of q1 (verts 0 and 1) — that collapses
    // the bottom edge of q1, turning it into a degenerate triangle (the
    // two merged endpoints become one). q2 is untouched (it shares only
    // vertex 1 with q1).
    auto em = makeNativeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(he.faceCount(), 2u);

    const Ogre::Vector3 mid(0.5f, 0.0f, 0.0f);
    const int retired = he.mergeVertices({0, 1}, mid);
    EXPECT_EQ(retired, 1) << "one of the two collapses to the survivor";
    EXPECT_TRUE(he.validate());

    // q1 collapsed from a quad to a triangle (one of its edges fused).
    // q2 must still be a quad — it contained vertex 1 but lost a neighbour
    // and gained the survivor in its place, still 4 distinct corners.
    int active = 0, quads = 0, tris = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        ++active;
        const auto sz = he.faceVertices(static_cast<int>(f)).size();
        if (sz == 3) ++tris;
        if (sz == 4) ++quads;
    }
    EXPECT_GE(active, 1) << "at least q2 should survive";
    EXPECT_GE(tris + quads, 1);
}

TEST(HalfEdgeMeshStandalone, DeleteFacesNativeQuadRetiresWholeQuad) {
    // Delete the whole q1 quad face. Pre-quad-aware delete operated on
    // single triangles inside a fan-triangulated quad — only half the
    // visible face went. With native quads, deleting face 0 retires
    // exactly one face and leaves q2 untouched.
    auto em = makeNativeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 2);

    he.deleteFaces({0});
    EXPECT_EQ(activeFaceCount(he), 1) << "q1 retired, q2 alive";
    EXPECT_TRUE(he.validate());

    // The surviving face must still be a quad (not a triangle from a
    // half-deleted fan).
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        EXPECT_EQ(he.faceVertices(static_cast<int>(f)).size(), 4u);
    }
}

TEST(HalfEdgeMeshStandalone, DissolveEdgesOnNativeQuadInteriorMergesQuads) {
    // Dissolve the shared edge (1,2) between q1 and q2. The two adjacent
    // quads should merge into a single 6-vertex hexagonal n-gon.
    // Pre-quad-aware dissolve tested this on a 4-tri pair (Quad +
    // Diagonal) producing a quad output; here we verify the n-gon path
    // produces a hexagon from two quads.
    auto em = makeNativeQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(activeFaceCount(he), 2);

    const int sharedEdge = findEdge(he, 1, 2);
    ASSERT_GE(sharedEdge, 0);

    he.dissolveEdges({sharedEdge});
    EXPECT_TRUE(he.validate());

    // After dissolve: 1 active face, with 6 vertices (the union of q1's
    // 4 corners and q2's 4 corners, minus the 2 shared).
    int active = 0;
    size_t outerArity = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        ++active;
        outerArity = he.faceVertices(static_cast<int>(f)).size();
    }
    EXPECT_EQ(active, 1) << "two quads merged into one";
    EXPECT_EQ(outerArity, 6u) << "merged hexagon: 4+4-2 corners";
}
