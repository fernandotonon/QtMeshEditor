/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — WeightPaintOps unit tests (Skel Slice D, issue #558)

Pure-data: the brush dab, flood fill, and the utility ops, over synthetic vertex
arrays with known geometry. No Ogre scene / GL — the acceptance criteria ask for
headless-CI coverage of normalize / mirror / smooth / lock / limit.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>

#include "SkinWeightsPost.h"
#include "WeightPaintOps.h"

#include <cmath>
#include <vector>

// SkinWeights is a CLASS, so VertexWeights is a member type — alias it
// rather than a using-declaration (which only works for namespaces).
using VertexWeights = SkinWeights::VertexWeights;
using WeightPaintOps::BrushMode;
using WeightPaintOps::BrushShape;
using WeightPaintOps::DabOptions;

namespace {

VertexWeights vw(std::initializer_list<std::pair<int, double>> entries)
{
    VertexWeights v;
    for (const auto& [bone, w] : entries) {
        if (v.count >= 8) break;
        v.boneIndices[v.count] = bone;
        v.weights[v.count] = w;
        ++v.count;
    }
    return v;
}

double rowSum(const VertexWeights& v)
{
    double s = 0.0;
    for (int k = 0; k < v.count; ++k) s += v.weights[k];
    return s;
}

/// A row of `n` vertices along X at unit spacing, all weighted 1.0 to bone 0.
struct Strip {
    std::vector<float> pos;
    std::vector<VertexWeights> w;
    std::vector<std::vector<int>> adj;

    explicit Strip(int n)
    {
        for (int i = 0; i < n; ++i) {
            pos.push_back(static_cast<float>(i));
            pos.push_back(0.0f);
            pos.push_back(0.0f);
            w.push_back(vw({{0, 1.0}}));
        }
        adj.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            if (i > 0) adj[static_cast<size_t>(i)].push_back(i - 1);
            if (i + 1 < n) adj[static_cast<size_t>(i)].push_back(i + 1);
        }
    }
    int count() const { return static_cast<int>(w.size()); }
};

} // namespace

// --- falloff ---------------------------------------------------------------

TEST(WeightPaintOpsTest, FalloffMatchesTheVertexColourBrushCurve) {
    // Same curve as EditModeController::applyVertexColorBrush: exponent
    // 1 + falloff*5 over (1 - d/r). If these diverge the two brushes feel
    // different under identical settings.
    EXPECT_NEAR(WeightPaintOps::falloffWeight(0.0, 1.0, 0.0, BrushShape::Round), 1.0, 1e-9);
    EXPECT_NEAR(WeightPaintOps::falloffWeight(1.0, 1.0, 0.0, BrushShape::Round), 0.0, 1e-9);
    EXPECT_NEAR(WeightPaintOps::falloffWeight(0.5, 1.0, 0.0, BrushShape::Round), 0.5, 1e-9);
    // falloff 1 => exponent 6 => 0.5^6
    EXPECT_NEAR(WeightPaintOps::falloffWeight(0.5, 1.0, 1.0, BrushShape::Round),
                std::pow(0.5, 6.0), 1e-9);
    // Outside the radius contributes nothing.
    EXPECT_DOUBLE_EQ(WeightPaintOps::falloffWeight(1.5, 1.0, 0.5, BrushShape::Round), 0.0);
    // Square is flat, as in the vertex-colour brush.
    EXPECT_DOUBLE_EQ(WeightPaintOps::falloffWeight(0.9, 1.0, 1.0, BrushShape::Square), 1.0);
    // Degenerate radius must not divide by zero.
    EXPECT_DOUBLE_EQ(WeightPaintOps::falloffWeight(0.0, 0.0, 0.5, BrushShape::Round), 0.0);
}

// --- row primitives --------------------------------------------------------

