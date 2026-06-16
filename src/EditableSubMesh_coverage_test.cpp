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
// EditableSubMesh free-function coverage gaps.
//
// These cases are pure-data and need NO Ogre / display, so they use plain
// TEST() with a DISTINCT suite name (EditableSubMeshCoverageTest) to avoid
// any ODR / duplicate-registration clash with the existing
// EditableMeshStandalone and EditableFaceStandalone suites.
//
// Targets (gaps not covered by EditableMesh_test.cpp / EditableFace_test.cpp):
//   1. mergeCoplanarTrianglesToQuads() invoked via the DEFAULT-arg overload
//      (angleThresholdDeg == 1.0f), exercising the documented default 1° path
//      through the no-second-argument call site. All 13 existing
//      MergeCoplanar* tests either pass an explicit threshold or test the
//      threshold behaviour; this drives the implicit default.
//   2. totalFaceCount() on a SINGLE submesh vector that MIXES a
//      faces-canonical submesh AND a triangle-only submesh — exercising the
//      per-submesh branch selection within one accumulation walk (existing
//      tests cover each representation in isolation only).
//   3. syncTriangulation() on a multi-submesh vector where one submesh has
//      faces and another is empty / tri-only — exercising the mixed walk in
//      a single call (existing tests cover the leave-tri-only-alone and
//      fan-triangulate-quad branches in separate single-submesh calls).
// ===========================================================================

#include <gtest/gtest.h>
#include "EditableMesh.h"

namespace {

// Mirror of the helper used in EditableMesh_test.cpp / EditableFace_test.cpp:
// a vertex positioned in the XY plane with a +Z normal. The merge logic only
// reads .position; the normal is set so the data looks well-formed.
EditableVertex covVert(float x, float y, float z) {
    EditableVertex v;
    v.position = Ogre::Vector3(x, y, z);
    v.normal = Ogre::Vector3::UNIT_Z;
    v.hasNormal = true;
    return v;
}

EditableTriangle covTri(unsigned int a, unsigned int b, unsigned int c) {
    EditableTriangle t{};
    t.indices[0] = a;
    t.indices[1] = b;
    t.indices[2] = c;
    return t;
}

EditableFace covFace(std::vector<unsigned int> idx) {
    EditableFace f;
    f.indices = std::move(idx);
    return f;
}

// Two triangles forming a planar unit quad on the XY plane.
EditableSubMesh planarQuadAsTris() {
    EditableSubMesh sub;
    sub.vertices = {
        covVert(0, 0, 0), covVert(1, 0, 0), covVert(1, 1, 0), covVert(0, 1, 0),
    };
    sub.triangles = {covTri(0, 1, 2), covTri(0, 2, 3)};
    return sub;
}

} // namespace

// ===========================================================================
// 1. mergeCoplanarTrianglesToQuads() — DEFAULT-arg (1°) overload
// ===========================================================================

// A perfectly coplanar quad split into two tris must merge under the default
// 1° threshold when called WITHOUT a second argument.
TEST(EditableSubMeshCoverageTest, MergeDefaultThresholdMergesCoplanarQuad) {
    EditableSubMesh sub = planarQuadAsTris();

    // No second argument => documented default angleThresholdDeg == 1.0f.
    const int merged = mergeCoplanarTrianglesToQuads(sub);

    EXPECT_EQ(merged, 1);
    ASSERT_EQ(sub.faces.size(), 1u);
    EXPECT_EQ(sub.faces[0].indices.size(), 4u);
    // triangulation mirror rebuilt from the new quad: 1 quad => 2 fan tris.
    EXPECT_EQ(sub.triangles.size(), 2u);
}

// A ~5° dihedral exceeds the strict default 1° threshold, so the default-arg
// call must NOT merge (proving the default really is the strict 1°, not a
// looser value). Existing MergeCoplanarRespectsAngleThreshold passes 1.0f
// explicitly; this confirms the implicit default matches.
TEST(EditableSubMeshCoverageTest, MergeDefaultThresholdRejectsTiltedPair) {
    EditableSubMesh sub;
    sub.vertices = {
        covVert(0, 0, 0),
        covVert(1, 0, 0),
        covVert(1, 1, 0),
        // 4th vertex tilted up in z by tan(5°) ~= 0.0875 => ~5° dihedral.
        covVert(0, 1, 0.0875f),
    };
    sub.triangles = {covTri(0, 1, 2), covTri(0, 2, 3)};

    const int merged = mergeCoplanarTrianglesToQuads(sub); // default 1°

    EXPECT_EQ(merged, 0);
    // No merge => both triangles emitted as 3-index faces.
    ASSERT_EQ(sub.faces.size(), 2u);
    EXPECT_EQ(sub.faces[0].indices.size(), 3u);
    EXPECT_EQ(sub.faces[1].indices.size(), 3u);
}

