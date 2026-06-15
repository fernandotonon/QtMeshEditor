/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

// Additional coverage for EditableMesh's flat-normals state pair, the
// numeric output of recalculateNormalsFlat(), and the default-epsilon
// path of countDegenerateTriangles(). These are pure in-memory / vector
// math operations that need NO Ogre Root, hardware buffers, or display —
// the EditableSubMesh / EditableMesh data structures are populated by hand.
//
// Distinct filename + distinct suite name (EditableMeshCoverageTest) from
// the existing EditableMesh_test.cpp (EditableMeshStandalone / EditableMeshTest)
// to avoid any ODR / duplicate-registration clash.

#include <gtest/gtest.h>

#include <cmath>

#include "EditableMesh.h"

namespace {

// Build a single-submesh mesh whose triangle lies in the XY plane with a
// +Z face normal: (0,0,0), (1,0,0), (0,1,0).
EditableVertex mkVtx(float x, float y, float z) {
    EditableVertex v;
    v.position = Ogre::Vector3(x, y, z);
    v.normal = Ogre::Vector3::ZERO;
    v.hasNormal = false;
    return v;
}

EditableTriangle mkTri(unsigned int a, unsigned int b, unsigned int c) {
    EditableTriangle t;
    t.indices[0] = a;
    t.indices[1] = b;
    t.indices[2] = c;
    return t;
}

} // namespace

// ===========================================================================
// setFlatNormals(bool) / isFlatNormals() — getter/setter state pair
// ===========================================================================

TEST(EditableMeshCoverageTest, FlatNormalsDefaultsToFalse) {
    EditableMesh mesh;
    EXPECT_FALSE(mesh.isFlatNormals());
}

TEST(EditableMeshCoverageTest, FlatNormalsSetTrueReadsBackTrue) {
    EditableMesh mesh;
    mesh.setFlatNormals(true);
    EXPECT_TRUE(mesh.isFlatNormals());
}

TEST(EditableMeshCoverageTest, FlatNormalsToggleBackToFalse) {
    EditableMesh mesh;
    mesh.setFlatNormals(true);
    ASSERT_TRUE(mesh.isFlatNormals());
    mesh.setFlatNormals(false);
    EXPECT_FALSE(mesh.isFlatNormals());
}

TEST(EditableMeshCoverageTest, FlatNormalsIdempotentSet) {
    // Setting the same value repeatedly must not flip state.
    EditableMesh mesh;
    mesh.setFlatNormals(true);
    mesh.setFlatNormals(true);
    EXPECT_TRUE(mesh.isFlatNormals());

    mesh.setFlatNormals(false);
    mesh.setFlatNormals(false);
    EXPECT_FALSE(mesh.isFlatNormals());
}

TEST(EditableMeshCoverageTest, FlatNormalsStateIndependentOfNormalRecalc) {
    // The flat-normals flag is pure state used by commit; calling the
    // recalc routines must not mutate the flag.
    EditableMesh mesh;
    mesh.setFlatNormals(true);
    mesh.recalculateNormals();
    EXPECT_TRUE(mesh.isFlatNormals());
    mesh.recalculateNormalsFlat();
    EXPECT_TRUE(mesh.isFlatNormals());
}

// ===========================================================================
// recalculateNormalsFlat() — per-vertex numeric output on a populated mesh
// ===========================================================================

TEST(EditableMeshCoverageTest, RecalculateNormalsFlatSingleTriangleZNormal) {
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.vertices = { mkVtx(0, 0, 0), mkVtx(1, 0, 0), mkVtx(0, 1, 0) };
    sub.triangles = { mkTri(0, 1, 2) };
    mesh.subMeshes().push_back(std::move(sub));

    mesh.recalculateNormalsFlat();

    // Face normal of (1,0,0)x(0,1,0) = (0,0,1), normalized.
    const auto& verts = mesh.subMeshes()[0].vertices;
    for (const auto& v : verts) {
        EXPECT_TRUE(v.hasNormal);
        EXPECT_NEAR(v.normal.x, 0.0f, 1e-5f);
        EXPECT_NEAR(v.normal.y, 0.0f, 1e-5f);
        EXPECT_NEAR(v.normal.z, 1.0f, 1e-5f);
    }
}

