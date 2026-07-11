#include "SkinWeights.h"

#include <gtest/gtest.h>

#include <QJsonObject>
#include <QJsonArray>

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
    ASSERT_TRUE(SkinWeights::computeWeights(
        kBarPositions.data(), 4, kTwoBones, optsLow, low));

    SkinWeightsOptions optsHigh = optsLow;
    optsHigh.falloff = 8.0;
    ASSERT_TRUE(SkinWeights::computeWeights(
        kBarPositions.data(), 4, kTwoBones, optsHigh, high));

    // Vertex 1 sits at y=1 — equidistant-ish from both bones.
    // High falloff should drive its primary weight higher.
    ASSERT_GE(low.size(), 2u);
    ASSERT_GE(high.size(), 2u);
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
    EXPECT_EQ(SkinWeights::algorithmToString(
                  SkinWeights::Algorithm::GeodesicVoxel),
              QStringLiteral("geodesic-voxel"));
    EXPECT_EQ(SkinWeights::algorithmToString(
                  SkinWeights::Algorithm::SkinTokens),
              QStringLiteral("skintokens"));
    EXPECT_EQ(SkinWeights::algorithmFromString("inverse-distance"),
              SkinWeights::Algorithm::InverseDistance);
    EXPECT_EQ(SkinWeights::algorithmFromString("geodesic-voxel"),
              SkinWeights::Algorithm::GeodesicVoxel);
    EXPECT_EQ(SkinWeights::algorithmFromString("skintokens"),
              SkinWeights::Algorithm::SkinTokens);
    // "unirig" stays as a deprecated alias of the ML skinner.
    EXPECT_EQ(SkinWeights::algorithmFromString("unirig"),
              SkinWeights::Algorithm::SkinTokens);
    // Unknown strings resolve to the default (the ML skinner).
    EXPECT_EQ(SkinWeights::algorithmFromString("unknown-fallback"),
              SkinWeights::Algorithm::SkinTokens);
}

// ─── Edge cases ──────────────────────────────────────────────────────────────

TEST(SkinWeightsTest, SingleBoneGetsFullWeight)
{
    // One bone, several verts — every vertex should be 100% bone 0.
    std::vector<SkinWeights::BoneSegment> oneBone = {
        { 0.0, 0.0, 0.0, 0.0, 3.0, 0.0 },
    };
    SkinWeightsOptions opts;
    opts.maxInfluenceDistance = 0;  // no cap
    std::vector<SkinWeights::VertexWeights> w;
    ASSERT_TRUE(SkinWeights::computeWeights(
        kBarPositions.data(), 4, oneBone, opts, w));
    for (const auto& vw : w) {
        ASSERT_EQ(vw.count, 1);
        EXPECT_EQ(vw.boneIndices[0], 0);
        EXPECT_NEAR(vw.weights[0], 1.0, 1e-9);
    }
}

TEST(SkinWeightsTest, LeafBonePointDistanceWorks)
{
    // A bone with head == tail degenerates to point distance. Verify
    // the vertex closest to the point gets the dominant weight.
    std::vector<SkinWeights::BoneSegment> bones = {
        { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },   // point at origin (leaf)
        { 0.0, 3.0, 0.0, 0.0, 3.0, 0.0 },   // point at y=3 (leaf)
    };
    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = 2;
    opts.maxInfluenceDistance   = 0;
    std::vector<SkinWeights::VertexWeights> w;
    ASSERT_TRUE(SkinWeights::computeWeights(
        kBarPositions.data(), 4, bones, opts, w));
    // Vertex 0 (y=0) closest to bone 0; vertex 3 (y=3) closest to bone 1.
    EXPECT_EQ(w[0].boneIndices[0], 0);
    EXPECT_EQ(w[3].boneIndices[0], 1);
}

TEST(SkinWeightsTest, VertexExactlyOnBoneDoesNotDivideByZero)
{
    // Vertex 0 sits exactly on bone 0's head. The eps in the
    // inverse-distance formula must keep the weight finite and
    // normalized — not NaN/Inf.
    std::vector<SkinWeights::VertexWeights> w;
    SkinWeightsOptions opts;
    opts.maxInfluenceDistance = 0;
    ASSERT_TRUE(SkinWeights::computeWeights(
        kBarPositions.data(), 4, kTwoBones, opts, w));
    for (const auto& vw : w) {
        for (int i = 0; i < vw.count; ++i) {
            EXPECT_TRUE(std::isfinite(vw.weights[i]))
                << "weight is NaN/Inf — eps guard failed";
        }
    }
}

TEST(SkinWeightsTest, VertexOutsideAllRadiiPinsToBoneZero)
{
    // A lone vertex far from every bone, with a tight distance cap,
    // should fall through to the "pin to bone 0, weight 1.0"
    // fallback rather than ending up with zero influences (which
    // would leave it static while the rig animates).
    std::vector<float> farVert = { 1000.0f, 1000.0f, 1000.0f };
    SkinWeightsOptions opts;
    opts.maxInfluenceDistance = 0.01;  // tiny cap → excludes the bar bones
    std::vector<SkinWeights::VertexWeights> w;
    ASSERT_TRUE(SkinWeights::computeWeights(
        farVert.data(), 1, kTwoBones, opts, w));
    ASSERT_EQ(w.size(), 1u);
    EXPECT_EQ(w[0].count, 1);
    EXPECT_EQ(w[0].boneIndices[0], 0);
    EXPECT_NEAR(w[0].weights[0], 1.0, 1e-9);
}

