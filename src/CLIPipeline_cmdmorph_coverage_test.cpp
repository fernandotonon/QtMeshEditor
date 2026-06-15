// Coverage tests for CLIPipeline::cmdMorph — the morph-target / blend-shape
// list subcommand parser.
//
// These exercise the pure-logic argument-validation branches that all return
// BEFORE initOgreHeadless() is ever called, so they need no Ogre, no display,
// and no QApplication of their own (src/test_main.cpp owns the single
// QCoreApplication). err() writes to a static QTextStream over stderr and is
// safe to invoke headlessly.
//
// The three reachable-without-Ogre branches of cmdMorph are:
//   1. No input file specified                       -> return 2
//   2. File given but --list not passed              -> return 2
//   3. --list + nonexistent file                     -> return 1
// (A valid existing file would fall through to initOgreHeadless(), which we
// avoid here because Ogre cannot initialise on a headless macOS test host.)
//
// Distinct filename + distinct suite name (CLIPipeline_cmdMorphCoverageTest)
// from the existing CLIPipeline_test.cpp so there is no ODR clash / duplicate
// registration with any prior cmdMorph coverage.

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
class MorphArgv {
public:
    MorphArgv(std::initializer_list<const char*> args)
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
// Branch 1: No input file specified -> usage error, return 2
// ---------------------------------------------------------------------------

TEST(CLIPipeline_cmdMorphCoverageTest, NoInputFileReturns2)
{
    MorphArgv args({"morph"});
    EXPECT_EQ(2, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdMorphCoverageTest, NoInputFileWithListFlagOnlyReturns2)
{
    // --list present but still no positional file: the empty-file check is
    // evaluated first, so this must return 2.
    MorphArgv args({"morph", "--list"});
    EXPECT_EQ(2, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdMorphCoverageTest, NoInputFileWithListAndJsonReturns2)
{
    MorphArgv args({"morph", "--list", "--json"});
    EXPECT_EQ(2, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdMorphCoverageTest, OnlyJsonFlagNoFileReturns2)
{
    MorphArgv args({"morph", "--json"});
    EXPECT_EQ(2, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdMorphCoverageTest, CliFlagOnlyNoFileReturns2)
{
    // The "--cli" token is explicitly skipped by the parser; with no file it
    // still hits the empty-file branch.
    MorphArgv args({"--cli", "morph"});
    EXPECT_EQ(2, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdMorphCoverageTest, UnknownDashFlagIsNotTreatedAsFileReturns2)
{
    // Tokens starting with '-' are never captured as the positional file,
    // so an unknown flag leaves filePath empty -> return 2.
    MorphArgv args({"morph", "--bogus"});
    EXPECT_EQ(2, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// Branch 2: File given but --list not passed -> return 2
// ---------------------------------------------------------------------------

TEST(CLIPipeline_cmdMorphCoverageTest, FileWithoutListReturns2)
{
    // A bare positional file without --list: parser captures filePath, then
    // the "!listMode" guard returns 2 (other modes unimplemented).
    MorphArgv args({"morph", "model.fbx"});
    EXPECT_EQ(2, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdMorphCoverageTest, FileWithJsonButNoListReturns2)
{
    // --json does NOT enable list mode; the --list gate still fails -> 2.
    MorphArgv args({"morph", "model.fbx", "--json"});
    EXPECT_EQ(2, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdMorphCoverageTest, ExistingFileWithoutListReturns2)
{
    // Even with a real, existing file, the missing --list flag short-circuits
    // to 2 before the file-existence check is reached.
    QTemporaryFile tmp;
    ASSERT_TRUE(tmp.open());
    tmp.write("not a real mesh");
    tmp.flush();
    const QByteArray path = tmp.fileName().toUtf8();

    MorphArgv args({"morph", path.constData()});
    EXPECT_EQ(2, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdMorphCoverageTest, FirstPositionalWinsWithoutListReturns2)
{
    // Two positional tokens: only the first is captured (filePath.isEmpty()
    // guard); without --list it still returns 2.
    MorphArgv args({"morph", "first.fbx", "second.fbx"});
    EXPECT_EQ(2, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// Branch 3: --list + nonexistent file -> return 1
// ---------------------------------------------------------------------------

TEST(CLIPipeline_cmdMorphCoverageTest, ListWithNonexistentFileReturns1)
{
    MorphArgv args({"morph", "definitely_does_not_exist_12345.fbx", "--list"});
    EXPECT_EQ(1, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdMorphCoverageTest, ListJsonWithNonexistentFileReturns1)
{
    MorphArgv args({"morph", "no_such_file_abcdef.gltf", "--list", "--json"});
    EXPECT_EQ(1, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdMorphCoverageTest, ListWithNonexistentFileInTempDirReturns1)
{
    // Build a path guaranteed to be absent inside a fresh temp dir, so we are
    // certain the file-existence branch (return 1) is the one exercised.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString missing = dir.filePath("ghost_mesh.fbx");
    ASSERT_FALSE(QFileInfo::exists(missing));
    const QByteArray path = missing.toUtf8();

    MorphArgv args({"morph", path.constData(), "--list"});
    EXPECT_EQ(1, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdMorphCoverageTest, ListFlagBeforeFileNonexistentReturns1)
{
    // Argument order should not matter — --list before the path still parses
    // the file and reaches the not-found branch.
    MorphArgv args({"morph", "--list", "missing_reordered.dae"});
    EXPECT_EQ(1, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdMorphCoverageTest, ListNonexistentFileNotSubcommandHeader)
{
    // The parser skips EVERY token equal to "morph" by value (argv[0] handling),
    // so a distinct non-flag token is needed to populate the file path. With a
    // real (nonexistent) filename + --list we reach the not-found branch (1).
    MorphArgv args({"morph", "not_a_real_morph_file.dae", "--list"});
    EXPECT_EQ(1, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdMorphCoverageTest, SecondMorphTokenIsSkippedNotTreatedAsFile)
{
    // Documents the by-value skip: a literal second "morph" is NOT captured as
    // the file path, so filePath stays empty and we hit the no-input branch (2).
    MorphArgv args({"morph", "morph", "--list"});
    EXPECT_EQ(2, CLIPipeline::cmdMorph(args.argc(), args.argv()));
}
