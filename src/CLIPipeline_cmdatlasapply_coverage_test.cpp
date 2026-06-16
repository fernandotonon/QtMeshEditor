// Coverage tests for CLIPipeline::cmdAtlasApply success path.
//
// CLIPipeline_test.cpp already covers cmdAtlasApply's early returns
// (missing args -> 2, invalid match mode -> 2, missing files -> 1, invalid
// manifest -> 1). This file exercises the SUCCESS path (CLIPipeline.cpp
// lines ~3703-3820): read manifest -> ApplyAtlas::parseManifestJson ->
// import -> addResourceLocation -> applyToEntity per-entity loop ->
// MeshImporterExporter::exporter -> JSON / text report emission.
//
// Branches covered:
//   - default text report branch
//   - --json report branch
//   - --match fullpath vs basename ApplyOptions wiring
//   - --no-clamp -> opts.clampOutOfRangeUVs=false (+ "skipped" suffix word)
//   - --keep-extras -> opts.stripNonDiffuseTextures=false
//   - totalSubmeshes / totalRewritten / totalOutOfRange aggregation
//   - a tile source that matches the mesh diffuse (rewrite reported) AND a
//     tile source that does not match (loop still imports/exports/reports)
//
// Distinct filename + distinct suite name (CLIPipelineCmdAtlasApplyCoverage)
// from the existing CLIPipelineCmdAtlasApply suite to avoid any ODR /
// duplicate-registration clash. The local helpers live in an anonymous
// namespace so they don't collide with the identically-named helpers in
// CLIPipeline_test.cpp.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <vector>

#include "CLIPipeline.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

namespace {

// --- RAII argv builder (mirrors the helper in CLIPipeline_test.cpp) ---------
class AtlasApplyArgv {
public:
    AtlasApplyArgv(std::initializer_list<const char*> args)
    {
        for (auto* a : args)
            m_storage.push_back(QByteArray(a));
        for (auto& ba : m_storage)
            m_argv.push_back(ba.data());
        m_argc = static_cast<int>(m_argv.size());
    }
    int argc() const { return m_argc; }
    char** argv() { return m_argv.data(); }
private:
    std::vector<QByteArray> m_storage;
    std::vector<char*> m_argv;
    int m_argc = 0;
};

// Write an OBJ with a UV channel + a material library that references a
// named diffuse texture. The diffuse name is what ApplyAtlas matches the
// manifest tiles against. UV coordinates allow the UV-rewrite path to run.
QString writeTexturedObj(const QString& dir, const QString& objName,
                         const QString& mtlName, const QString& diffuseTex)
{
    const QString objPath = QDir(dir).filePath(objName);
    const QString mtlPath = QDir(dir).filePath(mtlName);

    QFile mtl(mtlPath);
    if (!mtl.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
    QByteArray mtlData;
    mtlData += "newmtl TileMat\n";
    mtlData += "Kd 1 1 1\n";
    mtlData += ("map_Kd " + diffuseTex + "\n").toUtf8();
    mtl.write(mtlData);
    mtl.close();

    QFile obj(objPath);
    if (!obj.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
    QByteArray objData;
    objData += ("mtllib " + mtlName + "\n").toUtf8();
    objData += "o Quad\n";
    objData += "v 0 0 0\n";
    objData += "v 1 0 0\n";
    objData += "v 1 1 0\n";
    objData += "v 0 1 0\n";
    objData += "vt 0 0\n";
    objData += "vt 1 0\n";
    objData += "vt 1 1\n";
    objData += "vt 0 1\n";
    objData += "vn 0 0 1\n";
    objData += "usemtl TileMat\n";
    objData += "f 1/1/1 2/2/1 3/3/1\n";
    objData += "f 1/1/1 3/3/1 4/4/1\n";
    obj.write(objData);
    obj.close();
    return objPath;
}

// Minimal triangle OBJ with no material (still drives import/export/report).
QString writePlainObj(const QString& dir, const QString& name)
{
    const QString path = QDir(dir).filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
    f.write(
        "o Tri\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f 1 2 3\n");
    f.close();
    return path;
}

QString writeGreyAtlas(const QString& dir, const QString& name, int w, int h)
{
    QImage img(w, h, QImage::Format_RGBA8888);
    img.fill(qRgba(128, 128, 128, 255));
    const QString path = QDir(dir).filePath(name);
    img.save(path, "PNG");
    return path;
}

// Build a manifest JSON matching ApplyAtlas::parseManifestJson's schema:
//   { width, height, padding, tiles: [{source,x,y,w,h,u0,v0,u1,v1}] }
QString writeManifest(const QString& dir, const QString& name,
                      const QStringList& tileSources)
{
    QJsonObject root;
    root["width"] = 256;
    root["height"] = 256;
    root["padding"] = 2;
    QJsonArray tiles;
    int idx = 0;
    for (const QString& src : tileSources) {
        QJsonObject t;
        t["source"] = src;
        t["x"] = idx * 64;
        t["y"] = 0;
        t["w"] = 64;
        t["h"] = 64;
        // sub-rect inside the atlas in [0..1] UV space
        t["u0"] = double(idx) * 0.25;
        t["v0"] = 0.0;
        t["u1"] = double(idx) * 0.25 + 0.25;
        t["v1"] = 0.25;
        tiles.append(t);
        ++idx;
    }
    root["tiles"] = tiles;

    const QString path = QDir(dir).filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    return path;
}

class CLIPipelineCmdAtlasApplyCoverage : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();
    }

    void TearDown() override {
        if (SelectionSet::getSingletonPtr())
            SelectionSet::getSingleton()->clear();
        if (!Manager::getSingletonPtr()) return;
        auto nodes = Manager::getSingleton()->getSceneNodes(); // copy
        for (auto* node : nodes) {
            Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
            Manager::getSingleton()->destroySceneNode(node);
        }
    }
};

} // namespace

