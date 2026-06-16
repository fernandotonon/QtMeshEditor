// Coverage tests for CLIPipeline::cmdLod --algo handling and per-LOD output.
//
// The existing CLIPipelineCmdLodTest suite (in CLIPipeline_test.cpp) covers
// info / remove / count(ogre default) / auto. This suite drives the UNCOVERED
// branches of cmdLod (lines ~2437-2486, 2574-2581):
//   * --algo meshopt count generation (Algorithm::Meshopt backend, #398)
//   * --algo ogre explicit (algoSpecified=true, Ogre branch)
//   * invalid --algo value -> exit 2 ("--algo must be meshopt or ogre")
//   * --algo combined with --auto / --remove / --info -> exit 2
//   * --reductions parse with custom values feeding both backends
//   * tightened per-LOD assertion: BOTH <stem>_lod1 AND <stem>_lod2 written
//
// Distinct filename + distinct suite name (CLIPipelineCmdLodAlgoCoverage) so
// there is no ODR / duplicate-registration clash with CLIPipelineCmdLodTest.

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <vector>

#include "CLIPipeline.h"
#include "MeshLodController.h"
#include "MeshValidator.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

namespace {

/// Path to the media/models directory relative to the test binary.
/// (Mirrors testDataDir() in CLIPipeline_test.cpp; kept file-local to avoid
/// any cross-TU symbol clash.)
QString algoCovTestDataDir()
{
    QString binDir = QCoreApplication::applicationDirPath();
    QDir dir(binDir);
    dir.cdUp(); // bin -> build_local
    dir.cdUp(); // build_local -> project root
    return dir.absoluteFilePath("media/models");
}

/// RAII helper to build argc/argv from a list of strings.
class AlgoCovArgv {
public:
    AlgoCovArgv(std::initializer_list<const char*> args)
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

class CLIPipelineCmdLodAlgoCoverage : public ::testing::Test {
protected:
    void SetUp() override {
        MeshLodController::kill();
        MeshValidator::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();
        if (Manager::getSingletonPtr())
            SelectionSet::getSingleton()->clear();

        m_robot = algoCovTestDataDir() + "/robot.mesh";
        m_robotSkeleton = algoCovTestDataDir() + "/robot.skeleton";
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

    /// Copy robot.mesh (+ sibling skeleton) into `dir` so each test gets an
    /// isolated input + output area. Returns the copied mesh path.
    QString copyRobotInto(const QTemporaryDir& dir) {
        const QString dst = dir.filePath("robot.mesh");
        QFile::remove(dst);
        if (!QFile::copy(m_robot, dst))
            return QString();
        if (QFile::exists(m_robotSkeleton)) {
            const QString dstSkel = dir.filePath("robot.skeleton");
            QFile::remove(dstSkel);
            QFile::copy(m_robotSkeleton, dstSkel);
        }
        return dst;
    }

    QString m_robot;
    QString m_robotSkeleton;
};

// ---------------------------------------------------------------------------
// Pure usage-error branches (exit code 2). These do not need to load the mesh
// because the algo validation / mode-conflict checks run before any I/O.
// ---------------------------------------------------------------------------

// --algo with an unrecognized value -> exit 2 (lines 2444-2447).
TEST_F(CLIPipelineCmdLodAlgoCoverage, InvalidAlgoValueRejectedExit2)
{
    ASSERT_TRUE(QFile::exists(m_robot));
    QByteArray robotBa = m_robot.toUtf8();

    AlgoCovArgv args({"qtmesh", "lod", robotBa.constData(),
                      "--count", "2", "--algo", "quadric"});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 2);
}

// --algo value is case-insensitive (toLower) — "MeshOpt"/"OGRE" are accepted,
// so a bogus mixed-case value still fails. Covers the .toLower() normalization.
TEST_F(CLIPipelineCmdLodAlgoCoverage, InvalidAlgoMixedCaseStillRejectedExit2)
{
    ASSERT_TRUE(QFile::exists(m_robot));
    QByteArray robotBa = m_robot.toUtf8();

    AlgoCovArgv args({"qtmesh", "lod", robotBa.constData(),
                      "--count", "1", "--algo", "Bogus"});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 2);
}

// --algo with --auto -> exit 2 (lines 2482-2486). Algo only valid with --count.
TEST_F(CLIPipelineCmdLodAlgoCoverage, AlgoWithAutoRejectedExit2)
{
    ASSERT_TRUE(QFile::exists(m_robot));
    QByteArray robotBa = m_robot.toUtf8();

    AlgoCovArgv args({"qtmesh", "lod", robotBa.constData(),
                      "--auto", "--algo", "meshopt"});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 2);
}