TEST(WeightPaintOpsTest, SetWeightInsertsAndUpdates) {
    VertexWeights v = vw({{0, 1.0}});
    EXPECT_TRUE(WeightPaintOps::setWeight(v, 3, 0.25));
    EXPECT_EQ(v.count, 2);
    EXPECT_NEAR(WeightPaintOps::weightOf(v, 3), 0.25, 1e-9);
    EXPECT_TRUE(WeightPaintOps::setWeight(v, 3, 0.75));
    EXPECT_EQ(v.count, 2) << "updating an existing bone must not add a slot";
    EXPECT_NEAR(WeightPaintOps::weightOf(v, 3), 0.75, 1e-9);
    EXPECT_DOUBLE_EQ(WeightPaintOps::weightOf(v, 99), 0.0) << "absent bone reads 0";
}

TEST(WeightPaintOpsTest, SetWeightRefusesAFullRowForANewBone) {
    VertexWeights v;
    for (int b = 0; b < 8; ++b) ASSERT_TRUE(WeightPaintOps::setWeight(v, b, 0.1));
    EXPECT_EQ(v.count, 8);
    EXPECT_FALSE(WeightPaintOps::setWeight(v, 42, 0.5))
        << "a full row must report failure so the caller can prune and retry";
    EXPECT_TRUE(WeightPaintOps::setWeight(v, 3, 0.9)) << "but updating still works";
}

TEST(WeightPaintOpsTest, NormalizeRowLeavesAZeroRowAlone) {
    VertexWeights v = vw({{0, 0.0}, {1, 0.0}});
    WeightPaintOps::normalizeRow(v);
    // Inventing a distribution for an all-zero row would be worse than leaving
    // it visible to the caller.
    EXPECT_DOUBLE_EQ(rowSum(v), 0.0);

    VertexWeights u = vw({{0, 2.0}, {1, 2.0}});
    WeightPaintOps::normalizeRow(u);
    EXPECT_NEAR(rowSum(u), 1.0, 1e-9);
    EXPECT_NEAR(u.weights[0], 0.5, 1e-9);
}

// --- the dab ---------------------------------------------------------------

TEST(WeightPaintOpsTest, AddDabRaisesTheActiveBoneAndKeepsRowsNormalized) {
    Strip s(5);
    const double center[3] = {2.0, 0.0, 0.0};
    DabOptions o;
    o.radius = 2.5; o.strength = 1.0; o.falloff = 0.0; o.mode = BrushMode::Add;

    const int n = WeightPaintOps::applyDab(s.pos.data(), s.count(), s.w, center, 7, o);
    EXPECT_GT(n, 0);
    // Centre vertex is fully inside => bone 7 dominates there.
    EXPECT_GT(WeightPaintOps::weightOf(s.w[2], 7), 0.9);
    // Falloff means the edge is touched less than the centre.
    EXPECT_LT(WeightPaintOps::weightOf(s.w[0], 7),
              WeightPaintOps::weightOf(s.w[2], 7));
    for (const auto& v : s.w) EXPECT_NEAR(rowSum(v), 1.0, 1e-6) << "must stay normalized";
}

TEST(WeightPaintOpsTest, SubtractDabLowersTheActiveBoneAndRedistributes) {
    Strip s(3);
    // Give bone 7 half the weight first.
    for (auto& v : s.w) { WeightPaintOps::setWeight(v, 7, 1.0); WeightPaintOps::normalizeRow(v); }
    const double before = WeightPaintOps::weightOf(s.w[1], 7);
    ASSERT_GT(before, 0.0);

    const double center[3] = {1.0, 0.0, 0.0};
    DabOptions o;
    o.radius = 2.0; o.strength = 1.0; o.falloff = 0.0; o.mode = BrushMode::Subtract;
    EXPECT_GT(WeightPaintOps::applyDab(s.pos.data(), s.count(), s.w, center, 7, o), 0);

    EXPECT_LT(WeightPaintOps::weightOf(s.w[1], 7), before);
    // The removed influence went to the other bones, not into thin air.
    EXPECT_NEAR(rowSum(s.w[1]), 1.0, 1e-6);
}