TEST(EditableMeshCoverageTest, RecalculateNormalsFlatNormalsAreUnitLength) {
    EditableMesh mesh;
    EditableSubMesh sub;
    // A tilted triangle so the normal is not axis-aligned.
    sub.vertices = { mkVtx(0, 0, 0), mkVtx(2, 0, 0), mkVtx(0, 2, 2) };
    sub.triangles = { mkTri(0, 1, 2) };
    mesh.subMeshes().push_back(std::move(sub));

    mesh.recalculateNormalsFlat();

    for (const auto& v : mesh.subMeshes()[0].vertices) {
        EXPECT_NEAR(v.normal.length(), 1.0f, 1e-5f);
    }
}

TEST(EditableMeshCoverageTest, RecalculateNormalsFlatLastTriangleWinsForSharedVertex) {
    // Per the header: shared vertices get the normal of the LAST triangle
    // processed (no averaging). Build two triangles sharing vertex 0, with
    // opposing face normals; the shared vertex should end up with the
    // SECOND triangle's normal, not an average of the two.
    EditableMesh mesh;
    EditableSubMesh sub;
    // Triangle A in XY plane (normal +Z): v0,v1,v2
    // Triangle B in XZ plane (normal -Y or +Y depending on winding): v0,v3,v4
    sub.vertices = {
        mkVtx(0, 0, 0),   // 0 shared
        mkVtx(1, 0, 0),   // 1
        mkVtx(0, 1, 0),   // 2
        mkVtx(1, 0, 0),   // 3
        mkVtx(0, 0, 1),   // 4
    };
    // A: (0,1,2) -> normal +Z
    // B: (0,3,4) -> (1,0,0)x(0,0,1) = (0*1-0*0, 0*0-1*1, 1*0-0*0) = (0,-1,0)
    sub.triangles = { mkTri(0, 1, 2), mkTri(0, 3, 4) };
    mesh.subMeshes().push_back(std::move(sub));

    mesh.recalculateNormalsFlat();

    const auto& verts = mesh.subMeshes()[0].vertices;
    // Shared vertex 0 should have triangle B's normal (last wins): (0,-1,0).
    EXPECT_NEAR(verts[0].normal.x, 0.0f, 1e-5f);
    EXPECT_NEAR(verts[0].normal.y, -1.0f, 1e-5f);
    EXPECT_NEAR(verts[0].normal.z, 0.0f, 1e-5f);

    // Vertices unique to triangle A keep +Z.
    EXPECT_NEAR(verts[2].normal.z, 1.0f, 1e-5f);
    // Vertices unique to triangle B keep (0,-1,0).
    EXPECT_NEAR(verts[4].normal.y, -1.0f, 1e-5f);
}

TEST(EditableMeshCoverageTest, RecalculateNormalsFlatTriangulatesFacesFirst) {
    // When faces (n-gon) is canonical and triangles is stale/empty,
    // recalculateNormalsFlat must triangulate the faces first, then assign
    // the face normal per vertex. Quad in the XY plane -> +Z normals.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.vertices = {
        mkVtx(0, 0, 0), mkVtx(1, 0, 0), mkVtx(1, 1, 0), mkVtx(0, 1, 0),
    };
    EditableFace f;
    f.indices = {0, 1, 2, 3};
    sub.faces.push_back(std::move(f));
    // triangles intentionally empty.
    mesh.subMeshes().push_back(std::move(sub));

    mesh.recalculateNormalsFlat();

    // triangles must have been synced from the quad (2 fan tris).
    EXPECT_EQ(mesh.subMeshes()[0].triangles.size(), 2u);
    for (const auto& v : mesh.subMeshes()[0].vertices) {
        EXPECT_TRUE(v.hasNormal);
        EXPECT_NEAR(v.normal.z, 1.0f, 1e-5f);
        EXPECT_NEAR(v.normal.x, 0.0f, 1e-5f);
        EXPECT_NEAR(v.normal.y, 0.0f, 1e-5f);
    }
}

