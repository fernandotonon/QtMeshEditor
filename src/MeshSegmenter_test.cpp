#include <gtest/gtest.h>

#include "MeshSegmenter.h"

#include <array>
#include <cstdint>
#include <vector>

// MeshSegmenter's core is Ogre-free and ONNX-optional, so these tests run on any
// build with no GL/model — they exercise the pure-data helpers and the
// deterministic geometric fallback (the path used whenever the model is absent).

using MS = MeshSegmenter;

namespace {
// Two disjoint triangles (two islands), 6 verts.
void twoTriangles(std::vector<float>& pos, std::vector<uint32_t>& idx)
{
    pos = {
        0,0,0,  1,0,0,  0,1,0,        // tri A (verts 0,1,2)
        5,0,0,  6,0,0,  5,1,0,        // tri B (verts 3,4,5)
    };
    idx = { 0,1,2, 3,4,5 };
}
} // namespace

// ---- partName / count -----------------------------------------------------

TEST(MeshSegmenter, PartTaxonomyStable)
{
    EXPECT_EQ(MS::partCount(), static_cast<int>(MS::Part::Count));
    EXPECT_EQ(MS::partName(MS::Part::Head).toStdString(), "head");
    EXPECT_EQ(MS::partName(MS::Part::Torso).toStdString(), "torso");
    EXPECT_EQ(MS::partName(MS::Part::LeftArm).toStdString(), "left_arm");
    EXPECT_EQ(MS::partName(MS::Part::RightLeg).toStdString(), "right_leg");
    EXPECT_EQ(MS::partName(0).toStdString(), "unknown");
    EXPECT_EQ(MS::partName(999).toStdString(), "unknown");   // out of range
}

// ---- connectedComponents --------------------------------------------------

TEST(MeshSegmenter, ConnectedComponentsCountsIslands)
{
    std::vector<float> pos; std::vector<uint32_t> idx;
    twoTriangles(pos, idx);
    std::vector<int> island;
    const int n = MS::connectedComponents(6, idx.data(), (int)idx.size(), island);
    EXPECT_EQ(n, 2);
    ASSERT_EQ(island.size(), 6u);
    EXPECT_EQ(island[0], island[1]);          // tri A together
    EXPECT_EQ(island[1], island[2]);
    EXPECT_EQ(island[3], island[4]);          // tri B together
    EXPECT_NE(island[0], island[3]);          // A != B
}

TEST(MeshSegmenter, ConnectedComponentsSharedVertexMerges)
{
    // Two triangles sharing vertex 2 → one island.
    std::vector<float> pos(6*3, 0.0f);
    std::vector<uint32_t> idx = { 0,1,2, 2,3,4 };
    std::vector<int> island;
    const int n = MS::connectedComponents(5, idx.data(), (int)idx.size(), island);
    EXPECT_EQ(n, 1);
}

// ---- facesFromVertexLabels ------------------------------------------------

TEST(MeshSegmenter, FaceLabelIsMajorityOfVerts)
{
    std::vector<uint32_t> idx = { 0,1,2,  3,4,5 };
    //        v0 v1 v2 v3 v4 v5
    std::vector<int> vl = { 1,1,2,  3,4,4 };
    auto faces = MS::facesFromVertexLabels(vl, idx.data(), (int)idx.size());
    ASSERT_EQ(faces.size(), 2u);
    EXPECT_EQ(faces[0], 1);   // {1,1,2} → majority 1
    EXPECT_EQ(faces[1], 4);   // {3,4,4} → majority 4
}

TEST(MeshSegmenter, FaceLabelAllDistinctTakesLowest)
{
    std::vector<uint32_t> idx = { 0,1,2 };
    std::vector<int> vl = { 5,3,4 };
    auto faces = MS::facesFromVertexLabels(vl, idx.data(), (int)idx.size());
    ASSERT_EQ(faces.size(), 1u);
    EXPECT_EQ(faces[0], 3);   // no majority → min(5,3,4)
}

// ---- geometric fallback ---------------------------------------------------

TEST(MeshSegmenter, GeometricLabelsHeadAtTop)
{
    // A tall +Y strip: a vertex near the top should be Head, near the bottom a leg.
    std::vector<float> pos = {
        0,0,0,  0.1f,0,0,  0,0.05f,0,     // bottom cluster
        0,10,0, 0.1f,10,0, 0,9.95f,0,     // top cluster
    };
    std::vector<uint32_t> idx = { 0,1,2, 3,4,5 };
    MS::Options o; o.upAxis = 1;
    auto r = MS::segmentGeometric(pos.data(), 6, idx.data(), (int)idx.size(), o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.vertexLabels.size(), 6u);
    // Top cluster (island 2) → Head; bottom → a leg (not Head).
    EXPECT_EQ(r.vertexLabels[3], static_cast<int>(MS::Part::Head));
    EXPECT_NE(r.vertexLabels[0], static_cast<int>(MS::Part::Head));
    EXPECT_EQ(r.faceLabels.size(), 2u);
}

TEST(MeshSegmenter, GeometricBoneProximityOverrides)
{
    std::vector<float> pos; std::vector<uint32_t> idx;
    twoTriangles(pos, idx);
    // Force every vertex to RightArm via bone-proximity hints.
    std::vector<int> bone(6, static_cast<int>(MS::Part::RightArm));
    MS::Options o;
    auto r = MS::segmentGeometric(pos.data(), 6, idx.data(), (int)idx.size(), o, bone.data());
    ASSERT_TRUE(r.ok);
    for (int l : r.vertexLabels) EXPECT_EQ(l, static_cast<int>(MS::Part::RightArm));
}

TEST(MeshSegmenter, GeometricEmptyGeometryFails)
{
    auto r = MS::segmentGeometric(nullptr, 0, nullptr, 0, MS::Options{});
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

// ---- predict (model-or-fallback) ------------------------------------------

TEST(MeshSegmenter, PredictFallsBackWhenNoModel)
{
    std::vector<float> pos; std::vector<uint32_t> idx;
    twoTriangles(pos, idx);
    auto r = MS::predict(pos.data(), 6, idx.data(), (int)idx.size(),
                         QStringLiteral("/no/such/meshseg.onnx"), MS::Options{});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_FALSE(r.usedModel);
    EXPECT_FALSE(r.fallbackReason.isEmpty());
    EXPECT_EQ(r.vertexLabels.size(), 6u);
}

TEST(MeshSegmenter, ModelBackendAvailabilityMatchesBuild)
{
#ifdef ENABLE_ONNX
    EXPECT_TRUE(MS::isModelBackendAvailable());
#else
    EXPECT_FALSE(MS::isModelBackendAvailable());
#endif
}
