// Coverage tests for CLIPipeline::cmdAnim — the --analyze and --simplify
// SUCCESS execution paths.
//
// Existing tests in CLIPipeline_test.cpp cover --resample and --decimate-step
// success, plus the cmdAnim error/usage branches, but they never drive the
// --simplify success path (simplifyMode block, AnimationMerger::simplifyAnimation
// + export) nor the --analyze success path (the skeleton-structure analyze block
// that returns at CLIPipeline.cpp:1885). These tests exercise both, on a real
// animated mesh (media/models/Twist Dance.fbx), asserting exit code == 0 and
// (for --simplify) that the rewritten output file exists.
//
// Distinct filename + distinct suite name (CLIPipeline_cmdAnimSimplifyCoverageTest)
// from CLIPipeline_test.cpp so there is no ODR clash / duplicate registration.
//
// NOTE on Ogre: cmdAnim calls initOgreHeadless() and imports an FBX. CI provides
// Ogre + Xvfb. We follow the repo convention: in SetUp, ASSERT_TRUE(tryInitOgre())
// then createStandardOgreMaterials(); we never GTEST_SKIP. We never create a
// QApplication (test_main.cpp owns the single instance).

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
#include <vector>

#include <Ogre.h>

#include "CLIPipeline.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "TestHelpers.h"

namespace {

/// RAII helper to build argc/argv from a list of C-strings. Kept in this
/// translation unit's anonymous namespace so it does not collide with the
/// TestArgv defined in CLIPipeline_test.cpp.
class AnimArgv {
public:
    AnimArgv(std::initializer_list<const char*> args)
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

/// media/models directory relative to the test binary (bin -> build -> root).
QString animTestDataDir()
{
    QDir dir(QCoreApplication::applicationDirPath());
    dir.cdUp(); // bin -> build_local
    dir.cdUp(); // build_local -> project root
    return dir.absoluteFilePath("media/models");
}

QString twistDanceFbx()
{
    return animTestDataDir() + "/Twist Dance.fbx";
}

/// Destroy every scene node + attached movable so each test starts clean.
void clearScene()
{
    if (!Manager::getSingletonPtr())
        return;
    auto nodes = Manager::getSingleton()->getSceneNodes(); // copy
    for (auto* node : nodes) {
        Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
        Manager::getSingleton()->destroySceneNode(node);
    }
}

/// Import the file, return the name of its first skeletal animation (UTF-8),
/// then clear the scene. Empty if no skeleton/animation.
QByteArray firstAnimName(const QString& filePath)
{
    if (!Manager::getSingletonPtr())
        return QByteArray();

    MeshImporterExporter::importer({filePath});
    auto& entities = Manager::getSingleton()->getEntities();
    QByteArray name;
    if (!entities.isEmpty() && entities.first()->hasSkeleton()) {
        Ogre::SkeletonPtr skel = entities.first()->getMesh()->getSkeleton();
        if (skel && skel->getNumAnimations() > 0)
            name = QString::fromStdString(
                       skel->getAnimation(static_cast<unsigned short>(0))->getName())
                       .toUtf8();
    }
    clearScene();
    return name;
}

} // namespace

// ---------------------------------------------------------------------------
// Fixture: Ogre-backed (CI has Ogre + Xvfb). Mirrors CLIPipelineCmdTest.
// ---------------------------------------------------------------------------
class CLIPipeline_cmdAnimSimplifyCoverageTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        // Warm up the import pipeline once: the first FBX import in a process
        // can fail due to lazy plugin/resource init. Mirrors CLIPipelineCmdTest.
        if (!tryInitOgre() || !canLoadMeshFiles())
            return;
        createStandardOgreMaterials();
        CLIPipeline::initOgreHeadless();

        const QString warmup = twistDanceFbx();
        if (QFile::exists(warmup)) {
            MeshImporterExporter::importer({warmup});
            clearScene();
        }
    }

    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();
        ASSERT_TRUE(CLIPipeline::initOgreHeadless());
    }

    void TearDown() override
    {
        clearScene();
    }
};

