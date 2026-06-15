// Coverage tests for CLIPipeline::cmdLod's meshoptimizer backend (#398).
//
// The existing CLIPipeline_test.cpp cmdLod suite exercises --count (default
// ogre backend), --auto, --remove and --info (text + json) plus the usage
// error cases, but it NEVER passes `--algo meshopt`. That leaves the #398
// code path uncovered:
//   * the algo parse / validation branch (CLIPipeline.cpp ~2442-2451),
//   * the invalid --algo value rejection -> exit 2 (~2444-2446),
//   * the algoSpecified + non-count-mode guard -> exit 2 (~2482-2486),
//   * the MeshLodController::Algorithm::Meshopt branch selection (~2574-2581).
//
// These tests drive the real cmdLod(argc, argv) entry point (which returns an
// int exit code) on the in-repo robot.mesh fixture and assert exit code +
// that the per-LOD output files exist on disk.
//
// Distinct filename + distinct suite names (CLIPipelineCmdLodMeshoptCoverage*)
// from the existing CLIPipelineCmdLod* suites so there is no ODR clash /
// duplicate registration. No QApplication is created here — test_main.cpp owns
// the single QCoreApplication.

#include <gtest/gtest.h>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QTemporaryDir>
#include <initializer_list>

#include "CLIPipeline.h"
#include "Manager.h"
#include "MeshLodController.h"
#include "MeshValidator.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