// Default text-report branch: textured OBJ + a tile whose source matches the
// mesh's diffuse texture. Exercises import, addResourceLocation, the
// applyToEntity loop, exporter, and the text report aggregation/format.
TEST_F(CLIPipelineCmdAtlasApplyCoverage, TextReport_MatchingTile_Succeeds)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString mesh = writeTexturedObj(tmp.path(), "mesh.obj", "mesh.mtl",
                                           "tile_a.png");
    ASSERT_FALSE(mesh.isEmpty());
    // Manifest tile source matches the mesh diffuse by basename.
    const QString manifest = writeManifest(tmp.path(), "atlas.json",
                                            {"tile_a.png", "tile_b.png"});
    ASSERT_FALSE(manifest.isEmpty());
    const QString atlas = writeGreyAtlas(tmp.path(), "atlas.png", 256, 256);
    ASSERT_FALSE(atlas.isEmpty());
    const QString out = tmp.filePath("out.obj");

    const QByteArray meshArg = mesh.toUtf8();
    const QByteArray outArg = out.toUtf8();
    const QByteArray manArg = manifest.toUtf8();
    const QByteArray atlasArg = atlas.toUtf8();

    AtlasApplyArgv args({"qtmesh", "atlas-apply", meshArg.constData(),
                         "-o", outArg.constData(),
                         "--manifest", manArg.constData(),
                         "--atlas", atlasArg.constData()});
    EXPECT_EQ(CLIPipeline::cmdAtlasApply(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFileInfo::exists(out))
        << "atlas-apply should have written the output mesh";
}

