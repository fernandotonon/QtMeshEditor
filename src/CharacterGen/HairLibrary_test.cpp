#include <gtest/gtest.h>

#include "HairLibrary.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

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
    EXPECT_TRUE(Library::remove(id));
    EXPECT_TRUE(list().isEmpty());
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
