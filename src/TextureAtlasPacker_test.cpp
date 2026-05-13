#include <gtest/gtest.h>

#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <set>

#include "TextureAtlasPacker.h"

using namespace TextureAtlasPacker;

namespace {

// Helper: write a solid-colour PNG of given size to a temp dir.
QString writeSolidPng(const QTemporaryDir& dir,
                      const QString& name,
                      int w, int h, QRgb colour)
{
    QImage img(w, h, QImage::Format_RGBA8888);
    img.fill(colour);
    const QString path = dir.filePath(name);
    [&]() { ASSERT_TRUE(img.save(path, "PNG")) << path.toStdString(); }();
    return path;
}

} // namespace

TEST(TextureAtlasPackerTest, EmptyInputReturnsError)
{
    AtlasSpec spec;
    AtlasResult r = pack(spec);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

TEST(TextureAtlasPackerTest, ZeroSizeReturnsError)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;
    spec.sourcePaths << writeSolidPng(dir, "a.png", 4, 4, qRgba(255, 0, 0, 255));
    spec.atlasWidth = 0;
    AtlasResult r = pack(spec);
    EXPECT_FALSE(r.ok);
}

TEST(TextureAtlasPackerTest, OversizedInputReportsClearError)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;
    spec.sourcePaths << writeSolidPng(dir, "big.png", 100, 100, qRgba(0, 255, 0, 255));
    spec.atlasWidth = 64;
    spec.atlasHeight = 64;
    spec.padding = 0;
    AtlasResult r = pack(spec);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("larger than the atlas"))
        << r.error.toStdString();
}

TEST(TextureAtlasPackerTest, SingleImagePlacedAtPaddingOffset)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;
    spec.sourcePaths << writeSolidPng(dir, "tile.png", 16, 16, qRgba(255, 0, 0, 255));
    spec.atlasWidth = 64;
    spec.atlasHeight = 64;
    spec.padding = 2;

    AtlasResult r = pack(spec);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.tiles.size(), 1);
    EXPECT_EQ(r.tiles[0].x, 2);
    EXPECT_EQ(r.tiles[0].y, 2);
    EXPECT_EQ(r.tiles[0].width, 16);
    EXPECT_EQ(r.tiles[0].height, 16);
    // UVs are normalized against the atlas dimensions.
    EXPECT_NEAR(r.tiles[0].u0, 2.0f / 64.0f, 1e-6f);
    EXPECT_NEAR(r.tiles[0].v0, 2.0f / 64.0f, 1e-6f);
    EXPECT_NEAR(r.tiles[0].u1, 18.0f / 64.0f, 1e-6f);
    EXPECT_NEAR(r.tiles[0].v1, 18.0f / 64.0f, 1e-6f);
    // Composite should be red at the centre of the tile, transparent
    // outside.
    EXPECT_EQ(qRed(r.image.pixel(10, 10)), 255);
    EXPECT_EQ(qGreen(r.image.pixel(10, 10)), 0);
    EXPECT_EQ(qAlpha(r.image.pixel(50, 50)), 0);
}

TEST(TextureAtlasPackerTest, MultipleTilesFitOnOneShelfWhenWide)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;
    for (int i = 0; i < 4; ++i) {
        spec.sourcePaths << writeSolidPng(dir, QString("t%1.png").arg(i),
                                          16, 16, qRgba(50 * i, 0, 0, 255));
    }
    spec.atlasWidth = 128;
    spec.atlasHeight = 32;
    spec.padding = 0;

    AtlasResult r = pack(spec);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.tiles.size(), 4);
    // With no padding and equal heights, the four 16x16 tiles should sit
    // side-by-side starting at y=0.
    for (const auto& t : r.tiles) {
        EXPECT_EQ(t.y, 0);
        EXPECT_EQ(t.width, 16);
        EXPECT_EQ(t.height, 16);
    }
    // All four x coordinates should be unique and lie in [0, 112].
    std::set<int> xs;
    for (const auto& t : r.tiles) xs.insert(t.x);
    EXPECT_EQ(xs.size(), 4u);
}