TEST(SkinWeightsTest, MaxInfluencesClampedToUpperBound)
{
    // Request more than the hard cap of 8 — should be clamped, not
    // overflow the fixed-size VertexWeights arrays.
    std::vector<SkinWeights::BoneSegment> bones;
    for (int i = 0; i < 12; ++i) {
        const double y = i * 0.25;
        bones.push_back({ 0, y, 0, 0, y + 0.1, 0 });
    }
    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = 999;  // absurd — must clamp to 8
    opts.maxInfluenceDistance   = 0;
    std::vector<SkinWeights::VertexWeights> w;
    ASSERT_TRUE(SkinWeights::computeWeights(
        kBarPositions.data(), 4, bones, opts, w));
    for (const auto& vw : w)
        EXPECT_LE(vw.count, 8) << "influence count exceeded the hard cap of 8";
}

// ─── Report serialization ────────────────────────────────────────────────────

TEST(SkinWeightsTest, ReportToJsonRoundTrip)
{
    SkinWeightsReport report;
    report.meshName               = QStringLiteral("Hero");
    report.skeletonName           = QStringLiteral("Hero.skeleton");
    report.totalBones             = 30;
    report.totalVerticesProcessed = 1200;
    report.totalAssignmentsBefore = 0;
    report.totalAssignmentsAfter  = 4800;
    report.applied                = true;

    SkinWeightsSubmeshReport sub;
    sub.submeshIndex              = 0;
    sub.verticesProcessed         = 1200;
    sub.boneAssignmentsBefore     = 0;
    sub.boneAssignmentsAfter      = 4800;
    sub.verticesWithMaxInfluences = 1100;
    report.submeshes.push_back(sub);

    const auto json = SkinWeights::reportToJson(report);
    EXPECT_EQ(json["meshName"].toString(), QStringLiteral("Hero"));
    EXPECT_EQ(json["skeletonName"].toString(), QStringLiteral("Hero.skeleton"));
    EXPECT_EQ(json["totalBones"].toInt(), 30);
    EXPECT_EQ(json["totalVerticesProcessed"].toInt(), 1200);
    EXPECT_EQ(json["totalAssignmentsAfter"].toInt(), 4800);
    EXPECT_TRUE(json["applied"].toBool());
    ASSERT_TRUE(json["submeshes"].isArray());
    const auto subs = json["submeshes"].toArray();
    ASSERT_EQ(subs.size(), 1);
    EXPECT_EQ(subs[0].toObject()["verticesWithMaxInfluences"].toInt(), 1100);
}

TEST(SkinWeightsTest, ReportToJsonIncludesErrorOnFailure)
{
    SkinWeightsReport report;
    report.applied = false;
    report.error   = QStringLiteral("mesh has no skeleton attached");
    const auto json = SkinWeights::reportToJson(report);
    EXPECT_FALSE(json["applied"].toBool());
    EXPECT_EQ(json["error"].toString(),
              QStringLiteral("mesh has no skeleton attached"));
}

TEST(SkinWeightsTest, ReportToTextContainsKeyFields)
{
    SkinWeightsReport report;
    report.meshName               = QStringLiteral("Hero");
    report.skeletonName           = QStringLiteral("Hero.skeleton");
    report.totalBones             = 30;
    report.totalVerticesProcessed = 1200;
    report.totalAssignmentsBefore = 100;
    report.totalAssignmentsAfter  = 4800;
    report.applied                = true;

    const QString txt = SkinWeights::reportToText(report);
    EXPECT_TRUE(txt.contains("Hero"));
    EXPECT_TRUE(txt.contains("Hero.skeleton"));
    EXPECT_TRUE(txt.contains("30"));
    EXPECT_TRUE(txt.contains("1200"));
    EXPECT_TRUE(txt.contains("100"));
    EXPECT_TRUE(txt.contains("4800"));
}

TEST(SkinWeightsTest, FalloffClampedFromBelow)
{
    // A falloff below the 0.5 floor must not throw or produce
    // garbage — computeWeights clamps it internally. Verify the
    // result is still a valid normalized weight set.
    SkinWeightsOptions opts;
    opts.falloff = 0.0;  // below floor
    opts.maxInfluenceDistance = 0;
    std::vector<SkinWeights::VertexWeights> w;
    ASSERT_TRUE(SkinWeights::computeWeights(
        kBarPositions.data(), 4, kTwoBones, opts, w));
    for (const auto& vw : w) {
        double sum = 0.0;
        for (int i = 0; i < vw.count; ++i) sum += vw.weights[i];
        EXPECT_NEAR(sum, 1.0, 1e-6);
    }
}
