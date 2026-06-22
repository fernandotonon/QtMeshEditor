// Coverage tests for CLIPipeline::cmdMaterial focused on the *success* path
// (CLIPipeline.cpp lines ~3324-3425): import -> SelectionSet::append loop ->
// MaterialPresetLibrary::applyPreset -> MeshImporterExporter::exporter ->
// MaterialManager sidecar serialize+write.
//
// The existing CLIPipeline_test.cpp CLIPipelineCmdMaterial* suites only cover
// error returns (NoArgs=2, FileWithoutPreset=2, UnknownPreset=2,
// NonexistentFile=1) and --list-presets=0. None of them ever applies a preset
// and exports a mesh + .material sidecar. This file drives the real asset
// (testRobotMeshPath() copied into a QTemporaryDir, with its robot.skeleton
// sibling) through the full preset->export->sidecar pipeline.
//
// Suite name (CLIPipelineCmdMaterialCoverage) is unique vs the existing
// suites; all helpers live in this file's own anonymous namespace (no ODR
// clash with CLIPipeline_test.cpp's TestArgv).

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <vector>

#include <OgreMaterialManager.h>

#include "CLIPipeline.h"
#include "LLMManager.h"
#include "MaterialPresetLibrary.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

namespace {

// RAII argv builder driven by a QStringList (lets us assemble dynamic temp
// paths). Separate type in this file's anonymous namespace (no ODR clash).
class ArgvBuilder {
public:
    explicit ArgvBuilder(const QStringList& args)
    {
        for (const QString& a : args)
            m_storage.push_back(a.toUtf8());
        for (auto& ba : m_storage)
            m_argv.push_back(ba.data());
        m_argc = static_cast<int>(m_argv.size());
    }
    int argc() { return m_argc; }
    char** argv() { return m_argv.data(); }

private:
    std::vector<QByteArray> m_storage;
    std::vector<char*> m_argv;
    int m_argc = 0;
};

class CLIPipelineCmdMaterialCoverageTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles()) << "Ogre plugins/codecs not available";
        createStandardOgreMaterials();
        // The --generate-pbr path auto-downloads its ONNX models on first use;
        // forbid network in tests so the synchronous synthesize call can't hang
        // on a download (it returns the graceful "model not available" error).
        qputenv("QTMESH_PBR_NO_DOWNLOAD", "1");
        // Each cmdMaterial run appends to the global SelectionSet; clear it so
        // selectedEntityCount in the next run reflects only that run's import.
        if (auto* sel = SelectionSet::getSingletonPtr())
            sel->clear();
    }

    void TearDown() override
    {
        qunsetenv("QTMESH_PBR_NO_DOWNLOAD");
        if (auto* sel = SelectionSet::getSingletonPtr())
            sel->clear();
    }

    // Copy robot.mesh (+ sibling robot.skeleton) into a fresh temp dir so the
    // export can write next to it without touching the repo's media tree.
    // Returns the absolute path to the copied mesh, or empty on failure.
    QString copyRobotInto(QTemporaryDir& dir)
    {
        const QString fixture = testRobotMeshPath();
        if (fixture.isEmpty() || !QFile::exists(fixture))
            return QString();
        const QString src = dir.filePath("robot.mesh");
        QFile::remove(src);
        if (!QFile::copy(fixture, src))
            return QString();

        // Keep the sibling skeleton next to the mesh so Ogre resolves the link.
        const QString skelFixture =
            QFileInfo(fixture).absolutePath() + "/robot.skeleton";
        if (QFile::exists(skelFixture)) {
            const QString skelDst = dir.filePath("robot.skeleton");
            QFile::remove(skelDst);
            QFile::copy(skelFixture, skelDst);
        }
        return src;
    }
};

