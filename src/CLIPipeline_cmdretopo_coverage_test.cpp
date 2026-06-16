#include <gtest/gtest.h>

#include <QByteArray>
#include <QList>
#include <QString>

#include <vector>

#include "CLIPipeline.h"

// Coverage tests for CLIPipeline::cmdRetopo.
//
// cmdRetopo is a public static command handler whose argument parsing,
// required-arg checks, and four numeric-range validators all execute and
// return BEFORE any Ogre initialization (initOgreHeadless) or file load.
// Those branches are therefore fully testable headlessly with no display
// and no Ogre context.
//
// Parser detail (mirrors CLIPipeline.cpp): the loop starts at i = 1, so
// argv[0] is always treated as the program/command name and ignored. The
// helper below prepends a dummy "qtmesh" token at index 0 to match how the
// real entry point invokes the handler.
//
// Return-code contract (from the header):
//   0 = success, 1 = runtime error, 2 = usage error.
//
// Distinct suite name (CLIPipelineCmdRetopoCoverageTest) avoids any
// ODR / duplicate-registration clash with other CLIPipeline test suites.

namespace {

// Invoke cmdRetopo with the given argument tokens. A dummy program name is
// prepended at argv[0] because the parser skips index 0. Backing storage is
// kept alive for the duration of the call.
int callRetopo(const QList<QByteArray>& tokens)
{
    std::vector<QByteArray> storage;
    storage.reserve(tokens.size() + 1);
    storage.emplace_back("qtmesh"); // argv[0], ignored by the parser loop
    for (const QByteArray& t : tokens)
        storage.push_back(t);

    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (QByteArray& b : storage)
        argv.push_back(b.data());

    return CLIPipeline::cmdRetopo(static_cast<int>(argv.size()), argv.data());
}

// A path that is overwhelmingly unlikely to exist on disk, so that a
// fully-valid argument set still hits the "file not found" -> 1 branch
// before any Ogre work would occur.
QByteArray nonexistentInput()
{
    return QByteArray("/nonexistent/__qtmesh_cmdretopo_cov__/no_such_mesh.fbx");
}

} // namespace

