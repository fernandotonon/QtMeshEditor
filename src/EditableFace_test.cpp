/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

// Pure-data unit tests for the n-gon face helpers in EditableMesh.h:
//   - EditableFace::isValid() / vertexCount()
//   - promoteTrianglesToFaces()
//   - mergeCoplanarTrianglesToQuads() (convexity-rejection branch)
//
// These exercise zero Ogre/GL state (only Ogre::Vector3 value math), so they
// use plain TEST() like the existing EditableMeshStandalone cases — no Ogre
// init, no QApplication, no display.

#include <gtest/gtest.h>
#include "EditableMesh.h"

namespace {

// Mirror of the helper used in EditableMesh_test.cpp: a vertex positioned in
// the XY plane with a +Z normal (the merge logic only reads .position).
EditableVertex faceTestVert(float x, float y, float z) {
    EditableVertex v;
    v.position = Ogre::Vector3(x, y, z);
    v.normal = Ogre::Vector3::UNIT_Z;
    v.hasNormal = true;
    return v;
}

EditableFace faceWith(std::vector<unsigned int> idx) {
    EditableFace f;
    f.indices = std::move(idx);
    return f;
}

} // namespace

// ===========================================================================
// EditableFace::isValid()
// ===========================================================================

TEST(EditableFaceStandalone, IsValidEmptyFaceIsInvalid) {
    EditableFace f; // default-constructed, no indices
    EXPECT_FALSE(f.isValid());
}

TEST(EditableFaceStandalone, IsValidOneIndexIsInvalid) {
    EXPECT_FALSE(faceWith({0}).isValid());
}

TEST(EditableFaceStandalone, IsValidTwoIndicesIsInvalid) {
    // indices.size() < 3 branch.
    EXPECT_FALSE(faceWith({0, 1}).isValid());
}

TEST(EditableFaceStandalone, IsValidCleanTriangleIsValid) {
    EXPECT_TRUE(faceWith({0, 1, 2}).isValid());
}

TEST(EditableFaceStandalone, IsValidCleanQuadIsValid) {
    EXPECT_TRUE(faceWith({0, 1, 2, 3}).isValid());
}

TEST(EditableFaceStandalone, IsValidCleanPentagonIsValid) {
    EXPECT_TRUE(faceWith({0, 1, 2, 3, 4}).isValid());
}

TEST(EditableFaceStandalone, IsValidConsecutiveDuplicateInMiddleIsInvalid) {
    // The (i, i+1) interior duplicate branch: indices[1] == indices[2].
    EXPECT_FALSE(faceWith({0, 1, 1, 3}).isValid());
}

TEST(EditableFaceStandalone, IsValidConsecutiveDuplicateAtStartIsInvalid) {
    // indices[0] == indices[1].
    EXPECT_FALSE(faceWith({5, 5, 6, 7}).isValid());
}

TEST(EditableFaceStandalone, IsValidWrapAroundDuplicateIsInvalid) {
    // The wrap-around pair: indices[N-1] == indices[0] (last vs first).
    EXPECT_FALSE(faceWith({2, 0, 1, 2}).isValid());
}

TEST(EditableFaceStandalone, IsValidTriangleWrapAroundDuplicateIsInvalid) {
    // 3-gon where indices[2] == indices[0].
    EXPECT_FALSE(faceWith({7, 8, 7}).isValid());
}

TEST(EditableFaceStandalone, IsValidNonConsecutiveDuplicateIsStillValid) {
    // The naive check only flags *consecutive* duplicates. A repeated index
    // that is not adjacent (and not the wrap pair) passes. Documents that
    // isValid() is intentionally a sanity check, not full self-intersection.
    EXPECT_TRUE(faceWith({0, 1, 0, 2}).isValid());
}

// ===========================================================================
// EditableFace::vertexCount()
// ===========================================================================

TEST(EditableFaceStandalone, VertexCountEmpty) {
    EditableFace f;
    EXPECT_EQ(f.vertexCount(), 0u);
}

TEST(EditableFaceStandalone, VertexCountTriangle) {
    EXPECT_EQ(faceWith({0, 1, 2}).vertexCount(), 3u);
}

