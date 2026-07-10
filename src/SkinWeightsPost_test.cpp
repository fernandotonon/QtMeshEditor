#include "SkinWeightsPost.h"
#include "SkinWeights.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

// Unit tests for the Slice-B weight post-pass pipeline (issue #819).
// Pure-data — no Ogre / GL.

namespace {

SkinWeights::VertexWeights makeVW(std::initializer_list<std::pair<int, double>> ws)
{
    SkinWeights::VertexWeights vw;
    for (const auto& [bone, w] : ws) {
        vw.boneIndices[vw.count] = bone;
        vw.weights[vw.count]     = w;
        ++vw.count;
    }
    return vw;
}

double weightOnBone(const SkinWeights::VertexWeights& vw, int bone)
{
    for (int i = 0; i < vw.count; ++i)
        if (vw.boneIndices[i] == bone) return vw.weights[i];
    return 0.0;
}

double rowSum(const SkinWeights::VertexWeights& vw)
{
    double s = 0.0;
    for (int i = 0; i < vw.count; ++i) s += vw.weights[i];
    return s;
}

} // namespace

// ─── buildAdjacency ─────────────────────────────────────────────────────────

TEST(SkinWeightsPostTest, AdjacencyFromTriangles)
{
    // Quad as two triangles: 0-1-2, 0-2-3. Vertex 0 and 2 share the
    // diagonal; 1 and 3 do not touch.
    const std::vector<std::uint32_t> indices = { 0, 1, 2, 0, 2, 3 };
    const auto adj = SkinWeightsPost::buildAdjacency(4, indices.data(),
                                                     indices.size());
    ASSERT_EQ(adj.size(), 4u);
    EXPECT_EQ(adj[0], (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(adj[1], (std::vector<int>{0, 2}));
    EXPECT_EQ(adj[2], (std::vector<int>{0, 1, 3}));
    EXPECT_EQ(adj[3], (std::vector<int>{0, 2}));
}

TEST(SkinWeightsPostTest, AdjacencyHandlesEmptyAndOutOfRange)
{
    EXPECT_TRUE(SkinWeightsPost::buildAdjacency(0, nullptr, 0).empty());
    // Out-of-range indices are ignored, not crashed on.
    const std::vector<std::uint32_t> indices = { 0, 1, 99 };
    const auto adj = SkinWeightsPost::buildAdjacency(3, indices.data(),
                                                     indices.size());
    ASSERT_EQ(adj.size(), 3u);
    EXPECT_EQ(adj[0], (std::vector<int>{1}));
    EXPECT_TRUE(adj[2].empty());
}

// ─── laplacianSmooth ────────────────────────────────────────────────────────

TEST(SkinWeightsPostTest, SmoothingPreservesPartitionOfUnity)
{
    // A strip of 4 vertices: 0-1-2, 1-2-3. Hard 0/1 split between
    // two bones.
    const std::vector<std::uint32_t> indices = { 0, 1, 2, 1, 2, 3 };
    std::vector<SkinWeights::VertexWeights> w = {
        makeVW({{0, 1.0}}),
        makeVW({{0, 1.0}}),
        makeVW({{1, 1.0}}),
        makeVW({{1, 1.0}}),
    };
    const auto adj = SkinWeightsPost::buildAdjacency(4, indices.data(),
                                                     indices.size());
    SkinWeightsPost::laplacianSmooth(w, adj, 3);
    for (const auto& vw : w) {
        EXPECT_GE(vw.count, 1);
        EXPECT_NEAR(rowSum(vw), 1.0, 1e-9);
    }
    // The hard edge must have blended: vertex 1 now carries some of
    // bone 1, vertex 2 some of bone 0.
    EXPECT_GT(weightOnBone(w[1], 1), 0.05);
    EXPECT_GT(weightOnBone(w[2], 0), 0.05);
}

TEST(SkinWeightsPostTest, LockedVerticesAreDirichletConstraints)
{
    const std::vector<std::uint32_t> indices = { 0, 1, 2, 1, 2, 3 };
    std::vector<SkinWeights::VertexWeights> w = {
        makeVW({{0, 1.0}}),
        makeVW({{0, 1.0}}),
        makeVW({{1, 1.0}}),
        makeVW({{1, 1.0}}),
    };
    const std::vector<std::uint8_t> locked = { 0, 1, 0, 0 };
    const auto adj = SkinWeightsPost::buildAdjacency(4, indices.data(),
                                                     indices.size());
    SkinWeightsPost::laplacianSmooth(w, adj, 5, locked);
    // Vertex 1 is locked: bit-identical to its input.
    EXPECT_EQ(w[1].count, 1);
    EXPECT_EQ(w[1].boneIndices[0], 0);
    EXPECT_DOUBLE_EQ(w[1].weights[0], 1.0);
    // …but it still influenced its neighbours: vertex 2 picked up
    // bone 0 from the locked vertex.
    EXPECT_GT(weightOnBone(w[2], 0), 0.05);
}

TEST(SkinWeightsPostTest, ZeroIterationsIsANoOp)
{
    const std::vector<std::uint32_t> indices = { 0, 1, 2 };
    std::vector<SkinWeights::VertexWeights> w = {
        makeVW({{0, 1.0}}),
        makeVW({{1, 1.0}}),
        makeVW({{0, 0.5}, {1, 0.5}}),
    };
    const auto before = w;
    const auto adj = SkinWeightsPost::buildAdjacency(3, indices.data(),
                                                     indices.size());
    SkinWeightsPost::laplacianSmooth(w, adj, 0);
    for (size_t v = 0; v < w.size(); ++v) {
        ASSERT_EQ(w[v].count, before[v].count);
        for (int i = 0; i < w[v].count; ++i) {
            EXPECT_EQ(w[v].boneIndices[i], before[v].boneIndices[i]);
            EXPECT_DOUBLE_EQ(w[v].weights[i], before[v].weights[i]);
        }
    }
}

TEST(SkinWeightsPostTest, SmoothingRenormalizesWhenTruncatingPastEightBones)
{
    // A hub vertex ringed by 10 neighbours, each weighted to a
    // DISTINCT bone: one smoothing step spreads the hub across 11
    // bones, which must truncate back to the 8-slot cap AND still
    // sum to one.
    std::vector<std::uint32_t> indices;
    const int ring = 10;
    for (int i = 0; i < ring; ++i) {
        indices.push_back(0);
        indices.push_back(std::uint32_t(1 + i));
        indices.push_back(std::uint32_t(1 + (i + 1) % ring));
    }
    std::vector<SkinWeights::VertexWeights> w;
    w.push_back(makeVW({{100, 1.0}}));           // hub, its own bone
    for (int i = 0; i < ring; ++i)
        w.push_back(makeVW({{i, 1.0}}));         // unique bone per spoke

    const auto adj = SkinWeightsPost::buildAdjacency(
        ring + 1, indices.data(), indices.size());
    SkinWeightsPost::laplacianSmooth(w, adj, 1);

    EXPECT_LE(w[0].count, 8);
    EXPECT_NEAR(rowSum(w[0]), 1.0, 1e-9)
        << "truncating a >8-bone smoothed row must renormalize";
}

// ─── pruneAndRenormalize ────────────────────────────────────────────────────

TEST(SkinWeightsPostTest, PruneDropsTinyWeightsAndRenormalizes)
{
    std::vector<SkinWeights::VertexWeights> w = {
        makeVW({{0, 0.6}, {1, 0.3}, {2, 0.095}, {3, 0.005}}),
    };
    SkinWeightsPost::pruneAndRenormalize(w, 4, 0.01);
    ASSERT_EQ(w[0].count, 3);   // the 0.005 entry dropped
    EXPECT_NEAR(rowSum(w[0]), 1.0, 1e-9);
    EXPECT_DOUBLE_EQ(weightOnBone(w[0], 3), 0.0);
}

TEST(SkinWeightsPostTest, PruneEnforcesMaxInfluences)
{
    std::vector<SkinWeights::VertexWeights> w = {
        makeVW({{0, 0.4}, {1, 0.3}, {2, 0.2}, {3, 0.1}}),
    };
    SkinWeightsPost::pruneAndRenormalize(w, 2, 0.01);
    ASSERT_EQ(w[0].count, 2);
    // Top-2 kept, renormalized: 0.4/0.7 and 0.3/0.7.
    EXPECT_NEAR(weightOnBone(w[0], 0), 0.4 / 0.7, 1e-9);
    EXPECT_NEAR(weightOnBone(w[0], 1), 0.3 / 0.7, 1e-9);
    EXPECT_NEAR(rowSum(w[0]), 1.0, 1e-9);
}

TEST(SkinWeightsPostTest, PruneNeverEmptiesARow)
{
    // Every weight below threshold — the largest must survive.
    std::vector<SkinWeights::VertexWeights> w = {
        makeVW({{0, 0.006}, {1, 0.004}}),
    };
    SkinWeightsPost::pruneAndRenormalize(w, 4, 0.01);
    ASSERT_EQ(w[0].count, 1);
    EXPECT_EQ(w[0].boneIndices[0], 0);
    EXPECT_DOUBLE_EQ(w[0].weights[0], 1.0);
}

// ─── bleedFraction ──────────────────────────────────────────────────────────

TEST(SkinWeightsPostTest, BleedFractionCountsDisallowedBones)
{
    std::vector<SkinWeights::VertexWeights> w = {
        makeVW({{0, 0.5}, {1, 0.5}}),   // bone 1 not allowed → bleed
        makeVW({{0, 1.0}}),             // clean
    };
    const std::vector<std::vector<int>> allowed = {
        { 0 },
        { 0, 1 },
    };
    // 3 entries total, 1 bleeding.
    EXPECT_NEAR(SkinWeightsPost::bleedFraction(w, allowed), 1.0 / 3.0, 1e-9);
}

TEST(SkinWeightsPostTest, BleedFractionReturnsMinusOneWhenUncomputable)
{
    std::vector<SkinWeights::VertexWeights> w = { makeVW({{0, 1.0}}) };
    EXPECT_DOUBLE_EQ(SkinWeightsPost::bleedFraction(w, {}), -1.0);
    // Allowed sets present but all empty → no data → -1.
    const std::vector<std::vector<int>> allowed = { {} };
    EXPECT_DOUBLE_EQ(SkinWeightsPost::bleedFraction(w, allowed), -1.0);
}
