// Coverage tests for CLIPipeline::cmdSkin.
//
// cmdSkin validates its required args and four numeric ranges
// (--max-influences [1,8], --falloff [0.5,16], --max-distance [0,10])
// BEFORE any Ogre initialisation, returning 2 on a bad argument and 1
// when the (otherwise valid) input file does not exist. Every assertion
// here exercises a path that returns before initOgreHeadless() is ever
// reached, so the suite is pure-logic and needs no Ogre / display.
//
// Distinct filename + distinct suite names (CLIPipelineCmdSkinCoverage*)
// keep this independent of the existing CLIPipeline_test.cpp.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QList>

#include <initializer_list>
#include <vector>

#include "CLIPipeline.h"

namespace {

/// RAII helper to build argc/argv from a list of string literals.
/// Local to this translation unit (own anonymous-namespace name) so there
/// is no ODR clash with the TestArgv in CLIPipeline_test.cpp.
class SkinArgv {
public:
    SkinArgv(std::initializer_list<const char*> args)
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

/// A path that is essentially guaranteed not to exist on disk, used to
/// hit the `!fi.exists()` (return 1) branch with otherwise-valid args.
const char* kMissingFile = "/nonexistent_qtmesh_skin_input_zzz.fbx";

} // anonymous namespace

// ---------------------------------------------------------------------------
// Required-argument checks (return 2)
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdSkinCoverageError, NoInputFile)
{
    // Bare subcommand: no positional input file at all.
    SkinArgv args({"skin"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdSkinCoverageError, NoInputFileButFlagsPresent)
{
    // Only flags, still no positional input — must still be the no-input error.
    SkinArgv args({"skin", "--json", "--skip-unweighted"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdSkinCoverageError, InputGivenButNoOutput)
{
    // Input present, but -o/--output missing -> required-output error (2).
    // This is checked before any file-existence probe.
    SkinArgv args({"skin", kMissingFile});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdSkinCoverageError, InputAndFlagsButNoOutput)
{
    SkinArgv args({"skin", kMissingFile, "--max-influences", "4", "--falloff", "4"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 2);
}

// ---------------------------------------------------------------------------
// --max-influences range / parse validation (return 2)
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdSkinCoverageMaxInfluences, AboveMaxIsError)
{
    SkinArgv args({"skin", kMissingFile, "-o", "out.fbx", "--max-influences", "9"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdSkinCoverageMaxInfluences, NonIntegerIsError)
{
    SkinArgv args({"skin", kMissingFile, "-o", "out.fbx", "--max-influences", "abc"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdSkinCoverageMaxInfluences, MinBoundaryIsAcceptedThenFileMissing)
{
    // value 1 is in-range -> passes parse, then nonexistent file -> 1.
    SkinArgv args({"skin", kMissingFile, "-o", "out.fbx", "--max-influences", "1"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdSkinCoverageMaxInfluences, MaxBoundaryIsAcceptedThenFileMissing)
{
    // value 8 is in-range -> passes parse, then nonexistent file -> 1.
    SkinArgv args({"skin", kMissingFile, "-o", "out.fbx", "--max-influences", "8"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 1);
}

// ---------------------------------------------------------------------------
// --falloff range / parse validation (return 2)
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdSkinCoverageFalloff, BelowMinIsError)
{
    SkinArgv args({"skin", kMissingFile, "-o", "out.fbx", "--falloff", "0.4"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdSkinCoverageFalloff, AboveMaxIsError)
{
    SkinArgv args({"skin", kMissingFile, "-o", "out.fbx", "--falloff", "16.1"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdSkinCoverageFalloff, NonNumericIsError)
{
    SkinArgv args({"skin", kMissingFile, "-o", "out.fbx", "--falloff", "notanumber"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdSkinCoverageFalloff, MinBoundaryIsAcceptedThenFileMissing)
{
    SkinArgv args({"skin", kMissingFile, "-o", "out.fbx", "--falloff", "0.5"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdSkinCoverageFalloff, MaxBoundaryIsAcceptedThenFileMissing)
{
    SkinArgv args({"skin", kMissingFile, "-o", "out.fbx", "--falloff", "16"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 1);
}

// ---------------------------------------------------------------------------
// --max-distance range / parse validation (return 2)
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdSkinCoverageMaxDistance, BelowMinIsError)
{
    SkinArgv args({"skin", kMissingFile, "-o", "out.fbx", "--max-distance", "-0.1"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdSkinCoverageMaxDistance, AboveMaxIsError)
{
    SkinArgv args({"skin", kMissingFile, "-o", "out.fbx", "--max-distance", "10.5"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdSkinCoverageMaxDistance, NonNumericIsError)
{
    SkinArgv args({"skin", kMissingFile, "-o", "out.fbx", "--max-distance", "xyz"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdSkinCoverageMaxDistance, MinBoundaryIsAcceptedThenFileMissing)
{
    SkinArgv args({"skin", kMissingFile, "-o", "out.fbx", "--max-distance", "0"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdSkinCoverageMaxDistance, MaxBoundaryIsAcceptedThenFileMissing)
{
    SkinArgv args({"skin", kMissingFile, "-o", "out.fbx", "--max-distance", "10"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 1);
}

// ---------------------------------------------------------------------------
// Valid args, nonexistent input file (return 1)
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdSkinCoverageMissingFile, DefaultsWithOutput)
{
    // All defaults in-range, only required input+output supplied, but the
    // input does not exist -> file-not-found error (1), before Ogre init.
    SkinArgv args({"skin", kMissingFile, "-o", "out.fbx"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdSkinCoverageMissingFile, LongOutputFlag)
{
    // Same path, exercising the --output spelling of -o.
    SkinArgv args({"skin", kMissingFile, "--output", "out.fbx"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdSkinCoverageMissingFile, AllValidFlagsCombined)
{
    // Every optional flag set to a valid value; still missing file -> 1.
    SkinArgv args({"skin", kMissingFile,
                   "-o", "out.fbx",
                   "--max-influences", "4",
                   "--falloff", "4.0",
                   "--max-distance", "0.5",
                   "--skip-unweighted",
                   "--merge",
                   "--json"});
    EXPECT_EQ(CLIPipeline::cmdSkin(args.argc(), args.argv()), 1);
}
