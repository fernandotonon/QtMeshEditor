#include <gtest/gtest.h>

#include "HairLibrary.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include <limits>

using namespace CharacterGen::Hair;
using namespace CharacterGen::Hair::Library;

namespace {

struct LibFixture : ::testing::Test {
    QTemporaryDir tmp;
    void SetUp() override { qputenv("QTMESH_HAIR_LIBRARY_DIR", (tmp.path() + "/lib").toLocal8Bit()); }
    void TearDown() override { qunsetenv("QTMESH_HAIR_LIBRARY_DIR"); }
};

ImportedHair quad(float x0 = 0)
{
    ImportedHair h;
    h.name = "Test Quad";
    h.pos = {{x0, 0, 0}, {x0 + 1, 0, 0}, {x0 + 1, 1, 0}, {x0, 1, 0}};
    h.uv = {0, 0, 1, 0, 1, 1, 0, 1};
    h.idx = {0, 1, 2, 0, 2, 3};
    return h;
}

void writeObj(const QString& path)
{
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("o Bob_Cut\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nf 1/1 2/2 3/3 4/4\n"
            "o Long_Wavy\nv 0 0 1\nv 1 0 1\nv 1 -2 1\nv 0 -2 1\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nf 5/5 6/6 7/7 8/8\n");
}

}  // namespace

TEST_F(LibFixture, SaveLoadRoundTripKeepsGeometryUvsAndMetadata)
{
    Meta m;
    m.name = "Test Quad";
    m.author = "Someone";
    m.license = "CC-BY-4.0";
    m.source = "https://example.invalid/hair";
    QString id, dup;
    ASSERT_TRUE(save(quad(), m, &id, &dup));
    ASSERT_FALSE(id.isEmpty());
    ImportedHair h;
    Meta back;
    ASSERT_TRUE(load(id, &h, &back));
    EXPECT_EQ(h.pos.size(), 4u);
    EXPECT_EQ(h.idx.size(), 6u);
    EXPECT_FLOAT_EQ(h.uv[2], 1.0f);
    EXPECT_EQ(back.license, "CC-BY-4.0");
    EXPECT_EQ(back.author, "Someone");
    EXPECT_EQ(back.triangles, 2);
    EXPECT_EQ(list().size(), 1);
}

TEST_F(LibFixture, DuplicateGeometryIsNotRegisteredTwice)
{
    QString id, dup;
    ASSERT_TRUE(save(quad(), Meta(), &id, &dup));
    ImportedHair reordered = quad();
    std::swap(reordered.idx[0], reordered.idx[3]);    // same triangles, different order of the first corner reading
    reordered.idx = {0, 2, 3, 0, 1, 2};               // same two triangles, other order and winding start
    QString id2, dup2;
    ASSERT_TRUE(save(reordered, Meta(), &id2, &dup2));
    EXPECT_TRUE(id2.isEmpty());
    EXPECT_EQ(dup2, id);
    EXPECT_EQ(list().size(), 1);
    QString id3, dup3;
    ASSERT_TRUE(save(quad(5.0f), Meta(), &id3, &dup3));
    EXPECT_FALSE(id3.isEmpty());
    EXPECT_EQ(list().size(), 2);
}

TEST_F(LibFixture, RemoveAndBadIdsAreSafe)
{
    QString id, dup;
    ASSERT_TRUE(save(quad(), Meta(), &id, &dup));
    EXPECT_FALSE(Library::remove(QStringLiteral("../escape")));
    EXPECT_FALSE(load(QStringLiteral("../escape"), nullptr));
    ImportedHair h;
    EXPECT_FALSE(load(QStringLiteral("../../etc"), &h));
    EXPECT_FALSE(Library::remove(QStringLiteral(".")));          // "." would have meant the whole imported folder
    EXPECT_EQ(list().size(), 1);
    EXPECT_TRUE(Library::remove(id));
    EXPECT_TRUE(list().isEmpty());
    // removed, not destroyed: undo may still have to rebuild a character that used it, until the trash is emptied
    EXPECT_TRUE(load(id, &h));
    Library::purgeTrash();
    EXPECT_FALSE(load(id, &h));
}

TEST_F(LibFixture, ADamagedMetaFileMakesTheEntryDamaged)
{
    QString id, dup;
    ASSERT_TRUE(save(quad(), Meta(), &id, &dup));
    QFile meta(Library::dir() + "/imported/" + id + "/meta.json");
    ASSERT_TRUE(meta.open(QIODevice::WriteOnly | QIODevice::Truncate));
    meta.write("[ not an object");
    meta.close();
    ImportedHair h;
    EXPECT_FALSE(load(id, &h));
}