// ===========================================================================
// --analyze success (text) : exercises the skeleton-structure analyze block
// (CLIPipeline.cpp:1856-1885). Read-only, no -o required. rc == 0.
// ===========================================================================
TEST_F(CLIPipeline_cmdAnimSimplifyCoverageTest, AnalyzeTextSucceeds)
{
    const QString file = twistDanceFbx();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(), "--analyze"});
    EXPECT_EQ(0, CLIPipeline::cmdAnim(args.argc(), args.argv()));
}

// --analyze --json success. Same block, JSON branch. rc == 0.
TEST_F(CLIPipeline_cmdAnimSimplifyCoverageTest, AnalyzeJsonSucceeds)
{
    const QString file = twistDanceFbx();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(), "--analyze", "--json"});
    EXPECT_EQ(0, CLIPipeline::cmdAnim(args.argc(), args.argv()));
}

// --analyze with --cli token interleaved (token-skipping branch) still rc == 0.
TEST_F(CLIPipeline_cmdAnimSimplifyCoverageTest, AnalyzeWithCliFlagSucceeds)
{
    const QString file = twistDanceFbx();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    AnimArgv args({"qtmesh", "--cli", "anim", fileBa.constData(), "--analyze"});
    EXPECT_EQ(0, CLIPipeline::cmdAnim(args.argc(), args.argv()));
}

// --analyze with tolerance/preset flags parsed but ignored by the structure
// analyze block. Confirms the flag parser accepts them and still returns 0.
TEST_F(CLIPipeline_cmdAnimSimplifyCoverageTest, AnalyzeWithPresetAndToleranceSucceeds)
{
    const QString file = twistDanceFbx();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(), "--analyze",
                   "--preset", "aggressive",
                   "--tolerance", "0.002",
                   "--rotation-tolerance-deg", "1.0"});
    EXPECT_EQ(0, CLIPipeline::cmdAnim(args.argc(), args.argv()));
}

// ===========================================================================
// --simplify -o success : exercises the simplifyMode branch
// (CLIPipeline.cpp:2253-2283), AnimationMerger::simplifyAnimation per anim +
// MeshImporterExporter::exporter. rc == 0 and the rewritten mesh exists.
// We copy the source into a QTemporaryDir so we never overwrite test data.
// ===========================================================================
TEST_F(CLIPipeline_cmdAnimSimplifyCoverageTest, SimplifyToOutputSucceeds)
{
    const QString file = twistDanceFbx();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = QDir(tmp.path()).filePath("simplified.mesh");
    QByteArray outBa = outFile.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--simplify",
                   "-o", outBa.constData()});
    EXPECT_EQ(0, CLIPipeline::cmdAnim(args.argc(), args.argv()));
    EXPECT_TRUE(QFile::exists(outFile)) << "simplify output not written: " << outFile.toStdString();
}

// --simplify with an explicit --animation filter targeting the first clip.
// Exercises the filter-skip branch (only the named clip is simplified) +
// the wholeFile==false projection path. rc == 0, output exists.
TEST_F(CLIPipeline_cmdAnimSimplifyCoverageTest, SimplifyWithAnimationFilterSucceeds)
{
    const QString file = twistDanceFbx();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    const QByteArray animName = firstAnimName(file);
    ASSERT_FALSE(animName.isEmpty()) << "Could not discover an animation name";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = QDir(tmp.path()).filePath("simplified_filtered.mesh");
    QByteArray outBa = outFile.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--simplify",
                   "--animation", animName.constData(),
                   "-o", outBa.constData()});
    EXPECT_EQ(0, CLIPipeline::cmdAnim(args.argc(), args.argv()));
    EXPECT_TRUE(QFile::exists(outFile)) << "filtered simplify output not written: "
                                        << outFile.toStdString();
}

// --simplify --json : simplifyMode does not branch on jsonOutput (the JSON
// branch lives in the analyze-only block), so the flag is accepted but the
// summary is still emitted. Confirms rc == 0 and output exists.
TEST_F(CLIPipeline_cmdAnimSimplifyCoverageTest, SimplifyJsonFlagStillSucceeds)
{
    const QString file = twistDanceFbx();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = QDir(tmp.path()).filePath("simplified_json.mesh");
    QByteArray outBa = outFile.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--simplify", "--json",
                   "-o", outBa.constData()});
    EXPECT_EQ(0, CLIPipeline::cmdAnim(args.argc(), args.argv()));
    EXPECT_TRUE(QFile::exists(outFile));
}

