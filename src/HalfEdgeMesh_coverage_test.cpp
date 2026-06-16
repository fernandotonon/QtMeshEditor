/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

// ===========================================================================
// HalfEdgeMesh_coverage_test.cpp
//
// Additional, NON-overlapping coverage for HalfEdgeMesh, targeting narrow
// guard-branch / default-argument cases that the large primary suite
// (HalfEdgeMesh_test.cpp, suite name "HalfEdgeMeshStandalone") does not
// exercise:
//
//   - mergeVerticesByDistance(indices) DEFAULT-threshold overload (1e-4f):
//     every existing call passes an explicit threshold, so the default-arg
//     path and the sub-0.1mm clustering branch are untested here.
//   - deleteVertices({}) empty / no-op return.
//   - deleteEdges({}) empty / no-op return.
//   - dissolveVertices({}) empty / no-op return.
//   - dissolveEdges with two edges sharing a vertex — the documented
//     "sequential against live topology, late entries become no-ops" branch.
//   - validate() returning FALSE — the twin-asymmetry / unclosed-loop /
//     prev-next-mismatch / out-of-range failure branches, reached purely
//     in-memory by corrupting m_halfEdges via the public halfEdge(int)&
//     accessor.
//
// All tests are pure-data: EditableMesh is built in-memory with Ogre::Vector
// types only (no Ogre::Root / SceneManager / display). They use a DISTINCT
// suite name ("HalfEdgeMeshCoverageTest") and file-local helpers to avoid any
// ODR clash with the primary suite.
// ===========================================================================

#include <gtest/gtest.h>
#include <vector>
#include <cmath>

#include "HalfEdgeMesh.h"
#include "EditableMesh.h"

namespace {

// ---------------------------------------------------------------------------
// File-local helpers (kept in an anonymous namespace; distinct from the
// primary suite's free functions to avoid duplicate-symbol issues).
// ---------------------------------------------------------------------------

EditableVertex covMkV(float x, float y, float z)
{
    EditableVertex v;
    v.position = Ogre::Vector3(x, y, z);
    v.normal = Ogre::Vector3(0, 0, 1);
    v.hasNormal = true;
    v.uv = Ogre::Vector2(0, 0);
    v.hasUV = true;
    return v;
}

EditableTriangle covMkT(int a, int b, int c)
{
    EditableTriangle t;
    t.indices[0] = a;
    t.indices[1] = b;
    t.indices[2] = c;
    return t;
}

// A single triangle in the XY plane.
EditableMesh covTriangleMesh()
{
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "CovMat";
    sub.vertices = { covMkV(0, 0, 0), covMkV(1, 0, 0), covMkV(0, 1, 0) };
    sub.triangles = { covMkT(0, 1, 2) };
    mesh.subMeshes().push_back(std::move(sub));
    return mesh;
}

// Two triangles sharing the v1->v2 diagonal (a unit quad).
EditableMesh covQuadMesh()
{
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "CovQuad";
    sub.vertices = {
        covMkV(0, 0, 0),  // 0
        covMkV(1, 0, 0),  // 1
        covMkV(0, 1, 0),  // 2
        covMkV(1, 1, 0),  // 3
    };
    sub.triangles = { covMkT(0, 1, 2), covMkT(1, 3, 2) };
    mesh.subMeshes().push_back(std::move(sub));
    return mesh;
}

// 6-spoke hexagonal fan around a central vertex v0. Used to provide an
// interior, manifold vertex whose incident radial edges share v0 — needed
// for the "two edges sharing a vertex" dissolve case.
EditableMesh covHexFan()
{
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "CovHex";
    sub.vertices = {
        covMkV(0.0f, 0.0f, 0),     // 0 center
        covMkV(1.0f, 0.0f, 0),     // 1
        covMkV(0.5f, 0.866f, 0),   // 2
        covMkV(-0.5f, 0.866f, 0),  // 3
        covMkV(-1.0f, 0.0f, 0),    // 4
        covMkV(-0.5f, -0.866f, 0), // 5
        covMkV(0.5f, -0.866f, 0),  // 6
    };
    sub.triangles = {
        covMkT(0, 1, 2), covMkT(0, 2, 3), covMkT(0, 3, 4),
        covMkT(0, 4, 5), covMkT(0, 5, 6), covMkT(0, 6, 1),
    };
    mesh.subMeshes().push_back(std::move(sub));
    return mesh;
}

int covActiveFaceCount(const HalfEdgeMesh& he)
{
    int n = 0;
    for (size_t f = 0; f < he.faceCount(); ++f)
        if (he.face(static_cast<int>(f)).halfEdge >= 0) ++n;
    return n;
}

// Returns HE edge index between vertices a and b (any order), or -1.
int covFindEdge(const HalfEdgeMesh& he, int a, int b)
{
    int lo = std::min(a, b), hi = std::max(a, b);
    for (size_t e = 0; e < he.edgeCount(); ++e) {
        auto [ev1, ev2] = he.edgeVertices(static_cast<int>(e));
        if (std::min(ev1, ev2) == lo && std::max(ev1, ev2) == hi)
            return static_cast<int>(e);
    }
    return -1;
}

// Find any interior (two-face) half-edge index, or -1 if none.
int covFindInteriorHalfEdge(const HalfEdgeMesh& he)
{
    for (size_t i = 0; i < he.halfEdgeCount(); ++i) {
        const HalfEdge& h = he.halfEdge(static_cast<int>(i));
        if (h.face >= 0 && h.twin >= 0) return static_cast<int>(i);
    }
    return -1;
}

// Find any half-edge that belongs to a live face, or -1.
int covFindFaceHalfEdge(const HalfEdgeMesh& he)
{
    for (size_t i = 0; i < he.halfEdgeCount(); ++i)
        if (he.halfEdge(static_cast<int>(i)).face >= 0) return static_cast<int>(i);
    return -1;
}

} // namespace

