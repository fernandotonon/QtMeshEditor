// Coverage tests for CLIPipeline::cmdAnim --bake-fps path.
//
// The existing src/CLIPipeline_test.cpp exercises --list/--rename/--merge/
// --resample/--decimate-step but has ZERO coverage of the --bake-fps block
// (CLIPipeline.cpp ~2062-2119). This file targets that block:
//   - successful bake exports a file and returns 0
//   - --bake-fps with --animation filter (per-anim path)
//   - --bake-fps 0 hits the `bakeFps < 1` guard and returns 2
//   - --animation <missing> hits the `animsProcessed == 0` guard, returns 1
//   - no -o overwrites the input in place (default-output branch) and returns 0
//
// All names here are deliberately distinct from CLIPipeline_test.cpp to avoid
// ODR clashes / duplicate registration (separate anonymous namespace + a
// _Bake-suffixed suite name + a local TestArgv copy).

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <vector>

#include "CLIPipeline.h"
#include "MeshImporterExporter.h"
#include "Manager.h"
#include "TestHelpers.h"

namespace {

// Path to the media/models directory relative to the test binary.
// (Self-contained copy; the one in CLIPipeline_test.cpp is in a different TU.)
QString bakeTestDataDir()
{
    QString binDir = QCoreApplication::applicationDirPath();
    QDir dir(binDir);
    dir.cdUp(); // bin -> build_local
    dir.cdUp(); // build_local -> project root
    return dir.absoluteFilePath("media/models");
}

// RAII helper to build argc/argv from a list of strings (local copy).
class BakeTestArgv {
public:
    BakeTestArgv(std::initializer_list<const char*> args)
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

// Discover the first skeletal animation name in an animated file, then wipe
// the scene so each test starts clean. Returns empty if no animation found.
QByteArray firstAnimNameForFile(const QString& filePath)
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
                skel->getAnimation(static_cast<unsigned short>(0))->getName()).toUtf8();
    }

    auto nodes = Manager::getSingleton()->getSceneNodes();
    for (auto* node : nodes) {
        Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
        Manager::getSingleton()->destroySceneNode(node);
    }
    return name;
}

} // anonymous namespace

class CLIPipelineCmdAnimBakeCoverageTest : public ::testing::Test {
protected:
    // Warm up the FBX import pipeline once (first import in a process can fail
    // due to lazy plugin/resource init), matching CLIPipelineCmdTest.
    static void SetUpTestSuite() {
        if (!tryInitOgre() || !canLoadMeshFiles()) return;
        createStandardOgreMaterials();

        QString warmupFile = bakeTestDataDir() + "/Twist Dance.fbx";
        if (QFile::exists(warmupFile)) {
            CLIPipeline::initOgreHeadless();
            MeshImporterExporter::importer({warmupFile});
            if (Manager::getSingletonPtr()) {
                auto nodes = Manager::getSingleton()->getSceneNodes();
                for (auto* node : nodes) {
                    Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
                    Manager::getSingleton()->destroySceneNode(node);
                }
            }
        }
    }

    void SetUp() override {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();
    }

    void TearDown() override {
        if (!Manager::getSingletonPtr()) return;
        auto nodes = Manager::getSingleton()->getSceneNodes(); // copy
        for (auto* node : nodes) {
            Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
            Manager::getSingleton()->destroySceneNode(node);
        }
    }

    QString inputFile() const { return bakeTestDataDir() + "/Twist Dance.fbx"; }
};

// --- Success path: bake all animations, explicit -o ---

