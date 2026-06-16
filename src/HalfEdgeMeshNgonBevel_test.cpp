/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

// ===========================================================================
// Focused branch coverage for HalfEdgeMesh's n-gon-aware bevel overloads:
//   - bevelVerticesNgon(vertexIndices, width, ...)
//   - bevelEdgesNgon(edgeIndices, width, ...)
//
// These are PURE-DATA operations: an EditableMesh is built entirely in
// memory, fed through buildFromEditableMesh, mutated, then asserted via
// validate(), faceCount(), and faceVertices() arity. No Ogre Root /
// SceneManager / Material / RenderWindow and no display are required, so
// every test below runs unconditionally (no GTEST_SKIP needed).
//
// The happy-path quad-corner case is already covered in HalfEdgeMesh_test.cpp
// (BevelVerticesNgonOnQuadCornerKeepsQuads). This file targets the UNTESTED
// rejection / no-op / sequential branches plus the reserved-arg contract for
// bevelEdgesNgon's profile/profilePoints.
// ===========================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "HalfEdgeMesh.h"
#include "EditableMesh.h"

namespace {

// ---------------------------------------------------------------------------
// Local fixtures + helpers (file-local; the ones in HalfEdgeMesh_test.cpp are
// static to that translation unit and not visible here).
// ---------------------------------------------------------------------------

EditableVertex mkVertex(float x, float y, float z = 0.0f)
{
    EditableVertex v;
    v.position = Ogre::Vector3(x, y, z);
    v.normal = Ogre::Vector3(0, 0, 1);
    v.hasNormal = true;
    return v;
}

// A single triangle (the smallest closed surface). Every vertex is on the
// boundary and has valence 2, so it exercises both the boundary-vertex skip
// AND the valence<3 skip in bevelVerticesNgon, and has no interior edge for
// bevelEdgesNgon.
EditableMesh makeTriangle()
{
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "Tri";
    sub.vertices = { mkVertex(0, 0), mkVertex(1, 0), mkVertex(0, 1) };
    EditableTriangle t;
    t.indices[0] = 0; t.indices[1] = 1; t.indices[2] = 2;
    sub.triangles = { t };
    mesh.subMeshes().push_back(std::move(sub));
    return mesh;
}

// Four quads in a "+" arrangement around the central vertex v4 (valence 4).
//
//    v6 - v7 - v8
//    |    |    |
//    v3 - v4 - v5
//    |    |    |
//    v0 - v1 - v2
//
// Quads: qA(0,1,4,3) qB(1,2,5,4) qC(3,4,7,6) qD(4,5,8,7).
//
// v4 is interior (valence 4); v1/v3/v5/v7 are interior of valence 3; the
// 8 perimeter corners/edges are boundary. This mirrors the fixture used by
// the existing BevelVerticesNgonOnQuadCornerKeepsQuads / chained-selection
// tests so the branch outcomes line up with the documented behavior.
EditableMesh makePlusOfQuads()
{
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "Plus";
    sub.vertices = {
        mkVertex(0, 0), mkVertex(1, 0), mkVertex(2, 0),
        mkVertex(0, 1), mkVertex(1, 1), mkVertex(2, 1),
        mkVertex(0, 2), mkVertex(1, 2), mkVertex(2, 2),
    };
    EditableFace qA, qB, qC, qD;
    qA.indices = {0, 1, 4, 3};
    qB.indices = {1, 2, 5, 4};
    qC.indices = {3, 4, 7, 6};
    qD.indices = {4, 5, 8, 7};
    sub.faces = { qA, qB, qC, qD };
    triangulateFaces(sub);
    mesh.subMeshes().push_back(std::move(sub));
    return mesh;
}

// A quad cube: 8 verts, 6 quad faces, fully closed (every edge interior).
// Used for the edge-bevel reserved-arg + shared-endpoint tests.
EditableMesh makeQuadCube()
{
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.materialName = "Cube";
    auto v = [](float x, float y, float z) { return mkVertex(x, y, z); };
    sub.vertices = {
        v(-1,-1,-1), v(1,-1,-1), v(-1,1,-1), v(1,1,-1), // 0..3
        v(-1,-1, 1), v(1,-1, 1), v(-1,1, 1), v(1,1, 1), // 4..7
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
    mesh.subMeshes().push_back(std::move(sub));
    return mesh;
}

// HE edge index joining v1/v2 (any order), or -1 if not found.
int findEdge(const HalfEdgeMesh& he, int v1, int v2)
{
    const int a = std::min(v1, v2);
    const int b = std::max(v1, v2);
    for (size_t e = 0; e < he.edgeCount(); ++e) {
        auto [ev1, ev2] = he.edgeVertices(static_cast<int>(e));
        const int ea = std::min(ev1, ev2);
        const int eb = std::max(ev1, ev2);
        if (ea == a && eb == b) return static_cast<int>(e);
    }
    return -1;
}

// True when every undirected triangle edge of the output mesh is shared by
// exactly 1 or 2 triangles and no shared edge has the same winding twice.
bool isManifold(const EditableMesh& em)
{
    std::map<std::pair<unsigned, unsigned>, int> edgeUse;
    std::map<std::pair<unsigned, unsigned>, int> directed;
    for (const auto& sub : em.subMeshes()) {
        for (const auto& t : sub.triangles) {
            for (int k = 0; k < 3; ++k) {
                const unsigned a = t.indices[k];
                const unsigned b = t.indices[(k + 1) % 3];
                if (a == b) return false; // degenerate
                ++edgeUse[{std::min(a, b), std::max(a, b)}];
                ++directed[{a, b}];
            }
        }
    }
    for (const auto& [key, count] : edgeUse) {
        (void)key;
        if (count < 1 || count > 2) return false;
    }
    for (const auto& [key, count] : directed) {
        (void)key;
        if (count > 1) return false;
    }
    return true;
}

// Count active (non-retired) faces in the HE structure.
int activeFaceCount(const HalfEdgeMesh& he)
{
    int active = 0;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge >= 0) ++active;
    }
    return active;
}

