#include "SkinTokensPredictor.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

// Pure-data tests for the SkinTokens predictor (#819 Slice C
// follow-up): the tokenizer serialization, coordinate discretization,
// and the sampled→full-res weight transfer. The ONNX inference path
// needs the downloaded models and is exercised by integration runs
// (it falls back to GeodesicVoxel when absent — covered by the
// SkinWeights dispatch tests).

namespace {

SkinTokensPredictor::TokenizerLayout defaultLayout()
{
    SkinTokensPredictor::TokenizerLayout l;
    l.numDiscrete = 256;
    l.rangeLo = -1.0;
    l.rangeHi = 1.0;
    l.tokBranch = 256;
    l.tokBos = 257;
    l.tokEos = 258;
    l.tokPad = 259;
    l.tokClsNone = 263;
    return l;
}

} // namespace

TEST(SkinTokensPredictorTest, DiscretizeMirrorsUpstream)
{
    // bin = (t - lo) / (hi - lo) * N clamped to [0, N-1] — the
    // upstream tokenizer's discretize().
    EXPECT_EQ(SkinTokensPredictor::discretize(-1.0, -1.0, 1.0, 256), 0);
    EXPECT_EQ(SkinTokensPredictor::discretize(1.0, -1.0, 1.0, 256), 255);
    EXPECT_EQ(SkinTokensPredictor::discretize(0.0, -1.0, 1.0, 256), 128);
    EXPECT_EQ(SkinTokensPredictor::discretize(-5.0, -1.0, 1.0, 256), 0);
    EXPECT_EQ(SkinTokensPredictor::discretize(5.0, -1.0, 1.0, 256), 255);
    // Degenerate range never divides by zero.
    EXPECT_EQ(SkinTokensPredictor::discretize(0.5, 1.0, 1.0, 256), 0);
}

TEST(SkinTokensPredictorTest, TokenizeChainAndBranch)
{
    // 4 joints: root(0) → 1 → 2, plus 3 branching off the root.
    // Bones 1..2 continue the chain (no branch); bone 3's parent (0)
    // != previous bone (2) → a branch record with the PARENT joint's
    // 3 bins first (the upstream TokenizeInput.bones layout).
    std::vector<SkinTokensPredictor::Joint> joints(4);
    joints[0].pos = { 0, 0, 0 };  joints[0].parent = -1;
    joints[1].pos = { 0, 0.5, 0 };  joints[1].parent = 0;
    joints[2].pos = { 0, 1.0, 0 };  joints[2].parent = 1;
    joints[3].pos = { 0.5, 0, 0 };  joints[3].parent = 0;

    const auto l = defaultLayout();
    const auto ids = SkinTokensPredictor::tokenizeSkeleton(joints, l);
    // [bos, cls_none,
    //  root x,y,z, j1 x,y,z, j2 x,y,z,
    //  branch, parent(root) x,y,z, j3 x,y,z,
    //  eos]
    ASSERT_EQ(int(ids.size()), 2 + 3 * 3 + (1 + 6) + 1);
    EXPECT_EQ(ids[0], l.tokBos);
    EXPECT_EQ(ids[1], l.tokClsNone);
    // Root at (0,0,0) → bin 128 each.
    EXPECT_EQ(ids[2], 128);
    EXPECT_EQ(ids[3], 128);
    EXPECT_EQ(ids[4], 128);
    // Joint 1 y=0.5 → bin 192.
    EXPECT_EQ(ids[6], 192);
    // Branch marker before joint 3's record.
    EXPECT_EQ(ids[11], l.tokBranch);
    // The branch record repeats the PARENT (root) position first.
    EXPECT_EQ(ids[12], 128);
    EXPECT_EQ(ids[13], 128);
    EXPECT_EQ(ids[14], 128);
    // Then joint 3 (x=0.5 → 192).
    EXPECT_EQ(ids[15], 192);
    EXPECT_EQ(ids.back(), l.tokEos);
}