// ===========================================================================
// mergeVerticesByDistance — DEFAULT-threshold (1e-4f) overload
// ===========================================================================

// The default 1e-4f threshold (~0.1 mm) must fuse a pair separated by a
// sub-0.1mm gap, exercising the default-arg dispatch + clustering branch.
TEST(HalfEdgeMeshCoverageTest, MergeByDistanceDefaultThresholdFusesSubMillimetrePair)
{
    EditableMesh em;
    EditableSubMesh sub;
    sub.materialName = "M";
    // Two triangles whose left corners are 1e-6 apart — well under 1e-4.
    sub.vertices = {
        covMkV(0.0f, 0.0f, 0.0f),     // 0
        covMkV(1e-6f, 0.0f, 0.0f),    // 1  ~coincident with 0
        covMkV(1.0f, 0.0f, 0.0f),     // 2
        covMkV(0.5f, 1.0f, 0.0f),     // 3 apex
    };
    sub.triangles = { covMkT(0, 2, 3), covMkT(1, 3, 2) };
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(covActiveFaceCount(he), 2);

    // NOTE: no threshold argument — exercises the 1e-4f default overload.
    const int retired = he.mergeVerticesByDistance({0, 1, 2, 3});
    EXPECT_EQ(retired, 1) << "the sub-0.1mm pair {0,1} fuses; nothing else does";
    EXPECT_TRUE(he.validate());
}

// With the default threshold, vertices that sit ~1 unit apart (far beyond
// 0.1 mm) must NOT fuse — the negative clustering branch of the default path.
TEST(HalfEdgeMeshCoverageTest, MergeByDistanceDefaultThresholdLeavesSpacedVertsUntouched)
{
    auto em = covQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const auto before = he.vertexCount();

    const int retired = he.mergeVerticesByDistance({0, 1, 2, 3}); // default 1e-4f
    EXPECT_EQ(retired, 0) << "quad verts are 1 unit apart, far above 0.1mm";
    EXPECT_EQ(he.vertexCount(), before);
    EXPECT_TRUE(he.validate());
}

