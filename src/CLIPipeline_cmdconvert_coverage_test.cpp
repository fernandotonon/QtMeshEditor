// Coverage tests for CLIPipeline::cmdConvert — the mesh format-conversion
// subcommand. The existing CLIPipeline_test.cpp covers convert only for the
// FBX-input -> .mesh-output case; this suite drives the *uncovered* branches:
//
//   * .mesh input with the output format inferred from the output extension
//     (format.isEmpty() -> formatForExtension(outputPath), CLIPipeline.cpp:1468)
//   * .mesh -> .obj round-trip: assert the .obj exists, is non-empty, and is
//     itself loadable (re-run cmdInfo on the produced .obj)
//   * explicit --format that differs from the output extension (the
//     format-non-empty branch)
//   * the export-failure path (CLIPipeline.cpp:1474-1478) by pointing -o at an
//     output path inside a directory that does not exist / is unwritable
//
// All outputs go under a QTemporaryDir. cmdConvert needs Ogre (it loads the
// mesh through MeshImporterExporter + Manager), so the fixture does
// ASSERT_TRUE(tryInitOgre()) + createStandardOgreMaterials() and clears the
// scene between cases (cmdConvert grabs Manager::getEntities().first()).
//
// Distinct filename + distinct suite name (CLIPipelineConvertCoverageTest) from
// CLIPipeline_test.cpp's CLIPipelineCmdTest so there is no ODR clash /
// duplicate-registration.

#include <gtest/gtest.h>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QTemporaryDir>
#include <initializer_list>

#include "CLIPipeline.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "TestHelpers.h"

namespace {

/// RAII helper to build argc/argv from a list of C-strings. Kept in an
/// anonymous namespace so it does not collide with the TestArgv in
/// CLIPipeline_test.cpp.
class ConvertArgv {
public:
    ConvertArgv(std::initializer_list<const char*> args)
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
    QList<QByteArray> m_storage;
    QList<char*> m_argv;
    int m_argc = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// Pure-logic check on the extension->format mapper (no Ogre needed). This
// guards the inference helper that cmdConvert relies on at line 1468.
// ---------------------------------------------------------------------------
TEST(CLIPipelineConvertFormatMap, InfersKnownExtensions)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("/tmp/out.obj"),  QString("OBJ (*.obj)"));
    EXPECT_EQ(CLIPipeline::formatForExtension("/tmp/out.mesh"), QString("Ogre Mesh (*.mesh)"));
    EXPECT_EQ(CLIPipeline::formatForExtension("/tmp/out.ply"),  QString("PLY (*.ply)"));
    EXPECT_EQ(CLIPipeline::formatForExtension("/tmp/OUT.OBJ"),  QString("OBJ (*.obj)"));
    // Unknown extension falls back to Ogre Mesh.
    EXPECT_EQ(CLIPipeline::formatForExtension("/tmp/out.unknownext"),
              QString("Ogre Mesh (*.mesh)"));
}

// ---------------------------------------------------------------------------
// Ogre-backed fixture for the convert execution paths.
// ---------------------------------------------------------------------------
class CLIPipelineConvertCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();
        m_robot = testRobotMeshPath();
        ASSERT_FALSE(m_robot.isEmpty())
            << "media/models/robot.mesh not found next to the test binary";
        ASSERT_TRUE(m_tmp.isValid());
        clearScene();
    }

    void TearDown() override {
        clearScene();
    }

    // cmdConvert keys off Manager::getEntities().first(); each case must start
    // from an empty scene so it converts the file it just imported.
    void clearScene() {
        if (!Manager::getSingletonPtr()) return;
        auto nodes = Manager::getSingleton()->getSceneNodes(); // copy
        for (auto* node : nodes) {
            Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
            Manager::getSingleton()->destroySceneNode(node);
        }
    }

    QString m_robot;
    QTemporaryDir m_tmp;
};

// ---------------------------------------------------------------------------
// .mesh input, output format INFERRED from the .obj extension (the
// format.isEmpty() -> formatForExtension(outputPath) branch at line 1468).
// Also the core round-trip assertion: exit 0 AND the output file exists.
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineConvertCoverageTest, MeshToObj_ExtensionInferredFormat_Succeeds)
{
    const QString outPath = m_tmp.path() + "/robot_inferred.obj";
    QByteArray inBa  = m_robot.toUtf8();
    QByteArray outBa = outPath.toUtf8();

    ConvertArgv args({"qtmesh", "convert", inBa.constData(), "-o", outBa.constData()});
    EXPECT_EQ(0, CLIPipeline::cmdConvert(args.argc(), args.argv()));
    EXPECT_TRUE(QFile::exists(outPath)) << "expected " << outPath.toStdString();
}

