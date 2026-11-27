// Generic segmentation pipeline (#1179) with a fake 2D segmenter: masks are
// the rendered faces of each ground-truth part, plus noise. Exercises lifting,
// the merge graph, the face vote, unseen faces and cleanup with no model.

#include "GenericSeg/GenericSegmenter.h"
#include "GenericSeg/GenericSegBenchmark.h"
#include "GenericSeg/GenericSegTestMeshes.h"
#include "GenericSeg/SamMaskProvider.h"
#include "GenericSeg/TriangleBvh.h"
#include "ModelDownloader.h"

#include <gtest/gtest.h>

#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

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
            Mask& m = byPart[claimedPart ? claimedPart(f) : mesh.truth[f]];
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
        if (wholeObject && !byPart.empty()) {
            Mask all = byPart.begin()->second;
            for (const auto& [p, m] : byPart)
                for (size_t i = 0; i < all.pixels.size(); ++i) all.pixels[i] |= m.pixels[i];
            all.score = 1.0f;
            out.push_back(all);
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
    bool wholeObject = false;   // also return one mask of everything (SAM does)
    std::function<bool(int face)> hide;   // faces never put in a mask
    std::function<int(int face)> claimedPart;   // the part every mask puts a face in (default: truth)
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

TEST(GenericSegmenter, AWholeObjectMaskInEveryViewDoesNotSwallowTheParts)
{
    // SAM returns the whole object as one of its masks in every image; those
    // masks form one big community covering every patch. Painting the masks
    // smallest-wins keeps them from outvoting the parts.
    const TestMesh table = GenericSegTest::makeTable();
    TruthMaskProvider provider(table);
    provider.wholeObject = true;
    const Result r = segment(table.positions.data(), table.vertexCount(),
                             table.indices.data(), table.indexCount(), provider, fastOptions());
    expectMatchesTruth(table, r, 5);
    // No "guards off" counter-check: on this synthetic table the unguarded
    // run does not collapse either (macOS: 8 pure parts, Linux: 5), so it
    // proved nothing and its outcome depended on platform floating point.
}

TEST(GenericSegmenter, AProviderExceptionBecomesAnError)
{
    struct Throwing : MaskProvider {
        std::vector<Mask> masksFor(const ViewImage&) override { throw std::bad_alloc(); }
    } provider;
    const TestMesh bar = GenericSegTest::makeBar();
    const Result r = segment(bar.positions.data(), bar.vertexCount(),
                             bar.indices.data(), bar.indexCount(), provider, fastOptions());
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

TEST(GenericSegmenter, GraphCutOverTheMemoryBudgetIsSkipped)
{
    // Over the (face x part) budget the refinement is skipped: same labels as
    // with the graph cut off, and the stats say so.
    const TestMesh table = GenericSegTest::makeTable();
    Options tiny = fastOptions();
    tiny.graphCutMaxEntries = 10;
    TruthMaskProvider a(table);
    const Result skipped = segment(table.positions.data(), table.vertexCount(),
                                   table.indices.data(), table.indexCount(), a, tiny);
    ASSERT_TRUE(skipped.ok);
    EXPECT_TRUE(skipped.stats.graphCutSkipped);
    Options off = fastOptions();
    off.graphCut = false;
    TruthMaskProvider b(table);
    const Result noCut = segment(table.positions.data(), table.vertexCount(),
                                 table.indices.data(), table.indexCount(), b, off);
    EXPECT_EQ(skipped.faceLabels, noCut.faceLabels);
    TruthMaskProvider c(table);
    EXPECT_FALSE(segment(table.positions.data(), table.vertexCount(), table.indices.data(),
                         table.indexCount(), c, fastOptions()).stats.graphCutSkipped);
}

TEST(GenericSegmenter, BothSidesOfAThinPartStayTogether)
{
    // A thin slab: left half part 0, right half part 1. Every view puts the
    // right half's UNDERSIDE in part 0's mask (the rat tail's underside,
    // which the views from below lump with the body). The vote alone gives
    // that underside part 0's label; the graph cut's through links (inward
    // rays that exit the slab right away) tie it to the top above it.
    TestMesh m;
    GenericSegTest::addBox(m, {-1.0f, -0.04f, -0.4f}, {1.0f, 0.04f, 0.4f}, 16,
                           [](float x, float, float) { return x < 0 ? 0 : 1; });
    auto underside = [&m](int f) { return m.centroid(f)[0] > 0.0f && m.centroid(f)[1] < -0.035f; };
    auto top = [&m](int f) { return m.centroid(f)[0] > 0.0f && m.centroid(f)[1] > 0.035f; };
    auto claimed = [&](int f) { return underside(f) ? 0 : m.truth[f]; };
    // Share of the right half's underside that got the label of the top above it.
    auto agreeing = [&](const Result& r) {
        std::map<int, int> topLabels;
        for (int f = 0; f < m.faceCount(); ++f) if (top(f)) ++topLabels[r.faceLabels[f]];
        int topLabel = -1, best = 0;
        for (const auto& [l, n] : topLabels) if (n > best) { best = n; topLabel = l; }
        int n = 0, same = 0;
        for (int f = 0; f < m.faceCount(); ++f)
            if (underside(f)) { ++n; same += r.faceLabels[f] == topLabel; }
        return double(same) / std::max(1, n);
    };

    TruthMaskProvider provider(m);
    provider.claimedPart = claimed;
    Options o = fastOptions();
    o.graphCut = false;
    const Result voted = segment(m.positions.data(), m.vertexCount(), m.indices.data(), m.indexCount(), provider, o);
    ASSERT_TRUE(voted.ok);
    EXPECT_LT(agreeing(voted), 0.5);

    TruthMaskProvider again(m);
    again.claimedPart = claimed;
    const Result r = segment(m.positions.data(), m.vertexCount(), m.indices.data(), m.indexCount(),
                             again, fastOptions());
    ASSERT_TRUE(r.ok);
    EXPECT_GT(agreeing(r), 0.95);
    expectMatchesTruth(m, r, 2);
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
    // A small end part (x > 0.75, the end cap + one ring: about 17 % of the
    // surface): a 20 % floor merges it into its neighbour, a 0 floor keeps it.
    // The boundary sits on a grid line; a centroid threshold inside a ring
    // would make a saw-tooth boundary the graph cut straightens onto the
    // nearest grid line, changing the part's size.
    TestMesh bar;
    GenericSegTest::addBox(bar, {-1.0f, -0.25f, -0.25f}, {1.0f, 0.25f, 0.25f}, 8,
                           [](float x, float, float) { return x < 0 ? 0 : (x > 0.75f ? 2 : 1); });
    TruthMaskProvider provider(bar);
    Options o = fastOptions();
    o.minPartFraction = 0.2f;
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

// Opt-in visual dump (no model). QTMESH_GENSEG_DUMP=<dir> writes:
//  - <name>_result.png: synthetic meshes segmented with noisy ground-truth
//    masks, one colour per predicted part, from 4 views;
//  - <obj>_views.png (with QTMESH_GENSEG_DUMP_OBJ=<a.obj;b.obj>): the normal
//    and shape-diameter images the 2D segmenter will be shown, 6 views.
// Run with --gtest_also_run_disabled_tests --gtest_filter='*VisualDump*'.
namespace {

QImage contactSheet(const std::vector<QImage>& tiles, int cols)
{
    if (tiles.empty()) return {};
    const int w = tiles[0].width(), h = tiles[0].height();
    const int rows = (int(tiles.size()) + cols - 1) / cols;
    QImage sheet(w * cols, h * rows, QImage::Format_RGB32);
    sheet.fill(Qt::black);
    for (size_t i = 0; i < tiles.size(); ++i)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                sheet.setPixel(int(i % cols) * w + x, int(i / cols) * h + y, tiles[i].pixel(x, y));
    return sheet;
}

QImage shadeLabels(const Raster& r, const SegMesh& mesh, const OrthoCamera& cam,
                   const std::vector<int>& faceLabels)
{
    static const QRgb palette[] = {0x808080, 0xe6194b, 0x3cb44b, 0xffe119, 0x4363d8, 0xf58231,
                                   0x911eb4, 0x46f0f0, 0xf032e6, 0xbcf60c, 0xfabebe, 0x008080,
                                   0xe6beff, 0x9a6324, 0xfffac8, 0x800000, 0xaaffc3, 0x808000,
                                   0xffd8b1, 0x000075, 0xffffff, 0x5f9ea0, 0xff7f50, 0x6a5acd};
    constexpr int kColours = int(sizeof(palette) / sizeof(palette[0]));
    const QImage matte = shadeMatte(r, mesh, cam);
    QImage img(r.size, r.size, QImage::Format_RGB32);
    img.fill(Qt::black);
    for (int y = 0; y < r.size; ++y)
        for (int x = 0; x < r.size; ++x) {
            const int f = r.at(x, y);
            if (f < 0) continue;
            const QRgb c = palette[faceLabels[f] == 0 ? 0 : 1 + (faceLabels[f] - 1) % (kColours - 1)];
            const float s = 0.35f + 0.65f * qGray(matte.pixel(x, y)) / 255.0f;
            img.setPixel(x, y, qRgb(int(qRed(c) * s), int(qGreen(c) * s), int(qBlue(c) * s)));
        }
    return img;
}

// Disk mask-cache key part identifying the mesh by content (two different
// "chair.obj" files must not share masks): FNV-1a over positions + indices.
QString meshKey(const std::vector<float>& pos, const std::vector<uint32_t>& idx)
{
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](const void* data, size_t bytes) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    };
    mix(pos.data(), pos.size() * sizeof(float));
    mix(idx.data(), idx.size() * sizeof(uint32_t));
    return QString::number(h, 16).left(12);
}

// Mask-cache key part identifying the SAM graphs: QTMESH_SAM2_DIR may point at
// any export, so the digests (not the folder) say whose masks are cached.
QString graphKey(const QString& samDir)
{
    QString key;
    for (const QString& f : {SamMaskProvider::encoderFileName(), SamMaskProvider::decoderFileName()})
        key += ModelDownloader::sha256HexOfFile(samDir + "/" + f).left(8);
    return key;
}

class CachedProvider : public MaskProvider {
public:
    CachedProvider(MaskProvider& p, std::map<std::pair<int, int>, std::vector<Mask>>& c) : inner(p), cache(c) {}
    std::vector<Mask> masksFor(const ViewImage& v) override
    {
        const auto key = std::make_pair(v.view, int(v.kind));
        auto it = cache.find(key);
        if (it == cache.end()) {
            // Optional disk cache (QTMESH_GENSEG_MASKCACHE=<dir>): iterate on the
            // post-processing without re-running SAM.
            const QString file = diskPrefix.isEmpty() ? QString()
                : QStringLiteral("%1_v%2_%3.masks").arg(diskPrefix).arg(v.view).arg(imageKindName(v.kind));
            std::vector<Mask> masks;
            bool loaded = false;
            if (!file.isEmpty()) {
                QFile f(file);
                if (f.open(QIODevice::ReadOnly)) {
                    QDataStream in(qUncompress(f.readAll()));
                    quint32 n = 0;
                    in >> n;
                    for (quint32 i = 0; i < n && in.status() == QDataStream::Ok; ++i) {
                        Mask m;
                        QByteArray px;
                        in >> m.score >> px;
                        m.pixels.assign(px.begin(), px.end());
                        masks.push_back(std::move(m));
                    }
                    loaded = in.status() == QDataStream::Ok;
                }
            }
            if (!loaded) {
                masks = inner.masksFor(v);
                if (!file.isEmpty()) {
                    QByteArray buf;
                    QDataStream out(&buf, QIODevice::WriteOnly);
                    out << quint32(masks.size());
                    for (const Mask& m : masks)
                        out << m.score << QByteArray(reinterpret_cast<const char*>(m.pixels.data()), int(m.pixels.size()));
                    QFile f(file);
                    if (f.open(QIODevice::WriteOnly)) f.write(qCompress(buf));
                }
            }
            it = cache.emplace(key, std::move(masks)).first;
            const QString dumpDir = qEnvironmentVariable("QTMESH_GENSEG_MASKDUMP");
            if (!dumpDir.isEmpty() && v.view < 3) {   // the image + one tile per mask
                std::vector<QImage> tiles{v.image.scaled(256, 256)};
                for (const Mask& m : it->second) {
                    QImage t = v.image.convertToFormat(QImage::Format_RGB32);
                    for (int y = 0; y < t.height(); ++y)
                        for (int x = 0; x < t.width(); ++x)
                            if (m.pixels[size_t(y) * t.width() + x]) t.setPixel(x, y, qRgb(255, 40, 40));
                    tiles.push_back(t.scaled(256, 256));
                }
                contactSheet(tiles, 8).save(QStringLiteral("%1/masks_v%2_%3.png").arg(dumpDir)
                                                .arg(v.view).arg(imageKindName(v.kind)));
            }
        }
        return it->second;
    }
    MaskProvider& inner;
    std::map<std::pair<int, int>, std::vector<Mask>>& cache;
    QString diskPrefix;
};

bool loadObj(const QString& path, std::vector<float>& pos, std::vector<uint32_t>& idx)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.startsWith("v ")) {
            const auto p = line.mid(2).simplified().split(' ');
            for (int k = 0; k < 3; ++k) pos.push_back(p.value(k).toFloat());
        } else if (line.startsWith("f ")) {
            const auto p = line.mid(2).simplified().split(' ');
            std::vector<uint32_t> poly;
            for (const auto& t : p) {
                const int v = t.split('/').value(0).toInt();
                poly.push_back(uint32_t(v > 0 ? v - 1 : int(pos.size() / 3) + v));
            }
            for (size_t k = 1; k + 1 < poly.size(); ++k)
                idx.insert(idx.end(), {poly[0], poly[k], poly[k + 1]});
        }
    }
    return !idx.empty();
}

} // namespace

