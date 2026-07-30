// Pure-data tests for the PartOps core (#859). No Ogre buffers / GL — these
// exercise SubMeshOps entirely on in-memory EditableSubMesh data, so they run
// headless under Xvfb like the rest of the CI suite.

#include <gtest/gtest.h>

#include "SubMeshOps.h"
#include "MeshSegmenter.h"

#include <cmath>
#include <map>
#include <utility>

namespace {

EditableVertex vtx(float x, float y, float z)
{
    EditableVertex v;
    v.position = Ogre::Vector3(x, y, z);
    v.normal = Ogre::Vector3(0, 1, 0);
    v.hasNormal = true;
    return v;
}

void addTri(EditableSubMesh& sm, unsigned a, unsigned b, unsigned c)
{
    EditableTriangle t;
    t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
    sm.triangles.push_back(t);
}

// A single submesh with 4 verts / 2 tris forming a quad in the XZ plane, plus
// a second disjoint quad — used to exercise label grouping and splitting.
// Layout: tris 0,1 = "quad A" (label 1), tris 2,3 = "quad B" (label 2).
EditableSubMesh twoQuadSubmesh()
{
    EditableSubMesh sm;
    sm.materialName = "Mat";
    // quad A
    sm.vertices.push_back(vtx(0, 0, 0)); // 0
    sm.vertices.push_back(vtx(1, 0, 0)); // 1
    sm.vertices.push_back(vtx(1, 0, 1)); // 2
    sm.vertices.push_back(vtx(0, 0, 1)); // 3
    // quad B (shares edge 1-2 with A → verts 1,2 are the boundary)
    sm.vertices.push_back(vtx(2, 0, 0)); // 4
    sm.vertices.push_back(vtx(2, 0, 1)); // 5
    addTri(sm, 0, 1, 2); // tri 0 (A)
    addTri(sm, 0, 2, 3); // tri 1 (A)
    addTri(sm, 1, 4, 5); // tri 2 (B)
    addTri(sm, 1, 5, 2); // tri 3 (B)  shares verts 1,2 with A
    return sm;
}

} // namespace

TEST(SubMeshOpsTest, GroupFacesByLabelStableOrder)
{
    // labels: two tris of head(1), one torso(2), one unknown(0)
    std::vector<int> labels = {1, 1, 2, 0};
    auto groups = SubMeshOps::groupFacesByLabel(labels);
    ASSERT_EQ(groups.size(), 3u);
    // Sorted by label → unknown(0), head(1), torso(2).
    EXPECT_EQ(groups[0].label, 0);
    EXPECT_EQ(groups[0].name, MeshSegmenter::partName(0));
    EXPECT_EQ(groups[1].label, 1);
    EXPECT_EQ(groups[1].triangles.size(), 2u);
    EXPECT_EQ(groups[1].triangles[0], 0u);
    EXPECT_EQ(groups[1].triangles[1], 1u);
    EXPECT_EQ(groups[2].label, 2);
    EXPECT_EQ(groups[2].triangles.size(), 1u);
    EXPECT_EQ(groups[2].triangles[0], 2u);
}

TEST(SubMeshOpsTest, SplitByFaceGroupsCreatesOneSubmeshPerLabel)
{
    std::vector<EditableSubMesh> in = {twoQuadSubmesh()};
    // tris 0,1 → label 1 (head); tris 2,3 → label 2 (torso)
    std::vector<int> faceLabels = {1, 1, 2, 2};
    auto groups = SubMeshOps::groupFacesByLabel(faceLabels);

    SubMeshOps::SplitResult r = SubMeshOps::splitByFaceGroups(in, faceLabels, groups);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.subMeshes.size(), 2u);
    EXPECT_EQ(r.createdSubMeshes, 2);

    // Part A: 2 tris, 4 unique verts. Part B: 2 tris, 4 unique verts.
    EXPECT_EQ(r.subMeshes[0].triangles.size(), 2u);
    EXPECT_EQ(r.subMeshes[1].triangles.size(), 2u);
    EXPECT_EQ(r.subMeshes[0].vertices.size(), 4u);
    EXPECT_EQ(r.subMeshes[1].vertices.size(), 4u);

    // The two quads share verts 1 and 2 → both got duplicated into part B.
    EXPECT_EQ(r.duplicatedBoundaryVertices, 2);

    // Material preserved by default.
    EXPECT_EQ(r.subMeshes[0].materialName, "Mat");
    EXPECT_EQ(r.subMeshes[1].materialName, "Mat");

    // Part names come from the labels.
    EXPECT_EQ(r.partNames[0], MeshSegmenter::partName(1));
    EXPECT_EQ(r.partNames[1], MeshSegmenter::partName(2));

    // Total triangles preserved (nothing dropped when all labels accepted).
    EXPECT_EQ(SubMeshOps::totalTriangleCount(r.subMeshes), 4u);
}

