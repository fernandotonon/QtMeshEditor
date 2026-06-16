// Coverage tests for CLIPipeline::cmdBakeVertexColors — the bake-vertex-colors
// subcommand parser.
//
// These exercise the pure-logic argument-validation branches that all return
// BEFORE initOgreHeadless() is ever called, so they need no Ogre, no display,
// and no QApplication of their own (src/test_main.cpp owns the single
// QCoreApplication). err() writes to a static QTextStream over stderr and is
// safe to invoke headlessly.
//
// Validation branches under test (from CLIPipeline.cpp cmdBakeVertexColors):
//   - missing input OR missing -o (combined gate)          -> 2
//   - --resolution non-int / < 16 / > 8192                 -> 2
//   - --dilation   non-int / < 0  / > 64                   -> 2
//   - valid args + nonexistent input file                  -> 1
//
// Distinct filename + distinct suite name
// (CLIPipeline_cmdBakeVertexColorsCoverageTest) from CLIPipeline_test.cpp and
// the other cmd*_coverage_test.cpp files so there is no ODR clash / duplicate
// registration.

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
class BakeVcArgv {
public:
    BakeVcArgv(std::initializer_list<const char*> args)
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
// Combined missing-arg gate: inputPath.isEmpty() || outputPath.isEmpty() -> 2
// ---------------------------------------------------------------------------

// No positional file and no -o.
TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, NoArgsReturns2)
{
    BakeVcArgv args({"qtmesh", "bake-vertex-colors"});
    EXPECT_EQ(2, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

// Input file present but no -o output specified -> hits combined gate -> 2.
TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, MissingOutputReturns2)
{
    BakeVcArgv args({"qtmesh", "bake-vertex-colors", "model.fbx"});
    EXPECT_EQ(2, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

// -o output present but no positional input file -> combined gate -> 2.
TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, MissingInputReturns2)
{
    BakeVcArgv args({"qtmesh", "bake-vertex-colors", "-o", "out.png"});
    EXPECT_EQ(2, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

// --output (long form) but still no positional input file -> 2.
TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, MissingInputLongOutputReturns2)
{
    BakeVcArgv args({"qtmesh", "bake-vertex-colors", "--output", "out.png"});
    EXPECT_EQ(2, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

// Only --json flag, neither input nor output -> 2.
TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, OnlyJsonFlagReturns2)
{
    BakeVcArgv args({"qtmesh", "bake-vertex-colors", "--json"});
    EXPECT_EQ(2, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

// -o as the trailing token (no value follows): the i+1<argc guard prevents
// consumption, so outputPath stays empty -> combined gate -> 2.
TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, OutputFlagNoValueReturns2)
{
    BakeVcArgv args({"qtmesh", "bake-vertex-colors", "model.fbx", "-o"});
    EXPECT_EQ(2, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --resolution validation: non-int / out of [16..8192] -> 2
// These checks run during the parse loop, BEFORE the missing-arg gate, so they
// return 2 even with full input+output present.
// ---------------------------------------------------------------------------

TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, ResolutionNonIntReturns2)
{
    BakeVcArgv args({"qtmesh", "bake-vertex-colors", "model.fbx",
                     "-o", "out.png", "--resolution", "abc"});
    EXPECT_EQ(2, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, ResolutionTooLowReturns2)
{
    BakeVcArgv args({"qtmesh", "bake-vertex-colors", "model.fbx",
                     "-o", "out.png", "--resolution", "15"});
    EXPECT_EQ(2, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, ResolutionZeroReturns2)
{
    BakeVcArgv args({"qtmesh", "bake-vertex-colors", "model.fbx",
                     "-o", "out.png", "--resolution", "0"});
    EXPECT_EQ(2, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, ResolutionNegativeReturns2)
{
    BakeVcArgv args({"qtmesh", "bake-vertex-colors", "model.fbx",
                     "-o", "out.png", "--resolution", "-256"});
    EXPECT_EQ(2, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, ResolutionTooHighReturns2)
{
    BakeVcArgv args({"qtmesh", "bake-vertex-colors", "model.fbx",
                     "-o", "out.png", "--resolution", "8193"});
    EXPECT_EQ(2, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

// --resolution as the trailing token (no value): the i+1<argc guard means the
// flag is not consumed and resolution keeps its default; with input+output
// present and an existing-but-... no, file is missing -> reaches not-found -> 1.
// (Pairs with the boundary cases; documents the no-value fall-through path.)
TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, ResolutionFlagNoValueFallsThrough)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("res_noval.fbx");
    const QByteArray missingBa = missing.toUtf8();

    BakeVcArgv args({"qtmesh", "bake-vertex-colors", missingBa.constData(),
                     "-o", "out.png", "--resolution"});
    EXPECT_EQ(1, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --dilation validation: non-int / out of [0..64] -> 2
// ---------------------------------------------------------------------------

TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, DilationNonIntReturns2)
{
    BakeVcArgv args({"qtmesh", "bake-vertex-colors", "model.fbx",
                     "-o", "out.png", "--dilation", "wide"});
    EXPECT_EQ(2, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, DilationNegativeReturns2)
{
    BakeVcArgv args({"qtmesh", "bake-vertex-colors", "model.fbx",
                     "-o", "out.png", "--dilation", "-1"});
    EXPECT_EQ(2, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, DilationTooHighReturns2)
{
    BakeVcArgv args({"qtmesh", "bake-vertex-colors", "model.fbx",
                     "-o", "out.png", "--dilation", "65"});
    EXPECT_EQ(2, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

// dilation == 0 is the lower boundary and IS valid -> with a missing file we
// fall through the parse loop to the file-not-found check -> 1.
TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, DilationZeroBoundaryValidMissingFileReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("dil0.fbx");
    const QByteArray missingBa = missing.toUtf8();

    BakeVcArgv args({"qtmesh", "bake-vertex-colors", missingBa.constData(),
                     "-o", "out.png", "--dilation", "0"});
    EXPECT_EQ(1, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

// dilation == 64 is the upper boundary and IS valid -> missing file -> 1.
TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, DilationMaxBoundaryValidMissingFileReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("dil64.fbx");
    const QByteArray missingBa = missing.toUtf8();

    BakeVcArgv args({"qtmesh", "bake-vertex-colors", missingBa.constData(),
                     "-o", "out.png", "--dilation", "64"});
    EXPECT_EQ(1, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --resolution boundaries that ARE valid (16 and 8192): combined with a
// missing file, the parse loop accepts the value and we reach not-found -> 1.
// ---------------------------------------------------------------------------

TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, ResolutionMinBoundaryValidMissingFileReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("res16.fbx");
    const QByteArray missingBa = missing.toUtf8();

    BakeVcArgv args({"qtmesh", "bake-vertex-colors", missingBa.constData(),
                     "-o", "out.png", "--resolution", "16"});
    EXPECT_EQ(1, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, ResolutionMaxBoundaryValidMissingFileReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("res8192.fbx");
    const QByteArray missingBa = missing.toUtf8();

    BakeVcArgv args({"qtmesh", "bake-vertex-colors", missingBa.constData(),
                     "-o", "out.png", "--resolution", "8192"});
    EXPECT_EQ(1, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// Valid args + nonexistent input file -> file-not-found -> 1
// ---------------------------------------------------------------------------

TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, FileNotFoundReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("nonexistent_bakevc_model.fbx");
    ASSERT_FALSE(QFileInfo::exists(missing));
    const QByteArray missingBa = missing.toUtf8();

    BakeVcArgv args({"qtmesh", "bake-vertex-colors", missingBa.constData(),
                     "-o", "out.png"});
    EXPECT_EQ(1, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

// All optional numeric args in-range + --json + missing file -> still 1.
TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, AllValidArgsMissingFileReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("absent_bakevc_model.fbx");
    const QByteArray missingBa = missing.toUtf8();

    BakeVcArgv args({"qtmesh", "bake-vertex-colors", missingBa.constData(),
                     "-o", "out.png", "--resolution", "2048",
                     "--dilation", "8", "--json"});
    EXPECT_EQ(1, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}

// Output path supplied via --output long form + missing file -> 1.
TEST(CLIPipeline_cmdBakeVertexColorsCoverageTest, LongOutputFormMissingFileReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("longout_bakevc.fbx");
    const QByteArray missingBa = missing.toUtf8();

    BakeVcArgv args({"qtmesh", "bake-vertex-colors", missingBa.constData(),
                     "--output", "out.png"});
    EXPECT_EQ(1, CLIPipeline::cmdBakeVertexColors(args.argc(), args.argv()));
}