TEST(GenericSegmenter, ViewCountIsAutomaticUnlessSet)
{
    // Benchmark result (slice 4): about 20-24 SAM images per mesh, i.e. 12
    // views of normal + SDF images, 20 views of the single matte image.
    Options o;
    EXPECT_EQ(o.views, 0);
    EXPECT_TRUE(usesMatteImages(o.matteBelowFaces - 1, o));
    EXPECT_EQ(viewCountFor(o.matteBelowFaces - 1, o), 20);
    EXPECT_EQ(imagesPerViewFor(o.matteBelowFaces - 1, o), 1);
    EXPECT_EQ(viewCountFor(o.matteBelowFaces, o), 12);
    EXPECT_EQ(imagesPerViewFor(o.matteBelowFaces, o), 2);
    o.sdfImages = false;
    EXPECT_EQ(imagesPerViewFor(o.matteBelowFaces, o), 1);
    o.views = 7;
    EXPECT_EQ(viewCountFor(10, o), 7);
    EXPECT_EQ(viewCountFor(100000, o), 7);
}

TEST(GenericSegmenter, SamModelsHonourTheNoDownloadSwitch)
{
    // The suite runs with QTMESH_SEGMENT_NO_DOWNLOAD set: missing graphs are
    // reported with the reason, never fetched. Present graphs (a developer
    // machine) are only verified against their pinned digests.
    ASSERT_FALSE(qEnvironmentVariableIsEmpty("QTMESH_SEGMENT_NO_DOWNLOAD"));
    QString why;
    const bool ok = SamMaskProvider::ensureModelsBlocking(&why);
#ifdef ENABLE_ONNX
    if (SamMaskProvider::modelsPresent()) {
        SUCCEED() << "graphs present; verified: " << ok << " " << why.toStdString();
    } else {
        EXPECT_FALSE(ok);
        EXPECT_TRUE(why.contains(QLatin1String("QTMESH_SEGMENT_NO_DOWNLOAD"))) << why.toStdString();
    }
#else
    EXPECT_FALSE(ok);
    EXPECT_TRUE(why.contains(QLatin1String("ENABLE_ONNX"))) << why.toStdString();
#endif
    EXPECT_TRUE(SamMaskProvider::modelDir().endsWith(QLatin1String("segment/")));
}