// --preset <simple> with -o <output>: assert exit 0, the output mesh exists,
// and the <basename>.material sidecar is written next to it (non-empty).
// Exercises the simple "Plastic (Red)" applyPreset branch.
TEST_F(CLIPipelineCmdMaterialCoverageTest, SimplePresetWithOutputWritesMeshAndSidecar)
{
    QTemporaryDir src;
    ASSERT_TRUE(src.isValid());
    const QString mesh = copyRobotInto(src);
    ASSERT_FALSE(mesh.isEmpty()) << "robot.mesh fixture unavailable";

    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    const QString outMesh = out.filePath("plastic_out.mesh");

    ArgvBuilder args({"qtmesh", "material", mesh,
                      "--preset", "Plastic (Red)",
                      "-o", outMesh});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 0);

    EXPECT_TRUE(QFile::exists(outMesh)) << outMesh.toStdString();

    const QString sidecar =
        QDir(out.path()).filePath(QStringLiteral("plastic_out.material"));
    EXPECT_TRUE(QFile::exists(sidecar)) << sidecar.toStdString();
    EXPECT_GT(QFileInfo(sidecar).size(), 0);

    // The preset material resource must have been created under "Preset/<name>".
    auto* mm = Ogre::MaterialManager::getSingletonPtr();
    ASSERT_NE(mm, nullptr);
    EXPECT_TRUE(mm->resourceExists("Preset/Plastic (Red)"));
}

// --preset 'Metallic-Roughness' hits the PBR applyPbrTemplate branch (distinct
// from the simple startsWith("Plastic") branch). Both reach the same
// matName "Preset/<name>" resourceExists/getByName sidecar path.
TEST_F(CLIPipelineCmdMaterialCoverageTest, PbrPresetWithOutputWritesMeshAndSidecar)
{
    QTemporaryDir src;
    ASSERT_TRUE(src.isValid());
    const QString mesh = copyRobotInto(src);
    ASSERT_FALSE(mesh.isEmpty()) << "robot.mesh fixture unavailable";

    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    const QString outMesh = out.filePath("pbr_out.mesh");

    ArgvBuilder args({"qtmesh", "material", mesh,
                      "--preset", "Metallic-Roughness",
                      "--output", outMesh});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 0);

    EXPECT_TRUE(QFile::exists(outMesh)) << outMesh.toStdString();

    const QString sidecar =
        QDir(out.path()).filePath(QStringLiteral("pbr_out.material"));
    EXPECT_TRUE(QFile::exists(sidecar)) << sidecar.toStdString();
    EXPECT_GT(QFileInfo(sidecar).size(), 0);

    auto* mm = Ogre::MaterialManager::getSingletonPtr();
    ASSERT_NE(mm, nullptr);
    EXPECT_TRUE(mm->resourceExists("Preset/Metallic-Roughness"));
}

// --preset without -o: outputPath defaults to inputPath (line 3330). The mesh
// is overwritten in place and the sidecar lands next to it. The singular
// "(1 entity)" report-string branch is hit since robot.mesh has one entity.
TEST_F(CLIPipelineCmdMaterialCoverageTest, PresetWithoutOutputDefaultsToInputPath)
{
    QTemporaryDir src;
    ASSERT_TRUE(src.isValid());
    const QString mesh = copyRobotInto(src);
    ASSERT_FALSE(mesh.isEmpty()) << "robot.mesh fixture unavailable";

    ArgvBuilder args({"qtmesh", "material", mesh,
                      "--preset", "Metal (Gold)"});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 0);

    // In-place rewrite: the input mesh still exists.
    EXPECT_TRUE(QFile::exists(mesh)) << mesh.toStdString();

    // Sidecar uses the input's complete base name ("robot.material").
    const QString sidecar =
        QDir(src.path()).filePath(QStringLiteral("robot.material"));
    EXPECT_TRUE(QFile::exists(sidecar)) << sidecar.toStdString();
    EXPECT_GT(QFileInfo(sidecar).size(), 0);

    auto* mm = Ogre::MaterialManager::getSingletonPtr();
    ASSERT_NE(mm, nullptr);
    EXPECT_TRUE(mm->resourceExists("Preset/Metal (Gold)"));
}

