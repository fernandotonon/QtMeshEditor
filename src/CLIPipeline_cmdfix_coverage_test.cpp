// Coverage tests for CLIPipeline::cmdFix exercised on a real .mesh asset
// (media/models/robot.mesh) rather than the FBX the existing CLIPipeline_test.cpp
// cmdFix cases use. Running cmdFix on a .mesh hits code paths that an FBX input
// never reaches:
//
//   * The Assimp "before counts" raw ReadFile block (CLIPipeline.cpp ~1534-1559):
//     Assimp cannot parse Ogre's binary .mesh, so rawScene is null and
//     vertsBefore/trisBefore stay 0 -> the `vertsBefore > 0` guard takes its
//     FALSE branch (the percent-change report is suppressed). The FBX cases all
//     take the TRUE branch, so this is the complementary path.
//   * The in-place overwrite branch (lines ~1514-1516): omitting -o sets
//     outputPath = inputPath. We copy robot.mesh into a QTemporaryDir and fix it
//     in place.
//   * The `--all` flag setting BOTH opts (lines ~1524-1527) AND the
//     opts.anySet() "Extra:" report line (lines ~1606-1611) on a real .mesh
//     (the existing --all test uses FBX).
//
// Distinct filename + distinct suite name (CLIPipeline_cmdFixCoverageTest) from
// CLIPipeline_test.cpp's CLIPipelineCmdTest so there is no ODR clash / duplicate
// TEST registration. Ogre IS available in CI (Linux + Xvfb): SetUp does
// ASSERT_TRUE(tryInitOgre()) + createStandardOgreMaterials(), never GTEST_SKIP.

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
#include "TestHelpers.h"

namespace {

/// RAII argc/argv builder, mirroring TestArgv in CLIPipeline_test.cpp but kept
/// in this TU's anonymous namespace so it does not collide at link time.
class FixArgv {
public:
    FixArgv(std::initializer_list<const char*> args)
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

class CLIPipeline_cmdFixCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();
        m_robot = testRobotMeshPath();
        ASSERT_FALSE(m_robot.isEmpty()) << "robot.mesh not found";
        ASSERT_TRUE(QFile::exists(m_robot));
    }

    // Destroy any scene nodes/entities cmdFix imported so each test starts clean
    // and we don't accumulate Ogre state across the suite.
    void TearDown() override {
        if (!Manager::getSingletonPtr()) return;
        auto nodes = Manager::getSingleton()->getSceneNodes(); // copy
        for (auto* node : nodes) {
            Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
            Manager::getSingleton()->destroySceneNode(node);
        }
    }

    QString m_robot;
};

// ---------------------------------------------------------------------------
// fix robot.mesh -> .mesh with explicit -o.
//
// Covers: the Assimp raw-read "before" block where rawScene is NULL for a .mesh
// (vertsBefore stays 0), the full import -> extractMeshInfo after-count loop,
// the Ogre export, and the `vertsBefore > 0` FALSE branch (percent report
// suppressed). Asserts exit 0 + output file exists.
// ---------------------------------------------------------------------------
TEST_F(CLIPipeline_cmdFixCoverageTest, FixMeshWithOutputReturns0AndWritesFile)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString robotBaSrc = m_robot;
    const QByteArray inBa = robotBaSrc.toUtf8();

    const QString outFile = QDir(tmp.path()).filePath("robot_fixed.mesh");
    const QByteArray outBa = outFile.toUtf8();
    ASSERT_FALSE(QFile::exists(outFile));

    FixArgv args({"qtmesh", "fix", inBa.constData(), "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
}

// ---------------------------------------------------------------------------
// In-place overwrite: omit -o so outputPath = inputPath (lines ~1514-1516).
//
// Copy robot.mesh into a QTemporaryDir first, then fix it in place. Assert exit
// 0 and the file still exists afterward. Working on a copy keeps the repo's
// canonical robot.mesh untouched.
// ---------------------------------------------------------------------------
TEST_F(CLIPipeline_cmdFixCoverageTest, FixMeshInPlaceReturns0AndFileStillExists)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString copy = QDir(tmp.path()).filePath("robot_inplace.mesh");
    ASSERT_TRUE(QFile::copy(m_robot, copy));
    ASSERT_TRUE(QFile::exists(copy));

    const QByteArray copyBa = copy.toUtf8();

    // No -o -> outputPath defaults to inputPath (overwrite in place).
    FixArgv args({"qtmesh", "fix", copyBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(copy));
}

// ---------------------------------------------------------------------------
// --all on a real .mesh: sets BOTH opts.removeDegenerates and
// opts.mergeMaterials (lines ~1524-1527), so opts.anySet() is true and the
// "Extra: remove-degenerates, merge-materials" report line (lines ~1606-1611)
// is emitted. The existing --all coverage uses FBX; this is the .mesh variant.
// ---------------------------------------------------------------------------
TEST_F(CLIPipeline_cmdFixCoverageTest, FixMeshAllFlagSetsBothOptsReturns0)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QByteArray inBa = m_robot.toUtf8();
    const QString outFile = QDir(tmp.path()).filePath("robot_fixed_all.mesh");
    const QByteArray outBa = outFile.toUtf8();
    ASSERT_FALSE(QFile::exists(outFile));

    FixArgv args({"qtmesh", "fix", inBa.constData(), "-o", outBa.constData(), "--all"});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
}