namespace {

/// RAII helper to build argc/argv from a list of C-strings. Kept in an
/// anonymous namespace so it does not collide with the TestArgv in
/// CLIPipeline_test.cpp or other coverage translation units.
class LodArgv {
public:
    LodArgv(std::initializer_list<const char*> args)
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

/// media/models directory relative to the test binary (mirrors the local
/// helper in CLIPipeline_test.cpp, redefined here since that one lives in a
/// different translation unit's anonymous namespace).
QString meshoptTestDataDir()
{
    QString binDir = QCoreApplication::applicationDirPath();
    QDir dir(binDir);
    dir.cdUp(); // bin -> build_local
    dir.cdUp(); // build_local -> project root
    return dir.absoluteFilePath("media/models");
}

} // namespace

// ---------------------------------------------------------------------------
// Pure-argument-validation cases. These all return BEFORE initOgreHeadless()
// is reached, so they need no Ogre / GL / display. They are kept as plain
// TEST() (no fixture) on purpose.
// ---------------------------------------------------------------------------

// --algo with an unrecognized value is rejected at parse time -> exit 2.
TEST(CLIPipelineCmdLodMeshoptCoverageError, InvalidAlgoValueReturns2)
{
    LodArgv args({"qtmesh", "lod", "/tmp/whatever.fbx",
                  "--count", "2", "--algo", "bogus"});
    EXPECT_EQ(2, CLIPipeline::cmdLod(args.argc(), args.argv()));
}

// The algo comparison is case-insensitive on the value but "garbage" is still
// invalid -> exit 2.
TEST(CLIPipelineCmdLodMeshoptCoverageError, InvalidAlgoMixedCaseReturns2)
{
    LodArgv args({"qtmesh", "lod", "/tmp/whatever.fbx",
                  "--count", "2", "--algo", "MeshOptimizer"});
    EXPECT_EQ(2, CLIPipeline::cmdLod(args.argc(), args.argv()));
}

// --algo is only valid with the explicit --count path: combining it with
// --auto must fail fast -> exit 2 (algoSpecified + autoMode guard).
TEST(CLIPipelineCmdLodMeshoptCoverageError, AlgoMeshoptWithAutoReturns2)
{
    LodArgv args({"qtmesh", "lod", "/tmp/whatever.fbx",
                  "--auto", "--algo", "meshopt"});
    EXPECT_EQ(2, CLIPipeline::cmdLod(args.argc(), args.argv()));
}

// --algo meshopt combined with --remove -> exit 2 (algoSpecified + removeMode).
TEST(CLIPipelineCmdLodMeshoptCoverageError, AlgoMeshoptWithRemoveReturns2)
{
    LodArgv args({"qtmesh", "lod", "/tmp/whatever.fbx",
                  "--remove", "--algo", "meshopt"});
    EXPECT_EQ(2, CLIPipeline::cmdLod(args.argc(), args.argv()));
}

// --algo meshopt combined with --info -> exit 2 (algoSpecified + infoMode).
TEST(CLIPipelineCmdLodMeshoptCoverageError, AlgoMeshoptWithInfoReturns2)
{
    LodArgv args({"qtmesh", "lod", "/tmp/whatever.fbx",
                  "--info", "--algo", "meshopt"});
    EXPECT_EQ(2, CLIPipeline::cmdLod(args.argc(), args.argv()));
}

// Explicitly passing the default --algo ogre WITH --auto also trips the guard
// (algoSpecified is set regardless of the chosen value) -> exit 2.
TEST(CLIPipelineCmdLodMeshoptCoverageError, AlgoOgreWithAutoReturns2)
{
    LodArgv args({"qtmesh", "lod", "/tmp/whatever.fbx",
                  "--auto", "--algo", "ogre"});
    EXPECT_EQ(2, CLIPipeline::cmdLod(args.argc(), args.argv()));
}

// Valid --algo meshopt but a nonexistent input file: parse + guards pass, the
// file-not-found check fires -> exit 1.
TEST(CLIPipelineCmdLodMeshoptCoverageError, AlgoMeshoptMissingFileReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("absent_meshopt_lod.fbx");
    ASSERT_FALSE(QFileInfo::exists(missing));
    const QByteArray missingBa = missing.toUtf8();

    LodArgv args({"qtmesh", "lod", missingBa.constData(),
                  "--count", "2", "--algo", "meshopt"});
    EXPECT_EQ(1, CLIPipeline::cmdLod(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// Ogre-backed success cases. These actually load a mesh, run the meshopt LOD
// backend (MeshLodController::Algorithm::Meshopt) and assert the exported LOD
// files exist. Same fixture pattern as CLIPipelineCmdLodTest.
// ---------------------------------------------------------------------------
class CLIPipelineCmdLodMeshoptCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        MeshLodController::kill();
        MeshValidator::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();
        if (Manager::getSingletonPtr())
            SelectionSet::getSingleton()->clear();
    }
    void TearDown() override {
        if (Manager::getSingletonPtr()) {
            SelectionSet::getSingleton()->clear();
            auto nodes = Manager::getSingleton()->getSceneNodes();
            for (auto* node : nodes) {
                Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
                Manager::getSingleton()->destroySceneNode(node);
            }
        }
        MeshLodController::kill();
        MeshValidator::kill();
    }

    // Copy the robot.mesh (+ sibling skeleton) fixture into a private temp dir
    // so we never write LOD outputs next to the repo's checked-in asset.
    // Returns the copied mesh path, or empty on failure.
    static QString stageRobotMesh(QTemporaryDir& dir) {
        if (!dir.isValid())
            return {};
        const QString fixtureMesh = meshoptTestDataDir() + "/robot.mesh";
        if (!QFile::exists(fixtureMesh))
            return {};
        const QString staged = dir.filePath("robot.mesh");
        QFile::remove(staged);
        if (!QFile::copy(fixtureMesh, staged))
            return {};
        const QString fixtureSkel = meshoptTestDataDir() + "/robot.skeleton";
        if (QFile::exists(fixtureSkel)) {
            const QString stagedSkel = dir.filePath("robot.skeleton");
            QFile::remove(stagedSkel);
            QFile::copy(fixtureSkel, stagedSkel);
        }
        return staged;
    }
};

// --count 2 --algo meshopt -o out.mesh: drives the Meshopt branch end to end.
// Expect exit 0 and both per-LOD output files written.
TEST_F(CLIPipelineCmdLodMeshoptCoverageTest, CountTwoMeshoptGeneratesLodFiles)
{
    QTemporaryDir sourceDir;
    const QString sourceFile = stageRobotMesh(sourceDir);
    ASSERT_FALSE(sourceFile.isEmpty()) << "robot.mesh fixture not found";
    ASSERT_TRUE(QFile::exists(sourceFile));
    const QByteArray sourceBa = sourceFile.toUtf8();

    QTemporaryDir outDir;
    ASSERT_TRUE(outDir.isValid());
    const QString outputStem = outDir.filePath("meshopt_out.mesh");
    const QByteArray outputBa = outputStem.toUtf8();

    LodArgv args({"qtmesh", "lod", sourceBa.constData(),
                  "--count", "2",
                  "--algo", "meshopt",
                  "-o", outputBa.constData()});
    EXPECT_EQ(0, CLIPipeline::cmdLod(args.argc(), args.argv()));

    const QString lod1 = outDir.filePath("meshopt_out_lod1.mesh");
    const QString lod2 = outDir.filePath("meshopt_out_lod2.mesh");
    EXPECT_TRUE(QFile::exists(lod1)) << "missing " << lod1.toStdString();
    EXPECT_TRUE(QFile::exists(lod2)) << "missing " << lod2.toStdString();
}

// --count 2 --reductions r,... --algo meshopt -o out.mesh: explicit reductions
// flow through to the Meshopt backend. Expect exit 0 and outputs present.
TEST_F(CLIPipelineCmdLodMeshoptCoverageTest, CountWithReductionsMeshoptGeneratesLodFiles)
{
    QTemporaryDir sourceDir;
    const QString sourceFile = stageRobotMesh(sourceDir);
    ASSERT_FALSE(sourceFile.isEmpty()) << "robot.mesh fixture not found";
    const QByteArray sourceBa = sourceFile.toUtf8();

    QTemporaryDir outDir;
    ASSERT_TRUE(outDir.isValid());
    const QString outputStem = outDir.filePath("meshopt_red_out.mesh");
    const QByteArray outputBa = outputStem.toUtf8();

    LodArgv args({"qtmesh", "lod", sourceBa.constData(),
                  "--count", "2",
                  "--reductions", "0.5,0.25",
                  "--algo", "meshopt",
                  "-o", outputBa.constData()});
    EXPECT_EQ(0, CLIPipeline::cmdLod(args.argc(), args.argv()));

    const QString lod1 = outDir.filePath("meshopt_red_out_lod1.mesh");
    const QString lod2 = outDir.filePath("meshopt_red_out_lod2.mesh");
    EXPECT_TRUE(QFile::exists(lod1)) << "missing " << lod1.toStdString();
    EXPECT_TRUE(QFile::exists(lod2)) << "missing " << lod2.toStdString();
}

// Single LOD with --algo meshopt and no explicit -o: outputs are written next
// to the (temp-staged) source as <basename>_lod1.mesh. Confirms the
// default-output-naming + Meshopt branch combination.
TEST_F(CLIPipelineCmdLodMeshoptCoverageTest, CountOneMeshoptDefaultOutputNaming)
{
    QTemporaryDir sourceDir;
    const QString sourceFile = stageRobotMesh(sourceDir);
    ASSERT_FALSE(sourceFile.isEmpty()) << "robot.mesh fixture not found";
    const QByteArray sourceBa = sourceFile.toUtf8();

    LodArgv args({"qtmesh", "lod", sourceBa.constData(),
                  "--count", "1",
                  "--algo", "meshopt"});
    EXPECT_EQ(0, CLIPipeline::cmdLod(args.argc(), args.argv()));

    const QString lod1 = sourceDir.filePath("robot_lod1.mesh");
    EXPECT_TRUE(QFile::exists(lod1)) << "missing " << lod1.toStdString();
}

// Sanity: the default backend (no --algo flag) still succeeds for the same
// fixture, so the Meshopt-specific cases above isolate the #398 branch rather
// than a generic regression. Exit 0 + LOD outputs present.
TEST_F(CLIPipelineCmdLodMeshoptCoverageTest, CountTwoDefaultOgreStillGeneratesLodFiles)
{
    QTemporaryDir sourceDir;
    const QString sourceFile = stageRobotMesh(sourceDir);
    ASSERT_FALSE(sourceFile.isEmpty()) << "robot.mesh fixture not found";
    const QByteArray sourceBa = sourceFile.toUtf8();

    QTemporaryDir outDir;
    ASSERT_TRUE(outDir.isValid());
    const QString outputStem = outDir.filePath("ogre_out.mesh");
    const QByteArray outputBa = outputStem.toUtf8();

    LodArgv args({"qtmesh", "lod", sourceBa.constData(),
                  "--count", "2",
                  "-o", outputBa.constData()});
    EXPECT_EQ(0, CLIPipeline::cmdLod(args.argc(), args.argv()));

    const QString lod1 = outDir.filePath("ogre_out_lod1.mesh");
    const QString lod2 = outDir.filePath("ogre_out_lod2.mesh");
    EXPECT_TRUE(QFile::exists(lod1)) << "missing " << lod1.toStdString();
    EXPECT_TRUE(QFile::exists(lod2)) << "missing " << lod2.toStdString();
}
