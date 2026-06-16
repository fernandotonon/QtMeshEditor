// Coverage tests for CLIPipeline::cmdAnim per-mode guard branches.
//
// The existing src/CLIPipeline_cmdanimbake_coverage_test.cpp covers only the
// --bake-fps block's `bakeFps < 1` guard. This file targets the resample /
// decimate per-mode guards that are otherwise uncovered:
//
//   - --resample 1  -> resampleCount < 2 guard (CLIPipeline.cpp ~1942-1945) -> 2
//   - --resample 0  -> same guard                                            -> 2
//   - --decimate-step 1 -> decimateStep < 2 guard (~2003-2006)               -> 2
//   - --resample N --animation <missing> -> animsProcessed == 0, with the
//       "Available animations:" listing branch (~1967-1975)                  -> 1
//   - --decimate-step S --animation <missing> -> animsProcessed == 0
//       listing branch (~2028-2036)                                          -> 1
//
// These guards return AFTER initOgreHeadless() + a successful import reaches
// the per-mode block (the skeleton check precedes them), so a valid rigged
// input is required. robot.mesh (testRobotMeshPath, references robot.skeleton)
// is used so the import succeeds and execution reaches the per-mode guard.
//
// All identifiers are deliberately distinct from the other CLIPipeline anim
// coverage TUs (distinct anonymous namespace contents, a _Guards-suffixed
// suite name, a local TestArgv copy) to avoid ODR clashes / duplicate
// registration.

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <initializer_list>

#include "CLIPipeline.h"
#include "MeshImporterExporter.h"
#include "Manager.h"
#include "TestHelpers.h"

namespace {

// RAII helper to build argc/argv from a list of strings (self-contained copy).
class GuardsTestArgv {
public:
    GuardsTestArgv(std::initializer_list<const char*> args)
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

} // anonymous namespace

class CLIPipelineCmdAnimGuardsCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();
        ASSERT_TRUE(CLIPipeline::initOgreHeadless());
    }

    void TearDown() override {
        clearScene();
    }

    // Wipe all scene nodes / attached objects between runs so each cmdAnim
    // invocation imports into a clean scene.
    static void clearScene() {
        if (!Manager::getSingletonPtr()) return;
        auto nodes = Manager::getSingleton()->getSceneNodes(); // copy
        for (auto* node : nodes) {
            Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
            Manager::getSingleton()->destroySceneNode(node);
        }
    }

    QString robotMesh() const { return testRobotMeshPath(); }
};

// --- Resample guard: N == 1 hits `resampleCount < 2`, returns usage exit 2 ---

TEST_F(CLIPipelineCmdAnimGuardsCoverageTest, ResampleOne_ReturnsUsageError_NoOutput)
{
    const QString file = robotMesh();
    ASSERT_FALSE(file.isEmpty()) << "robot.mesh fixture not found";
    ASSERT_TRUE(QFile::exists(file));
    QByteArray fileBa = file.toUtf8();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("resample_one.mesh");
    QByteArray outBa = outFile.toUtf8();

    GuardsTestArgv args({"qtmesh", "anim", fileBa.constData(),
                         "--resample", "1",
                         "-o", outBa.constData()});
    // resampleCount (1) < 2 -> usage error 2, before any export.
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 2);
    EXPECT_FALSE(QFile::exists(outFile)) << "guard must not write an output file";
}

// --- Resample guard: N == 0 hits the same `resampleCount < 2` guard ---