TEST(SubMeshOpsTest, SplitKeepsSourceMaterialsWhenLabelSpansMaterials)
{
    // #859 review (Codex P2): a single part label whose triangles come from
    // TWO source materials must emit two submeshes — one per material — not
    // collapse onto whichever triangle came first. Two source submeshes with
    // different materials; every triangle labelled the same part (1).
    EditableSubMesh matA;
    matA.materialName = "MatA";
    matA.vertices = {vtx(0, 0, 0), vtx(1, 0, 0), vtx(0, 0, 1)};
    addTri(matA, 0, 1, 2);
    EditableSubMesh matB;
    matB.materialName = "MatB";
    matB.vertices = {vtx(2, 0, 0), vtx(3, 0, 0), vtx(2, 0, 1)};
    addTri(matB, 0, 1, 2);
    std::vector<EditableSubMesh> in = {matA, matB};
    std::vector<int> faceLabels = {1, 1}; // both tris → same part label

    auto groups = SubMeshOps::groupFacesByLabel(faceLabels);
    SubMeshOps::SplitResult r = SubMeshOps::splitByFaceGroups(in, faceLabels, groups);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    // Two output submeshes: one per source material, both from part "head"(1).
    ASSERT_EQ(r.subMeshes.size(), 2u);
    std::set<std::string> mats = {r.subMeshes[0].materialName, r.subMeshes[1].materialName};
    EXPECT_TRUE(mats.count("MatA"));
    EXPECT_TRUE(mats.count("MatB"));
    // Both name variants derive from the same part; the second gets a suffix.
    const QString base = MeshSegmenter::partName(1);
    EXPECT_EQ(r.partNames[0], base);
    EXPECT_EQ(r.partNames[1], base + QStringLiteral(".1"));
}

TEST(SubMeshOpsTest, SplitAssignPartMaterialsCollapsesAcrossSourceMaterials)
{
    // With assignPartMaterials the part gets ONE generated material, so a label
    // spanning source materials becomes a single submesh (the material split is
    // intentionally suppressed).
    EditableSubMesh matA; matA.materialName = "MatA";
    matA.vertices = {vtx(0,0,0), vtx(1,0,0), vtx(0,0,1)}; addTri(matA,0,1,2);
    EditableSubMesh matB; matB.materialName = "MatB";
    matB.vertices = {vtx(2,0,0), vtx(3,0,0), vtx(2,0,1)}; addTri(matB,0,1,2);
    std::vector<EditableSubMesh> in = {matA, matB};
    std::vector<int> faceLabels = {1, 1};
    auto groups = SubMeshOps::groupFacesByLabel(faceLabels);
    SubMeshOps::SplitOptions opts;
    opts.assignPartMaterials = true;
    opts.namePrefix = QStringLiteral("Body");
    SubMeshOps::SplitResult r = SubMeshOps::splitByFaceGroups(in, faceLabels, groups, opts);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.subMeshes.size(), 1u);
    EXPECT_EQ(r.subMeshes[0].materialName,
              (QStringLiteral("Body.") + MeshSegmenter::partName(1)).toStdString());
}

TEST(SubMeshOpsTest, SplitExcludesGroupAndDropsItsFaces)
{
    std::vector<EditableSubMesh> in = {twoQuadSubmesh()};
    std::vector<int> faceLabels = {1, 1, 2, 2};
    auto groups = SubMeshOps::groupFacesByLabel(faceLabels);
    // Exclude torso (label 2).
    for (auto& g : groups)
        if (g.label == 2) g.excluded = true;

    SubMeshOps::SplitResult r = SubMeshOps::splitByFaceGroups(in, faceLabels, groups);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.subMeshes.size(), 1u);
    EXPECT_EQ(r.partNames[0], MeshSegmenter::partName(1));
    EXPECT_EQ(r.subMeshes[0].triangles.size(), 2u);
    // Excluded faces are dropped, not merged elsewhere.
    EXPECT_EQ(SubMeshOps::totalTriangleCount(r.subMeshes), 2u);
}