// Snapshot the multiset of active-face arities (e.g. {3,3,4,4,...}).
// Used to assert two bevels produced geometrically identical topology.
std::vector<int> activeFaceArities(const HalfEdgeMesh& he)
{
    std::vector<int> arities;
    for (size_t f = 0; f < he.faceCount(); ++f) {
        if (he.face(static_cast<int>(f)).halfEdge < 0) continue;
        arities.push_back(static_cast<int>(
            he.faceVertices(static_cast<int>(f)).size()));
    }
    std::sort(arities.begin(), arities.end());
    return arities;
}

} // namespace

// ===========================================================================
// bevelVerticesNgon — rejection / no-op branches
// ===========================================================================

// Empty selection: early `vertexIndices.empty()` guard returns empty, mesh
// untouched.
TEST(HalfEdgeMeshNgonVertexBevel, EmptySelectionIsNoOp) {
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(makePlusOfQuads()));
    const size_t before = he.faceCount();
    const int activeBefore = activeFaceCount(he);

    const auto out = he.bevelVerticesNgon({}, 0.1f);

    EXPECT_TRUE(out.empty());
    EXPECT_EQ(he.faceCount(), before);
    EXPECT_EQ(activeFaceCount(he), activeBefore);
    EXPECT_TRUE(he.validate());
}

// width <= 0 hits the same early guard as the empty case — no-op for both
// the zero and negative cases.
TEST(HalfEdgeMeshNgonVertexBevel, NonPositiveWidthIsNoOp) {
    for (float w : { 0.0f, -0.5f }) {
        HalfEdgeMesh he;
        ASSERT_TRUE(he.buildFromEditableMesh(makePlusOfQuads()));
        const int activeBefore = activeFaceCount(he);

        const auto out = he.bevelVerticesNgon({4}, w);

        EXPECT_TRUE(out.empty()) << "width=" << w;
        EXPECT_EQ(activeFaceCount(he), activeBefore) << "width=" << w;
        EXPECT_TRUE(he.validate()) << "width=" << w;
    }
}

// A boundary vertex is skipped (isVertexBoundary branch). On the "+" fixture
// v0 is a corner (boundary). Result: empty, no new faces.
TEST(HalfEdgeMeshNgonVertexBevel, BoundaryVertexIsSkipped) {
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(makePlusOfQuads()));
    ASSERT_TRUE(he.isVertexBoundary(0));
    const int activeBefore = activeFaceCount(he);

    const auto out = he.bevelVerticesNgon({0}, 0.1f);

    EXPECT_TRUE(out.empty());
    EXPECT_EQ(activeFaceCount(he), activeBefore);
    EXPECT_TRUE(he.validate());
}

// A valence<3 vertex is skipped (`incident.size() < 3` branch). Every vertex
// of a lone triangle is both a boundary vertex and valence 2 — beveling any
// of them is a no-op.
TEST(HalfEdgeMeshNgonVertexBevel, LowValenceVertexIsSkipped) {
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(makeTriangle()));
    ASSERT_EQ(he.faceCount(), 1u);

    const auto out = he.bevelVerticesNgon({0, 1, 2}, 0.25f);

    EXPECT_TRUE(out.empty());
    EXPECT_EQ(activeFaceCount(he), 1) << "the triangle is untouched";
    EXPECT_TRUE(he.validate());
}