// --json report branch. The JSON itself is written to the saved-stdout fd
// (not capturable here), so assert the exit code + output existence, which
// proves the json branch's QJsonDocument serialization ran without crashing.
TEST_F(CLIPipelineCmdAtlasApplyCoverage, JsonReport_Succeeds)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString mesh = writeTexturedObj(tmp.path(), "mesh.obj", "mesh.mtl",
                                           "tile_a.png");
    ASSERT_FALSE(mesh.isEmpty());
    const QString manifest = writeManifest(tmp.path(), "atlas.json",
                                            {"tile_a.png"});
    ASSERT_FALSE(manifest.isEmpty());
    const QString atlas = writeGreyAtlas(tmp.path(), "atlas.png", 128, 128);
    const QString out = tmp.filePath("out_json.glb");

    const QByteArray meshArg = mesh.toUtf8();
    const QByteArray outArg = out.toUtf8();
    const QByteArray manArg = manifest.toUtf8();
    const QByteArray atlasArg = atlas.toUtf8();

    AtlasApplyArgv args({"qtmesh", "atlas-apply", meshArg.constData(),
                         "-o", outArg.constData(),
                         "--manifest", manArg.constData(),
                         "--atlas", atlasArg.constData(),
                         "--json"});
    EXPECT_EQ(CLIPipeline::cmdAtlasApply(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFileInfo::exists(out));
}

// --match fullpath wires opts.matchMode = FullPath. With a basename-only
// tile source the diffuse won't full-path-match, but the loop, exporter,
// and report still run (totalRewritten aggregation = 0 path).
TEST_F(CLIPipelineCmdAtlasApplyCoverage, MatchFullPath_Succeeds)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString mesh = writeTexturedObj(tmp.path(), "mesh.obj", "mesh.mtl",
                                           "tile_a.png");
    ASSERT_FALSE(mesh.isEmpty());
    const QString manifest = writeManifest(tmp.path(), "atlas.json",
                                            {"tile_a.png"});
    const QString atlas = writeGreyAtlas(tmp.path(), "atlas.png", 64, 64);
    const QString out = tmp.filePath("out_full.obj");

    const QByteArray meshArg = mesh.toUtf8();
    const QByteArray outArg = out.toUtf8();
    const QByteArray manArg = manifest.toUtf8();
    const QByteArray atlasArg = atlas.toUtf8();

    AtlasApplyArgv args({"qtmesh", "atlas-apply", meshArg.constData(),
                         "-o", outArg.constData(),
                         "--manifest", manArg.constData(),
                         "--atlas", atlasArg.constData(),
                         "--match", "fullpath"});
    EXPECT_EQ(CLIPipeline::cmdAtlasApply(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFileInfo::exists(out));
}

// --no-clamp + --match fullpath + --keep-extras combination. Covers
// opts.clampOutOfRangeUVs=false, opts.stripNonDiffuseTextures=false, the
// FullPath wiring, and the "skipped" suffix-word selection in the text
// report (only emitted when totalOutOfRange > 0, otherwise the suffix is
// empty — either way the branch is evaluated).
TEST_F(CLIPipelineCmdAtlasApplyCoverage, NoClampKeepExtrasFullPath_Succeeds)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString mesh = writeTexturedObj(tmp.path(), "mesh.obj", "mesh.mtl",
                                           "tile_a.png");
    ASSERT_FALSE(mesh.isEmpty());
    const QString manifest = writeManifest(tmp.path(), "atlas.json",
                                            {"tile_a.png", "tile_b.png"});
    const QString atlas = writeGreyAtlas(tmp.path(), "atlas.png", 256, 256);
    const QString out = tmp.filePath("out_noclamp.obj");

    const QByteArray meshArg = mesh.toUtf8();
    const QByteArray outArg = out.toUtf8();
    const QByteArray manArg = manifest.toUtf8();
    const QByteArray atlasArg = atlas.toUtf8();

    AtlasApplyArgv args({"qtmesh", "atlas-apply", meshArg.constData(),
                         "-o", outArg.constData(),
                         "--manifest", manArg.constData(),
                         "--atlas", atlasArg.constData(),
                         "--no-clamp",
                         "--keep-extras",
                         "--match", "fullpath"});
    EXPECT_EQ(CLIPipeline::cmdAtlasApply(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFileInfo::exists(out));
}