// --simplify with an aggressive preset (looser tolerances → more keyframes
// removed). Exercises tolerancesForPreset() wiring into SimplifyTolerances.
// rc == 0, output exists.
TEST_F(CLIPipeline_cmdAnimSimplifyCoverageTest, SimplifyAggressivePresetSucceeds)
{
    const QString file = twistDanceFbx();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = QDir(tmp.path()).filePath("simplified_aggressive.mesh");
    QByteArray outBa = outFile.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--simplify", "--preset", "aggressive",
                   "-o", outBa.constData()});
    EXPECT_EQ(0, CLIPipeline::cmdAnim(args.argc(), args.argv()));
    EXPECT_TRUE(QFile::exists(outFile));
}

// --simplify with explicit --tolerance / --rotation-tolerance-deg overrides.
// Exercises the manual-tolerance parsing branch. rc == 0, output exists.
TEST_F(CLIPipeline_cmdAnimSimplifyCoverageTest, SimplifyExplicitTolerancesSucceeds)
{
    const QString file = twistDanceFbx();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = QDir(tmp.path()).filePath("simplified_tol.mesh");
    QByteArray outBa = outFile.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--simplify",
                   "--tolerance", "0.005",
                   "--rotation-tolerance-deg", "2.0",
                   "-o", outBa.constData()});
    EXPECT_EQ(0, CLIPipeline::cmdAnim(args.argc(), args.argv()));
    EXPECT_TRUE(QFile::exists(outFile));
}

// ===========================================================================
// Error/edge paths that share the simplify/analyze parser.
// ===========================================================================

// --simplify with an --animation that matches nothing -> "No matching
// animation found." -> rc == 1. (Hits the matched==0 branch at 2161.)
TEST_F(CLIPipeline_cmdAnimSimplifyCoverageTest, SimplifyNoMatchingAnimationReturns1)
{
    const QString file = twistDanceFbx();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = QDir(tmp.path()).filePath("simplified_nomatch.mesh");
    QByteArray outBa = outFile.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--simplify",
                   "--animation", "__no_such_animation__",
                   "-o", outBa.constData()});
    EXPECT_EQ(1, CLIPipeline::cmdAnim(args.argc(), args.argv()));
    EXPECT_FALSE(QFile::exists(outFile));
}

// --analyze on a file with no skeleton -> rc == 1 ("No skeleton found.").
TEST_F(CLIPipeline_cmdAnimSimplifyCoverageTest, AnalyzeNoSkeletonReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString objFile = QDir(tmp.path()).filePath("noskel.obj");
    {
        QFile f(objFile);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        f.close();
    }
    QByteArray fileBa = objFile.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(), "--analyze"});
    EXPECT_EQ(1, CLIPipeline::cmdAnim(args.argc(), args.argv()));
}

// --analyze on a nonexistent file -> rc == 1 ("File not found.").
TEST_F(CLIPipeline_cmdAnimSimplifyCoverageTest, AnalyzeMissingFileReturns1)
{
    AnimArgv args({"qtmesh", "anim",
                   "/tmp/nonexistent_cli_anim_analyze_999999.fbx", "--analyze"});
    EXPECT_EQ(1, CLIPipeline::cmdAnim(args.argc(), args.argv()));
}

// --simplify on a nonexistent file -> rc == 1. (outputPath defaults to the
// input via the "overwrite in place" branch, then file-not-found fires.)
TEST_F(CLIPipeline_cmdAnimSimplifyCoverageTest, SimplifyMissingFileReturns1)
{
    AnimArgv args({"qtmesh", "anim",
                   "/tmp/nonexistent_cli_anim_simplify_999999.fbx", "--simplify"});
    EXPECT_EQ(1, CLIPipeline::cmdAnim(args.argc(), args.argv()));
}

// --simplify with an unknown --preset -> usage error rc == 2 (before any I/O).
TEST_F(CLIPipeline_cmdAnimSimplifyCoverageTest, SimplifyUnknownPresetReturns2)
{
    const QString file = twistDanceFbx();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--simplify", "--preset", "__bogus__", "-o", "out.mesh"});
    EXPECT_EQ(2, CLIPipeline::cmdAnim(args.argc(), args.argv()));
}