TEST_F(CLIPipelineCmdAnimGuardsCoverageTest, ResampleZero_ReturnsUsageError_NoOutput)
{
    const QString file = robotMesh();
    ASSERT_FALSE(file.isEmpty()) << "robot.mesh fixture not found";
    ASSERT_TRUE(QFile::exists(file));
    QByteArray fileBa = file.toUtf8();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("resample_zero.mesh");
    QByteArray outBa = outFile.toUtf8();

    GuardsTestArgv args({"qtmesh", "anim", fileBa.constData(),
                         "--resample", "0",
                         "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 2);
    EXPECT_FALSE(QFile::exists(outFile));
}

// --- Decimate guard: S == 1 hits `decimateStep < 2`, returns usage exit 2 ---

TEST_F(CLIPipelineCmdAnimGuardsCoverageTest, DecimateStepOne_ReturnsUsageError_NoOutput)
{
    const QString file = robotMesh();
    ASSERT_FALSE(file.isEmpty()) << "robot.mesh fixture not found";
    ASSERT_TRUE(QFile::exists(file));
    QByteArray fileBa = file.toUtf8();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("decimate_one.mesh");
    QByteArray outBa = outFile.toUtf8();

    GuardsTestArgv args({"qtmesh", "anim", fileBa.constData(),
                         "--decimate-step", "1",
                         "-o", outBa.constData()});
    // decimateStep (1) < 2 -> usage error 2, before any export.
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 2);
    EXPECT_FALSE(QFile::exists(outFile)) << "guard must not write an output file";
}

// --- Decimate guard: S == 0 hits the same `decimateStep < 2` guard ---

TEST_F(CLIPipelineCmdAnimGuardsCoverageTest, DecimateStepZero_ReturnsUsageError_NoOutput)
{
    const QString file = robotMesh();
    ASSERT_FALSE(file.isEmpty()) << "robot.mesh fixture not found";
    ASSERT_TRUE(QFile::exists(file));
    QByteArray fileBa = file.toUtf8();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("decimate_zero.mesh");
    QByteArray outBa = outFile.toUtf8();

    GuardsTestArgv args({"qtmesh", "anim", fileBa.constData(),
                         "--decimate-step", "0",
                         "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 2);
    EXPECT_FALSE(QFile::exists(outFile));
}

// --- Resample no-match: valid N but --animation does not exist ->
//     animsProcessed == 0, exercises the "Available animations:" listing
//     branch, returns runtime error 1, no output. ---

TEST_F(CLIPipelineCmdAnimGuardsCoverageTest, ResampleMissingAnimation_ReturnsError_NoOutput)
{
    const QString file = robotMesh();
    ASSERT_FALSE(file.isEmpty()) << "robot.mesh fixture not found";
    ASSERT_TRUE(QFile::exists(file));
    QByteArray fileBa = file.toUtf8();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("resample_nomatch.mesh");
    QByteArray outBa = outFile.toUtf8();

    GuardsTestArgv args({"qtmesh", "anim", fileBa.constData(),
                         "--resample", "30",
                         "--animation", "NoSuchAnimation_Resample_Guard",
                         "-o", outBa.constData()});
    // Valid N (>=2) passes the guard, but the filter matches nothing ->
    // animsProcessed == 0 -> error 1 with the available-animations listing.
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 1);
    EXPECT_FALSE(QFile::exists(outFile)) << "no-match must not write an output file";
}

// --- Decimate no-match: valid S but --animation does not exist ->
//     animsProcessed == 0 listing branch, returns runtime error 1, no output. ---

TEST_F(CLIPipelineCmdAnimGuardsCoverageTest, DecimateMissingAnimation_ReturnsError_NoOutput)
{
    const QString file = robotMesh();
    ASSERT_FALSE(file.isEmpty()) << "robot.mesh fixture not found";
    ASSERT_TRUE(QFile::exists(file));
    QByteArray fileBa = file.toUtf8();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("decimate_nomatch.mesh");
    QByteArray outBa = outFile.toUtf8();

    GuardsTestArgv args({"qtmesh", "anim", fileBa.constData(),
                         "--decimate-step", "5",
                         "--animation", "NoSuchAnimation_Decimate_Guard",
                         "-o", outBa.constData()});
    // Valid S (>=2) passes the guard, but the filter matches nothing ->
    // animsProcessed == 0 -> error 1 with the available-animations listing.
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 1);
    EXPECT_FALSE(QFile::exists(outFile)) << "no-match must not write an output file";
}