TEST(WeightPaintOpsTest, BlurDabIsANoOpWithoutAdjacency) {
    Strip s(5);
    s.w[2] = vw({{0, 0.5}, {7, 0.5}});
    const auto before = s.w;
    const double center[3] = {2.0, 0.0, 0.0};
    DabOptions o;
    o.radius = 3.0; o.strength = 1.0; o.mode = BrushMode::Blur;

    // No adjacency supplied: must do nothing rather than silently acting like
    // Add, which would be a surprising substitution.
    EXPECT_EQ(WeightPaintOps::applyDab(s.pos.data(), s.count(), s.w, center, 7, o), 0);
    for (size_t i = 0; i < s.w.size(); ++i)
        EXPECT_NEAR(WeightPaintOps::weightOf(s.w[i], 7),
                    WeightPaintOps::weightOf(before[i], 7), 1e-12);
}

TEST(WeightPaintOpsTest, BlurDabEvensOutASpike) {
    Strip s(5);
    // A spike on the middle vertex, with neighbours already carrying SOME of
    // bone 7. Neighbours at exactly zero would mean blurring drives the centre
    // to zero on every bone, which the degenerate-row guard correctly refuses
    // (a vertex must keep some influence) — so that setup cannot demonstrate
    // blurring and is not what a real mesh looks like mid-stroke.
    s.w[2] = vw({{0, 0.0}, {7, 1.0}});
    s.w[1] = vw({{0, 0.7}, {7, 0.3}});
    s.w[3] = vw({{0, 0.7}, {7, 0.3}});
    const double spikeBefore = WeightPaintOps::weightOf(s.w[2], 7);
    const double neighbourBefore = WeightPaintOps::weightOf(s.w[1], 7);

    const double center[3] = {2.0, 0.0, 0.0};
    DabOptions o;
    o.radius = 3.0; o.strength = 1.0; o.falloff = 0.0; o.mode = BrushMode::Blur;
    EXPECT_GT(WeightPaintOps::applyDab(s.pos.data(), s.count(), s.w, center, 7, o,
                                       {}, s.adj), 0);

    EXPECT_LT(WeightPaintOps::weightOf(s.w[2], 7), spikeBefore) << "spike must come down";
    EXPECT_GT(WeightPaintOps::weightOf(s.w[1], 7), neighbourBefore)
        << "and its neighbours must come up";
}

TEST(WeightPaintOpsTest, DabRejectsBadInput) {
    Strip s(3);
    const double center[3] = {0.0, 0.0, 0.0};
    DabOptions o;
    o.radius = 1.0;
    EXPECT_EQ(WeightPaintOps::applyDab(nullptr, 3, s.w, center, 0, o), 0);
    EXPECT_EQ(WeightPaintOps::applyDab(s.pos.data(), 0, s.w, center, 0, o), 0);
    EXPECT_EQ(WeightPaintOps::applyDab(s.pos.data(), 3, s.w, center, -1, o), 0)
        << "no active bone => nothing to paint";
    DabOptions bad = o; bad.radius = 0.0;
    EXPECT_EQ(WeightPaintOps::applyDab(s.pos.data(), 3, s.w, center, 0, bad), 0);
    // Mismatched weights array must be refused, not read out of bounds.
    std::vector<VertexWeights> shortW(1);
    EXPECT_EQ(WeightPaintOps::applyDab(s.pos.data(), 3, shortW, center, 0, o), 0);
}

// --- lock bone -------------------------------------------------------------

TEST(WeightPaintOpsTest, PaintingALockedBoneDoesNothing) {
    Strip s(3);
    std::vector<std::uint8_t> locked(8, 0);
    locked[7] = 1;                       // bone 7 is frozen
    const double center[3] = {1.0, 0.0, 0.0};
    DabOptions o;
    o.radius = 2.0; o.strength = 1.0; o.mode = BrushMode::Add;

    // Silently painting and then having the renormalise revert it would look
    // like a broken brush, so the dab must refuse outright.
    EXPECT_EQ(WeightPaintOps::applyDab(s.pos.data(), s.count(), s.w, center, 7, o, locked), 0);
    for (const auto& v : s.w) EXPECT_DOUBLE_EQ(WeightPaintOps::weightOf(v, 7), 0.0);
}