// ---------------------------------------------------------------------------
// --remove-degenerates alone on a .mesh: anySet() true via a single extra,
// exercising the "Extra:" report line with one entry. Distinct from the --all
// (two-entry) case above.
// ---------------------------------------------------------------------------
TEST_F(CLIPipeline_cmdFixCoverageTest, FixMeshRemoveDegeneratesReturns0)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QByteArray inBa = m_robot.toUtf8();
    const QString outFile = QDir(tmp.path()).filePath("robot_fixed_degen.mesh");
    const QByteArray outBa = outFile.toUtf8();

    FixArgv args({"qtmesh", "fix", inBa.constData(), "-o", outBa.constData(),
                  "--remove-degenerates"});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
}

// ---------------------------------------------------------------------------
// --merge-materials alone on a .mesh: the other single-extra "Extra:" branch.
// ---------------------------------------------------------------------------
TEST_F(CLIPipeline_cmdFixCoverageTest, FixMeshMergeMaterialsReturns0)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QByteArray inBa = m_robot.toUtf8();
    const QString outFile = QDir(tmp.path()).filePath("robot_fixed_merge.mesh");
    const QByteArray outBa = outFile.toUtf8();

    FixArgv args({"qtmesh", "fix", inBa.constData(), "-o", outBa.constData(),
                  "--merge-materials"});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
}

// ---------------------------------------------------------------------------
// Long-form --output on a .mesh: confirms the -o/--output alias parsing reaches
// the same success path with a .mesh input.
// ---------------------------------------------------------------------------
TEST_F(CLIPipeline_cmdFixCoverageTest, FixMeshLongFormOutputReturns0)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QByteArray inBa = m_robot.toUtf8();
    const QString outFile = QDir(tmp.path()).filePath("robot_fixed_long.mesh");
    const QByteArray outBa = outFile.toUtf8();

    FixArgv args({"qtmesh", "fix", inBa.constData(), "--output", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
}

// ---------------------------------------------------------------------------
// Error path: missing input file argument -> usage error, return 2 (lines
// ~1508-1512). Pure parser path, no Ogre needed, but kept under the fixture so
// the suite has a uniform setup.
// ---------------------------------------------------------------------------
TEST_F(CLIPipeline_cmdFixCoverageTest, NoInputFileReturns2)
{
    FixArgv args({"qtmesh", "fix"});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 2);
}

// ---------------------------------------------------------------------------
// Error path: input file does not exist -> return 1 (lines ~1518-1522). The
// nonexistent path still has a .mesh suffix so it would route to the same
// format handling had it existed.
// ---------------------------------------------------------------------------
TEST_F(CLIPipeline_cmdFixCoverageTest, NonexistentMeshReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("does_not_exist_robot.mesh");
    ASSERT_FALSE(QFile::exists(missing));
    const QByteArray missingBa = missing.toUtf8();

    FixArgv args({"qtmesh", "fix", missingBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 1);
}