TEST_F(LibFixture, SaveRefusesWhatLoadCouldNotRead)
{
    ImportedHair h = quad();
    h.pos[1].x = std::numeric_limits<float>::quiet_NaN();
    QString id, dup, err;
    EXPECT_FALSE(save(h, Meta(), &id, &dup, &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_TRUE(list().isEmpty());
    ImportedHair odd = quad();
    odd.idx.pop_back();                                          // not a multiple of 3
    EXPECT_FALSE(save(odd, Meta(), &id, &dup, &err));
}

TEST_F(LibFixture, PresetNamesThatSanitiseAlikeStaySeparateFiles)
{
    Params a = presetParams("curly"), b = presetParams("bob");
    ASSERT_TRUE(savePreset("Long/curls", a));
    ASSERT_TRUE(savePreset("Long?curls", b));
    const QStringList names = presetNames();
    EXPECT_TRUE(names.contains("Long/curls"));
    EXPECT_TRUE(names.contains("Long?curls"));
    Params qa, qb;
    ASSERT_TRUE(loadPreset("Long/curls", &qa));
    ASSERT_TRUE(loadPreset("Long?curls", &qb));
    EXPECT_NE(qa.curliness, qb.curliness);                       // neither overwrote the other
    EXPECT_TRUE(removePreset("Long/curls"));
    EXPECT_TRUE(loadPreset("Long?curls", &qb));
}

TEST_F(LibFixture, AConfigWithoutParametersResolvesTheNamedPreset)
{
    StudioConfig c;
    QJsonObject o = toJson(c);
    o["style"] = "proc:curly";
    o.remove("procedural");
    StudioConfig back;
    ASSERT_TRUE(fromJson(o, &back));
    EXPECT_FLOAT_EQ(back.procedural.curliness, presetParams("curly").curliness);
    ASSERT_TRUE(savePreset("Mine", presetParams("afro")));
    o["style"] = "proc:Mine";
    ASSERT_TRUE(fromJson(o, &back));
    EXPECT_FLOAT_EQ(back.procedural.volume, presetParams("afro").volume);
    o["style"] = "proc:nonexistent";
    EXPECT_FALSE(fromJson(o, &back));                            // not silently default hair
}

TEST_F(LibFixture, DamagedEntryFailsToLoadInsteadOfCrashing)
{
    QString id, dup;
    ASSERT_TRUE(save(quad(), Meta(), &id, &dup));
    QFile f(dir() + "/imported/" + id + "/hair.qmh");
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("garbage");
    f.close();
    ImportedHair h;
    EXPECT_FALSE(load(id, &h));
}

TEST_F(LibFixture, TagsAreInferredFromGeometryAndName)
{
    ImportedHair h = quad();
    h.pos[2].y = h.pos[3].y = 3.0f;     // tall and narrow: long
    Meta m;
    m.name = "Curly Bob";
    inferTags(h, &m);
    EXPECT_EQ(m.length, "long");
    EXPECT_EQ(m.texture, "curly");
    EXPECT_EQ(m.style, "bob");
}

TEST_F(LibFixture, PresetsSaveLoadList)
{
    Params p = presetParams("curly");
    p.seed = 77;
    ASSERT_TRUE(savePreset("My curls", p));
    EXPECT_TRUE(presetNames().contains("My curls"));
    Params q;
    ASSERT_TRUE(loadPreset("My curls", &q));
    EXPECT_EQ(q.seed, 77u);
    EXPECT_FALSE(savePreset("  ", p));
    EXPECT_TRUE(removePreset("My curls"));
    EXPECT_FALSE(loadPreset("My curls", &q));
}

TEST_F(LibFixture, ImportsACollectionAsSeparateHairstylesWithoutDuplicates)
{
    const QString obj = tmp.path() + "/collection.obj";
    writeObj(obj);
    ImportOptions opt;
    opt.author = "Vincent";
    opt.license = "CC-BY-4.0";
    const ImportResult r = importFile(obj, opt);
    EXPECT_EQ(r.found, 2) << r.errors.join("; ").toStdString();
    EXPECT_EQ(r.added, 2);
    const QList<Meta> all = list();
    ASSERT_EQ(all.size(), 2);
    EXPECT_EQ(all[0].license, "CC-BY-4.0");
    EXPECT_EQ(all[0].collection, "collection");
    const ImportResult again = importFile(obj, opt);
    EXPECT_EQ(again.added, 0);
    EXPECT_EQ(again.duplicates, 2);
    EXPECT_EQ(list().size(), 2);
}

namespace {
QByteArray solidPng(const QColor& c, int w = 8, int h = 8)
{
    QImage img(w, h, QImage::Format_RGBA8888);
    img.fill(c);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}
}  // namespace

TEST(HairTextures, AStyleWithTwoMaterialsKeepsEachPartsTextureInAnAtlas)
{
    // two quads (4 vertices each), different materials with different textures: the merged mesh has ONE texture, so each part must still
    // sample its own colours (before, the first texture was used for every part and the other part rendered with the wrong colour/alpha)
    ImportedHair h;
    for (int part = 0; part < 2; ++part)
        for (const std::pair<float, float>& uv : {std::make_pair(0.0f, 0.0f), std::make_pair(1.0f, 0.0f), std::make_pair(1.0f, 1.0f), std::make_pair(0.0f, 1.0f)}) {
            h.pos.push_back({float(part), uv.first, uv.second});
            h.uv.push_back(uv.first);
            h.uv.push_back(uv.second);
        }
    const std::map<unsigned, QByteArray> png = {{0u, solidPng(QColor(220, 20, 20))}, {1u, solidPng(QColor(20, 20, 220), 16, 16)}};
    assignTextures(h, {{0, 4, 0}, {4, 4, 1}}, png);
    QImage atlas;
    ASSERT_TRUE(atlas.loadFromData(h.texturePng, "PNG"));
    EXPECT_EQ(atlas.width(), 16);                       // the common width
    EXPECT_EQ(atlas.height(), 8 * 2 + 16);              // the 8x8 red part scaled to 16 wide + the 16x16 blue one
    int redHits = 0, blueHits = 0;
    for (size_t v = 0; v < h.pos.size(); ++v) {
        const float u = h.uv[v * 2], vv = h.uv[v * 2 + 1];
        const QColor c = QColor::fromRgb(atlas.pixel(std::min(atlas.width() - 1, int(u * float(atlas.width()))), std::min(atlas.height() - 1, int((1.0f - vv) * float(atlas.height())))));
        const bool firstPart = v < 4;
        if (c.red() > 150 && c.blue() < 100) { ++redHits; EXPECT_TRUE(firstPart) << "vertex " << v; }
        if (c.blue() > 150 && c.red() < 100) { ++blueHits; EXPECT_FALSE(firstPart) << "vertex " << v; }
    }
    EXPECT_EQ(redHits, 4);
    EXPECT_EQ(blueHits, 4);
}

TEST(HairTextures, ASinglePartTextureIsKeptUntouchedAndAMissingOneGetsAFlatTile)
{
    ImportedHair one;
    one.uv = {0.25f, 0.5f, 0.75f, 0.5f};
    const QByteArray red = solidPng(QColor(220, 20, 20));
    assignTextures(one, {{0, 2, 7}}, {{7u, red}});
    EXPECT_EQ(one.texturePng, red);
    EXPECT_FLOAT_EQ(one.uv[0], 0.25f);

    ImportedHair mixed;
    mixed.uv = {0.5f, 0.5f, 0.5f, 0.5f};
    assignTextures(mixed, {{0, 1, 0}, {1, 1, 1}}, {{0u, red}, {1u, QByteArray()}});      // part 1 has no texture
    QImage atlas;
    ASSERT_TRUE(atlas.loadFromData(mixed.texturePng, "PNG"));
    const QColor c = QColor::fromRgb(atlas.pixel(int(mixed.uv[2] * float(atlas.width())), std::min(atlas.height() - 1, int((1.0f - mixed.uv[3]) * float(atlas.height())))));
    EXPECT_EQ(qAlpha(atlas.pixel(int(mixed.uv[2] * float(atlas.width())), std::min(atlas.height() - 1, int((1.0f - mixed.uv[3]) * float(atlas.height()))))), 255);   // opaque, not someone else's alpha
    EXPECT_NEAR(c.red(), 128, 2);
}

TEST_F(LibFixture, FolderImportSkipsUnrelatedFilesAndReportsBrokenOnes)
{
    QDir().mkpath(tmp.path() + "/src");
    writeObj(tmp.path() + "/src/a.obj");
    QFile junk(tmp.path() + "/src/readme.txt");
    ASSERT_TRUE(junk.open(QIODevice::WriteOnly));
    junk.write("not a model");
    junk.close();
    QFile broken(tmp.path() + "/src/broken.fbx");
    ASSERT_TRUE(broken.open(QIODevice::WriteOnly));
    broken.write("nope");
    broken.close();
    const ImportResult r = importFolder(tmp.path() + "/src", ImportOptions());
    EXPECT_EQ(r.added, 2);
    EXPECT_GE(r.failed, 1);
    EXPECT_FALSE(r.errors.isEmpty());
}