TEST(TextureAtlasPackerTest, OverflowOpensNewShelf)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;
    // Three 32x32 tiles in a 64-wide atlas: row 1 fits two, the third
    // spills onto a new shelf at y=32.
    spec.sourcePaths << writeSolidPng(dir, "a.png", 32, 32, qRgba(255, 0, 0, 255));
    spec.sourcePaths << writeSolidPng(dir, "b.png", 32, 32, qRgba(0, 255, 0, 255));
    spec.sourcePaths << writeSolidPng(dir, "c.png", 32, 32, qRgba(0, 0, 255, 255));
    spec.atlasWidth = 64;
    spec.atlasHeight = 64;
    spec.padding = 0;

    AtlasResult r = pack(spec);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.tiles.size(), 3);
    int rowYTop = 0;
    int rowYBot = 0;
    for (const auto& t : r.tiles) {
        if (t.y == 0)  rowYTop++;
        if (t.y == 32) rowYBot++;
    }
    EXPECT_EQ(rowYTop, 2);
    EXPECT_EQ(rowYBot, 1);
}

TEST(TextureAtlasPackerTest, AtlasTooSmallReturnsError)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;
    for (int i = 0; i < 5; ++i) {
        spec.sourcePaths << writeSolidPng(dir, QString("t%1.png").arg(i),
                                          32, 32, qRgba(255, 0, 0, 255));
    }
    spec.atlasWidth = 64;     // only fits 4 of them (2x2)
    spec.atlasHeight = 64;
    spec.padding = 0;

    AtlasResult r = pack(spec);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("Atlas too small")) << r.error.toStdString();
}

TEST(TextureAtlasPackerTest, ManifestJsonShapeIsStable)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;
    spec.sourcePaths << writeSolidPng(dir, "tile.png", 16, 16, qRgba(255, 0, 0, 255));
    spec.atlasWidth = 32;
    spec.atlasHeight = 32;
    spec.padding = 1;

    AtlasResult r = pack(spec);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    const QString json = manifestToJson(r, spec.padding);

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    ASSERT_EQ(err.error, QJsonParseError::NoError) << err.errorString().toStdString();
    QJsonObject root = doc.object();
    EXPECT_EQ(root["width"].toInt(), 32);
    EXPECT_EQ(root["height"].toInt(), 32);
    EXPECT_EQ(root["padding"].toInt(), 1);

    QJsonArray tiles = root["tiles"].toArray();
    ASSERT_EQ(tiles.size(), 1);
    QJsonObject t0 = tiles[0].toObject();
    EXPECT_EQ(t0["w"].toInt(), 16);
    EXPECT_EQ(t0["h"].toInt(), 16);
    EXPECT_EQ(t0["x"].toInt(), 1);
    EXPECT_EQ(t0["y"].toInt(), 1);
    EXPECT_TRUE(t0.contains("source"));
    EXPECT_TRUE(t0.contains("u0"));
    EXPECT_TRUE(t0.contains("u1"));
    EXPECT_TRUE(t0.contains("v0"));
    EXPECT_TRUE(t0.contains("v1"));
}

TEST(TextureAtlasPackerTest, PackToFileWritesAtlas)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;
    spec.sourcePaths << writeSolidPng(dir, "tile.png", 8, 8, qRgba(0, 0, 255, 255));
    spec.atlasWidth = 16;
    spec.atlasHeight = 16;
    spec.padding = 0;

    const QString outPath = dir.filePath("atlas.png");
    AtlasResult r = packToFile(spec, outPath);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    QImage written;
    ASSERT_TRUE(written.load(outPath));
    EXPECT_EQ(written.width(), 16);
    EXPECT_EQ(written.height(), 16);
}