// Default-threshold overload with fewer than two candidates is a hard no-op
// (guards the `vertexIndices.size() < 2` early return without an explicit
// threshold argument).
TEST(HalfEdgeMeshCoverageTest, MergeByDistanceDefaultThresholdLessThanTwoIsNoOp)
{
    auto em = covTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const auto before = he.vertexCount();

    EXPECT_EQ(he.mergeVerticesByDistance({}), 0);
    EXPECT_EQ(he.mergeVerticesByDistance({0}), 0);
    EXPECT_EQ(he.vertexCount(), before);
    EXPECT_TRUE(he.validate());
}

// A three-vertex cluster all within the default threshold collapses to one
// survivor (two retired) using the default overload.
TEST(HalfEdgeMeshCoverageTest, MergeByDistanceDefaultThresholdThreeVertCluster)
{
    EditableMesh em;
    EditableSubMesh sub;
    sub.materialName = "M";
    // Verts 0,1,2 are all within ~2e-6 of each other; vert 3,4 are far.
    sub.vertices = {
        covMkV(0.0f, 0.0f, 0.0f),    // 0
        covMkV(1e-6f, 0.0f, 0.0f),   // 1
        covMkV(0.0f, 1e-6f, 0.0f),   // 2
        covMkV(1.0f, 0.0f, 0.0f),    // 3
        covMkV(0.5f, 1.0f, 0.0f),    // 4
    };
    // Triangles that keep 0,1,2 in distinct faces.
    sub.triangles = { covMkT(0, 3, 4), covMkT(1, 4, 3), covMkT(2, 3, 4) };
    em.subMeshes().push_back(std::move(sub));

    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    const int retired = he.mergeVerticesByDistance({0, 1, 2, 3, 4}); // default
    EXPECT_EQ(retired, 2) << "cluster {0,1,2} collapses to one survivor";
    EXPECT_TRUE(he.validate());
}

// ===========================================================================
// Empty-input no-op guards for the delete / dissolve family that lacked one.
// ===========================================================================

TEST(HalfEdgeMeshCoverageTest, DeleteVerticesEmptyIsNoOp)
{
    auto em = covQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    const auto vBefore = he.vertexCount();

    EXPECT_EQ(he.deleteVertices({}), 0);
    EXPECT_EQ(covActiveFaceCount(he), 2);
    EXPECT_EQ(he.vertexCount(), vBefore);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshCoverageTest, DeleteEdgesEmptyIsNoOp)
{
    auto em = covQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    EXPECT_EQ(he.deleteEdges({}), 0);
    EXPECT_EQ(covActiveFaceCount(he), 2);
    EXPECT_TRUE(he.validate());
}

// deleteEdges with only invalid / already-dropped indices hits the
// `doomedFaces.empty()` early-return branch (distinct from the empty-input
// short-circuit above).
TEST(HalfEdgeMeshCoverageTest, DeleteEdgesAllInvalidIndicesIsNoOp)
{
    auto em = covQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    EXPECT_EQ(he.deleteEdges({-1, 9999, -5}), 0);
    EXPECT_EQ(covActiveFaceCount(he), 2);
    EXPECT_TRUE(he.validate());
}

TEST(HalfEdgeMeshCoverageTest, DissolveVerticesEmptyIsNoOp)
{
    auto em = covQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    EXPECT_EQ(he.dissolveVertices({}), 0);
    EXPECT_EQ(covActiveFaceCount(he), 2);
    EXPECT_TRUE(he.validate());
}

// dissolveVertices with only out-of-range indices is also a no-op (no live
// vertices resolved → nothing dissolved).
TEST(HalfEdgeMeshCoverageTest, DissolveVerticesAllInvalidIndicesIsNoOp)
{
    auto em = covHexFan();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    EXPECT_EQ(he.dissolveVertices({-1, 9999}), 0);
    EXPECT_EQ(covActiveFaceCount(he), 6);
    EXPECT_TRUE(he.validate());
}

// ===========================================================================
// dissolveEdges — two edges sharing a vertex (sequential-against-live-topology
// branch: an earlier dissolve can invalidate a later entry, which then becomes
// a no-op rather than an error).
// ===========================================================================