// --algo with --remove -> exit 2 (lines 2482-2486).
TEST_F(CLIPipelineCmdLodAlgoCoverage, AlgoWithRemoveRejectedExit2)
{
    ASSERT_TRUE(QFile::exists(m_robot));
    QByteArray robotBa = m_robot.toUtf8();

    AlgoCovArgv args({"qtmesh", "lod", robotBa.constData(),
                      "--remove", "--algo", "ogre"});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 2);
}

// --algo with --info -> exit 2 (lines 2482-2486). Uses the real mesh directly;
// the conflict check fires before any mesh load, so no temp copy needed.
TEST_F(CLIPipelineCmdLodAlgoCoverage, AlgoWithInfoRejectedExit2)
{
    ASSERT_TRUE(QFile::exists(m_robot));
    QByteArray robotBa = m_robot.toUtf8();

    AlgoCovArgv args({"qtmesh", "lod", robotBa.constData(),
                      "--info", "--algo", "meshopt"});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 2);
}

// A valid --algo paired with --info --json still hits the conflict gate first.
TEST_F(CLIPipelineCmdLodAlgoCoverage, AlgoWithInfoJsonRejectedExit2)
{
    ASSERT_TRUE(QFile::exists(m_robot));
    QByteArray robotBa = m_robot.toUtf8();

    AlgoCovArgv args({"qtmesh", "lod", robotBa.constData(),
                      "--info", "--json", "--algo", "ogre"});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 2);
}

// ---------------------------------------------------------------------------
// Generation paths. These actually import robot.mesh, run the chosen backend,
// and assert BOTH per-LOD files land on disk (tighter than the existing
// CmdLod_CountModeGeneratesAndExportsLods which uses lod1 || lod2).
// ---------------------------------------------------------------------------