// Another simple-branch preset to exercise the report-string path again and
// confirm distinct preset names each get their own "Preset/<name>" resource.
TEST_F(CLIPipelineCmdMaterialCoverageTest, UnlitPresetWritesSidecar)
{
    QTemporaryDir src;
    ASSERT_TRUE(src.isValid());
    const QString mesh = copyRobotInto(src);
    ASSERT_FALSE(mesh.isEmpty()) << "robot.mesh fixture unavailable";

    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    const QString outMesh = out.filePath("unlit_out.mesh");

    ArgvBuilder args({"qtmesh", "material", mesh,
                      "--preset", "Unlit PBR",
                      "-o", outMesh});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 0);

    const QString sidecar =
        QDir(out.path()).filePath(QStringLiteral("unlit_out.material"));
    EXPECT_TRUE(QFile::exists(sidecar)) << sidecar.toStdString();
    EXPECT_GT(QFileInfo(sidecar).size(), 0);

    auto* mm = Ogre::MaterialManager::getSingletonPtr();
    ASSERT_NE(mm, nullptr);
    EXPECT_TRUE(mm->resourceExists("Preset/Unlit PBR"));
}

// Sanity: every preset name reported by the library is recognized by
// cmdMaterial (none falls into the "Unknown preset" guard, return 2). This
// asserts the preset-name contract between MaterialPresetLibrary and the CLI
// validator without re-running a full export for each name.
TEST_F(CLIPipelineCmdMaterialCoverageTest, AllLibraryPresetsAreAcceptedByValidator)
{
    auto* lib = MaterialPresetLibrary::instance();
    ASSERT_NE(lib, nullptr);
    const QStringList names = lib->presetNames();
    ASSERT_FALSE(names.isEmpty());

    // A nonexistent file + a *valid* preset returns 1 (file-not-found), never
    // 2 (unknown-preset). If any name returned 2, the validator rejected it.
    for (const QString& n : names) {
        ArgvBuilder args({"qtmesh", "material",
                          "/tmp/qtmesh_cmdmaterial_cov_no_such_file_zz.mesh",
                          "--preset", n});
        const int rc = CLIPipeline::cmdMaterial(args.argc(), args.argv());
        EXPECT_EQ(rc, 1) << "preset '" << n.toStdString()
                         << "' should pass validation (file-not-found=1, not unknown=2)";
    }
}

// --generate-texture (#403). The SD model dir is empty in CI, so the
// deterministic, crash-free outcome we can assert is the clean failure path:
//
//  * built WITH ENABLE_STABLE_DIFFUSION → import + Ogre init succeed, then the
//    "no SD model found" guard returns 1 (never crashes, never writes output).
//  * built WITHOUT it → the #ifndef branch returns 1 immediately.
//
// Either way the exported mesh must NOT be produced. A nonexistent input is a
// separate guard (return 1) that needs no SD model and no Ogre RTT.
TEST_F(CLIPipelineCmdMaterialCoverageTest, GenerateTextureNonexistentFileReturnsError)
{
    ArgvBuilder args({"qtmesh", "material",
                      "/tmp/qtmesh_gentex_no_such_file_zz.mesh",
                      "--generate-texture", "rusty bronze armor"});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 1);
}

TEST_F(CLIPipelineCmdMaterialCoverageTest, GenerateTextureRejectsOutOfRangeSize)
{
    QTemporaryDir src;
    ASSERT_TRUE(src.isValid());
    const QString mesh = copyRobotInto(src);
    ASSERT_FALSE(mesh.isEmpty()) << "robot.mesh fixture unavailable";

#ifdef ENABLE_STABLE_DIFFUSION
    // Width out of [64,2048] → usage error (2), before any model/RTT work.
    ArgvBuilder args({"qtmesh", "material", mesh,
                      "--generate-texture", "x", "--width", "16"});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 2);
#else
    // Not built with SD → the feature returns 1 regardless of args.
    ArgvBuilder args({"qtmesh", "material", mesh,
                      "--generate-texture", "x"});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 1);
#endif
}

// With a valid mesh + valid size but an empty SD model dir, generation can't
// proceed: exit 1 and NO output mesh written. (When built without SD the same
// exit-1 / no-output contract holds via the #ifndef branch.)
TEST_F(CLIPipelineCmdMaterialCoverageTest, GenerateTextureNoModelFailsCleanly)
{
    QTemporaryDir src;
    ASSERT_TRUE(src.isValid());
    const QString mesh = copyRobotInto(src);
    ASSERT_FALSE(mesh.isEmpty()) << "robot.mesh fixture unavailable";

    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    const QString outMesh = out.filePath("gentex_out.mesh");

    ArgvBuilder args({"qtmesh", "material", mesh,
                      "--generate-texture", "rusty bronze armor",
                      "-o", outMesh});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 1);
    EXPECT_FALSE(QFile::exists(outMesh)) << "no mesh should be written on failure";
}