TEST_F(CLIPipelineCmdAnimBakeCoverageTest, BakeFps_AllAnimations_ExportsFileReturnsZero)
{
    QString file = inputFile();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("baked_all.mesh");
    QByteArray outBa = outFile.toUtf8();

    BakeTestArgv args({"qtmesh", "anim", fileBa.constData(),
                       "--bake-fps", "30",
                       "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile)) << "bake-fps should export an output mesh";
    if (QFile::exists(outFile))
        EXPECT_GT(QFileInfo(outFile).size(), 0);
}

// --- Success path: per-animation filter branch ---

TEST_F(CLIPipelineCmdAnimBakeCoverageTest, BakeFps_WithAnimationFilter_ReturnsZero)
{
    QString file = inputFile();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    const QByteArray animName = firstAnimNameForFile(file);
    ASSERT_FALSE(animName.isEmpty()) << "Could not discover animation name";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("baked_filtered.mesh");
    QByteArray outBa = outFile.toUtf8();

    BakeTestArgv args({"qtmesh", "anim", fileBa.constData(),
                       "--bake-fps", "24",
                       "--animation", animName.constData(),
                       "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
}

// --- Success path: no -o overwrites the input in place (default-output branch) ---

TEST_F(CLIPipelineCmdAnimBakeCoverageTest, BakeFps_NoOutput_OverwritesInPlaceReturnsZero)
{
    QString src = inputFile();
    ASSERT_TRUE(QFile::exists(src)) << "Test data not found: " << src.toStdString();

    // Copy into a temp dir so the in-place overwrite never touches the shared
    // media fixture. Use .mesh so export goes through the Ogre exporter path.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString work = tmp.filePath("inplace.fbx");
    ASSERT_TRUE(QFile::copy(src, work)) << "Failed to stage temp copy";
    const QFileInfo before(work);
    ASSERT_GT(before.size(), 0);

    QByteArray workBa = work.toUtf8();
    BakeTestArgv args({"qtmesh", "anim", workBa.constData(),
                       "--bake-fps", "15"});
    // No -o: bakeFpsMode defaults outputPath to filePath (overwrite in place).
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(work)) << "in-place output should still exist";
}

// --- Guard: --bake-fps 0 hits `bakeFps < 1` and returns usage exit 2 ---

TEST_F(CLIPipelineCmdAnimBakeCoverageTest, BakeFps_ZeroReturnsUsageError)
{
    QString file = inputFile();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("baked_zero.mesh");
    QByteArray outBa = outFile.toUtf8();

    BakeTestArgv args({"qtmesh", "anim", fileBa.constData(),
                       "--bake-fps", "0",
                       "-o", outBa.constData()});
    // The `bakeFps < 1` guard returns 2 (usage error) and does NOT export.
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 2);
    EXPECT_FALSE(QFile::exists(outFile));
}

// --- Guard: negative fps also hits `bakeFps < 1` ---

TEST_F(CLIPipelineCmdAnimBakeCoverageTest, BakeFps_NegativeReturnsUsageError)
{
    QString file = inputFile();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("baked_neg.mesh");
    QByteArray outBa = outFile.toUtf8();

    BakeTestArgv args({"qtmesh", "anim", fileBa.constData(),
                       "--bake-fps", "-5",
                       "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 2);
    EXPECT_FALSE(QFile::exists(outFile));
}

// --- Guard: --animation with no match hits `animsProcessed == 0`, returns 1 ---

TEST_F(CLIPipelineCmdAnimBakeCoverageTest, BakeFps_NoMatchingAnimationReturnsError)
{
    QString file = inputFile();
    ASSERT_TRUE(QFile::exists(file)) << "Test data not found: " << file.toStdString();
    QByteArray fileBa = file.toUtf8();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("baked_nomatch.mesh");
    QByteArray outBa = outFile.toUtf8();

    BakeTestArgv args({"qtmesh", "anim", fileBa.constData(),
                       "--bake-fps", "30",
                       "--animation", "NoSuchAnimation_Bake",
                       "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 1);
    EXPECT_FALSE(QFile::exists(outFile));
}

// --- Guard: missing input file returns 1 (file-not-found, pre-bake) ---

TEST_F(CLIPipelineCmdAnimBakeCoverageTest, BakeFps_MissingInputFileReturnsError)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = tmp.filePath("does_not_exist.fbx");
    QByteArray missingBa = missing.toUtf8();
    const QString outFile = tmp.filePath("baked_missing.mesh");
    QByteArray outBa = outFile.toUtf8();

    BakeTestArgv args({"qtmesh", "anim", missingBa.constData(),
                       "--bake-fps", "30",
                       "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 1);
}