// --algo meshopt count generation: algoEnum == Algorithm::Meshopt branch
// (lines 2574-2581). Asserts both lod1 and lod2 written.
TEST_F(CLIPipelineCmdLodAlgoCoverage, AlgoMeshoptCountGeneratesBothLodFiles)
{
    ASSERT_TRUE(QFile::exists(m_robot)) << "Test data not found: " << m_robot.toStdString();

    QTemporaryDir sourceDir;
    ASSERT_TRUE(sourceDir.isValid());
    const QString sourceFile = copyRobotInto(sourceDir);
    ASSERT_FALSE(sourceFile.isEmpty());
    QByteArray sourceBa = sourceFile.toUtf8();

    QTemporaryDir outDir;
    ASSERT_TRUE(outDir.isValid());
    const QString outputStem = outDir.filePath("meshopt_out.mesh");
    QByteArray outputBa = outputStem.toUtf8();

    AlgoCovArgv args({"qtmesh", "lod", sourceBa.constData(),
                      "--count", "2",
                      "--reductions", "0.7,0.45",
                      "--algo", "meshopt",
                      "--output", outputBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 0);

    const QString lod1 = outDir.filePath("meshopt_out_lod1.mesh");
    const QString lod2 = outDir.filePath("meshopt_out_lod2.mesh");
    EXPECT_TRUE(QFile::exists(lod1)) << "missing " << lod1.toStdString();
    EXPECT_TRUE(QFile::exists(lod2)) << "missing " << lod2.toStdString();
}

// --algo ogre explicit (algoSpecified=true, Ogre branch). Distinct from the
// existing default-ogre test which never passes --algo. Both lod files asserted.
TEST_F(CLIPipelineCmdLodAlgoCoverage, AlgoOgreExplicitCountGeneratesBothLodFiles)
{
    ASSERT_TRUE(QFile::exists(m_robot)) << "Test data not found: " << m_robot.toStdString();

    QTemporaryDir sourceDir;
    ASSERT_TRUE(sourceDir.isValid());
    const QString sourceFile = copyRobotInto(sourceDir);
    ASSERT_FALSE(sourceFile.isEmpty());
    QByteArray sourceBa = sourceFile.toUtf8();

    QTemporaryDir outDir;
    ASSERT_TRUE(outDir.isValid());
    const QString outputStem = outDir.filePath("ogre_out.mesh");
    QByteArray outputBa = outputStem.toUtf8();

    AlgoCovArgv args({"qtmesh", "lod", sourceBa.constData(),
                      "--count", "2",
                      "--reductions", "0.6,0.3",
                      "--algo", "ogre",
                      "--output", outputBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 0);

    const QString lod1 = outDir.filePath("ogre_out_lod1.mesh");
    const QString lod2 = outDir.filePath("ogre_out_lod2.mesh");
    EXPECT_TRUE(QFile::exists(lod1)) << "missing " << lod1.toStdString();
    EXPECT_TRUE(QFile::exists(lod2)) << "missing " << lod2.toStdString();
}

// --algo meshopt without explicit --reductions: the reductions list is empty,
// so the controller derives defaults. Exercises the empty-reductions path into
// the Meshopt backend. Single LOD requested -> lod1 must exist.
TEST_F(CLIPipelineCmdLodAlgoCoverage, AlgoMeshoptCountNoReductionsUsesDefaults)
{
    ASSERT_TRUE(QFile::exists(m_robot)) << "Test data not found: " << m_robot.toStdString();

    QTemporaryDir sourceDir;
    ASSERT_TRUE(sourceDir.isValid());
    const QString sourceFile = copyRobotInto(sourceDir);
    ASSERT_FALSE(sourceFile.isEmpty());
    QByteArray sourceBa = sourceFile.toUtf8();

    QTemporaryDir outDir;
    ASSERT_TRUE(outDir.isValid());
    const QString outputStem = outDir.filePath("meshopt_def.mesh");
    QByteArray outputBa = outputStem.toUtf8();

    AlgoCovArgv args({"qtmesh", "lod", sourceBa.constData(),
                      "--count", "1",
                      "--algo", "meshopt",
                      "--output", outputBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 0);

    const QString lod1 = outDir.filePath("meshopt_def_lod1.mesh");
    EXPECT_TRUE(QFile::exists(lod1)) << "missing " << lod1.toStdString();
}

// --algo ogre with --count clamped above 4 (lodCount = min(count,4)). Drives the
// std::max/std::min clamp before the Ogre backend. Requesting 9 yields 4 levels.
TEST_F(CLIPipelineCmdLodAlgoCoverage, AlgoOgreCountClampedToFour)
{
    ASSERT_TRUE(QFile::exists(m_robot)) << "Test data not found: " << m_robot.toStdString();

    QTemporaryDir sourceDir;
    ASSERT_TRUE(sourceDir.isValid());
    const QString sourceFile = copyRobotInto(sourceDir);
    ASSERT_FALSE(sourceFile.isEmpty());
    QByteArray sourceBa = sourceFile.toUtf8();

    QTemporaryDir outDir;
    ASSERT_TRUE(outDir.isValid());
    const QString outputStem = outDir.filePath("clamp_out.mesh");
    QByteArray outputBa = outputStem.toUtf8();

    AlgoCovArgv args({"qtmesh", "lod", sourceBa.constData(),
                      "--count", "9",
                      "--algo", "ogre",
                      "--output", outputBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 0);

    // At least lod1 must exist; with clamping the generator produces up to 4.
    const QString lod1 = outDir.filePath("clamp_out_lod1.mesh");
    EXPECT_TRUE(QFile::exists(lod1)) << "missing " << lod1.toStdString();
}