TEST(SubMeshOpsTest, SplitRejectsMismatchedLabelCount)
{
    std::vector<EditableSubMesh> in = {twoQuadSubmesh()};
    std::vector<int> faceLabels = {1, 1, 2}; // 3 labels, mesh has 4 tris
    auto groups = SubMeshOps::groupFacesByLabel(faceLabels);
    SubMeshOps::SplitResult r = SubMeshOps::splitByFaceGroups(in, faceLabels, groups);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

TEST(SubMeshOpsTest, SplitPreservesVertexAttributesAndBoneWeights)
{
    EditableSubMesh sm;
    sm.materialName = "Skin";
    for (int i = 0; i < 3; ++i) {
        EditableVertex v = vtx(float(i), 0, 0);
        v.hasUV = true;
        v.uv = Ogre::Vector2(0.25f * i, 0.5f);
        v.hasColor = true;
        v.color = Ogre::ColourValue(0.1f, 0.2f, 0.3f, 1.0f);
        EditableBoneAssignment ba;
        ba.boneIndex = static_cast<unsigned short>(i);
        ba.weight = 1.0f;
        v.boneAssignments.push_back(ba);
        sm.vertices.push_back(v);
    }
    addTri(sm, 0, 1, 2);
    std::vector<EditableSubMesh> in = {sm};
    std::vector<int> faceLabels = {1};
    auto groups = SubMeshOps::groupFacesByLabel(faceLabels);

    SubMeshOps::SplitResult r = SubMeshOps::splitByFaceGroups(in, faceLabels, groups);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.subMeshes.size(), 1u);
    const auto& out = r.subMeshes[0];
    ASSERT_EQ(out.vertices.size(), 3u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_TRUE(out.vertices[i].hasUV);
        EXPECT_TRUE(out.vertices[i].hasColor);
        ASSERT_EQ(out.vertices[i].boneAssignments.size(), 1u);
        EXPECT_EQ(out.vertices[i].boneAssignments[0].boneIndex, i);
        EXPECT_FLOAT_EQ(out.vertices[i].boneAssignments[0].weight, 1.0f);
    }
}

TEST(SubMeshOpsTest, JoinBakesTransformIntoPositions)
{
    // Two single-tri parts, same material. Part B translated +10 in X.
    EditableSubMesh a;
    a.materialName = "Mat";
    a.vertices = {vtx(0, 0, 0), vtx(1, 0, 0), vtx(0, 0, 1)};
    addTri(a, 0, 1, 2);
    EditableSubMesh b = a;

    SubMeshOps::JoinPart pa{{a}, Ogre::Matrix4::IDENTITY};
    SubMeshOps::JoinPart pb;
    pb.subMeshes = {b};
    pb.transform = Ogre::Matrix4::IDENTITY;
    pb.transform.setTrans(Ogre::Vector3(10, 0, 0));

    auto r = SubMeshOps::joinParts({pa, pb});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    // Same material → one merged submesh with 6 verts / 2 tris.
    ASSERT_EQ(r.subMeshes.size(), 1u);
    EXPECT_EQ(r.subMeshes[0].vertices.size(), 6u);
    EXPECT_EQ(r.subMeshes[0].triangles.size(), 2u);
    // Part B's first vertex baked to x=10.
    EXPECT_FLOAT_EQ(r.subMeshes[0].vertices[3].position.x, 10.0f);
    // Indices offset correctly (second tri references 3,4,5).
    EXPECT_EQ(r.subMeshes[0].triangles[1].indices[0], 3u);
}

TEST(SubMeshOpsTest, JoinBakesRotationIntoPositionsAndNormals)
{
    // One tri with an up (+Y) normal, rotated 90° about +X: +Y should map to
    // +Z for both the position vector and the normal (Slice C #862 "moving
    // exploded parts and joining back bakes transforms correctly").
    EditableSubMesh a;
    a.materialName = "Mat";
    a.vertices = {vtx(0, 0, 0), vtx(0, 1, 0), vtx(1, 0, 0)};
    addTri(a, 0, 1, 2);

    // 90° about X: rotates (0,1,0) → (0,0,1).
    Ogre::Quaternion q(Ogre::Degree(90), Ogre::Vector3::UNIT_X);
    Ogre::Matrix4 M(q);

    SubMeshOps::JoinPart pa;
    pa.subMeshes = {a};
    pa.transform = M;

    auto r = SubMeshOps::joinParts({pa});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.subMeshes.size(), 1u);
    ASSERT_EQ(r.subMeshes[0].vertices.size(), 3u);
    // vertex 1 was (0,1,0) → (0,0,1).
    const auto& p1 = r.subMeshes[0].vertices[1].position;
    EXPECT_NEAR(p1.x, 0.0f, 1e-5f);
    EXPECT_NEAR(p1.y, 0.0f, 1e-5f);
    EXPECT_NEAR(p1.z, 1.0f, 1e-5f);
    // The +Y normal rotated to +Z too (inverse-transpose == rotation here).
    const auto& n0 = r.subMeshes[0].vertices[0].normal;
    EXPECT_NEAR(n0.x, 0.0f, 1e-5f);
    EXPECT_NEAR(n0.y, 0.0f, 1e-5f);
    EXPECT_NEAR(n0.z, 1.0f, 1e-5f);
}

