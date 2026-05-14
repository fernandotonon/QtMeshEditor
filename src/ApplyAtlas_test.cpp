#include <gtest/gtest.h>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "ApplyAtlas.h"

using namespace ApplyAtlas;

namespace {

// Build a minimal manifest JSON in memory — matches the shape that
// TextureAtlasPacker::manifestToJson emits, so the parse path is the
// same one production code travels.
QByteArray buildManifestJson(int w, int h, int padding,
                             const QList<ManifestTile>& tiles)
{
    QJsonObject root;
    root["width"]   = w;
    root["height"]  = h;
    root["padding"] = padding;
    QJsonArray arr;
    for (const auto& t : tiles) {
        QJsonObject o;
        o["source"] = t.sourcePath;
        o["x"]      = t.x;
        o["y"]      = t.y;
        o["w"]      = t.w;
        o["h"]      = t.h;
        o["u0"]     = static_cast<double>(t.u0);
        o["v0"]     = static_cast<double>(t.v0);
        o["u1"]     = static_cast<double>(t.u1);
        o["v1"]     = static_cast<double>(t.v1);
        arr.append(o);
    }
    root["tiles"] = arr;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace

// -------- parseManifestJson --------

TEST(ApplyAtlasStandaloneTest, ParseValidManifestRoundtripsTiles)
{
    QList<ManifestTile> in;
    in.push_back(ManifestTile{
        "/some/dir/diffuse.png", 4, 4, 1024, 1024,
        0.001953125f, 0.001953125f, 0.501953125f, 0.501953125f});
    in.push_back(ManifestTile{
        "soccer.png", 1036, 4, 256, 256,
        0.505859375f, 0.001953125f, 0.630859375f, 0.126953125f});
    const QByteArray json = buildManifestJson(2048, 2048, 4, in);

    const ParseResult r = parseManifestJson(json);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.manifest.width, 2048);
    EXPECT_EQ(r.manifest.height, 2048);
    EXPECT_EQ(r.manifest.padding, 4);
    ASSERT_EQ(r.manifest.tiles.size(), 2);
    EXPECT_EQ(r.manifest.tiles[0].sourcePath.toStdString(), "/some/dir/diffuse.png");
    EXPECT_EQ(r.manifest.tiles[0].w, 1024);
    EXPECT_FLOAT_EQ(r.manifest.tiles[0].u0, 0.001953125f);
    EXPECT_FLOAT_EQ(r.manifest.tiles[0].u1, 0.501953125f);
    EXPECT_EQ(r.manifest.tiles[1].sourcePath.toStdString(), "soccer.png");
}

TEST(ApplyAtlasStandaloneTest, ParseRejectsMalformedJson)
{
    const QByteArray junk = "{not actually json";
    const ParseResult r = parseManifestJson(junk);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("parse error", Qt::CaseInsensitive));
}

TEST(ApplyAtlasStandaloneTest, ParseRejectsMissingTileField)
{
    // Hand-build a tile that's missing 'v1' so the strict validator
    // surfaces the specific missing key instead of accepting a half-
    // baked manifest and silently producing wrong UVs downstream.
    QJsonObject root;
    root["width"] = 1024;
    root["height"] = 1024;
    root["padding"] = 2;
    QJsonObject tile;
    tile["source"] = "x.png";
    tile["x"] = 0; tile["y"] = 0; tile["w"] = 64; tile["h"] = 64;
    tile["u0"] = 0.0; tile["v0"] = 0.0; tile["u1"] = 0.5;
    // (v1 missing intentionally)
    QJsonArray arr; arr.append(tile);
    root["tiles"] = arr;
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);

    const ParseResult r = parseManifestJson(json);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("v1")) << r.error.toStdString();
}

TEST(ApplyAtlasStandaloneTest, ParseRejectsZeroDimensions)
{
    QJsonObject root;
    root["width"] = 0; root["height"] = 1024;
    root["tiles"] = QJsonArray{};
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);

    const ParseResult r = parseManifestJson(json);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("width") || r.error.contains("height"))
        << r.error.toStdString();
}

// -------- ApplyReport JSON --------

TEST(ApplyAtlasStandaloneTest, ReportJsonHasExpectedShape)
{
    ApplyReport rep;
    rep.ok = true;
    SubmeshReport s;
    s.submeshIndex = 0;
    s.materialName = "Skin_MAT";
    s.diffuseTextureName = "Boss_diffuse.png";
    s.matchedTileSource = "/abs/Boss_diffuse.png";
    s.uvsRewritten = true;
    s.materialUpdated = true;
    s.verticesTouched = 1234;
    s.outOfRangeUVs = 0;
    s.strippedExtraTextures = 2;  // e.g. normal + AO
    rep.submeshes.append(s);

    const QJsonObject o = rep.toJson();
    EXPECT_TRUE(o.value("ok").toBool());
    EXPECT_EQ(o.value("submeshCount").toInt(), 1);
    EXPECT_EQ(o.value("rewrittenCount").toInt(), 1);
    ASSERT_TRUE(o.value("submeshes").isArray());
    const QJsonObject sub = o.value("submeshes").toArray().first().toObject();
    EXPECT_EQ(sub.value("submeshIndex").toInt(), 0);
    EXPECT_EQ(sub.value("verticesTouched").toInt(), 1234);
    EXPECT_EQ(sub.value("strippedExtraTextures").toInt(), 2);
    EXPECT_TRUE(sub.value("uvsRewritten").toBool());
}

TEST(ApplyAtlasStandaloneTest, RewrittenCountIgnoresUnmatchedSubmeshes)
{
    ApplyReport rep;
    SubmeshReport matched; matched.uvsRewritten = true;
    SubmeshReport unmatched; unmatched.uvsRewritten = false;
    rep.submeshes << matched << unmatched << matched;
    EXPECT_EQ(rep.rewrittenCount(), 2);
}