TEST(WeightPaintOpsTest, LockedBoneKeepsItsValueWhileOthersAbsorb) {
    Strip s(1);
    // Bone 0 locked at 0.25; paint bone 7 and bone 0 must not move.
    s.w[0] = vw({{0, 0.25}, {1, 0.75}});
    std::vector<std::uint8_t> locked(8, 0);
    locked[0] = 1;

    const double center[3] = {0.0, 0.0, 0.0};
    DabOptions o;
    o.radius = 1.0; o.strength = 1.0; o.falloff = 0.0; o.mode = BrushMode::Add;
    EXPECT_GT(WeightPaintOps::applyDab(s.pos.data(), s.count(), s.w, center, 7, o, locked), 0);

    EXPECT_NEAR(WeightPaintOps::weightOf(s.w[0], 0), 0.25, 1e-6)
        << "a locked bone must hold its exact value";
    EXPECT_GT(WeightPaintOps::weightOf(s.w[0], 7), 0.0);
    EXPECT_NEAR(rowSum(s.w[0]), 1.0, 1e-6);
}

// --- fill connected --------------------------------------------------------

TEST(WeightPaintOpsTest, FillConnectedSpreadsByHopsAndRespectsMaxHops) {
    Strip s(6);
    const int n = WeightPaintOps::fillConnected(s.w, s.adj, /*seed=*/0, 7,
                                                /*strength=*/1.0, /*falloff=*/0.0,
                                                /*maxHops=*/2);
    EXPECT_GT(n, 0);
    EXPECT_GT(WeightPaintOps::weightOf(s.w[0], 7), 0.0) << "seed is filled";
    EXPECT_GT(WeightPaintOps::weightOf(s.w[0], 7),
              WeightPaintOps::weightOf(s.w[2], 7)) << "falloff by hop distance";
    // Beyond maxHops nothing is touched.
    EXPECT_DOUBLE_EQ(WeightPaintOps::weightOf(s.w[5], 7), 0.0);
}

TEST(WeightPaintOpsTest, FillConnectedDoesNotLeakAcrossDisconnectedPieces) {
    // Two 3-vertex strips, spatially adjacent but NOT connected in adjacency.
    // A radius-based fill would leak; a geodesic one must not.
    Strip s(6);
    s.adj[2].clear();
    s.adj[3].clear();
    s.adj[2].push_back(1);
    s.adj[3].push_back(4);

    WeightPaintOps::fillConnected(s.w, s.adj, /*seed=*/0, 7, 1.0, 0.0, /*maxHops=*/0);
    EXPECT_GT(WeightPaintOps::weightOf(s.w[2], 7), 0.0) << "same island filled";
    EXPECT_DOUBLE_EQ(WeightPaintOps::weightOf(s.w[3], 7), 0.0)
        << "the disconnected island must stay untouched";
}

TEST(WeightPaintOpsTest, FillConnectedRejectsBadInput) {
    Strip s(3);
    EXPECT_EQ(WeightPaintOps::fillConnected(s.w, s.adj, -1, 0, 1.0, 0.0), 0);
    EXPECT_EQ(WeightPaintOps::fillConnected(s.w, s.adj, 99, 0, 1.0, 0.0), 0);
    EXPECT_EQ(WeightPaintOps::fillConnected(s.w, s.adj, 0, -1, 1.0, 0.0), 0);
    std::vector<std::vector<int>> shortAdj(1);
    EXPECT_EQ(WeightPaintOps::fillConnected(s.w, shortAdj, 0, 0, 1.0, 0.0), 0);
}

// --- utility ops -----------------------------------------------------------

TEST(WeightPaintOpsTest, NormalizeMakesEveryRowSumToOneWithoutDroppingInfluences) {
    std::vector<VertexWeights> w{
        vw({{0, 2.0}, {1, 2.0}}),
        vw({{0, 0.1}, {1, 0.2}, {2, 0.3}, {3, 0.001}}),
    };
    const int countBefore = w[1].count;
    WeightPaintOps::normalize(w);
    for (const auto& v : w) EXPECT_NEAR(rowSum(v), 1.0, 1e-9);
    // Normalize must NOT prune: a tiny influence survives (that is
    // limitInfluences' job, not normalize's).
    EXPECT_EQ(w[1].count, countBefore);
}