TEST(GenericSegmenter, DISABLED_VisualDump)
{
    const QString dir = qEnvironmentVariable("QTMESH_GENSEG_DUMP");
    if (dir.isEmpty()) GTEST_SKIP() << "set QTMESH_GENSEG_DUMP=<dir>";
    QDir().mkpath(dir);
    const std::vector<Vec3> dirs = {normalized(Vec3(0.5f, 0.4f, 1)), normalized(Vec3(-1, 0.3f, 0.4f)),
                                    normalized(Vec3(0.2f, 1, 0.3f)), normalized(Vec3(0.6f, -0.5f, -1))};

    struct Case { const char* name; TestMesh mesh; int merge, split; };
    std::vector<Case> cases;
    cases.push_back({"table", GenericSegTest::makeTable(), 6, 0});
    cases.push_back({"bar_noisy", GenericSegTest::makeBar(), 4, 5});
    {
        TestMesh m;
        GenericSegTest::addBox(m, {-1.0f, -0.25f, -0.25f}, {1.0f, 0.25f, 0.25f}, 40,
                               [](float x, float, float) { return x < -0.3f ? 0 : (x < 0.4f ? 1 : 2); });
        cases.push_back({"dense_bar_3parts", m, 5, 7});
    }
    for (Case& c : cases) {
        TruthMaskProvider provider(c.mesh);
        provider.mergeEvery = c.merge;
        provider.splitEvery = c.split;
        Options o;
        o.views = 20;
        o.resolution = 256;
        const Result r = segment(c.mesh.positions.data(), c.mesh.vertexCount(),
                                 c.mesh.indices.data(), c.mesh.indexCount(), provider, o);
        ASSERT_TRUE(r.ok) << c.name;
        const SegMesh sm = SegMesh::build(c.mesh.positions.data(), c.mesh.vertexCount(),
                                          c.mesh.indices.data(), c.mesh.indexCount());
        std::vector<int> truthLabels(c.mesh.truth.size());
        for (size_t i = 0; i < truthLabels.size(); ++i) truthLabels[i] = c.mesh.truth[i] + 1;
        std::vector<QImage> tiles;
        for (const auto& labels : {truthLabels, r.faceLabels})
            for (const Vec3& d : dirs) {
                const OrthoCamera cam = OrthoCamera::lookingFrom(d);
                const Raster ras = rasterize(sm, cam, 256);
                tiles.push_back(shadeLabels(ras, sm, cam, labels));
            }
        contactSheet(tiles, 4).save(dir + "/" + c.name + "_result.png");
        fprintf(stderr, "  %s: %d parts, %d masks, %d nodes, %d edges\n", c.name, r.parts,
                r.stats.masks, r.stats.nodes, r.stats.edges);
    }

    for (const QString& path : qEnvironmentVariable("QTMESH_GENSEG_DUMP_OBJ").split(';', Qt::SkipEmptyParts)) {
        std::vector<float> pos;
        std::vector<uint32_t> idx;
        ASSERT_TRUE(loadObj(path, pos, idx)) << qPrintable(path);
        const SegMesh sm = SegMesh::build(pos.data(), int(pos.size() / 3), idx.data(), int(idx.size()));
        std::vector<float> flat;
        for (const Vec3& p : sm.positions) flat.insert(flat.end(), {p.x, p.y, p.z});
        TriangleBvh bvh;
        bvh.build(flat.data(), int(sm.positions.size()), sm.indices.data(), int(sm.indices.size()));
        const std::vector<float> sdf = normalizeSdf(shapeDiameter(sm, bvh));
        std::vector<QImage> normals, sdfs;
        const std::vector<Vec3> all = viewDirections(20);
        for (int v : {0, 3, 6, 9, 12, 15}) {
            const OrthoCamera cam = OrthoCamera::lookingFrom(all[v]);
            const Raster ras = rasterize(sm, cam, 384);
            normals.push_back(shadeNormals(ras, sm));
            sdfs.push_back(shadeFaceScalar(ras, sdf));
        }
        normals.insert(normals.end(), sdfs.begin(), sdfs.end());
        const QString stem = QFileInfo(path).completeBaseName().replace(' ', '_');
        contactSheet(normals, 6).save(dir + "/" + stem + "_views.png");
        fprintf(stderr, "  %s: %d faces\n", qPrintable(stem), sm.faceCount());

        // With the exported SAM 2.1 graphs: the real pipeline, per granularity.
        // QTMESH_SAM2_DIR=<dir with the graphs>, or "download" for the app's own
        // first-use path (run with QTMESH_SEGMENT_NO_DOWNLOAD= to allow it).
        QString samDir = qEnvironmentVariable("QTMESH_SAM2_DIR");
        if (samDir.isEmpty()) continue;
        if (samDir == QLatin1String("download")) {
            QString why;
            ASSERT_TRUE(SamMaskProvider::ensureModelsBlocking(&why)) << why.toStdString();
            samDir = SamMaskProvider::modelDir();
        }
        SamMaskProvider::Options so;
        so.pointsPerSide = qEnvironmentVariableIntValue("QTMESH_SAM2_POINTS") > 0
                               ? qEnvironmentVariableIntValue("QTMESH_SAM2_POINTS") : so.pointsPerSide;
        SamMaskProvider sam(samDir + "/" + SamMaskProvider::encoderFileName(),
                            samDir + "/" + SamMaskProvider::decoderFileName(), so);
        ASSERT_TRUE(sam.ok()) << sam.error().toStdString();
        std::map<std::pair<int, int>, std::vector<Mask>> cache;   // same renders per granularity
        std::vector<QImage> tiles, detail;
        const QString cacheDir = qEnvironmentVariable("QTMESH_GENSEG_MASKCACHE");
        if (!cacheDir.isEmpty()) QDir().mkpath(cacheDir);
        const QString samGraphs = cacheDir.isEmpty() ? QString() : graphKey(samDir);
        std::vector<Granularity> grans{Granularity::Coarse, Granularity::Medium, Granularity::Fine};
        if (qEnvironmentVariableIsSet("QTMESH_GENSEG_GRAN"))
            grans = {granularityFromName(qEnvironmentVariable("QTMESH_GENSEG_GRAN"))};
        for (Granularity g : grans) {
            CachedProvider cached(sam, cache);
            Options o;
            o.granularity = g;
            if (!cacheDir.isEmpty())
                cached.diskPrefix = QStringLiteral("%1/%2_%3_v%4_r%5_p%6")
                                        .arg(cacheDir, QFileInfo(path).completeBaseName().replace(' ', '_'),
                                             meshKey(pos, idx) + "_" + samGraphs)
                                        .arg(viewCountFor(sm.faceCount(), o)).arg(o.resolution).arg(so.pointsPerSide);
            // Tuning knobs: QTMESH_GENSEG_GC="smoothness,through,thickness,voteSmoothing", or "off".
            const QStringList gc = qEnvironmentVariable("QTMESH_GENSEG_GC").split(',', Qt::SkipEmptyParts);
            if (gc.value(0) == QLatin1String("off")) o.graphCut = false;
            else {
                if (gc.size() > 0) o.smoothness = gc[0].toFloat();
                if (gc.size() > 1) o.throughWeight = gc[1].toFloat();
                if (gc.size() > 2) o.throughThickness = gc[2].toFloat();
                if (gc.size() > 3) o.voteSmoothing = gc[3].toFloat();
                if (gc.size() > 4) o.voteAreaExponent = gc[4].toFloat();
            }
            QElapsedTimer t;
            t.start();
            const Result r = segment(pos.data(), int(pos.size() / 3), idx.data(), int(idx.size()), cached, o);
            ASSERT_TRUE(r.ok) << r.error.toStdString();
            fprintf(stderr, "    %s: %d parts, %d masks, %d nodes, %d edges, %lld ms\n",
                    qPrintable(granularityName(g)), r.parts, r.stats.masks, r.stats.nodes,
                    r.stats.edges, t.elapsed());
            for (int v : {0, 6, 12, 15}) {
                const OrthoCamera cam = OrthoCamera::lookingFrom(all[v]);
                tiles.push_back(shadeLabels(rasterize(sm, cam, 384), sm, cam, r.faceLabels));
            }
            if (g == grans.back())   // close-up sheet: 8 views at 512 px
                for (int v : {0, 3, 6, 9, 12, 15, 17, 19}) {
                    const OrthoCamera cam = OrthoCamera::lookingFrom(all[v]);
                    detail.push_back(shadeLabels(rasterize(sm, cam, 512), sm, cam, r.faceLabels));
                }
        }
        contactSheet(detail, 4).save(dir + "/" + stem + "_detail_" + granularityName(grans.back()) + ".png");
        const auto tm = sam.timing();
        fprintf(stderr, "    SAM: %d images, encode %.0f ms, decode %.0f ms, post %.0f ms, "
                "%d prompts, %d candidates, %d kept\n", tm.images, tm.encodeMs, tm.decodeMs,
                tm.postMs, tm.prompts, tm.candidates, tm.kept);
        contactSheet(tiles, 4).save(dir + "/" + stem + "_sam_segments_p" + QString::number(so.pointsPerSide) + ".png");
    }
}

