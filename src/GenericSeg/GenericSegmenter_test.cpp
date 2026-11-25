// Generic segmentation pipeline (#1179) with a fake 2D segmenter: masks are
// the rendered faces of each ground-truth part, plus noise. Exercises lifting,
// the merge graph, the face vote, unseen faces and cleanup with no model.

#include "GenericSeg/GenericSegmenter.h"
#include "GenericSeg/GenericSegTestMeshes.h"

#include <gtest/gtest.h>

#include <QElapsedTimer>

#include <cstdio>

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>

using namespace GenericSeg;
using GenericSegTest::TestMesh;

namespace {

// Returns one mask per ground-truth part visible in the image. Optional noise:
// every `mergeEvery`-th image also gets one mask covering two parts, every
// `splitEvery`-th image splits each part mask in two halves (left/right).
class TruthMaskProvider : public MaskProvider {
public:
    explicit TruthMaskProvider(const TestMesh& m) : mesh(m) {}
    std::vector<Mask> masksFor(const ViewImage& v) override
    {
        ++calls;
        const Raster& r = *v.raster;
        std::map<int, Mask> byPart;
        for (size_t i = 0; i < r.face.size(); ++i) {
            const int f = r.face[i];
            if (f < 0 || (hide && hide(f))) continue;
            Mask& m = byPart[mesh.truth[f]];
            if (m.pixels.empty()) m.pixels.assign(r.face.size(), 0);
            m.pixels[i] = 1;
        }
        std::vector<Mask> out;
        for (auto& [p, m] : byPart) {
            (void)p;
            if (splitEvery > 0 && calls % splitEvery == 0) {
                Mask left = m, right = m;
                for (size_t i = 0; i < m.pixels.size(); ++i)
                    ((int(i % r.size) < r.size / 2) ? right : left).pixels[i] = 0;
                out.push_back(left);
                out.push_back(right);
            } else {
                out.push_back(m);
            }
        }
        if (mergeEvery > 0 && calls % mergeEvery == 0 && byPart.size() >= 2) {
            Mask merged = byPart.begin()->second;
            const Mask& second = std::next(byPart.begin())->second;
            for (size_t i = 0; i < merged.pixels.size(); ++i) merged.pixels[i] |= second.pixels[i];
            merged.score = 0.6f;
            out.push_back(merged);
        }
        return out;
    }
    const TestMesh& mesh;
    int calls = 0;
    int mergeEvery = 0;
    int splitEvery = 0;
    std::function<bool(int face)> hide;   // faces never put in a mask
};

Options fastOptions()
{
    Options o;
    o.views = 12;
    o.resolution = 128;
    return o;
}

// Each predicted part must be (almost) one true part, and each true part one
// predicted part: purity both ways, area-agnostic (face counts).
void expectMatchesTruth(const TestMesh& m, const Result& r, int expectedParts, double minPurity = 0.95)
{
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.parts, expectedParts);
    std::map<int, std::map<int, int>> predToTruth, truthToPred;
    for (int f = 0; f < m.faceCount(); ++f) {
        ++predToTruth[r.faceLabels[f]][m.truth[f]];
        ++truthToPred[m.truth[f]][r.faceLabels[f]];
    }
    auto purity = [](const std::map<int, std::map<int, int>>& table) {
        int best = 0, total = 0;
        for (const auto& [k, counts] : table) {
            (void)k;
            int mx = 0;
            for (const auto& [l, c] : counts) { (void)l; mx = std::max(mx, c); total += c; }
            best += mx;
        }
        return total ? double(best) / total : 0.0;
    };
    EXPECT_GE(purity(predToTruth), minPurity);
    EXPECT_GE(purity(truthToPred), minPurity);
}

} // namespace