// ---------------------------------------------------------------------------
// .mesh -> .obj round-trip: the produced .obj must be non-empty AND itself
// loadable (re-run cmdInfo on it and assert exit 0). This validates the
// converted file rather than just its existence.
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineConvertCoverageTest, MeshToObj_RoundTrip_OutputIsNonEmptyAndLoadable)
{
    const QString outPath = m_tmp.path() + "/robot_roundtrip.obj";
    QByteArray inBa  = m_robot.toUtf8();
    QByteArray outBa = outPath.toUtf8();

    ConvertArgv convertArgs({"qtmesh", "convert", inBa.constData(), "-o", outBa.constData()});
    ASSERT_EQ(0, CLIPipeline::cmdConvert(convertArgs.argc(), convertArgs.argv()));

    ASSERT_TRUE(QFile::exists(outPath));
    EXPECT_GT(QFileInfo(outPath).size(), 0) << ".obj output should not be empty";

    // The converted file should be loadable on its own. cmdInfo imports it and
    // extracts mesh info; exit 0 means a valid scene came back. Clear the scene
    // first so cmdInfo loads the .obj fresh.
    clearScene();
    QByteArray reBa = outPath.toUtf8();
    ConvertArgv infoArgs({"qtmesh", "info", reBa.constData()});
    EXPECT_EQ(0, CLIPipeline::cmdInfo(infoArgs.argc(), infoArgs.argv()));
}

// ---------------------------------------------------------------------------
// Explicit --format that differs from the output extension (the
// format-non-empty branch: cmdConvert uses `format` verbatim instead of
// inferring from the extension). Here the file is named .out but we force the
// Ogre Mesh format, producing a valid .mesh payload under an arbitrary suffix.
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineConvertCoverageTest, ExplicitFormatDiffersFromExtension_Succeeds)
{
    // Extension is .obj-ish but we explicitly ask for the Ogre Mesh format,
    // so the format-non-empty branch is taken (not formatForExtension).
    const QString outPath = m_tmp.path() + "/robot_forced.dat";
    QByteArray inBa  = m_robot.toUtf8();
    QByteArray outBa = outPath.toUtf8();

    ConvertArgv args({"qtmesh", "convert", inBa.constData(),
                      "-o", outBa.constData(),
                      "--format", "Ogre Mesh (*.mesh)"});
    EXPECT_EQ(0, CLIPipeline::cmdConvert(args.argc(), args.argv()));
    EXPECT_TRUE(QFile::exists(outPath)) << "expected " << outPath.toStdString();
    EXPECT_GT(QFileInfo(outPath).size(), 0);
}

// Same branch, but produce a real .mesh from robot.mesh with an explicit
// format that matches the format-string the inference would have chosen — this
// still takes the format-non-empty path, just confirming a .mesh-out works.
TEST_F(CLIPipelineConvertCoverageTest, ExplicitMeshFormat_ProducesMesh)
{
    const QString outPath = m_tmp.path() + "/robot_explicit.mesh";
    QByteArray inBa  = m_robot.toUtf8();
    QByteArray outBa = outPath.toUtf8();

    ConvertArgv args({"qtmesh", "convert", inBa.constData(),
                      "-o", outBa.constData(),
                      "--format", "Ogre Mesh (*.mesh)"});
    EXPECT_EQ(0, CLIPipeline::cmdConvert(args.argc(), args.argv()));
    EXPECT_TRUE(QFile::exists(outPath));
    EXPECT_GT(QFileInfo(outPath).size(), 0);
}

// ---------------------------------------------------------------------------
// Export-failure path (CLIPipeline.cpp:1474-1478): a valid input file but an
// output path inside a subdirectory that does not exist (and is NOT created).
// The import succeeds, so we get past the import-failure exit 1 at line 1462
// and reach the exporter, which fails -> exit 1.
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineConvertCoverageTest, ExportFailure_NonexistentOutputDir_Returns1)
{
    // Deliberately do NOT mkpath this subdir.
    const QString outPath = m_tmp.path() + "/no_such_subdir/out.mesh";
    ASSERT_FALSE(QFileInfo(QFileInfo(outPath).absolutePath()).exists())
        << "test precondition: parent dir must not exist";

    QByteArray inBa  = m_robot.toUtf8();
    QByteArray outBa = outPath.toUtf8();

    ConvertArgv args({"qtmesh", "convert", inBa.constData(), "-o", outBa.constData()});
    EXPECT_EQ(1, CLIPipeline::cmdConvert(args.argc(), args.argv()));
    EXPECT_FALSE(QFile::exists(outPath));
}

// Same export-failure branch with an explicit --format, ensuring the failure
// path is independent of how the format was resolved.
TEST_F(CLIPipelineConvertCoverageTest, ExportFailure_NonexistentDirWithExplicitFormat_Returns1)
{
    const QString outPath = m_tmp.path() + "/missing_dir/out.obj";
    ASSERT_FALSE(QFileInfo(QFileInfo(outPath).absolutePath()).exists());

    QByteArray inBa  = m_robot.toUtf8();
    QByteArray outBa = outPath.toUtf8();

    ConvertArgv args({"qtmesh", "convert", inBa.constData(),
                      "-o", outBa.constData(),
                      "--format", "OBJ (*.obj)"});
    EXPECT_EQ(1, CLIPipeline::cmdConvert(args.argc(), args.argv()));
}