TEST(SubMeshOpsTest, JoinReversesWindingUnderMirrorTransform)
{
    // A negative-X-scale transform (determinant < 0) mirrors positions; join
    // must reverse triangle winding + flip tangent handedness so the part
    // doesn't render back-facing / with inverted normal mapping.
    EditableSubMesh a;
    a.materialName = "Mat";
    a.vertices = {vtx(0, 0, 0), vtx(1, 0, 0), vtx(0, 0, 1)};
    a.vertices[0].hasTangent = true; a.vertices[0].tangent = Ogre::Vector4(1, 0, 0, 1);
    addTri(a, 0, 1, 2);

    Ogre::Matrix4 mirror = Ogre::Matrix4::IDENTITY;
    mirror[0][0] = -1.0f; // flip X

    SubMeshOps::JoinPart pa;
    pa.subMeshes = {a};
    pa.transform = mirror;

    auto r = SubMeshOps::joinParts({pa});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.subMeshes.size(), 1u);
    // Winding reversed: last two corners swapped (0,1,2 → 0,2,1).
    EXPECT_EQ(r.subMeshes[0].triangles[0].indices[0], 0u);
    EXPECT_EQ(r.subMeshes[0].triangles[0].indices[1], 2u);
    EXPECT_EQ(r.subMeshes[0].triangles[0].indices[2], 1u);
    // Tangent handedness flipped (+1 → −1).
    EXPECT_FLOAT_EQ(r.subMeshes[0].vertices[0].tangent.w, -1.0f);
}

TEST(SubMeshOpsTest, JoinKeepsWindingUnderNonMirrorTransform)
{
    // A plain rotation (determinant +1) must NOT reverse winding.
    EditableSubMesh a;
    a.materialName = "Mat";
    a.vertices = {vtx(0, 0, 0), vtx(1, 0, 0), vtx(0, 0, 1)};
    addTri(a, 0, 1, 2);
    Ogre::Matrix4 rot(Ogre::Quaternion(Ogre::Degree(45), Ogre::Vector3::UNIT_Y));

    SubMeshOps::JoinPart pa; pa.subMeshes = {a}; pa.transform = rot;
    auto r = SubMeshOps::joinParts({pa});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.subMeshes[0].triangles[0].indices[0], 0u);
    EXPECT_EQ(r.subMeshes[0].triangles[0].indices[1], 1u);
    EXPECT_EQ(r.subMeshes[0].triangles[0].indices[2], 2u);
}

TEST(SubMeshOpsTest, JoinRejectsFewerThanExpectedIsCallerConcern)
{
    // joinParts itself accepts a single part (used by the explode-then-rejoin
    // one-part edge case); the >=2 guard lives in the scene adapter. A single
    // identity part round-trips unchanged.
    EditableSubMesh a; a.materialName = "M";
    a.vertices = {vtx(0,0,0), vtx(1,0,0), vtx(0,0,1)}; addTri(a,0,1,2);
    auto r = SubMeshOps::joinParts({{{a}, Ogre::Matrix4::IDENTITY}});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.subMeshes.size(), 1u);
    EXPECT_EQ(r.subMeshes[0].vertices.size(), 3u);
}

TEST(SubMeshOpsTest, JoinKeepsDistinctMaterialsSeparate)
{
    EditableSubMesh a; a.materialName = "A"; a.vertices = {vtx(0,0,0), vtx(1,0,0), vtx(0,0,1)}; addTri(a,0,1,2);
    EditableSubMesh b; b.materialName = "B"; b.vertices = {vtx(0,0,0), vtx(1,0,0), vtx(0,0,1)}; addTri(b,0,1,2);
    SubMeshOps::JoinPart pa{{a}, Ogre::Matrix4::IDENTITY};
    SubMeshOps::JoinPart pb{{b}, Ogre::Matrix4::IDENTITY};
    auto r = SubMeshOps::joinParts({pa, pb});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.subMeshes.size(), 2u);
}

