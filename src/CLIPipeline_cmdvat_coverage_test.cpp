// Coverage tests for CLIPipeline::cmdVat — the OpenVAT bake subcommand parser.
//
// These exercise the pure-logic argument-validation branches that all return
// BEFORE initOgreHeadless() is ever called, so they need no Ogre, no display,
// and no QApplication of their own (src/test_main.cpp owns the single
// QCoreApplication). err() writes to a static QTextStream over stderr and is
// safe to invoke headlessly.
//
// Distinct filename + distinct suite name (CLIPipeline_cmdVatCoverageTest) from
// the existing CLIPipeline_test.cpp so there is no ODR clash / duplicate
// registration with any prior cmdVat coverage.

#include <gtest/gtest.h>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QTemporaryDir>
#include <initializer_list>

#include "CLIPipeline.h"

namespace {

/// RAII helper to build argc/argv from a list of C-strings, mirroring the
/// TestArgv used in CLIPipeline_test.cpp (kept in an anonymous namespace here
/// so it does not collide with that translation unit's copy).
class VatArgv {
public:
    VatArgv(std::initializer_list<const char*> args)
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
// No input file -> usage error, return 2
// ---------------------------------------------------------------------------
TEST(CLIPipeline_cmdVatCoverageTest, NoInputFileReturns2)
{
    VatArgv args({"qtmesh", "vat"});
    EXPECT_EQ(2, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

// A flag-only invocation (no positional file) is also "no input file".
TEST(CLIPipeline_cmdVatCoverageTest, OnlyFlagsNoFileReturns2)
{
    VatArgv args({"qtmesh", "vat", "--json"});
    EXPECT_EQ(2, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// File given but no --anim -> return 2
// ---------------------------------------------------------------------------
TEST(CLIPipeline_cmdVatCoverageTest, MissingAnimReturns2)
{
    VatArgv args({"qtmesh", "vat", "model.fbx"});
    EXPECT_EQ(2, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

// --anim provided but with an empty value is treated as still-missing.
TEST(CLIPipeline_cmdVatCoverageTest, EmptyAnimValueReturns2)
{
    VatArgv args({"qtmesh", "vat", "model.fbx", "--anim", ""});
    EXPECT_EQ(2, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

// --anim as the trailing token (no value follows) -> falls through; the flag
// is consumed only via the i+1<argc guard, so animName stays empty -> 2.
TEST(CLIPipeline_cmdVatCoverageTest, AnimWithNoValueReturns2)
{
    VatArgv args({"qtmesh", "vat", "model.fbx", "--anim"});
    EXPECT_EQ(2, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// Invalid --fps -> return 2
// ---------------------------------------------------------------------------
TEST(CLIPipeline_cmdVatCoverageTest, NonNumericFpsReturns2)
{
    VatArgv args({"qtmesh", "vat", "model.fbx", "--anim", "Walk", "--fps", "abc"});
    EXPECT_EQ(2, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdVatCoverageTest, ZeroFpsReturns2)
{
    VatArgv args({"qtmesh", "vat", "model.fbx", "--anim", "Walk", "--fps", "0"});
    EXPECT_EQ(2, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdVatCoverageTest, NegativeFpsReturns2)
{
    VatArgv args({"qtmesh", "vat", "model.fbx", "--anim", "Walk", "--fps", "-30"});
    EXPECT_EQ(2, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// Invalid --bake-precision (not 16/32) -> return 2
// ---------------------------------------------------------------------------
TEST(CLIPipeline_cmdVatCoverageTest, BakePrecision8Returns2)
{
    VatArgv args({"qtmesh", "vat", "model.fbx", "--anim", "Walk",
                  "--bake-precision", "8"});
    EXPECT_EQ(2, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

TEST(CLIPipeline_cmdVatCoverageTest, BakePrecisionNonNumericReturns2)
{
    VatArgv args({"qtmesh", "vat", "model.fbx", "--anim", "Walk",
                  "--bake-precision", "high"});
    EXPECT_EQ(2, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --include-shaders with no value -> return 2
// ---------------------------------------------------------------------------
TEST(CLIPipeline_cmdVatCoverageTest, IncludeShadersNoValueReturns2)
{
    VatArgv args({"qtmesh", "vat", "model.fbx", "--anim", "Walk",
                  "--include-shaders"});
    EXPECT_EQ(2, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --emit-uv2 0 -> would overwrite the diffuse UV -> return 2
// ---------------------------------------------------------------------------
TEST(CLIPipeline_cmdVatCoverageTest, EmitUv2ChannelZeroReturns2)
{
    VatArgv args({"qtmesh", "vat", "model.fbx", "--anim", "Walk",
                  "--emit-uv2", "0"});
    EXPECT_EQ(2, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --emit-uv2 with out-of-range channel (>7) -> return 2
// ---------------------------------------------------------------------------
TEST(CLIPipeline_cmdVatCoverageTest, EmitUv2ChannelTooHighReturns2)
{
    VatArgv args({"qtmesh", "vat", "model.fbx", "--anim", "Walk",
                  "--emit-uv2", "9"});
    EXPECT_EQ(2, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

// A negative channel is numeric-looking (leading '-') and out of range -> 2.
TEST(CLIPipeline_cmdVatCoverageTest, EmitUv2NegativeChannelReturns2)
{
    VatArgv args({"qtmesh", "vat", "model.fbx", "--anim", "Walk",
                  "--emit-uv2", "-1"});
    EXPECT_EQ(2, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --emit-uv2 with a non-integer token: the token does NOT look numeric, so it
// is left for the positional-file branch. Here we supply only the flag with a
// non-numeric value and no real positional, so the value becomes the file path.
// To assert the "out-of-range channel" numeric-peek path specifically, the
// dedicated cases above cover it; this case verifies the bare/defaulted form
// still requires --anim and an existing file. We feed a non-numeric token that
// is consumed as the positional file -> file-not-found path -> return 1.
// ---------------------------------------------------------------------------
TEST(CLIPipeline_cmdVatCoverageTest, EmitUv2NonNumericTokenTreatedAsPositional)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("does_not_exist_emituv2.fbx");
    const QByteArray missingBa = missing.toUtf8();

    // `--emit-uv2 <file>`: the file token is non-numeric, so it is NOT consumed
    // as a channel; --emit-uv2 defaults to channel 1, and the token becomes the
    // positional file path. With --anim present and the file absent -> 1.
    VatArgv args({"qtmesh", "vat", "--anim", "Walk", "--emit-uv2",
                  missingBa.constData()});
    EXPECT_EQ(1, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// Valid args, nonexistent path -> file-not-found -> return 1
// ---------------------------------------------------------------------------
TEST(CLIPipeline_cmdVatCoverageTest, FileNotFoundReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("nonexistent_vat_model.fbx");
    ASSERT_FALSE(QFileInfo::exists(missing));
    const QByteArray missingBa = missing.toUtf8();

    VatArgv args({"qtmesh", "vat", missingBa.constData(), "--anim", "Walk"});
    EXPECT_EQ(1, CLIPipeline::cmdVat(args.argc(), args.argv()));
}

// Valid args with --fps/--bake-precision in their accepted ranges but a missing
// file -> the numeric branches pass and we still reach file-not-found -> 1.
TEST(CLIPipeline_cmdVatCoverageTest, ValidNumericArgsMissingFileReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("absent_vat_model.fbx");
    const QByteArray missingBa = missing.toUtf8();

    VatArgv args({"qtmesh", "vat", missingBa.constData(), "--anim", "Walk",
                  "--fps", "24", "--bake-precision", "32", "--emit-uv2", "2"});
    EXPECT_EQ(1, CLIPipeline::cmdVat(args.argc(), args.argv()));
}