// Default-arg call on an empty submesh: benign, returns 0, leaves both
// representations empty.
TEST(EditableSubMeshCoverageTest, MergeDefaultThresholdEmptySubMeshIsNoOp) {
    EditableSubMesh sub;
    EXPECT_EQ(mergeCoplanarTrianglesToQuads(sub), 0);
    EXPECT_TRUE(sub.faces.empty());
    EXPECT_TRUE(sub.triangles.empty());
}

// Default-arg call on a lone triangle (no interior edge to merge across):
// returns 0 but still promotes the triangle into a single 3-index face and
// keeps the triangle mirror.
TEST(EditableSubMeshCoverageTest, MergeDefaultThresholdSingleTriBecomesTriFace) {
    EditableSubMesh sub;
    sub.vertices = {covVert(0, 0, 0), covVert(1, 0, 0), covVert(0, 1, 0)};
    sub.triangles = {covTri(0, 1, 2)};

    EXPECT_EQ(mergeCoplanarTrianglesToQuads(sub), 0); // default 1°
    ASSERT_EQ(sub.faces.size(), 1u);
    EXPECT_EQ(sub.faces[0].indices.size(), 3u);
    EXPECT_EQ(sub.triangles.size(), 1u);
}

// ===========================================================================
// 2. totalFaceCount() — MIXED vector (faces-canonical + tri-only submeshes)
// ===========================================================================

// One submesh carries a canonical quad face; a SECOND submesh in the SAME
// vector is triangle-only. totalFaceCount must select faces.size() for the
// first and triangles.size() for the second within a single accumulation.
TEST(EditableSubMeshCoverageTest, TotalFaceCountMixedVectorSelectsPerSubmesh) {
    std::vector<EditableSubMesh> subs(2);

    // Submesh 0: faces-canonical — one quad (faces non-empty wins).
    subs[0].vertices = {
        covVert(0, 0, 0), covVert(1, 0, 0), covVert(1, 1, 0), covVert(0, 1, 0),
    };
    subs[0].faces = {covFace({0, 1, 2, 3})};
    triangulateFaces(subs[0]); // mirror: 2 tris — must NOT be counted

    // Submesh 1: legacy triangle-only — 3 triangles, faces empty.
    subs[1].vertices = {
        covVert(0, 0, 0), covVert(1, 0, 0), covVert(0, 1, 0), covVert(1, 1, 0),
    };
    subs[1].triangles = {covTri(0, 1, 2), covTri(0, 2, 3), covTri(1, 3, 2)};

    // 1 face (submesh 0) + 3 triangles (submesh 1) = 4. NOT 2 + 3 = 5.
    EXPECT_EQ(totalFaceCount(subs), 4u);
}

// Order-independence: swap the two submeshes; the per-submesh branch
// selection must still pick faces vs triangles correctly.
TEST(EditableSubMeshCoverageTest, TotalFaceCountMixedVectorOrderIndependent) {
    std::vector<EditableSubMesh> subs(2);

    // Submesh 0: triangle-only (2 tris).
    subs[0].vertices = {covVert(0, 0, 0), covVert(1, 0, 0), covVert(0, 1, 0),
                        covVert(1, 1, 0)};
    subs[0].triangles = {covTri(0, 1, 2), covTri(1, 3, 2)};

    // Submesh 1: two canonical quad faces.
    subs[1].vertices = {covVert(0, 0, 0), covVert(1, 0, 0), covVert(1, 1, 0),
                        covVert(0, 1, 0), covVert(2, 0, 0), covVert(2, 1, 0)};
    subs[1].faces = {covFace({0, 1, 2, 3}), covFace({1, 4, 5, 2})};
    triangulateFaces(subs[1]); // mirror: 4 tris — must NOT be counted

    // 2 triangles (submesh 0) + 2 faces (submesh 1) = 4.
    EXPECT_EQ(totalFaceCount(subs), 4u);
}

// A vector mixing a non-empty (faces) submesh with a completely EMPTY submesh
// (no faces, no triangles) — the empty one contributes 0 via the triangle
// fallback branch.
TEST(EditableSubMeshCoverageTest, TotalFaceCountMixedWithEmptySubmesh) {
    std::vector<EditableSubMesh> subs(2);

    subs[0].vertices = {covVert(0, 0, 0), covVert(1, 0, 0), covVert(1, 1, 0),
                        covVert(0, 1, 0)};
    subs[0].faces = {covFace({0, 1, 2, 3})};
    triangulateFaces(subs[0]);

    // subs[1] left fully empty (default-constructed).

    EXPECT_EQ(totalFaceCount(subs), 1u);
}