// Out-of-range / retired vertex indices are silently ignored, not crashes,
// and produce no new geometry.
TEST(HalfEdgeMeshNgonVertexBevel, InvalidIndicesAreIgnored) {
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(makePlusOfQuads()));
    const int activeBefore = activeFaceCount(he);

    const auto out =
        he.bevelVerticesNgon({-1, 99999, static_cast<int>(he.vertexCount())},
                             0.1f);

    EXPECT_TRUE(out.empty());
    EXPECT_EQ(activeFaceCount(he), activeBefore);
    EXPECT_TRUE(he.validate());
}

// ===========================================================================
// bevelVerticesNgon — success + sequential processing
// ===========================================================================

// Single interior vertex of valence 4 produces one inner vertex per incident
// face (4) and keeps the result valid + manifold. Mirrors the existing happy
// path but also asserts manifoldness of the round-tripped triangulation.
TEST(HalfEdgeMeshNgonVertexBevel, InteriorVertexProducesInnerPerFace) {
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(makePlusOfQuads()));
    ASSERT_FALSE(he.isVertexBoundary(4));

    const auto out = he.bevelVerticesNgon({4}, 0.1f);

    EXPECT_EQ(out.size(), 4u) << "one inner vertex per incident face";
    EXPECT_TRUE(he.validate());
    // 4 modified quads + 1 cap quad = 5 active faces, all quads.
    EXPECT_EQ(activeFaceCount(he), 5);

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
}

// A mixed multi-vertex selection where only ONE vertex is bevel-eligible
// (v4 interior; v0 boundary; v1 is interior but only valence 3 with one of
// its faces — still eligible, so we deliberately pick boundary v0 + interior
// v4). The eligible vertex is processed; the ineligible one is skipped; the
// sequential loop keeps validate()==true afterwards.
TEST(HalfEdgeMeshNgonVertexBevel, MixedSelectionProcessesEligibleSequentially) {
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(makePlusOfQuads()));

    // v0 boundary (skipped), v4 interior valence-4 (beveled).
    const auto out = he.bevelVerticesNgon({0, 4}, 0.1f);

    EXPECT_EQ(out.size(), 4u)
        << "only v4 contributes inner vertices; v0 is skipped";
    EXPECT_TRUE(he.validate());
    EXPECT_EQ(activeFaceCount(he), 5);

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
}

// Duplicate indices in the selection are de-duplicated (the `processed`
// set guard): beveling {4,4,4} behaves exactly like {4}.
TEST(HalfEdgeMeshNgonVertexBevel, DuplicateIndicesProcessedOnce) {
    HalfEdgeMesh heSingle;
    ASSERT_TRUE(heSingle.buildFromEditableMesh(makePlusOfQuads()));
    const auto single = heSingle.bevelVerticesNgon({4}, 0.1f);

    HalfEdgeMesh heDup;
    ASSERT_TRUE(heDup.buildFromEditableMesh(makePlusOfQuads()));
    const auto dup = heDup.bevelVerticesNgon({4, 4, 4}, 0.1f);

    EXPECT_EQ(dup.size(), single.size());
    EXPECT_EQ(activeFaceCount(heDup), activeFaceCount(heSingle));
    EXPECT_TRUE(heDup.validate());
}

// ===========================================================================
// bevelEdgesNgon — empty / rejection branches
// ===========================================================================

// Empty edge selection returns empty (early guard), mesh untouched.
TEST(HalfEdgeMeshNgonEdgeBevel, EmptySelectionIsNoOp) {
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(makeQuadCube()));
    const int activeBefore = activeFaceCount(he);

    const auto out = he.bevelEdgesNgon({}, 0.1f);

    EXPECT_TRUE(out.empty());
    EXPECT_EQ(activeFaceCount(he), activeBefore);
    EXPECT_TRUE(he.validate());
}

// width <= 0 returns empty (early guard) for both zero and negative.
TEST(HalfEdgeMeshNgonEdgeBevel, NonPositiveWidthIsNoOp) {
    for (float w : { 0.0f, -1.0f }) {
        HalfEdgeMesh he;
        ASSERT_TRUE(he.buildFromEditableMesh(makeQuadCube()));
        const int edge = findEdge(he, 2, 3);
        ASSERT_GE(edge, 0);
        const int activeBefore = activeFaceCount(he);

        const auto out = he.bevelEdgesNgon({edge}, w);

        EXPECT_TRUE(out.empty()) << "width=" << w;
        EXPECT_EQ(activeFaceCount(he), activeBefore) << "width=" << w;
        EXPECT_TRUE(he.validate()) << "width=" << w;
    }
}

