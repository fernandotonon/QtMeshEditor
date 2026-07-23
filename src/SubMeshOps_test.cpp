// Pure-data tests for the PartOps core (#859). No Ogre buffers / GL — these
// exercise SubMeshOps entirely on in-memory EditableSubMesh data, so they run
// headless under Xvfb like the rest of the CI suite.

#include <gtest/gtest.h>

#include "SubMeshOps.h"
#include "MeshSegmenter.h"

#include <cmath>

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

TEST(SubMeshOpsTest, BoundaryPlaneEstimatedFromSharedSeam)
{
    // Part A and B share a planar seam at x=0 (the YZ plane): 9 coincident
    // verts. A extends to -x, B to +x. Estimated normal ≈ ±X.
    EditableSubMesh a, b;
    for (int y = 0; y < 3; ++y)
        for (int z = 0; z < 3; ++z) {
            a.vertices.push_back(vtx(0, float(y), float(z)));   // seam
            b.vertices.push_back(vtx(0, float(y), float(z)));   // seam (coincident)
        }
    a.vertices.push_back(vtx(-1, 1, 1)); // A body
    b.vertices.push_back(vtx(1, 1, 1));  // B body
    // need a triangle so it's a valid submesh (not required by the estimator,
    // but keeps the fixture honest).
    addTri(a, 0, 1, 2);
    addTri(b, 0, 1, 2);

    auto plane = SubMeshOps::estimateBoundaryPlane({a}, {b});
    ASSERT_TRUE(plane.stable) << plane.reason.toStdString();
    EXPECT_NEAR(std::fabs(plane.normal.x), 1.0f, 1e-3f);
    EXPECT_NEAR(plane.center.x, 0.0f, 1e-4f);
    EXPECT_GT(plane.radius, 0.0f);
}

TEST(SubMeshOpsTest, BoundaryPlaneRejectsTinyBoundary)
{
    // Only 3 shared verts → below the 8-vertex minimum.
    EditableSubMesh a, b;
    for (int i = 0; i < 3; ++i) {
        a.vertices.push_back(vtx(0, float(i), 0));
        b.vertices.push_back(vtx(0, float(i), 0));
    }
    auto plane = SubMeshOps::estimateBoundaryPlane({a}, {b});
    EXPECT_FALSE(plane.stable);
    EXPECT_FALSE(plane.reason.isEmpty());
}

TEST(SubMeshOpsTest, AlignmentPegsGeneratedOnStablePlane)
{
    SubMeshOps::BoundaryPlane plane;
    plane.center = Ogre::Vector3(0, 0, 0);
    plane.normal = Ogre::Vector3::UNIT_X;
    plane.radius = 10.0f;
    plane.stable = true;

    SubMeshOps::PegOptions opts; // defaults: r=1.5, depth=4, maxPegs=3
    EditableSubMesh male, socket;
    int n = SubMeshOps::buildAlignmentPegs(plane, opts, male, socket);
    EXPECT_EQ(n, 3);
    EXPECT_FALSE(male.triangles.empty());
    EXPECT_FALSE(socket.triangles.empty());
    EXPECT_EQ(male.materialName, "connector_male");
    EXPECT_EQ(socket.materialName, "connector_socket");
    // Socket radius > peg radius (clearance) → socket cylinder verts spread wider.
    // Cheap check: socket has same vertex count structure as male (same segments).
    EXPECT_EQ(male.vertices.size(), socket.vertices.size());
}

TEST(SubMeshOpsTest, AlignmentPegsSkippedOnUnstablePlane)
{
    SubMeshOps::BoundaryPlane plane; // stable=false by default
    SubMeshOps::PegOptions opts;
    EditableSubMesh male, socket;
    EXPECT_EQ(SubMeshOps::buildAlignmentPegs(plane, opts, male, socket), 0);
    EXPECT_TRUE(male.triangles.empty());
}
