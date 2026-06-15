// Coverage tests for CLIPipeline::cmdNodeAnim — the node-animation list
// subcommand parser.
//
// These exercise the pure-logic argument-validation branches that all return
// BEFORE initOgreHeadless() is ever called, so they need no Ogre, no display,
// and no QApplication of their own (src/test_main.cpp owns the single
// QCoreApplication). err() writes to a static QTextStream over stderr and is
// safe to invoke headlessly.
//
// The three pre-Ogre branches in cmdNodeAnim:
//   1. no input file              -> usage error -> return 2
//   2. file given but no --list   -> "requires --list" error -> return 2
//   3. --list + nonexistent file  -> file-not-found -> return 1
// Anything past the file-existence check calls initOgreHeadless(), which is not
// testable headlessly, so those paths are deliberately not exercised here.
//
// Distinct filename + distinct suite name (CLIPipeline_cmdNodeAnimCoverageTest)
// from the existing CLIPipeline_test.cpp so there is no ODR clash / duplicate
// registration with any prior coverage.

#include <gtest/gtest.h>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <initializer_list>

#include "CLIPipeline.h"

namespace {

/// RAII helper to build argc/argv from a list of C-strings, mirroring the
/// TestArgv used in CLIPipeline_test.cpp (kept in an anonymous namespace here
/// so it does not collide with that translation unit's copy).
class NodeAnimArgv {
public:
    NodeAnimArgv(std::initializer_list<const char*> args)
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
// Branch 1: no input file -> usage error, return 2
// ---------------------------------------------------------------------------
TEST(CLIPipeline_cmdNodeAnimCoverageTest, NoInputFileReturns2)
{
    NodeAnimArgv args({"qtmesh", "nodeanim"});
    EXPECT_EQ(2, CLIPipeline::cmdNodeAnim(args.argc(), args.argv()));
}

// --list present but no positional file is still "no input file" (the file
// check runs before the --list check).
TEST(CLIPipeline_cmdNodeAnimCoverageTest, ListButNoFileReturns2)
{
    NodeAnimArgv args({"qtmesh", "nodeanim", "--list"});
    EXPECT_EQ(2, CLIPipeline::cmdNodeAnim(args.argc(), args.argv()));
}

// --list and --json present but no positional file -> still 2.
TEST(CLIPipeline_cmdNodeAnimCoverageTest, ListJsonNoFileReturns2)
{
    NodeAnimArgv args({"qtmesh", "nodeanim", "--list", "--json"});
    EXPECT_EQ(2, CLIPipeline::cmdNodeAnim(args.argc(), args.argv()));
}

// A leading-dash token is never accepted as the positional file, so an
// unknown flag does not satisfy the file requirement -> 2.
TEST(CLIPipeline_cmdNodeAnimCoverageTest, UnknownFlagOnlyReturns2)
{
    NodeAnimArgv args({"qtmesh", "nodeanim", "--bogus", "--list"});
    EXPECT_EQ(2, CLIPipeline::cmdNodeAnim(args.argc(), args.argv()));
}

// The --cli token is skipped just like "nodeanim"; with no real file it is 2.
TEST(CLIPipeline_cmdNodeAnimCoverageTest, CliTokenSkippedNoFileReturns2)
{
    NodeAnimArgv args({"qtmesh", "--cli", "nodeanim", "--list"});
    EXPECT_EQ(2, CLIPipeline::cmdNodeAnim(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// Branch 2: file given but no --list -> "requires --list" error, return 2
// ---------------------------------------------------------------------------
TEST(CLIPipeline_cmdNodeAnimCoverageTest, FileWithoutListReturns2)
{
    NodeAnimArgv args({"qtmesh", "nodeanim", "model.fbx"});
    EXPECT_EQ(2, CLIPipeline::cmdNodeAnim(args.argc(), args.argv()));
}

// File + --json but still no --list -> the missing-list gate fires first -> 2.
TEST(CLIPipeline_cmdNodeAnimCoverageTest, FileWithJsonButNoListReturns2)
{
    NodeAnimArgv args({"qtmesh", "nodeanim", "model.fbx", "--json"});
    EXPECT_EQ(2, CLIPipeline::cmdNodeAnim(args.argc(), args.argv()));
}

// Even an existing file without --list returns 2 (the --list gate is reached
// before the file-existence check).
TEST(CLIPipeline_cmdNodeAnimCoverageTest, ExistingFileWithoutListReturns2)
{
    QTemporaryFile tmp;
    ASSERT_TRUE(tmp.open());
    const QByteArray pathBa = tmp.fileName().toUtf8();
    ASSERT_TRUE(QFileInfo::exists(tmp.fileName()));

    NodeAnimArgv args({"qtmesh", "nodeanim", pathBa.constData()});
    EXPECT_EQ(2, CLIPipeline::cmdNodeAnim(args.argc(), args.argv()));
}

// Only the first non-dash token becomes the file; a second positional is
// ignored. Without --list this is still the missing-list branch -> 2.
TEST(CLIPipeline_cmdNodeAnimCoverageTest, ExtraPositionalNoListReturns2)
{
    NodeAnimArgv args({"qtmesh", "nodeanim", "first.fbx", "second.fbx"});
    EXPECT_EQ(2, CLIPipeline::cmdNodeAnim(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// Branch 3: --list + nonexistent file -> file-not-found, return 1
// ---------------------------------------------------------------------------
TEST(CLIPipeline_cmdNodeAnimCoverageTest, ListNonexistentFileReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("nonexistent_nodeanim.fbx");
    ASSERT_FALSE(QFileInfo::exists(missing));
    const QByteArray missingBa = missing.toUtf8();

    NodeAnimArgv args({"qtmesh", "nodeanim", missingBa.constData(), "--list"});
    EXPECT_EQ(1, CLIPipeline::cmdNodeAnim(args.argc(), args.argv()));
}

// Same as above but with --json also set: still hits file-not-found -> 1.
TEST(CLIPipeline_cmdNodeAnimCoverageTest, ListJsonNonexistentFileReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("absent_nodeanim.gltf");
    ASSERT_FALSE(QFileInfo::exists(missing));
    const QByteArray missingBa = missing.toUtf8();

    NodeAnimArgv args({"qtmesh", "nodeanim", missingBa.constData(),
                       "--list", "--json"});
    EXPECT_EQ(1, CLIPipeline::cmdNodeAnim(args.argc(), args.argv()));
}

// Argument order does not matter: --list before the file path still parses,
// missing file -> 1.
TEST(CLIPipeline_cmdNodeAnimCoverageTest, ListBeforeFileNonexistentReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("order_nodeanim.dae");
    const QByteArray missingBa = missing.toUtf8();

    NodeAnimArgv args({"qtmesh", "nodeanim", "--list", missingBa.constData()});
    EXPECT_EQ(1, CLIPipeline::cmdNodeAnim(args.argc(), args.argv()));
}