TEST(SubMeshOpsTest, ExplodeOffsetsPushOutwardFromCenter)
{
    std::vector<Ogre::Vector3> centroids = {
        Ogre::Vector3(-1, 0, 0), Ogre::Vector3(1, 0, 0)};
    Ogre::AxisAlignedBox bounds(Ogre::Vector3(-1, 0, 0), Ogre::Vector3(1, 0, 0));
    auto offs = SubMeshOps::explodeOffsets(centroids, bounds, 0.5f);
    ASSERT_EQ(offs.size(), 2u);
    // Opposite directions along X, equal magnitude.
    EXPECT_LT(offs[0].x, 0.0f);
    EXPECT_GT(offs[1].x, 0.0f);
    EXPECT_NEAR(offs[0].x, -offs[1].x, 1e-5f);
    // Magnitude = distance * diag (diag = 2 here) = 0.5*2 = 1.
    EXPECT_NEAR(offs[1].length(), 1.0f, 1e-5f);
}

// ---- capOpenBoundaries (#863 close split cut face) ------------------------

TEST(SubMeshOpsTest, CapOpenBoundaryClosesHole)
{
    // An open-topped box: 8 cube corners, all 5 side+bottom faces, TOP missing.
    // The top rim (verts 4,5,6,7 at y=1) is one open boundary loop of 4 edges.
    // capOpenBoundaries should fill it → 1 cap, +1 centre vert, +4 triangles.
    EditableSubMesh s;
    s.materialName = "Box";
    // bottom (y=0): 0,1,2,3   top (y=1): 4,5,6,7
    auto V = [](float x,float y,float z){ EditableVertex v; v.position=Ogre::Vector3(x,y,z); return v; };
    s.vertices = { V(0,0,0),V(1,0,0),V(1,0,1),V(0,0,1),
                   V(0,1,0),V(1,1,0),V(1,1,1),V(0,1,1) };
    auto Q = [&](unsigned a,unsigned b,unsigned c,unsigned d){ addTri(s,a,b,c); addTri(s,a,c,d); };
    Q(0,1,2,3);   // bottom
    Q(0,4,5,1);   // front
    Q(1,5,6,2);   // right
    Q(2,6,7,3);   // back
    Q(3,7,4,0);   // left
    // NO top → verts 4,5,6,7 form the open rim.

    const size_t triBefore = s.triangles.size();
    const size_t vBefore = s.vertices.size();
    const int caps = SubMeshOps::capOpenBoundaries(s);
    EXPECT_EQ(caps, 1);
    EXPECT_EQ(s.vertices.size(), vBefore + 1);          // one centroid vertex
    EXPECT_EQ(s.triangles.size(), triBefore + 4);       // one tri per rim edge
    // The new centre vertex sits at the rim centroid (0.5,1,0.5).
    const auto& cv = s.vertices.back();
    EXPECT_NEAR(cv.position.x, 0.5f, 1e-4f);
    EXPECT_NEAR(cv.position.y, 1.0f, 1e-4f);
    EXPECT_NEAR(cv.position.z, 0.5f, 1e-4f);
}

TEST(SubMeshOpsTest, CapOpenBoundariesNoOpWhenClosed)
{
    // A closed tetrahedron: every edge is shared by two faces → no boundary.
    EditableSubMesh s; s.materialName = "Tet";
    auto V = [](float x,float y,float z){ EditableVertex v; v.position=Ogre::Vector3(x,y,z); return v; };
    s.vertices = { V(0,0,0), V(1,0,0), V(0,1,0), V(0,0,1) };
    addTri(s,0,2,1); addTri(s,0,1,3); addTri(s,0,3,2); addTri(s,1,2,3);
    const size_t before = s.triangles.size();
    EXPECT_EQ(SubMeshOps::capOpenBoundaries(s), 0);
    EXPECT_EQ(s.triangles.size(), before);
}

// Count directed boundary edges (a→b with no b→a) — 0 means watertight.
static size_t boundaryEdgeCount(const EditableSubMesh& s)
{
    std::map<std::pair<unsigned,unsigned>,int> d;
    for (const auto& t : s.triangles) {
        d[{t.indices[0],t.indices[1]}]++;
        d[{t.indices[1],t.indices[2]}]++;
        d[{t.indices[2],t.indices[0]}]++;
    }
    size_t open = 0;
    for (const auto& kv : d)
        if (!d.count({kv.first.second, kv.first.first})) open += 1;
    return open;
}