TEST(GenericSegmenter, TableSplitsIntoTopAndFourLegs)
{
    const TestMesh table = GenericSegTest::makeTable();
    TruthMaskProvider provider(table);
    const Result r = segment(table.positions.data(), table.vertexCount(),
                             table.indices.data(), table.indexCount(), provider, fastOptions());
    expectMatchesTruth(table, r, 5);
    // part_01 is the largest part: the top.
    std::map<int, int> topVotes;
    for (int f = 0; f < table.faceCount(); ++f) if (table.truth[f] == 0) ++topVotes[r.faceLabels[f]];
    const auto topLabel = std::max_element(topVotes.begin(), topVotes.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; })->first;
    EXPECT_EQ(topLabel, 1);
    EXPECT_EQ(r.labelNames.size(), 6);
    EXPECT_EQ(r.labelNames[1], QStringLiteral("part_01"));
    EXPECT_EQ(int(r.vertexLabels.size()), table.vertexCount());
    EXPECT_EQ(r.stats.views, 12);
}

TEST(GenericSegmenter, ConnectedBarSplitsWhereTheMasksSay)
{
    // One connected box: geometry alone cannot split it, only the masks can.
    const TestMesh bar = GenericSegTest::makeBar();
    TruthMaskProvider provider(bar);
    const Result r = segment(bar.positions.data(), bar.vertexCount(),
                             bar.indices.data(), bar.indexCount(), provider, fastOptions());
    expectMatchesTruth(bar, r, 2);
}

TEST(GenericSegmenter, SurvivesMergedAndSplitMasks)
{
    // Some images merge both halves into one mask, some split every part in
    // two: the cross-view graph must still recover the two halves.
    const TestMesh bar = GenericSegTest::makeBar();
    TruthMaskProvider provider(bar);
    provider.mergeEvery = 4;
    provider.splitEvery = 5;
    const Result r = segment(bar.positions.data(), bar.vertexCount(),
                             bar.indices.data(), bar.indexCount(), provider, fastOptions());
    expectMatchesTruth(bar, r, 2, 0.9);
}

TEST(GenericSegmenter, FacesNoMaskCoversTakeTheirNeighboursPart)
{
    const TestMesh bar = GenericSegTest::makeBar();
    TruthMaskProvider provider(bar);
    provider.hide = [&](int f) { return bar.centroid(f)[0] < -0.9f; };   // the -X end
    const Result r = segment(bar.positions.data(), bar.vertexCount(),
                             bar.indices.data(), bar.indexCount(), provider, fastOptions());
    ASSERT_TRUE(r.ok);
    for (int f = 0; f < bar.faceCount(); ++f)
        EXPECT_GT(r.faceLabels[f], 0) << "face " << f;
    expectMatchesTruth(bar, r, 2);
}

TEST(GenericSegmenter, ShapeDiameterImagesPathRuns)
{
    // Above matteBelowFaces the normal + SDF images are rendered (2 per view).
    const TestMesh bar = GenericSegTest::makeBar();
    TruthMaskProvider provider(bar);
    Options o = fastOptions();
    o.matteBelowFaces = 0;
    const Result r = segment(bar.positions.data(), bar.vertexCount(),
                             bar.indices.data(), bar.indexCount(), provider, o);
    EXPECT_EQ(r.stats.images, 24);
    expectMatchesTruth(bar, r, 2);
}

TEST(GenericSegmenter, SmallPartsMergeIntoANeighbour)
{
    // A sliver part (x > 0.9, about 5 % of the surface): a 10 % floor merges
    // it into its neighbour, a 0 floor keeps it.
    TestMesh bar;
    GenericSegTest::addBox(bar, {-1.0f, -0.25f, -0.25f}, {1.0f, 0.25f, 0.25f}, 8,
                           [](float x, float, float) { return x < 0 ? 0 : (x > 0.9f ? 2 : 1); });
    TruthMaskProvider provider(bar);
    Options o = fastOptions();
    o.minPartFraction = 0.1f;
    const Result r = segment(bar.positions.data(), bar.vertexCount(),
                             bar.indices.data(), bar.indexCount(), provider, o);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.parts, 2);
    o.minPartFraction = 0.0f;
    const Result keep = segment(bar.positions.data(), bar.vertexCount(),
                                bar.indices.data(), bar.indexCount(), provider, o);
    EXPECT_EQ(keep.parts, 3);
}