TEST(EditableFaceStandalone, VertexCountQuad) {
    EXPECT_EQ(faceWith({0, 1, 2, 3}).vertexCount(), 4u);
}

TEST(EditableFaceStandalone, VertexCountPentagon) {
    EXPECT_EQ(faceWith({0, 1, 2, 3, 4}).vertexCount(), 5u);
}

TEST(EditableFaceStandalone, VertexCountTracksIndicesSize) {
    // vertexCount() == indices.size() even for "invalid" arities.
    EditableFace f = faceWith({9, 9});
    EXPECT_EQ(f.vertexCount(), f.indices.size());
    EXPECT_EQ(f.vertexCount(), 2u);
    EXPECT_FALSE(f.isValid());
}

// ===========================================================================
// promoteTrianglesToFaces()
// ===========================================================================

namespace {
EditableTriangle mkTri(unsigned a, unsigned b, unsigned c) {
    EditableTriangle t{};
    t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
    return t;
}
} // namespace

TEST(EditableFaceStandalone, PromoteEmptySubmeshYieldsNoFaces) {
    EditableSubMesh sub;
    promoteTrianglesToFaces(sub);
    EXPECT_TRUE(sub.faces.empty());
    EXPECT_TRUE(sub.triangles.empty());
}

TEST(EditableFaceStandalone, PromoteEachTriangleBecomesThreeIndexFace) {
    EditableSubMesh sub;
    sub.triangles = { mkTri(0, 1, 2), mkTri(2, 3, 4), mkTri(5, 6, 7) };

    promoteTrianglesToFaces(sub);

    ASSERT_EQ(sub.faces.size(), sub.triangles.size());
    for (const auto& f : sub.faces) {
        EXPECT_EQ(f.vertexCount(), 3u);
        EXPECT_EQ(f.indices.size(), 3u);
    }
}

TEST(EditableFaceStandalone, PromotePreservesPerTriangleIndicesAndOrder) {
    EditableSubMesh sub;
    sub.triangles = { mkTri(10, 11, 12), mkTri(20, 21, 22) };

    promoteTrianglesToFaces(sub);

    ASSERT_EQ(sub.faces.size(), 2u);
    EXPECT_EQ(sub.faces[0].indices, (std::vector<unsigned int>{10, 11, 12}));
    EXPECT_EQ(sub.faces[1].indices, (std::vector<unsigned int>{20, 21, 22}));
}

TEST(EditableFaceStandalone, PromoteSatisfiesCanonicalFanInvariant) {
    // The header guarantees triangles[i] == fan(faces[i]). For single-triangle
    // faces the fan IS the triangle, so each promoted face's index triple must
    // match the source triangle vertex-for-vertex, and the triangle array is
    // left untouched (no resync needed).
    EditableSubMesh sub;
    sub.triangles = { mkTri(3, 4, 5), mkTri(6, 7, 8), mkTri(0, 2, 1) };
    const auto trianglesBefore = sub.triangles; // value copy

    promoteTrianglesToFaces(sub);

    // triangles untouched
    ASSERT_EQ(sub.triangles.size(), trianglesBefore.size());
    for (size_t i = 0; i < trianglesBefore.size(); ++i) {
        EXPECT_EQ(sub.triangles[i].indices[0], trianglesBefore[i].indices[0]);
        EXPECT_EQ(sub.triangles[i].indices[1], trianglesBefore[i].indices[1]);
        EXPECT_EQ(sub.triangles[i].indices[2], trianglesBefore[i].indices[2]);
    }
    // faces[i] == fan(triangles[i]) (a single triangle for a 3-index face)
    ASSERT_EQ(sub.faces.size(), sub.triangles.size());
    for (size_t i = 0; i < sub.faces.size(); ++i) {
        ASSERT_EQ(sub.faces[i].indices.size(), 3u);
        EXPECT_EQ(sub.faces[i].indices[0], sub.triangles[i].indices[0]);
        EXPECT_EQ(sub.faces[i].indices[1], sub.triangles[i].indices[1]);
        EXPECT_EQ(sub.faces[i].indices[2], sub.triangles[i].indices[2]);
    }
}