// ---------------------------------------------------------------------------
// Required-argument checks (return 2 before any Ogre work).
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdRetopoCoverageTest, NoInputFileReturnsUsageError)
{
    // Only the subcommand token, no positional input file.
    EXPECT_EQ(2, callRetopo({"retopo"}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, NoInputFileWithFlagsStillUsageError)
{
    // Flags present but no positional input file.
    EXPECT_EQ(2, callRetopo({"retopo", "--target-faces", "100", "-o", "out.fbx"}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, InputButNoOutputReturnsUsageError)
{
    // Input file given but no -o / --output.
    EXPECT_EQ(2, callRetopo({"retopo", nonexistentInput()}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, InputButNoOutputWithOtherFlagsUsageError)
{
    EXPECT_EQ(2, callRetopo({"retopo", nonexistentInput(),
                             "--max-angle", "30", "--json"}));
}

// ---------------------------------------------------------------------------
// --target-faces validation (must be a positive integer).
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdRetopoCoverageTest, TargetFacesZeroRejected)
{
    EXPECT_EQ(2, callRetopo({"retopo", nonexistentInput(),
                             "--target-faces", "0", "-o", "out.fbx"}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, TargetFacesNegativeRejected)
{
    EXPECT_EQ(2, callRetopo({"retopo", nonexistentInput(),
                             "--target-faces", "-5", "-o", "out.fbx"}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, TargetFacesNonIntegerRejected)
{
    // "abc" -> toInt fails (ok == false).
    EXPECT_EQ(2, callRetopo({"retopo", nonexistentInput(),
                             "--target-faces", "abc", "-o", "out.fbx"}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, TargetFacesFloatRejected)
{
    // "12.5" is not a valid integer for QString::toInt -> ok == false.
    EXPECT_EQ(2, callRetopo({"retopo", nonexistentInput(),
                             "--target-faces", "12.5", "-o", "out.fbx"}));
}

// ---------------------------------------------------------------------------
// --max-angle validation (number in [0, 180]).
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdRetopoCoverageTest, MaxAngleBelowRangeRejected)
{
    EXPECT_EQ(2, callRetopo({"retopo", nonexistentInput(),
                             "--max-angle", "-1", "-o", "out.fbx"}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, MaxAngleAboveRangeRejected)
{
    EXPECT_EQ(2, callRetopo({"retopo", nonexistentInput(),
                             "--max-angle", "180.1", "-o", "out.fbx"}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, MaxAngleNonNumericRejected)
{
    EXPECT_EQ(2, callRetopo({"retopo", nonexistentInput(),
                             "--max-angle", "wide", "-o", "out.fbx"}));
}

// ---------------------------------------------------------------------------
// --shape-tol validation (number in [0, 90]).
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdRetopoCoverageTest, ShapeTolBelowRangeRejected)
{
    EXPECT_EQ(2, callRetopo({"retopo", nonexistentInput(),
                             "--shape-tol", "-0.5", "-o", "out.fbx"}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, ShapeTolAboveRangeRejected)
{
    EXPECT_EQ(2, callRetopo({"retopo", nonexistentInput(),
                             "--shape-tol", "90.5", "-o", "out.fbx"}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, ShapeTolNonNumericRejected)
{
    EXPECT_EQ(2, callRetopo({"retopo", nonexistentInput(),
                             "--shape-tol", "tight", "-o", "out.fbx"}));
}

// ---------------------------------------------------------------------------
// --max-aspect validation (number >= 1).
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdRetopoCoverageTest, MaxAspectBelowOneRejected)
{
    EXPECT_EQ(2, callRetopo({"retopo", nonexistentInput(),
                             "--max-aspect", "0.9", "-o", "out.fbx"}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, MaxAspectZeroRejected)
{
    EXPECT_EQ(2, callRetopo({"retopo", nonexistentInput(),
                             "--max-aspect", "0", "-o", "out.fbx"}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, MaxAspectNonNumericRejected)
{
    EXPECT_EQ(2, callRetopo({"retopo", nonexistentInput(),
                             "--max-aspect", "huge", "-o", "out.fbx"}));
}

// ---------------------------------------------------------------------------
// Valid args + nonexistent file -> runtime error (1), checked before Ogre.
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdRetopoCoverageTest, ValidArgsNonexistentFileReturnsRuntimeError)
{
    // All numeric flags within range; -o supplied; positional input given,
    // but the file does not exist. The "file not found" branch returns 1
    // before initOgreHeadless().
    EXPECT_EQ(1, callRetopo({"retopo", nonexistentInput(), "-o", "out.fbx"}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, AllValidNumericFlagsNonexistentFileReturnsRuntimeError)
{
    // Exercise the "accepted" side of every numeric validator (values
    // inside the valid range), then land on the file-not-found -> 1 branch.
    EXPECT_EQ(1, callRetopo({"retopo", nonexistentInput(),
                             "--target-faces", "500",
                             "--max-angle", "45",
                             "--shape-tol", "60",
                             "--max-aspect", "4",
                             "--json",
                             "-o", "out.fbx"}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, BoundaryValuesAcceptedThenFileNotFound)
{
    // Boundary values that are explicitly INSIDE each inclusive range:
    //   max-angle 0 and 180, shape-tol 0 and 90, max-aspect 1.
    // Split across two calls so each boundary is exercised.
    EXPECT_EQ(1, callRetopo({"retopo", nonexistentInput(),
                             "--max-angle", "0",
                             "--shape-tol", "0",
                             "--max-aspect", "1",
                             "-o", "out.fbx"}));
    EXPECT_EQ(1, callRetopo({"retopo", nonexistentInput(),
                             "--max-angle", "180",
                             "--shape-tol", "90",
                             "--max-aspect", "1",
                             "-o", "out.fbx"}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, LongOutputFlagAcceptedThenFileNotFound)
{
    // --output is the long form of -o; exercise that branch too.
    EXPECT_EQ(1, callRetopo({"retopo", nonexistentInput(),
                             "--output", "out.fbx"}));
}

TEST(CLIPipelineCmdRetopoCoverageTest, TargetFacesPositiveAcceptedThenFileNotFound)
{
    // Positive target-faces is accepted (ok && v > 0), then file-not-found.
    EXPECT_EQ(1, callRetopo({"retopo", nonexistentInput(),
                             "--target-faces", "1", "-o", "out.fbx"}));
}