TEST(HalfEdgeMeshCoverageTest, DissolveEdgesTwoSharingAVertexLateEntryBecomesNoOp)
{
    // The hex fan's interior diagonals are the 6 radial edges, all sharing
    // the center v0. Pick two adjacent radial edges (0-1) and (0-2). Both
    // are interior with two triangle faces, so the FIRST dissolves fine.
    // The two share endpoint v0, so after the first merge the second may no
    // longer reference a valid two-triangle edge — it must degrade to a
    // no-op, never corrupt the structure.
    auto em = covHexFan();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_EQ(covActiveFaceCount(he), 6);

    const int e01 = covFindEdge(he, 0, 1);
    const int e02 = covFindEdge(he, 0, 2);
    ASSERT_GE(e01, 0);
    ASSERT_GE(e02, 0);
    ASSERT_TRUE(he.isEdgeBoundary(e01) == false);
    ASSERT_TRUE(he.isEdgeBoundary(e02) == false);

    const int dissolved = he.dissolveEdges({e01, e02});
    // At least the first interior diagonal dissolves; the documented contract
    // allows the second (sharing v0) to become a no-op. Either way the count
    // is in [1, 2] and the mesh stays consistent.
    EXPECT_GE(dissolved, 1);
    EXPECT_LE(dissolved, 2);
    EXPECT_TRUE(he.validate())
        << "structure must remain valid even when a late entry no-ops";
}

// Same shared-vertex scenario but where one of the two inputs is a boundary
// edge (always skipped). Confirms the count reflects only the interior edge
// and the boundary entry is silently dropped without disturbing validity.
TEST(HalfEdgeMeshCoverageTest, DissolveEdgesInteriorPlusSharedBoundaryEdge)
{
    auto em = covHexFan();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));

    const int interior = covFindEdge(he, 0, 1);   // radial → interior
    const int boundary = covFindEdge(he, 1, 2);   // rim → boundary
    ASSERT_GE(interior, 0);
    ASSERT_GE(boundary, 0);
    ASSERT_TRUE(he.isEdgeBoundary(boundary));

    const int dissolved = he.dissolveEdges({interior, boundary});
    EXPECT_EQ(dissolved, 1) << "only the interior radial edge dissolves";
    EXPECT_TRUE(he.validate());
}

// ===========================================================================
// validate() returning FALSE — corrupt m_halfEdges via the non-const
// halfEdge(int)& accessor, then confirm each failure branch trips.
// ===========================================================================

// Sanity baseline: a freshly built mesh validates true (so the corruption
// tests below are meaningful — they start from a valid state).
TEST(HalfEdgeMeshCoverageTest, ValidateTrueOnFreshlyBuiltMesh)
{
    auto em = covQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    EXPECT_TRUE(he.validate());
}

// Check 1: twin pointing out of range → false.
TEST(HalfEdgeMeshCoverageTest, ValidateFalseOnTwinOutOfRange)
{
    auto em = covQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_TRUE(he.validate());

    const int interior = covFindInteriorHalfEdge(he);
    ASSERT_GE(interior, 0);
    // Point twin at a clearly out-of-range slot.
    he.halfEdge(interior).twin =
        static_cast<int>(he.halfEdgeCount()) + 100;
    EXPECT_FALSE(he.validate());
}

// Check 1: twin asymmetry — A.twin = B but B.twin != A → false.
TEST(HalfEdgeMeshCoverageTest, ValidateFalseOnTwinAsymmetry)
{
    auto em = covQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_TRUE(he.validate());

    const int interior = covFindInteriorHalfEdge(he);
    ASSERT_GE(interior, 0);
    const int twin = he.halfEdge(interior).twin;
    ASSERT_GE(twin, 0);

    // Break symmetry: keep interior.twin == twin, but redirect twin.twin to
    // a different valid half-edge so twin.twin != interior.
    int other = -1;
    for (size_t i = 0; i < he.halfEdgeCount(); ++i) {
        const int idx = static_cast<int>(i);
        if (idx != interior && idx != twin) { other = idx; break; }
    }
    ASSERT_GE(other, 0);
    he.halfEdge(twin).twin = other;
    EXPECT_FALSE(he.validate());
}