TEST(EditableMeshCoverageTest, RecalculateNormalsFlatSkipsOutOfRangeTriangle) {
    // A triangle whose indices exceed the vertex count must be skipped
    // without crashing; the valid triangle's verts still get a normal.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.vertices = { mkVtx(0, 0, 0), mkVtx(1, 0, 0), mkVtx(0, 1, 0) };
    sub.triangles = { mkTri(0, 1, 2), mkTri(0, 1, 99) /* OOB */ };
    mesh.subMeshes().push_back(std::move(sub));

    mesh.recalculateNormalsFlat();

    for (const auto& v : mesh.subMeshes()[0].vertices) {
        EXPECT_NEAR(v.normal.z, 1.0f, 1e-5f);
    }
}

TEST(EditableMeshCoverageTest, RecalculateNormalsFlatDegenerateTriangleLeavesZeroNormal) {
    // A degenerate (zero-area) triangle's cross product has length ~0, so
    // the < 1e-8 guard skips the divide and the assigned normal stays ZERO.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.vertices = { mkVtx(0, 0, 0), mkVtx(1, 0, 0), mkVtx(2, 0, 0) };
    sub.triangles = { mkTri(0, 1, 2) }; // collinear -> zero area
    mesh.subMeshes().push_back(std::move(sub));

    mesh.recalculateNormalsFlat();

    for (const auto& v : mesh.subMeshes()[0].vertices) {
        EXPECT_TRUE(v.hasNormal); // flag is always set
        EXPECT_NEAR(v.normal.length(), 0.0f, 1e-6f);
    }
}

TEST(EditableMeshCoverageTest, RecalculateNormalsFlatDiffersFromSmoothAtSharedVertex) {
    // Contrast smooth vs flat: a vertex shared by two non-coplanar tris
    // gets an averaged normal under smooth, but the last triangle's normal
    // under flat. This asserts the flat variant's distinct output.
    auto buildMesh = [](EditableMesh& m) {
        EditableSubMesh sub;
        sub.vertices = {
            mkVtx(0, 0, 0),   // 0 shared
            mkVtx(1, 0, 0),   // 1
            mkVtx(0, 1, 0),   // 2  (tri A: +Z)
            mkVtx(0, 0, 1),   // 3  (tri B)
        };
        sub.triangles = { mkTri(0, 1, 2), mkTri(0, 1, 3) };
        m.subMeshes().push_back(std::move(sub));
    };

    EditableMesh smoothMesh, flatMesh;
    buildMesh(smoothMesh);
    buildMesh(flatMesh);

    smoothMesh.recalculateNormals();
    flatMesh.recalculateNormalsFlat();

    const Ogre::Vector3 nSmooth = smoothMesh.subMeshes()[0].vertices[0].normal;
    const Ogre::Vector3 nFlat = flatMesh.subMeshes()[0].vertices[0].normal;

    // Flat = last triangle's face normal of (0,1,3):
    // (1,0,0)x(0,0,1) = (0,-1,0).
    EXPECT_NEAR(nFlat.x, 0.0f, 1e-5f);
    EXPECT_NEAR(nFlat.y, -1.0f, 1e-5f);
    EXPECT_NEAR(nFlat.z, 0.0f, 1e-5f);

    // Smooth must NOT equal the flat (last-wins) result at the shared vtx.
    const float diff = (nSmooth - nFlat).length();
    EXPECT_GT(diff, 1e-3f);
}

// ===========================================================================
// countDegenerateTriangles() — default-epsilon (1e-6f) overload boundary
// ===========================================================================

TEST(EditableMeshCoverageTest, CountDegenerateDefaultEpsilonValidTriangleIsZero) {
    // A clearly non-degenerate triangle: cross length = 1 >> 1e-6.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.vertices = { mkVtx(0, 0, 0), mkVtx(1, 0, 0), mkVtx(0, 1, 0) };
    sub.triangles = { mkTri(0, 1, 2) };
    mesh.subMeshes().push_back(std::move(sub));

    // Default-arg path (no epsilon passed).
    EXPECT_EQ(mesh.countDegenerateTriangles(), 0);
}