TEST(WeightPaintOpsTest, LimitInfluencesKeepsTheLargestAndRenormalizes) {
    std::vector<VertexWeights> w{
        vw({{0, 0.05}, {1, 0.40}, {2, 0.30}, {3, 0.20}, {4, 0.05}}),
    };
    WeightPaintOps::limitInfluences(w, 3);
    EXPECT_LE(w[0].count, 3);
    EXPECT_NEAR(rowSum(w[0]), 1.0, 1e-9);
    // The three biggest (bones 1, 2, 3) survive; the 0.05s are dropped.
    EXPECT_GT(WeightPaintOps::weightOf(w[0], 1), 0.0);
    EXPECT_GT(WeightPaintOps::weightOf(w[0], 2), 0.0);
    EXPECT_GT(WeightPaintOps::weightOf(w[0], 3), 0.0);
    EXPECT_DOUBLE_EQ(WeightPaintOps::weightOf(w[0], 0), 0.0);
    EXPECT_DOUBLE_EQ(WeightPaintOps::weightOf(w[0], 4), 0.0);
}

TEST(WeightPaintOpsTest, SmoothEvensOutAStepAndHonoursLockedVertices) {
    Strip s(5);
    for (auto& v : s.w) v = vw({{0, 1.0}, {7, 0.0}});
    s.w[2] = vw({{0, 0.0}, {7, 1.0}});          // a spike in the middle

    auto locked = std::vector<std::uint8_t>(s.w.size(), 0);
    locked[2] = 1;                               // freeze the spike itself
    const double spike = WeightPaintOps::weightOf(s.w[2], 7);

    WeightPaintOps::smooth(s.w, s.adj, /*iterations=*/2, locked);
    EXPECT_NEAR(WeightPaintOps::weightOf(s.w[2], 7), spike, 1e-9)
        << "a locked vertex must be bit-unchanged";
    EXPECT_GT(WeightPaintOps::weightOf(s.w[1], 7), 0.0)
        << "but it still bleeds into its neighbours";
}

// --- mirror ----------------------------------------------------------------

TEST(WeightPaintOpsTest, MirrorCopiesWeightsAcrossTheAxis) {
    // Symmetric pair at x = -1 and x = +1 about pivot 0.
    std::vector<float> pos{-1.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f};
    std::vector<VertexWeights> w{
        vw({{0, 1.0}}),          // left: all bone 0
        vw({{7, 1.0}}),          // right: all bone 7
    };
    const int n = WeightPaintOps::mirrorByPosition(pos.data(), 2, w, /*axis=*/0,
                                                   /*pivot=*/0.0, /*tolerance=*/0.1);
    EXPECT_EQ(n, 2);
    // Each vertex takes its mirror partner's weights — read from a snapshot, so
    // they swap rather than both ending up the same.
    EXPECT_NEAR(WeightPaintOps::weightOf(w[0], 7), 1.0, 1e-9);
    EXPECT_NEAR(WeightPaintOps::weightOf(w[1], 0), 1.0, 1e-9);
}

TEST(WeightPaintOpsTest, MirrorLeavesAVertexAloneWhenNoPartnerIsInTolerance) {
    // A lone vertex far off-axis has no mirror partner.
    std::vector<float> pos{5.0f, 0.0f, 0.0f};
    std::vector<VertexWeights> w{vw({{0, 1.0}})};
    const int n = WeightPaintOps::mirrorByPosition(pos.data(), 1, w, 0, 0.0,
                                                   /*tolerance=*/0.1);
    EXPECT_EQ(n, 0) << "no partner within tolerance => leave the weights alone";
    EXPECT_NEAR(WeightPaintOps::weightOf(w[0], 0), 1.0, 1e-9);
}