// Check 2: face loop never closes — set a face half-edge's next to -1.
TEST(HalfEdgeMeshCoverageTest, ValidateFalseOnBrokenFaceLoopNext)
{
    auto em = covTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_TRUE(he.validate());

    const int hf = covFindFaceHalfEdge(he);
    ASSERT_GE(hf, 0);
    he.halfEdge(hf).next = -1; // loop walk hits next < 0 → false
    EXPECT_FALSE(he.validate());
}

// Check 2: a face half-edge whose `face` field disagrees with the face it is
// reached from → false.
TEST(HalfEdgeMeshCoverageTest, ValidateFalseOnFaceFieldMismatch)
{
    auto em = covTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_TRUE(he.validate());

    const int hf = covFindFaceHalfEdge(he);
    ASSERT_GE(hf, 0);
    const int realFace = he.halfEdge(hf).face;
    ASSERT_GE(realFace, 0);
    // Walk to the next half-edge in the loop and corrupt its face tag so the
    // loop-closure check sees a half-edge whose face != f.
    const int nextHe = he.halfEdge(hf).next;
    ASSERT_GE(nextHe, 0);
    he.halfEdge(nextHe).face = realFace + 12345; // mismatched face id
    EXPECT_FALSE(he.validate());
}

// Check 3: prev/next inconsistency — A.next = B but B.prev != A → false.
TEST(HalfEdgeMeshCoverageTest, ValidateFalseOnPrevNextMismatch)
{
    auto em = covQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_TRUE(he.validate());

    // Find two distinct in-range half-edges A and B where we can set
    // A.next = B without B.prev == A. Pick A=0, and B = some index whose
    // prev is not 0.
    ASSERT_GE(he.halfEdgeCount(), 2u);
    int a = 0;
    int b = -1;
    for (size_t i = 0; i < he.halfEdgeCount(); ++i) {
        const int idx = static_cast<int>(i);
        if (idx == a) continue;
        if (he.halfEdge(idx).prev != a) { b = idx; break; }
    }
    ASSERT_GE(b, 0);
    he.halfEdge(a).next = b; // now A.next == B but B.prev != A
    EXPECT_FALSE(he.validate());
}

// Check 4: vertex's outgoing half-edge does not actually start from that
// vertex (prev->vertex != v) → false.
TEST(HalfEdgeMeshCoverageTest, ValidateFalseOnVertexHalfEdgeMismatch)
{
    auto em = covQuadMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_TRUE(he.validate());

    // Find a live vertex and an in-range half-edge whose prev->vertex is NOT
    // that vertex, then point the vertex at it.
    int targetVert = -1;
    for (size_t v = 0; v < he.vertexCount(); ++v) {
        if (he.vertex(static_cast<int>(v)).halfEdge >= 0) {
            targetVert = static_cast<int>(v);
            break;
        }
    }
    ASSERT_GE(targetVert, 0);

    int badHe = -1;
    for (size_t i = 0; i < he.halfEdgeCount(); ++i) {
        const int idx = static_cast<int>(i);
        const int prev = he.halfEdge(idx).prev;
        if (prev >= 0 && he.halfEdge(prev).vertex != targetVert) {
            badHe = idx;
            break;
        }
    }
    ASSERT_GE(badHe, 0);
    he.vertex(targetVert).halfEdge = badHe;
    EXPECT_FALSE(he.validate());
}

// Check 4: vertex's half-edge index is out of range → false.
TEST(HalfEdgeMeshCoverageTest, ValidateFalseOnVertexHalfEdgeOutOfRange)
{
    auto em = covTriangleMesh();
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(em));
    ASSERT_TRUE(he.validate());

    int targetVert = -1;
    for (size_t v = 0; v < he.vertexCount(); ++v) {
        if (he.vertex(static_cast<int>(v)).halfEdge >= 0) {
            targetVert = static_cast<int>(v);
            break;
        }
    }
    ASSERT_GE(targetVert, 0);
    he.vertex(targetVert).halfEdge =
        static_cast<int>(he.halfEdgeCount()) + 50; // out of range
    EXPECT_FALSE(he.validate());
}