TEST(EditableFaceStandalone, PromoteReplacesAnyExistingFaces) {
    // Header: "Existing contents of sub.faces are replaced."
    EditableSubMesh sub;
    sub.triangles = { mkTri(0, 1, 2) };
    sub.faces = { faceWith({100, 101, 102, 103}) }; // stale quad

    promoteTrianglesToFaces(sub);

    ASSERT_EQ(sub.faces.size(), 1u);
    EXPECT_EQ(sub.faces[0].indices, (std::vector<unsigned int>{0, 1, 2}));
}

// ===========================================================================
// mergeCoplanarTrianglesToQuads() — convexity-rejection branch
// ===========================================================================

TEST(EditableFaceStandalone, MergeRejectsCoplanarReflexQuad) {
    // Two coplanar triangles in the XY plane that together form a NON-convex
    // (reflex / arrowhead) quad. They are perfectly coplanar (both wound CCW
    // with +Z normal, dot == 1, well under any angle threshold), so the
    // coplanarity gate passes — but the resulting quad has a reflex vertex at
    // the notch, so buildQuadLoop()'s convexity check must reject the merge.
    //
    // Vertices (z = 0):
    //   0 = (0, 0)  reflex notch (points inward toward the tip)
    //   1 = (2, -1) outer wing
    //   2 = (0, 3)  tip
    //   3 = (-2,-1) outer wing
    // Both triangles share the diagonal edge (2, 0) = tip→notch.
    EditableSubMesh sub;
    sub.vertices = {
        faceTestVert(0.0f,  0.0f, 0.0f),
        faceTestVert(2.0f, -1.0f, 0.0f),
        faceTestVert(0.0f,  3.0f, 0.0f),
        faceTestVert(-2.0f, -1.0f, 0.0f),
    };
    // t1 = (1,2,0) and t2 = (2,3,0): both CCW (+Z normal), sharing edge (2,0).
    sub.triangles = { mkTri(1, 2, 0), mkTri(2, 3, 0) };

    const int merged = mergeCoplanarTrianglesToQuads(sub, 1.0f);

    // No merge: the reflex quad is rejected on convexity grounds.
    EXPECT_EQ(merged, 0);
    // Both triangles survive as separate 3-vertex faces.
    ASSERT_EQ(sub.faces.size(), 2u);
    EXPECT_EQ(sub.faces[0].indices.size(), 3u);
    EXPECT_EQ(sub.faces[1].indices.size(), 3u);
    // Triangulation mirror still mirrors the two unmerged triangles.
    EXPECT_EQ(sub.triangles.size(), 2u);
}

TEST(EditableFaceStandalone, MergeRejectsReflexQuadEvenWithLooseThreshold) {
    // Same reflex quad — confirm the rejection is driven by convexity, not the
    // angle gate, by passing a very loose threshold (the pair is exactly
    // coplanar so the angle gate would never block it).
    EditableSubMesh sub;
    sub.vertices = {
        faceTestVert(0.0f,  0.0f, 0.0f),
        faceTestVert(2.0f, -1.0f, 0.0f),
        faceTestVert(0.0f,  3.0f, 0.0f),
        faceTestVert(-2.0f, -1.0f, 0.0f),
    };
    sub.triangles = { mkTri(1, 2, 0), mkTri(2, 3, 0) };

    EXPECT_EQ(mergeCoplanarTrianglesToQuads(sub, 45.0f), 0);
    EXPECT_EQ(sub.faces.size(), 2u);
}

TEST(EditableFaceStandalone, MergeAcceptsConvexQuadSanityCheck) {
    // Control case proving the test rig's winding is correct: a convex unit
    // quad DOES merge. Mirrors the existing MergeCoplanarTrianglesProducesQuad
    // case so the reflex rejection above is meaningful (not a setup artefact).
    EditableSubMesh sub;
    sub.vertices = {
        faceTestVert(0, 0, 0), faceTestVert(1, 0, 0),
        faceTestVert(1, 1, 0), faceTestVert(0, 1, 0),
    };
    sub.triangles = { mkTri(0, 1, 2), mkTri(0, 2, 3) };

    const int merged = mergeCoplanarTrianglesToQuads(sub, 1.0f);
    EXPECT_EQ(merged, 1);
    ASSERT_EQ(sub.faces.size(), 1u);
    EXPECT_EQ(sub.faces[0].indices.size(), 4u);
}