TEST(WeightPaintOpsTest, MirrorRejectsBadInput) {
    std::vector<float> pos{0.0f, 0.0f, 0.0f};
    std::vector<VertexWeights> w{vw({{0, 1.0}})};
    EXPECT_EQ(WeightPaintOps::mirrorByPosition(nullptr, 1, w, 0, 0.0, 0.1), 0);
    EXPECT_EQ(WeightPaintOps::mirrorByPosition(pos.data(), 1, w, /*axis=*/3, 0.0, 0.1), 0);
    EXPECT_EQ(WeightPaintOps::mirrorByPosition(pos.data(), 1, w, 0, 0.0,
                                               /*tolerance=*/0.0), 0);
    std::vector<VertexWeights> shortW;
    EXPECT_EQ(WeightPaintOps::mirrorByPosition(pos.data(), 1, shortW, 0, 0.0, 0.1), 0);
}

TEST(WeightPaintOpsTest, TotalWeightOnBoneTracksPaintedChange) {
    Strip s(4);
    EXPECT_DOUBLE_EQ(WeightPaintOps::totalWeightOnBone(s.w, 7), 0.0);
    const double center[3] = {1.5, 0.0, 0.0};
    DabOptions o;
    o.radius = 5.0; o.strength = 1.0; o.falloff = 0.0;
    WeightPaintOps::applyDab(s.pos.data(), s.count(), s.w, center, 7, o);
    EXPECT_GT(WeightPaintOps::totalWeightOnBone(s.w, 7), 0.0)
        << "a readout must be able to show the paint actually landed";
}

// --- subtracting from a fully-weighted vertex (#558 follow-up) -------------

// Reported bug: once a vertex reached 1.0 it could never be painted DOWN again;
// below 1.0 subtract worked fine. Cause: at 1.0 after pruning, the painted bone
// is the row's ONLY influence, and since a row must sum to 1 the freed weight
// was handed straight back to it — pinning it at 1.0 forever.
TEST(WeightPaintOpsTest, SubtractWorksWhenTheActiveBoneIsTheSoleInfluence) {
    const float pos[3] = {0.f, 0.f, 0.f};
    const double centre[3] = {0.0, 0.0, 0.0};

    WeightPaintOps::DabOptions o;
    o.mode = WeightPaintOps::BrushMode::Subtract;
    o.radius = 1.0;
    o.strength = 0.5;
    o.falloff = 0.0;
    o.fallbackBoneHandle = 9;          // stands in for the parent bone

    std::vector<VertexWeights> w{vw({{3, 1.0}})};

    ASSERT_EQ(WeightPaintOps::applyDab(pos, 1, w, centre, 3, o), 1);

    const double painted = WeightPaintOps::weightOf(w[0], 3);
    EXPECT_LT(painted, 0.99)
        << "a vertex at 1.0 must be reducible; it stayed pinned at full weight";
    EXPECT_NEAR(painted, 0.5, 1e-6);

    // The freed weight has to go somewhere, or the row stops summing to 1 and
    // the later prune/renormalise springs the painted bone back to 1.0.
    EXPECT_NEAR(WeightPaintOps::weightOf(w[0], 9), 0.5, 1e-6)
        << "freed weight must land on the fallback bone";
    EXPECT_NEAR(rowSum(w[0]), 1.0, 1e-6) << "row must stay normalised";
}

// With no fallback available (single-bone skeleton) a sole influence genuinely
// cannot be reduced — the row has nowhere else to put the weight. Pin that so
// the behaviour is a documented limit rather than a silent regression.
TEST(WeightPaintOpsTest, SoleInfluenceStaysPinnedWithoutAFallbackBone) {
    const float pos[3] = {0.f, 0.f, 0.f};
    const double centre[3] = {0.0, 0.0, 0.0};

    WeightPaintOps::DabOptions o;
    o.mode = WeightPaintOps::BrushMode::Subtract;
    o.radius = 1.0;
    o.strength = 0.5;
    o.falloff = 0.0;
    o.fallbackBoneHandle = -1;         // none available

    std::vector<VertexWeights> w{vw({{3, 1.0}})};

    WeightPaintOps::applyDab(pos, 1, w, centre, 3, o);
    EXPECT_NEAR(WeightPaintOps::weightOf(w[0], 3), 1.0, 1e-6)
        << "with nowhere to move weight, the row must stay normalised at 1.0";
}