// Non-matching tile source: the manifest references textures the mesh does
// not use. The per-entity loop still imports, applies (0 rewrites), exports,
// and emits the report. Exercises the totalRewritten=0 aggregation path.
TEST_F(CLIPipelineCmdAtlasApplyCoverage, NonMatchingTile_StillExportsAndReports)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString mesh = writePlainObj(tmp.path(), "plain.obj");
    ASSERT_FALSE(mesh.isEmpty());
    const QString manifest = writeManifest(tmp.path(), "atlas.json",
                                            {"unrelated_texture.png"});
    const QString atlas = writeGreyAtlas(tmp.path(), "atlas.png", 32, 32);
    const QString out = tmp.filePath("out_plain.obj");

    const QByteArray meshArg = mesh.toUtf8();
    const QByteArray outArg = out.toUtf8();
    const QByteArray manArg = manifest.toUtf8();
    const QByteArray atlasArg = atlas.toUtf8();

    AtlasApplyArgv args({"qtmesh", "atlas-apply", meshArg.constData(),
                         "-o", outArg.constData(),
                         "--manifest", manArg.constData(),
                         "--atlas", atlasArg.constData()});
    EXPECT_EQ(CLIPipeline::cmdAtlasApply(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFileInfo::exists(out));
}

// Explicit --match basename (the default) with --json, re-registering the
// same atlas resource location to exercise the addResourceLocation
// duplicate-swallow try/catch on a second invocation in the same process.
TEST_F(CLIPipelineCmdAtlasApplyCoverage, BasenameMatchJson_ReRegistersAtlasLocation)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString mesh = writeTexturedObj(tmp.path(), "mesh.obj", "mesh.mtl",
                                           "tile_a.png");
    ASSERT_FALSE(mesh.isEmpty());
    const QString manifest = writeManifest(tmp.path(), "atlas.json",
                                            {"tile_a.png"});
    const QString atlas = writeGreyAtlas(tmp.path(), "atlas.png", 128, 128);

    const QByteArray meshArg = mesh.toUtf8();
    const QByteArray manArg = manifest.toUtf8();
    const QByteArray atlasArg = atlas.toUtf8();

    // First invocation registers the atlas dir as a resource location.
    const QString out1 = tmp.filePath("out_a.obj");
    const QByteArray out1Arg = out1.toUtf8();
    AtlasApplyArgv args1({"qtmesh", "atlas-apply", meshArg.constData(),
                          "-o", out1Arg.constData(),
                          "--manifest", manArg.constData(),
                          "--atlas", atlasArg.constData(),
                          "--match", "basename",
                          "--json"});
    EXPECT_EQ(CLIPipeline::cmdAtlasApply(args1.argc(), args1.argv()), 0);
    EXPECT_TRUE(QFileInfo::exists(out1));

    // Re-import for the second pass (the first pass's entities were exported
    // but remain in the scene; clear them so we exercise a fresh import).
    if (Manager::getSingletonPtr()) {
        auto nodes = Manager::getSingleton()->getSceneNodes();
        for (auto* node : nodes) {
            Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
            Manager::getSingleton()->destroySceneNode(node);
        }
    }

    // Second invocation hits the addResourceLocation duplicate path (same
    // atlas dir) -> the try/catch swallow branch.
    const QString out2 = tmp.filePath("out_b.obj");
    const QByteArray out2Arg = out2.toUtf8();
    AtlasApplyArgv args2({"qtmesh", "atlas-apply", meshArg.constData(),
                          "-o", out2Arg.constData(),
                          "--manifest", manArg.constData(),
                          "--atlas", atlasArg.constData(),
                          "--match", "basename"});
    EXPECT_EQ(CLIPipeline::cmdAtlasApply(args2.argc(), args2.argv()), 0);
    EXPECT_TRUE(QFileInfo::exists(out2));
}