TEST(EditableMeshCoverageTest, CountDegenerateDefaultEpsilonZeroAreaIsDegenerate) {
    EditableMesh mesh;
    EditableSubMesh sub;
    // Collinear -> cross length exactly 0 < 1e-6.
    sub.vertices = { mkVtx(0, 0, 0), mkVtx(1, 0, 0), mkVtx(2, 0, 0) };
    sub.triangles = { mkTri(0, 1, 2) };
    mesh.subMeshes().push_back(std::move(sub));

    EXPECT_EQ(mesh.countDegenerateTriangles(), 1);
}

TEST(EditableMeshCoverageTest, CountDegenerateDefaultEpsilonJustBelowBoundary) {
    // Construct a triangle whose cross-product LENGTH (the quantity the
    // impl compares, NOT 0.5*length) is just below 1e-6 so the default
    // epsilon classifies it as degenerate.
    //
    // cross length for base b along X and height h along Y of a triangle
    // (0,0,0),(b,0,0),(0,h,0) = |(b,0,0) x (0,h,0)| = b*h.
    // Pick b = 1, h = 5e-7 -> cross length = 5e-7 < 1e-6 -> degenerate.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.vertices = { mkVtx(0, 0, 0), mkVtx(1.0f, 0, 0), mkVtx(0, 5e-7f, 0) };
    sub.triangles = { mkTri(0, 1, 2) };
    mesh.subMeshes().push_back(std::move(sub));

    // cross length ~5e-7 < 1e-6 default epsilon.
    EXPECT_EQ(mesh.countDegenerateTriangles(), 1);
}

TEST(EditableMeshCoverageTest, CountDegenerateDefaultEpsilonJustAboveBoundary) {
    // cross length = b*h = 1 * 2e-6 = 2e-6 > 1e-6 default epsilon -> NOT
    // degenerate under the default, even though it is a razor-thin sliver.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.vertices = { mkVtx(0, 0, 0), mkVtx(1.0f, 0, 0), mkVtx(0, 2e-6f, 0) };
    sub.triangles = { mkTri(0, 1, 2) };
    mesh.subMeshes().push_back(std::move(sub));

    EXPECT_EQ(mesh.countDegenerateTriangles(), 0);

    // But with a larger explicit epsilon it IS classified as degenerate,
    // confirming the boundary is epsilon-driven.
    EXPECT_EQ(mesh.countDegenerateTriangles(1e-5f), 1);
}

TEST(EditableMeshCoverageTest, CountDegenerateDefaultEpsilonMixedTriangles) {
    // One valid, one near-but-not-degenerate (above default eps), one
    // genuinely degenerate (below default eps). Default-arg call should
    // count exactly the one that falls under 1e-6.
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.vertices = {
        mkVtx(0, 0, 0),       // 0
        mkVtx(1, 0, 0),       // 1
        mkVtx(0, 1, 0),       // 2  valid big triangle
        mkVtx(0, 2e-6f, 0),   // 3  -> with (0,1) gives cross 2e-6 (above eps)
        mkVtx(0, 5e-7f, 0),   // 4  -> with (0,1) gives cross 5e-7 (below eps)
    };
    sub.triangles = {
        mkTri(0, 1, 2), // area 1, valid
        mkTri(0, 1, 3), // cross 2e-6, NOT degenerate under default
        mkTri(0, 1, 4), // cross 5e-7, degenerate under default
    };
    mesh.subMeshes().push_back(std::move(sub));

    EXPECT_EQ(mesh.countDegenerateTriangles(), 1);
}

TEST(EditableMeshCoverageTest, CountDegenerateDefaultEpsilonSkipsOutOfRangeIndices) {
    // Out-of-range triangles are skipped (continue) — they are not counted
    // as degenerate by countDegenerateTriangles().
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.vertices = { mkVtx(0, 0, 0), mkVtx(1, 0, 0), mkVtx(0, 1, 0) };
    sub.triangles = { mkTri(0, 1, 2), mkTri(0, 1, 42) /* OOB */ };
    mesh.subMeshes().push_back(std::move(sub));

    EXPECT_EQ(mesh.countDegenerateTriangles(), 0);
}