// ── #404: --generate-pbr ─────────────────────────────────────────────────────

namespace {
// Write a small solid-colour albedo PNG; returns its path (empty on failure).
QString writeAlbedo(const QTemporaryDir& dir, const QString& name, int rgb)
{
    QImage img(16, 16, QImage::Format_RGB888);
    img.fill(rgb);
    const QString p = dir.filePath(name);
    if (!img.save(p, "PNG")) return QString();
    return p;
}
} // namespace

// --generate-pbr with no --texture → usage error (2) on an ONNX build; the
// not-built branch returns 1. Either way it must not crash.
TEST_F(CLIPipelineCmdMaterialCoverageTest, GeneratePbrMissingTexture)
{
    ArgvBuilder args({"qtmesh", "material", "--generate-pbr"});
    const int rc = CLIPipeline::cmdMaterial(args.argc(), args.argv());
#ifdef ENABLE_ONNX
    EXPECT_EQ(rc, 2);
#else
    EXPECT_EQ(rc, 1);
#endif
}

// Out-of-range --tile-size → usage error (2) on ONNX; not-built returns 1.
TEST_F(CLIPipelineCmdMaterialCoverageTest, GeneratePbrRejectsBadTileSize)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString albedo = writeAlbedo(dir, "albedo.png", qRgb(120, 120, 120));
    ASSERT_FALSE(albedo.isEmpty());

    ArgvBuilder args({"qtmesh", "material", "--texture", albedo,
                      "--generate-pbr", "--tile-size", "8"});
    const int rc = CLIPipeline::cmdMaterial(args.argc(), args.argv());
#ifdef ENABLE_ONNX
    EXPECT_EQ(rc, 2);
#else
    EXPECT_EQ(rc, 1);
#endif
}

// Roughness-only needs no model, so on an ONNX build it succeeds offline and
// writes <stem>_roughness.png next to the albedo. Without ONNX it returns 1.
TEST_F(CLIPipelineCmdMaterialCoverageTest, GeneratePbrRoughnessOnly)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString albedo = writeAlbedo(dir, "tex.png", qRgb(40, 40, 40));
    ASSERT_FALSE(albedo.isEmpty());

    ArgvBuilder args({"qtmesh", "material", "--texture", albedo,
                      "--generate-pbr", "--no-normal", "--no-height"});
    const int rc = CLIPipeline::cmdMaterial(args.argc(), args.argv());
#ifdef ENABLE_ONNX
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(QFile::exists(dir.filePath("tex_roughness.png")));
    EXPECT_FALSE(QFile::exists(dir.filePath("tex_normal.png")));
#else
    EXPECT_EQ(rc, 1);
#endif
}

// A full request (normal/height) with no downloaded model fails cleanly: exit 1,
// no maps written. (Non-ONNX build: same exit-1 contract.)
TEST_F(CLIPipelineCmdMaterialCoverageTest, GeneratePbrNoModelFailsCleanly)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString albedo = writeAlbedo(dir, "tex.png", qRgb(200, 180, 160));
    ASSERT_FALSE(albedo.isEmpty());

    ArgvBuilder args({"qtmesh", "material", "--texture", albedo, "--generate-pbr"});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 1);
    EXPECT_FALSE(QFile::exists(dir.filePath("tex_normal.png")));
}

// ── #405: --upscale ─────────────────────────────────────────────────────────
// The fixture sets QTMESH_PBR_NO_DOWNLOAD, so these exercise the validation +
// no-model contracts without hitting the network.

// --upscale without --texture → usage error (2) on ONNX; not-built returns 1.
TEST_F(CLIPipelineCmdMaterialCoverageTest, UpscaleMissingTexture)
{
    ArgvBuilder args({"qtmesh", "material", "--upscale", "4"});
    const int rc = CLIPipeline::cmdMaterial(args.argc(), args.argv());
#ifdef ENABLE_ONNX
    EXPECT_EQ(rc, 2);
#else
    EXPECT_EQ(rc, 1);
#endif
}