TEST(SkinTokensPredictorTest, TokenizeRejectsInvalidOrdering)
{
    const auto l = defaultLayout();
    // Parent after child → rejected.
    std::vector<SkinTokensPredictor::Joint> bad(2);
    bad[0].parent = -1;
    bad[1].parent = 1;   // self/forward reference
    EXPECT_TRUE(SkinTokensPredictor::tokenizeSkeleton(bad, l).empty());
    // Second root → rejected (single-root streams only).
    bad[1].parent = -1;
    EXPECT_TRUE(SkinTokensPredictor::tokenizeSkeleton(bad, l).empty());
    // Empty input → empty output.
    EXPECT_TRUE(SkinTokensPredictor::tokenizeSkeleton({}, l).empty());
    // Without a cls token the head is just [bos].
    auto noCls = l;
    noCls.tokClsNone = -1;
    std::vector<SkinTokensPredictor::Joint> one(1);
    one[0].parent = -1;
    const auto ids = SkinTokensPredictor::tokenizeSkeleton(one, noCls);
    ASSERT_EQ(int(ids.size()), 1 + 3 + 1);
    EXPECT_EQ(ids[0], noCls.tokBos);
}

TEST(SkinTokensPredictorTest, TransferWeightsInterpolatesAndNormalizes)
{
    // Two samples on the x axis with opposite one-hot joint columns;
    // vertices at, near, and between them.
    std::vector<std::array<float, 3>> samples = {
        { 0.f, 0.f, 0.f },
        { 1.f, 0.f, 0.f },
    };
    // Row-major [sample][joint], 2 joints.
    const std::vector<float> sw = {
        1.f, 0.f,
        0.f, 1.f,
    };
    const std::vector<float> verts = {
        0.f, 0.f, 0.f,     // on sample 0
        1.f, 0.f, 0.f,     // on sample 1
        0.5f, 0.f, 0.f,    // midpoint
    };
    std::vector<SkinTokensPredictor::Result::VertexWeights> out;
    SkinTokensPredictor::transferWeights(verts.data(), 3, samples, sw, 2,
                                         4, out);
    ASSERT_EQ(out.size(), 3u);

    auto weightOn = [](const SkinTokensPredictor::Result::VertexWeights& vw,
                       int j) {
        for (int i = 0; i < vw.count; ++i)
            if (vw.jointIndices[i] == j) return vw.weights[i];
        return 0.0;
    };
    // Vertex on sample 0 → dominated by joint 0.
    EXPECT_GT(weightOn(out[0], 0), 0.99);
    // Midpoint → an even split.
    EXPECT_NEAR(weightOn(out[2], 0), 0.5, 0.05);
    EXPECT_NEAR(weightOn(out[2], 1), 0.5, 0.05);
    // Partition of unity everywhere.
    for (const auto& vw : out) {
        double s = 0;
        for (int i = 0; i < vw.count; ++i) s += vw.weights[i];
        EXPECT_NEAR(s, 1.0, 1e-9);
    }
}

TEST(SkinTokensPredictorTest, TransferWeightsRespectsMaxInfluences)
{
    // 6 samples each one-hot on a distinct joint, all near one vertex
    // → the blend touches 6 joints but only the top-2 survive.
    std::vector<std::array<float, 3>> samples;
    std::vector<float> sw;
    for (int j = 0; j < 6; ++j) {
        samples.push_back({ 0.1f * j, 0.f, 0.f });
        for (int k = 0; k < 6; ++k) sw.push_back(k == j ? 1.f : 0.f);
    }
    const std::vector<float> verts = { 0.05f, 0.f, 0.f };
    std::vector<SkinTokensPredictor::Result::VertexWeights> out;
    SkinTokensPredictor::transferWeights(verts.data(), 1, samples, sw, 6,
                                         2, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_LE(out[0].count, 2);
    double s = 0;
    for (int i = 0; i < out[0].count; ++i) s += out[0].weights[i];
    EXPECT_NEAR(s, 1.0, 1e-9);
}

TEST(SkinTokensPredictorTest, GuardsAndPaths)
{
    // Empty inputs never crash.
    std::vector<SkinTokensPredictor::Result::VertexWeights> out;
    SkinTokensPredictor::transferWeights(nullptr, 0, {}, {}, 0, 4, out);
    EXPECT_TRUE(out.empty());

    EXPECT_TRUE(SkinTokensPredictor::modelDir().contains(
        QStringLiteral("skintokens")));
    EXPECT_TRUE(SkinTokensPredictor::manifestPath().endsWith(
        QStringLiteral("skintokens.json")));

#ifndef ENABLE_ONNX
    EXPECT_FALSE(SkinTokensPredictor::isAvailable());
    const auto r = SkinTokensPredictor::predict(nullptr, 0, nullptr, 0, {});
    EXPECT_FALSE(r.ok);
#endif
}