// Benchmark (#1179 slice 4) against CC0 models whose coloured pieces give a
// ground truth (GenericSegBenchmark.h). Opt-in:
//   QTMESH_GENSEG_BENCH=<dir of .obj | a.obj;b.obj>  QTMESH_SAM2_DIR=<graphs | download>
//   QTMESH_GENSEG_MASKCACHE=<dir>   SAM masks on disk: re-runs only redo the lifting
//   QTMESH_GENSEG_VARIANTS=low,dense  QTMESH_GENSEG_GRAN=fine,medium,coarse
//   QTMESH_GENSEG_OPT="views=12,smoothness=0.2,..."  (GenericSegBench::applyOverrides)
//   QTMESH_GENSEG_DUMP=<dir>        truth | prediction sheets
// Prints one line per (mesh, variant, granularity) and the means.
TEST(GenericSegmenter, DISABLED_Benchmark)
{
    const QString bench = qEnvironmentVariable("QTMESH_GENSEG_BENCH");
    QString samDir = qEnvironmentVariable("QTMESH_SAM2_DIR");
    if (bench.isEmpty() || samDir.isEmpty()) GTEST_SKIP() << "set QTMESH_GENSEG_BENCH and QTMESH_SAM2_DIR";
    if (samDir == QLatin1String("download")) {
        QString why;
        ASSERT_TRUE(SamMaskProvider::ensureModelsBlocking(&why)) << why.toStdString();
        samDir = SamMaskProvider::modelDir();
    }
    QStringList files;
    if (QFileInfo(bench).isDir()) {
        for (const QString& f : QDir(bench).entryList({QStringLiteral("*.obj")}, QDir::Files, QDir::Name))
            files << QDir(bench).filePath(f);
    } else {
        files = bench.split(';', Qt::SkipEmptyParts);
    }
    ASSERT_FALSE(files.isEmpty());
    const QStringList variants = qEnvironmentVariable("QTMESH_GENSEG_VARIANTS", QStringLiteral("low,dense"))
                                     .split(',', Qt::SkipEmptyParts);
    std::vector<Granularity> grans;
    for (const QString& g : qEnvironmentVariable("QTMESH_GENSEG_GRAN", QStringLiteral("coarse,medium,fine"))
                                .split(',', Qt::SkipEmptyParts))
        grans.push_back(granularityFromName(g));
    const QString optSpec = qEnvironmentVariable("QTMESH_GENSEG_OPT");
    {
        Options probe;
        const QStringList unknown = GenericSegBench::applyOverrides(probe, optSpec);
        ASSERT_TRUE(unknown.isEmpty()) << "unknown option: " << unknown.join(',').toStdString();
    }
    SamMaskProvider::Options so;
    if (qEnvironmentVariableIntValue("QTMESH_SAM2_POINTS") > 0)
        so.pointsPerSide = qEnvironmentVariableIntValue("QTMESH_SAM2_POINTS");
    so.preferGpu = qEnvironmentVariableIntValue("QTMESH_SAM2_GPU") > 0;
    SamMaskProvider sam(samDir + "/" + SamMaskProvider::encoderFileName(),
                        samDir + "/" + SamMaskProvider::decoderFileName(), so);
    ASSERT_TRUE(sam.ok()) << sam.error().toStdString();
    const QString cacheDir = qEnvironmentVariable("QTMESH_GENSEG_MASKCACHE");
    if (!cacheDir.isEmpty()) QDir().mkpath(cacheDir);
    const QString samGraphs = cacheDir.isEmpty() ? QString() : graphKey(samDir);
    const QString dumpDir = qEnvironmentVariable("QTMESH_GENSEG_DUMP");
    if (!dumpDir.isEmpty()) QDir().mkpath(dumpDir);

    struct Sum { double t = 0, p = 0, f = 0; long long ms = 0; int n = 0; };
    std::map<QString, Sum> sums;
    fprintf(stderr, "%-16s %-6s %-5s %-6s %6s %5s %5s %6s %6s %6s %7s\n", "mesh", "truth", "var", "gran", "faces",
            "truth", "pred", "tIoU", "pIoU", "F", "ms");
    for (const QString& path : files) {
        GenericSegBench::ObjMesh obj;
        ASSERT_TRUE(GenericSegBench::loadObj(path, obj)) << qPrintable(path);
        const QString stem = QFileInfo(path).completeBaseName().replace(' ', '_');
        const SegMesh lowMesh = SegMesh::build(obj.positions.data(), int(obj.positions.size() / 3),
                                               obj.indices.data(), int(obj.indices.size()));
        // Rigged assets: body regions from the skeleton (a painted animal's
        // colour patches are not its parts); else coloured pieces.
        const QString rigJson = QFileInfo(path).path() + "/" + QFileInfo(path).completeBaseName() + ".rig.json";
        std::vector<int> groups = GenericSegBench::rigFaceGroups(obj, rigJson);
        const QString truthKind = groups.empty() ? QStringLiteral("pieces") : QStringLiteral("rig");
        if (groups.empty()) groups = obj.faceGroup;
        const std::vector<int> lowTruth = GenericSegBench::truthRegions(lowMesh, groups);
        for (const QString& variant : variants) {
            std::vector<float> pos = obj.positions;
            std::vector<uint32_t> idx = obj.indices;
            std::vector<int> truth = lowTruth;
            if (variant == QLatin1String("dense")) {
                GenericSegBench::weld(pos, idx);
                for (int level = 0; level < 4 && idx.size() / 3 < 8000; ++level)
                    GenericSegBench::loopSubdivide(pos, idx, truth);
            }
            const SegMesh sm = SegMesh::build(pos.data(), int(pos.size() / 3), idx.data(), int(idx.size()));
            std::map<std::pair<int, int>, std::vector<Mask>> cache;
            for (Granularity g : grans) {
                Options o;
                o.granularity = g;
                GenericSegBench::applyOverrides(o, optSpec);
                CachedProvider cached(sam, cache);
                if (!cacheDir.isEmpty())
                    cached.diskPrefix = QStringLiteral("%1/%2_%3_%4_v%5_r%6_p%7_%8")
                                            .arg(cacheDir, stem, variant, meshKey(pos, idx) + "_" + samGraphs)
                                            .arg(viewCountFor(sm.faceCount(), o)).arg(o.resolution)
                                            .arg(so.pointsPerSide)
                                            .arg(sm.faceCount() < o.matteBelowFaces ? "matte" : "ns");
                QElapsedTimer t;
                t.start();
                const Result r = segment(pos.data(), int(pos.size() / 3), idx.data(), int(idx.size()), cached, o);
                const long long ms = t.elapsed();
                if (!r.ok) {   // scored as nothing found; the run goes on
                    fprintf(stderr, "%-22s %-5s %-6s FAILED: %s\n", qPrintable(stem.left(22)), qPrintable(variant),
                            qPrintable(granularityName(g)), qPrintable(r.error));
                    Sum& sum = sums[variant + QLatin1Char(' ') + granularityName(g)];
                    sum.ms += ms; ++sum.n;
                    continue;
                }
                const GenericSegBench::Score s = GenericSegBench::evaluate(sm.faceAreas, truth, r.faceLabels);
                fprintf(stderr, "%-16s %-6s %-5s %-6s %6d %5d %5d %6.3f %6.3f %6.3f %7lld\n", qPrintable(stem.left(16)),
                        qPrintable(truthKind), qPrintable(variant), qPrintable(granularityName(g)), sm.faceCount(), s.truthParts,
                        s.predParts, s.truthIoU, s.predIoU, s.f, ms);
                Sum& sum = sums[variant + QLatin1Char(' ') + granularityName(g)];
                sum.t += s.truthIoU; sum.p += s.predIoU; sum.f += s.f; sum.ms += ms; ++sum.n;
                if (!dumpDir.isEmpty()) {
                    std::vector<int> truthLabels(truth.size());
                    for (size_t i = 0; i < truth.size(); ++i) truthLabels[i] = truth[i] + 1;
                    std::vector<QImage> tiles;
                    const std::vector<Vec3> all = viewDirections(20);
                    for (const std::vector<int>* labels : {static_cast<const std::vector<int>*>(&truthLabels), &r.faceLabels})
                        for (int v : {0, 6, 12, 15}) {
                            const OrthoCamera cam = OrthoCamera::lookingFrom(all[v]);
                            tiles.push_back(shadeLabels(rasterize(sm, cam, 320), sm, cam, *labels));
                        }
                    contactSheet(tiles, 4).save(QStringLiteral("%1/%2_%3_%4.png").arg(dumpDir, stem, variant,
                                                                                     granularityName(g)));
                }
            }
        }
    }
    fprintf(stderr, "\nmeans:\n");
    for (const auto& [k, s] : sums)
        fprintf(stderr, "  %-14s tIoU %.3f  pIoU %.3f  F %.3f  (%d meshes, %.1f s)\n", qPrintable(k),
                s.t / s.n, s.p / s.n, s.f / s.n, s.n, s.ms / 1000.0);
    const auto tm = sam.timing();
    fprintf(stderr, "SAM: %d images, encode %.0f ms, decode %.0f ms\n", tm.images, tm.encodeMs, tm.decodeMs);
}