// Invalid factor (3) → usage error (2) on ONNX; not-built returns 1.
TEST_F(CLIPipelineCmdMaterialCoverageTest, UpscaleRejectsBadFactor)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString tex = writeAlbedo(dir, "low.png", qRgb(90, 90, 90));
    ASSERT_FALSE(tex.isEmpty());
    ArgvBuilder args({"qtmesh", "material", "--texture", tex, "--upscale", "3"});
    const int rc = CLIPipeline::cmdMaterial(args.argc(), args.argv());
#ifdef ENABLE_ONNX
    EXPECT_EQ(rc, 2);
#else
    EXPECT_EQ(rc, 1);
#endif
}

// Non-numeric --upscale → usage error (2) regardless of build.
TEST_F(CLIPipelineCmdMaterialCoverageTest, UpscaleRejectsNonNumeric)
{
    ArgvBuilder args({"qtmesh", "material", "--texture", "/tmp/x.png",
                      "--upscale", "huge"});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 2);
}

// Valid factor + no downloaded model → exit 1, no output written. (Non-ONNX:
// same exit-1 contract via the #ifndef branch.)
TEST_F(CLIPipelineCmdMaterialCoverageTest, UpscaleNoModelFailsCleanly)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString tex = writeAlbedo(dir, "low.png", qRgb(60, 90, 120));
    ASSERT_FALSE(tex.isEmpty());
    const QString out = dir.filePath("high.png");
    ArgvBuilder args({"qtmesh", "material", "--texture", tex,
                      "--upscale", "4", "-o", out});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 1);
    EXPECT_FALSE(QFile::exists(out)) << "no output on failure";
}

// ── #406: --describe (LLM-assisted material) ────────────────────────────────
// CI has no GGUF model in the models dir, so (like the SD/PBR tests above)
// these exercise the validation + clean no-model fallback. The model-loaded
// generation path needs a real model and isn't run here.

// --describe with no description value → it's dropped by the parser (needs a
// following arg), so with only a mesh and no preset/describe this falls into
// the generic usage error (2). A nonexistent file with a valid description is a
// separate guard (1).
TEST_F(CLIPipelineCmdMaterialCoverageTest, DescribeNonexistentFileReturnsError)
{
    ArgvBuilder args({"qtmesh", "material",
                      "/tmp/qtmesh_describe_no_such_file_zz.mesh",
                      "--describe", "rusty bronze armor"});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 1);
}

// --describe with an empty description string → usage error (2), before any
// import or model work.
TEST_F(CLIPipelineCmdMaterialCoverageTest, DescribeEmptyPromptIsUsageError)
{
    QTemporaryDir src;
    ASSERT_TRUE(src.isValid());
    const QString mesh = copyRobotInto(src);
    ASSERT_FALSE(mesh.isEmpty()) << "robot.mesh fixture unavailable";

    ArgvBuilder args({"qtmesh", "material", mesh, "--describe", "   "});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 2);
}

// Valid mesh + description but no local LLM model available → exit 1 and NO
// output mesh written. This is the graceful fallback the issue requires; it
// holds whether or not the build has llama.cpp (no model loaded either way).
// Point the LLM at an empty models dir so the resolver finds nothing and bails
// before any (potentially blocking) load — deterministic even on a dev box that
// happens to have a GGUF model installed.
TEST_F(CLIPipelineCmdMaterialCoverageTest, DescribeNoModelFailsCleanly)
{
    QTemporaryDir src;
    ASSERT_TRUE(src.isValid());
    const QString mesh = copyRobotInto(src);
    ASSERT_FALSE(mesh.isEmpty()) << "robot.mesh fixture unavailable";

    QTemporaryDir emptyModels;
    ASSERT_TRUE(emptyModels.isValid());
    auto* llm = LLMManager::instance();
    ASSERT_NE(llm, nullptr);
    const QString prevDir = llm->modelsDirectory();
    llm->setModelsDirectory(emptyModels.path());

    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    const QString outMesh = out.filePath("describe_out.mesh");

    ArgvBuilder args({"qtmesh", "material", mesh,
                      "--describe", "rusty bronze armor",
                      "-o", outMesh});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 1);
    EXPECT_FALSE(QFile::exists(outMesh)) << "no mesh should be written on failure";

    llm->setModelsDirectory(prevDir);
}

} // namespace