TEST(GenericSegmenter, FailuresAndCancel)
{
    const TestMesh bar = GenericSegTest::makeBar();

    class NoMasks : public MaskProvider {
    public:
        std::vector<Mask> masksFor(const ViewImage&) override { return {}; }
    } none;
    const Result empty = segment(bar.positions.data(), bar.vertexCount(),
                                 bar.indices.data(), bar.indexCount(), none, fastOptions());
    EXPECT_FALSE(empty.ok);
    EXPECT_FALSE(empty.error.isEmpty());

    TruthMaskProvider provider(bar);
    const Result invalid = segment(nullptr, 0, nullptr, 0, provider, fastOptions());
    EXPECT_FALSE(invalid.ok);

    Options o = fastOptions();
    int calls = 0;
    o.progress = [&](const QString&, int, int) { return ++calls < 3; };
    const Result cancelled = segment(bar.positions.data(), bar.vertexCount(),
                                     bar.indices.data(), bar.indexCount(), provider, o);
    EXPECT_FALSE(cancelled.ok);
    EXPECT_TRUE(cancelled.cancelled);
}

TEST(GenericSegmenter, GranularityNamesRoundTrip)
{
    for (Granularity g : {Granularity::Coarse, Granularity::Medium, Granularity::Fine}) {
        bool ok = false;
        EXPECT_EQ(granularityFromName(granularityName(g), &ok), g);
        EXPECT_TRUE(ok);
    }
    bool ok = true;
    EXPECT_EQ(granularityFromName(QStringLiteral("nope"), &ok), Granularity::Medium);
    EXPECT_FALSE(ok);
    EXPECT_LT(leidenResolutionFor(Granularity::Coarse), leidenResolutionFor(Granularity::Fine));
    EXPECT_GT(minPartFractionFor(Granularity::Coarse), minPartFractionFor(Granularity::Fine));
}

TEST(GenericSegmenter, StableAcrossViewCountsAndResolutions)
{
    const TestMesh table = GenericSegTest::makeTable();
    for (int views : {12, 20})
        for (int res : {128, 256}) {
            TruthMaskProvider provider(table);
            provider.mergeEvery = 6;
            Options o;
            o.views = views;
            o.resolution = res;
            const Result r = segment(table.positions.data(), table.vertexCount(),
                                     table.indices.data(), table.indexCount(), provider, o);
            SCOPED_TRACE(testing::Message() << "views=" << views << " res=" << res);
            expectMatchesTruth(table, r, 5);
        }
}

// Opt-in timing on a dense mesh (no model): run with
// --gtest_also_run_disabled_tests --gtest_filter='*DenseMeshTiming*'.
TEST(GenericSegmenter, DISABLED_DenseMeshTiming)
{
    TestMesh bar;
    GenericSegTest::addBox(bar, {-1.0f, -0.25f, -0.25f}, {1.0f, 0.25f, 0.25f}, 90,
                           [](float x, float, float) { return x < -0.3f ? 0 : (x < 0.4f ? 1 : 2); });
    TruthMaskProvider provider(bar);
    Options o;   // defaults: 20 views x (normal + SDF), 512 px
    QElapsedTimer t;
    t.start();
    QString lastStage;
    qint64 stageStart = 0;
    o.progress = [&](const QString& stage, int, int) {
        if (stage != lastStage) {
            if (!lastStage.isEmpty())
                fprintf(stderr, "  %-15s %6lld ms\n", qPrintable(lastStage), t.elapsed() - stageStart);
            lastStage = stage;
            stageStart = t.elapsed();
        }
        return true;
    };
    const Result r = segment(bar.positions.data(), bar.vertexCount(),
                             bar.indices.data(), bar.indexCount(), provider, o);
    fprintf(stderr, "  %-15s %6lld ms\n  total %lld ms, %d faces, %d patches, %d nodes, %d edges\n",
            qPrintable(lastStage), t.elapsed() - stageStart, t.elapsed(), bar.faceCount(),
            r.stats.patches, r.stats.nodes, r.stats.edges);
    expectMatchesTruth(bar, r, 3);
}