// Empty vector => 0 (boundary).
TEST(EditableSubMeshCoverageTest, TotalFaceCountEmptyVectorIsZero) {
    std::vector<EditableSubMesh> subs;
    EXPECT_EQ(totalFaceCount(subs), 0u);
}

// ===========================================================================
// 3. syncTriangulation() — MIXED walk in a single call
// ===========================================================================

// One submesh has a quad face (must be fan-triangulated into 2 tris) and a
// SECOND submesh is triangle-only (faces empty — must be left untouched),
// all in a SINGLE syncTriangulation call over the shared vector.
TEST(EditableSubMeshCoverageTest, SyncTriangulationMixedWalkFacesAndTriOnly) {
    std::vector<EditableSubMesh> subs(2);

    // Submesh 0: canonical quad, triangles deliberately STALE (empty) so we
    // can prove sync rebuilt them.
    subs[0].vertices = {covVert(0, 0, 0), covVert(1, 0, 0), covVert(1, 1, 0),
                        covVert(0, 1, 0)};
    subs[0].faces = {covFace({0, 1, 2, 3})};
    subs[0].triangles.clear();

    // Submesh 1: legacy triangle-only — faces empty, a single triangle that
    // must survive the walk unchanged.
    subs[1].vertices = {covVert(0, 0, 0), covVert(1, 0, 0), covVert(0, 1, 0)};
    subs[1].triangles = {covTri(0, 1, 2)};

    syncTriangulation(subs);

    // Submesh 0: quad fan-triangulated to 2 tris.
    EXPECT_EQ(subs[0].triangles.size(), 2u)
        << "quad submesh must be fan-triangulated by syncTriangulation";
    // Submesh 1: untouched, still its single triangle, faces still empty.
    EXPECT_TRUE(subs[1].faces.empty());
    ASSERT_EQ(subs[1].triangles.size(), 1u);
    EXPECT_EQ(subs[1].triangles[0].indices[0], 0u);
    EXPECT_EQ(subs[1].triangles[0].indices[1], 1u);
    EXPECT_EQ(subs[1].triangles[0].indices[2], 2u);
}

// Mixed walk where the SECOND submesh is fully empty (no faces, no triangles)
// alongside a first submesh with a face — the empty one stays empty, the
// face one gets triangulated. Exercises both branches in one call with an
// empty-submesh edge case.
TEST(EditableSubMeshCoverageTest, SyncTriangulationMixedWalkWithEmptySubmesh) {
    std::vector<EditableSubMesh> subs(2);

    // Submesh 0: a pentagon face (5 verts -> 3 fan tris).
    subs[0].vertices = {covVert(0, 0, 0), covVert(2, 0, 0), covVert(2, 2, 0),
                        covVert(1, 3, 0), covVert(0, 2, 0)};
    subs[0].faces = {covFace({0, 1, 2, 3, 4})};
    subs[0].triangles.clear();

    // Submesh 1: completely empty.

    syncTriangulation(subs);

    EXPECT_EQ(subs[0].triangles.size(), 3u)
        << "pentagon must fan-triangulate to N-2 = 3 triangles";
    EXPECT_TRUE(subs[1].faces.empty());
    EXPECT_TRUE(subs[1].triangles.empty());
}

// syncTriangulation on an empty vector is a benign no-op (boundary).
TEST(EditableSubMeshCoverageTest, SyncTriangulationEmptyVectorIsNoOp) {
    std::vector<EditableSubMesh> subs;
    syncTriangulation(subs); // must not crash
    EXPECT_TRUE(subs.empty());
}

// Mixed walk where BOTH submeshes carry faces (one quad, one triangle face)
// — every submesh in the vector takes the faces branch in a single call.
TEST(EditableSubMeshCoverageTest, SyncTriangulationMixedWalkAllFaces) {
    std::vector<EditableSubMesh> subs(2);

    subs[0].vertices = {covVert(0, 0, 0), covVert(1, 0, 0), covVert(1, 1, 0),
                        covVert(0, 1, 0)};
    subs[0].faces = {covFace({0, 1, 2, 3})}; // quad
    subs[0].triangles.clear();

    subs[1].vertices = {covVert(0, 0, 0), covVert(1, 0, 0), covVert(0, 1, 0)};
    subs[1].faces = {covFace({0, 1, 2})}; // triangle face
    subs[1].triangles.clear();

    syncTriangulation(subs);

    EXPECT_EQ(subs[0].triangles.size(), 2u); // quad -> 2 tris
    EXPECT_EQ(subs[1].triangles.size(), 1u); // tri face -> 1 tri
}