TEST(SubMeshOpsTest, CapOpenBoundariesClosesBothEndsOfATube)
{
    // An open tube (a ring extruded along Y, NO end caps): TWO separate boundary
    // loops. The old single-successor walk capped only one; the multi-successor
    // walk must close BOTH → 0 boundary edges after, watertight.
    EditableSubMesh s; s.materialName = "Tube";
    const int seg = 8;
    auto V = [](float x,float y,float z){ EditableVertex v; v.position=Ogre::Vector3(x,y,z); return v; };
    for (int i = 0; i < seg; ++i) {
        const float a = 2.0f*float(M_PI)*float(i)/float(seg);
        s.vertices.push_back(V(std::cos(a), 0.f, std::sin(a)));  // bottom ring
        s.vertices.push_back(V(std::cos(a), 2.f, std::sin(a)));  // top ring
    }
    for (int i = 0; i < seg; ++i) {
        const int j = (i+1)%seg;
        const unsigned b0=2*i, t0=2*i+1, b1=2*j, t1=2*j+1;
        addTri(s, b0, b1, t1);
        addTri(s, b0, t1, t0);
    }
    ASSERT_GT(boundaryEdgeCount(s), 0u);          // open at both ends
    const int caps = SubMeshOps::capOpenBoundaries(s);
    EXPECT_EQ(caps, 2) << "both tube ends must be capped";
    EXPECT_EQ(boundaryEdgeCount(s), 0u) << "tube must be watertight after capping";
}

// ---- solidify (#863 follow-up: give a thin shell real wall volume) ---------

TEST(SubMeshOpsTest, SolidifyClosesAnOpenFlatQuadIntoASlab)
{
    // A single flat quad (2 tris, open on all 4 edges) — a zero-thickness shell.
    // Solidify must add an inner shell + a wall around the rim so the result is
    // a closed watertight slab (0 welded open edges), doubling the verts and
    // adding inner + wall triangles.
    EditableSubMesh s; s.materialName = "Shell";
    auto V = [](float x,float y,float z){ EditableVertex v; v.position=Ogre::Vector3(x,y,z); v.normal=Ogre::Vector3(0,1,0); v.hasNormal=true; return v; };
    s.vertices = { V(0,0,0), V(1,0,0), V(1,0,1), V(0,0,1) };
    addTri(s,0,1,2); addTri(s,0,2,3);
    ASSERT_GT(boundaryEdgeCount(s), 0u);              // open shell
    const size_t v0=s.vertices.size(), t0=s.triangles.size();
    const int walls = SubMeshOps::solidify(s, 0.1f);
    EXPECT_EQ(walls, 4) << "a quad rim has 4 boundary edges → 4 wall quads";
    EXPECT_EQ(s.vertices.size(), v0*2) << "inner shell duplicates every vertex";
    // outer tris + inner tris (=outer) + 2 tris per wall quad
    EXPECT_EQ(s.triangles.size(), t0*2 + 4u*2u);
    EXPECT_EQ(boundaryEdgeCount(s), 0u) << "solidified slab must be watertight";
    // The inner shell sits one thickness below the outer along -normal (y).
    float miny=1e9f, maxy=-1e9f;
    for (const auto& v : s.vertices){ miny=std::min(miny,v.position.y); maxy=std::max(maxy,v.position.y); }
    EXPECT_NEAR(maxy-miny, 0.1f, 1e-4f) << "slab thickness == requested";
}

TEST(SubMeshOpsTest, SolidifyAutoThicknessAndNoOpOnEmpty)
{
    EditableSubMesh empty;
    EXPECT_EQ(SubMeshOps::solidify(empty), 0);         // nothing to do
    // Auto thickness (<=0) picks a positive value from the AABB.
    EditableSubMesh s; s.materialName="S";
    auto V = [](float x,float y,float z){ EditableVertex v; v.position=Ogre::Vector3(x,y,z); v.normal=Ogre::Vector3(0,1,0); v.hasNormal=true; return v; };
    s.vertices = { V(0,0,0), V(2,0,0), V(2,0,2), V(0,0,2) };
    addTri(s,0,1,2); addTri(s,0,2,3);
    EXPECT_EQ(SubMeshOps::solidify(s, /*auto=*/0.0f), 4);
    EXPECT_EQ(boundaryEdgeCount(s), 0u);               // watertight
}