// All-invalid edge indices collapse to no info gathered → empty result.
TEST(HalfEdgeMeshNgonEdgeBevel, InvalidEdgeIndicesAreIgnored) {
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(makeQuadCube()));
    const int activeBefore = activeFaceCount(he);

    const auto out =
        he.bevelEdgesNgon({-1, 999999, static_cast<int>(he.edgeCount())},
                          0.1f);

    EXPECT_TRUE(out.empty());
    EXPECT_EQ(activeFaceCount(he), activeBefore);
    EXPECT_TRUE(he.validate());
}

// Two edges sharing an endpoint are both rejected (the chained-selection
// vertexUseCount filter leaves `clean` empty → early empty return). On the
// closed cube, edges (2,3) and (3,7) share v3 and are both interior.
TEST(HalfEdgeMeshNgonEdgeBevel, SharedEndpointSelectionIsRejected) {
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(makeQuadCube()));
    const int e1 = findEdge(he, 2, 3);
    const int e2 = findEdge(he, 3, 7);
    ASSERT_GE(e1, 0);
    ASSERT_GE(e2, 0);
    ASSERT_NE(e1, e2);
    const int activeBefore = activeFaceCount(he);

    const auto out = he.bevelEdgesNgon({e1, e2}, 0.1f);

    EXPECT_TRUE(out.empty())
        << "edges sharing v3 must both be rejected by the MVP";
    EXPECT_EQ(activeFaceCount(he), activeBefore);
    EXPECT_TRUE(he.validate());
}

// ===========================================================================
// bevelEdgesNgon — reserved-arg contract (profile / profilePoints no-op)
// ===========================================================================

// The MVP documents `profile` and `profilePoints` as reserved no-ops: a flat
// single-segment chamfer is produced regardless. Lock that contract by
// beveling the SAME edge twice — once with defaults, once with extreme
// non-default profile values — and asserting identical topology + manifold
// output both times.
TEST(HalfEdgeMeshNgonEdgeBevel, ReservedProfileArgsDoNotChangeFlatChamfer) {
    // Default profile (0.5, no profile points).
    HalfEdgeMesh heDefault;
    ASSERT_TRUE(heDefault.buildFromEditableMesh(makeQuadCube()));
    const int edgeD = findEdge(heDefault, 2, 3);
    ASSERT_GE(edgeD, 0);
    const auto outDefault =
        heDefault.bevelEdgesNgon({edgeD}, 0.1f /*segments*/, 1,
                                 /*profile*/ 0.5f, /*profilePoints*/ {});
    EXPECT_EQ(outDefault.size(), 4u);
    EXPECT_TRUE(heDefault.validate());

    // Extreme non-default profile + explicit profile points. The single-
    // segment MVP ignores both: same topology, same manifold result.
    HalfEdgeMesh heProfiled;
    ASSERT_TRUE(heProfiled.buildFromEditableMesh(makeQuadCube()));
    const int edgeP = findEdge(heProfiled, 2, 3);
    ASSERT_GE(edgeP, 0);
    const auto outProfiled =
        heProfiled.bevelEdgesNgon({edgeP}, 0.1f, /*segments*/ 1,
                                  /*profile*/ 1.0f,
                                  /*profilePoints*/ {0.0f, 0.9f, 0.3f});
    EXPECT_EQ(outProfiled.size(), 4u);
    EXPECT_TRUE(heProfiled.validate());

    // Same new-vertex count and same active-face arity multiset → the
    // reserved args produced an identical flat chamfer.
    EXPECT_EQ(outProfiled.size(), outDefault.size());
    EXPECT_EQ(activeFaceCount(heProfiled), activeFaceCount(heDefault));
    EXPECT_EQ(activeFaceArities(heProfiled), activeFaceArities(heDefault));

    EditableMesh backDefault, backProfiled;
    ASSERT_TRUE(heDefault.toEditableMesh(backDefault));
    ASSERT_TRUE(heProfiled.toEditableMesh(backProfiled));
    EXPECT_TRUE(isManifold(backDefault));
    EXPECT_TRUE(isManifold(backProfiled));
}

// Baseline single-edge bevel still yields a valid, manifold chamfer (the
// success branch of the gather→clean→build pipeline).
TEST(HalfEdgeMeshNgonEdgeBevel, SingleInteriorEdgeProducesManifoldChamfer) {
    HalfEdgeMesh he;
    ASSERT_TRUE(he.buildFromEditableMesh(makeQuadCube()));
    const int edge = findEdge(he, 2, 3);
    ASSERT_GE(edge, 0);
    ASSERT_FALSE(he.isEdgeBoundary(edge));

    const auto out = he.bevelEdgesNgon({edge}, 0.1f);

    EXPECT_EQ(out.size(), 4u);
    EXPECT_TRUE(he.validate());

    EditableMesh back;
    ASSERT_TRUE(he.toEditableMesh(back));
    EXPECT_TRUE(isManifold(back));
}