// An EXISTING zero-weight sibling must be preferred over inventing a new
// influence: it keeps the influence count down and respects the row the user
// already has.
TEST(WeightPaintOpsTest, SubtractPrefersAnExistingSiblingOverTheFallbackBone) {
    const float pos[3] = {0.f, 0.f, 0.f};
    const double centre[3] = {0.0, 0.0, 0.0};

    WeightPaintOps::DabOptions o;
    o.mode = WeightPaintOps::BrushMode::Subtract;
    o.radius = 1.0;
    o.strength = 0.5;
    o.falloff = 0.0;
    o.fallbackBoneHandle = 9;

    std::vector<VertexWeights> w{vw({{3, 1.0}, {7, 0.0}})};

    WeightPaintOps::applyDab(pos, 1, w, centre, 3, o);
    EXPECT_NEAR(WeightPaintOps::weightOf(w[0], 7), 0.5, 1e-6)
        << "the existing sibling should absorb the freed weight";
    EXPECT_NEAR(WeightPaintOps::weightOf(w[0], 9), 0.0, 1e-6)
        << "the fallback bone must not be added when a sibling already exists";
}

// A LOCKED bone must survive the post-dab influence cap untouched.
//
// SkinWeightsPost::pruneAndRenormalize is shared with the auto-skinners and has
// no lock concept: it keeps the top K by weight and renormalises everything. A
// locked bone holding a small weight on an over-cap row was therefore evicted
// by a dab that never touched it. Reported in review on PR #973.
TEST(WeightPaintOpsTest, PostDabInfluenceCapPreservesLockedBones) {
    const float pos[3] = {0.f, 0.f, 0.f};
    const double centre[3] = {0.0, 0.0, 0.0};

    WeightPaintOps::DabOptions o;
    o.mode = WeightPaintOps::BrushMode::Add;
    o.radius = 1.0;
    o.strength = 0.5;
    o.falloff = 0.0;
    o.maxInfluences = 4;                 // the default cap

    // Bone 5 is locked AND holds the smallest weight on a 5-influence row —
    // precisely what a top-4 prune evicts.
    std::vector<std::uint8_t> locked(16, 0);
    locked[5] = 1;

    std::vector<VertexWeights> w{
        vw({{1, 0.30}, {2, 0.30}, {3, 0.20}, {4, 0.19}, {5, 0.01}})};

    WeightPaintOps::applyDab(pos, 1, w, centre, 1, o, locked);

    EXPECT_NEAR(WeightPaintOps::weightOf(w[0], 5), 0.01, 1e-9)
        << "a locked bone must keep its EXACT weight through the cap";
    EXPECT_NEAR(rowSum(w[0]), 1.0, 1e-6) << "row must stay normalised";
}

// The cap still applies to the UNLOCKED remainder — preserving locks must not
// become a licence to exceed maxInfluences without bound.
TEST(WeightPaintOpsTest, PostDabInfluenceCapStillBoundsUnlockedBones) {
    const float pos[3] = {0.f, 0.f, 0.f};
    const double centre[3] = {0.0, 0.0, 0.0};

    WeightPaintOps::DabOptions o;
    o.mode = WeightPaintOps::BrushMode::Add;
    o.radius = 1.0;
    o.strength = 0.5;
    o.falloff = 0.0;
    o.maxInfluences = 2;

    std::vector<std::uint8_t> locked(16, 0);
    locked[5] = 1;

    std::vector<VertexWeights> w{
        vw({{1, 0.25}, {2, 0.25}, {3, 0.25}, {4, 0.15}, {5, 0.10}})};

    WeightPaintOps::applyDab(pos, 1, w, centre, 1, o, locked);

    int unlockedKept = 0;
    for (int k = 0; k < w[0].count; ++k)
        if (w[0].boneIndices[k] != 5) ++unlockedKept;
    EXPECT_LE(unlockedKept, 2) << "the cap must still bound unlocked influences";
    EXPECT_NEAR(WeightPaintOps::weightOf(w[0], 5), 0.10, 1e-9)
        << "the locked bone is kept on top of the cap, at its exact value";
    EXPECT_NEAR(rowSum(w[0]), 1.0, 1e-6);
}
