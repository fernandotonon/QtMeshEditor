// The benchmark's own measuring tools (#1179 slice 4): if the score or the
// truth regions are wrong, every tuning decision made with them is too.

#include "GenericSeg/GenericSegBenchmark.h"

#include <gtest/gtest.h>

using namespace GenericSegBench;

TEST(GenericSegBenchmark, PerfectPredictionScoresOne)
{
    const std::vector<float> area{1, 1, 2, 2};
    const std::vector<int> truth{0, 0, 1, 1};
    const Score s = evaluate(area, truth, {5, 5, 7, 7});
    EXPECT_DOUBLE_EQ(s.truthIoU, 1.0);
    EXPECT_DOUBLE_EQ(s.predIoU, 1.0);
    EXPECT_DOUBLE_EQ(s.f, 1.0);
    EXPECT_EQ(s.truthParts, 2);
    EXPECT_EQ(s.predParts, 2);
}

TEST(GenericSegBenchmark, MergingAndSplittingAreBothPenalised)
{
    const std::vector<float> area{1, 1, 1, 1};
    const std::vector<int> truth{0, 0, 1, 1};
    // Everything in one part: each truth part has IoU 0.5, the one predicted part 0.5.
    const Score merged = evaluate(area, truth, {1, 1, 1, 1});
    EXPECT_DOUBLE_EQ(merged.truthIoU, 0.5);
    EXPECT_DOUBLE_EQ(merged.predIoU, 0.5);
    // Every face its own part: truth parts 0.5, predicted parts 0.5.
    const Score split = evaluate(area, truth, {1, 2, 3, 4});
    EXPECT_DOUBLE_EQ(split.truthIoU, 0.5);
    EXPECT_DOUBLE_EQ(split.predIoU, 0.5);
}

TEST(GenericSegBenchmark, UnknownFacesCountAgainstTheTruthAndTinyPartsAreLeftOut)
{
    const std::vector<float> area{1, 1, 1, 1, 0.001f};
    const std::vector<int> truth{0, 0, 1, 1, 2};
    const Score s = evaluate(area, truth, {3, 0, 4, 4, 4}, 0.01);
    EXPECT_EQ(s.truthParts, 2);   // truth part 2 is below 1 %
    EXPECT_NEAR(s.truthIoU, (0.5 + 2.0 / 2.001) / 2.0, 1e-6);
    EXPECT_EQ(s.predParts, 2);
}

TEST(GenericSegBenchmark, TruthRegionsSplitAGroupIntoItsConnectedPieces)
{
    // Two separate quads in one group and one quad, touching the first, in another.
    const std::vector<float> pos{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0,
                                 5, 0, 0, 6, 0, 0, 6, 1, 0, 5, 1, 0,
                                 1, 0, 0, 2, 0, 0, 2, 1, 0, 1, 1, 0};
    const std::vector<uint32_t> idx{0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11};
    const GenericSeg::SegMesh m = GenericSeg::SegMesh::build(pos.data(), 12, idx.data(), int(idx.size()));
    const std::vector<int> r = truthRegions(m, {0, 0, 0, 0, 1, 1});
    EXPECT_EQ(r[0], r[1]);
    EXPECT_EQ(r[2], r[3]);
    EXPECT_EQ(r[4], r[5]);
    EXPECT_NE(r[0], r[2]);   // same group, not connected
    EXPECT_NE(r[0], r[4]);   // connected (welded by position), other group
}

TEST(GenericSegBenchmark, LoopSubdivisionKeepsLabelsAndSmoothsAClosedMesh)
{
    // Tetrahedron, two labels.
    std::vector<float> pos{1, 1, 1, -1, -1, 1, -1, 1, -1, 1, -1, -1};
    std::vector<uint32_t> idx{0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2};
    std::vector<int> labels{0, 0, 1, 1};
    loopSubdivide(pos, idx, labels);
    EXPECT_EQ(idx.size(), 48u);
    EXPECT_EQ(pos.size() / 3, 10u);   // 4 vertices + 6 edges
    EXPECT_EQ(std::count(labels.begin(), labels.end(), 0), 8);
    // The original corners move inward (the surface gets rounder).
    EXPECT_LT(std::abs(pos[0]), 1.0f);
    // Still closed: every edge has two faces.
    std::map<std::pair<uint32_t, uint32_t>, int> count;
    for (size_t f = 0; f < idx.size() / 3; ++f)
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = idx[f * 3 + k], b = idx[f * 3 + (k + 1) % 3];
            ++count[{std::min(a, b), std::max(a, b)}];
        }
    for (const auto& [e, c] : count) EXPECT_EQ(c, 2);
}

TEST(GenericSegBenchmark, WeldMergesCoincidentVertices)
{
    std::vector<float> pos{0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
    std::vector<uint32_t> idx{0, 1, 2, 3, 5, 4};
    weld(pos, idx);
    EXPECT_EQ(pos.size() / 3, 4u);
    EXPECT_EQ(idx[1], idx[3]);
    EXPECT_EQ(idx[2], idx[5]);
}

TEST(GenericSegBenchmark, OverridesSetKnownOptionsAndReportUnknownOnes)
{
    GenericSeg::Options o;
    const QStringList unknown = applyOverrides(o, QStringLiteral("views=12,smoothness=0.25,nope=1"));
    EXPECT_EQ(o.views, 12);
    EXPECT_FLOAT_EQ(o.smoothness, 0.25f);
    EXPECT_EQ(unknown, QStringList{QStringLiteral("nope")});
}
