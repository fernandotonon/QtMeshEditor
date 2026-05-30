#include "SkinWeights.h"

#include <gtest/gtest.h>

#include <vector>
#include <cmath>

// Unit tests for the inverse-distance skin weights (issue #402).
// Only the pure-data `computeWeights` path is covered here; the
// Ogre-backed `computeAndApply` requires a live entity + skeleton
// and is exercised by integration runs.

namespace {

// A vertical bar: 4 vertices at z = 0, 1, 2, 3 along the y axis.
// Bone 0 spans y=[0..1], bone 1 spans y=[2..3]. Expected outcome:
// vertices 0 and 1 weight strongly to bone 0; verts 2 and 3 to
// bone 1.
std::vector<float> kBarPositions = {
    0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f,
    0.0f, 2.0f, 0.0f,
    0.0f, 3.0f, 0.0f,
};

std::vector<SkinWeights::BoneSegment> kTwoBones = {
    { 0.0, 0.0, 0.0, 0.0, 1.0, 0.0 },   // bone 0: y=[0..1]
    { 0.0, 2.0, 0.0, 0.0, 3.0, 0.0 },   // bone 1: y=[2..3]
};

} // namespace

TEST(SkinWeightsTest, NearVerticesGetCorrectBone)
{
    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = 2;
    opts.maxInfluenceDistance   = 0;  // no cap so every bone is considered
    std::vector<SkinWeights::VertexWeights> w;
    ASSERT_TRUE(SkinWeights::computeWeights(
        kBarPositions.data(), 4, kTwoBones, opts, w));
    ASSERT_EQ(w.size(), 4u);

    // Vertex 0 sits on bone 0; should be ~100% bone 0.
    EXPECT_GE(w[0].count, 1);
    EXPECT_EQ(w[0].boneIndices[0], 0);
    EXPECT_GT(w[0].weights[0], 0.99);

    // Vertex 3 sits on bone 1.
    EXPECT_EQ(w[3].boneIndices[0], 1);
    EXPECT_GT(w[3].weights[0], 0.99);
}

TEST(SkinWeightsTest, WeightsSumToOne)
{
    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = 4;
    opts.maxInfluenceDistance   = 0;
    std::vector<SkinWeights::VertexWeights> w;
    ASSERT_TRUE(SkinWeights::computeWeights(
        kBarPositions.data(), 4, kTwoBones, opts, w));

    for (const auto& vw : w) {
        double sum = 0.0;
        for (int i = 0; i < vw.count; ++i) sum += vw.weights[i];
        EXPECT_NEAR(sum, 1.0, 1e-6)
            << "vertex weights don't sum to 1; count=" << vw.count;
    }
}

TEST(SkinWeightsTest, NonNegativeWeights)
{
    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = 4;
    std::vector<SkinWeights::VertexWeights> w;
    ASSERT_TRUE(SkinWeights::computeWeights(
        kBarPositions.data(), 4, kTwoBones, opts, w));

    for (const auto& vw : w) {
        for (int i = 0; i < vw.count; ++i)
            EXPECT_GE(vw.weights[i], 0.0);
    }
}

TEST(SkinWeightsTest, MaxInfluencesIsRespected)
{
    // 5 bones at different y positions, max influences clamped to 2.
    std::vector<SkinWeights::BoneSegment> bones;
    for (int i = 0; i < 5; ++i) {
        const double y = i * 1.0;
        bones.push_back({ 0, y, 0, 0, y + 0.5, 0 });
    }
    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = 2;
    opts.maxInfluenceDistance   = 0;
    std::vector<SkinWeights::VertexWeights> w;
    ASSERT_TRUE(SkinWeights::computeWeights(
        kBarPositions.data(), 4, bones, opts, w));

    for (const auto& vw : w) {
        EXPECT_LE(vw.count, 2)
            << "more than 2 influences leaked through despite maxInfluencesPerVertex=2";
    }
}

TEST(SkinWeightsTest, MaxDistanceCapExcludesFarBones)
{
    // Bar from y=0..3, plus a bone far away at y=100.
    std::vector<SkinWeights::BoneSegment> bones = kTwoBones;
    bones.push_back({ 0, 100, 0, 0, 101, 0 });

    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = 4;
    opts.maxInfluenceDistance   = 0.5;  // half the diagonal (~1.5 units)
    std::vector<SkinWeights::VertexWeights> w;
    ASSERT_TRUE(SkinWeights::computeWeights(
        kBarPositions.data(), 4, bones, opts, w));

    for (const auto& vw : w) {
        for (int i = 0; i < vw.count; ++i) {
            EXPECT_NE(vw.boneIndices[i], 2)
                << "the far bone 2 should have been distance-capped out";
        }
    }
}

TEST(SkinWeightsTest, FalloffSharpensTheBind)
{
    // With a high falloff, the weights should concentrate more
    // aggressively on the nearest bone.
    std::vector<SkinWeights::VertexWeights> low, high;

    SkinWeightsOptions optsLow;
    optsLow.maxInfluencesPerVertex = 2;
    optsLow.falloff = 1.0;
    optsLow.maxInfluenceDistance = 0;
    SkinWeights::computeWeights(kBarPositions.data(), 4, kTwoBones, optsLow, low);

    SkinWeightsOptions optsHigh = optsLow;
    optsHigh.falloff = 8.0;
    SkinWeights::computeWeights(kBarPositions.data(), 4, kTwoBones, optsHigh, high);

    // Vertex 1 sits at y=1 — equidistant-ish from both bones.
    // High falloff should drive its primary weight higher.
    ASSERT_GE(low[1].count, 1);
    ASSERT_GE(high[1].count, 1);
    EXPECT_GE(high[1].weights[0], low[1].weights[0])
        << "high falloff failed to concentrate weight on the nearest bone";
}

TEST(SkinWeightsTest, EmptyInputReturnsFalse)
{
    SkinWeightsOptions opts;
    std::vector<SkinWeights::VertexWeights> w;
    EXPECT_FALSE(SkinWeights::computeWeights(nullptr, 0, {}, opts, w));
    EXPECT_TRUE(w.empty());
}

TEST(SkinWeightsTest, AlgorithmStringRoundTrip)
{
    EXPECT_EQ(SkinWeights::algorithmToString(
                  SkinWeights::Algorithm::InverseDistance),
              QStringLiteral("inverse-distance"));
    EXPECT_EQ(SkinWeights::algorithmFromString("inverse-distance"),
              SkinWeights::Algorithm::InverseDistance);
    EXPECT_EQ(SkinWeights::algorithmFromString("unknown-fallback"),
              SkinWeights::Algorithm::InverseDistance);
}
