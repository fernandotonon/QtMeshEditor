#include <gtest/gtest.h>

#include <QByteArray>
#include <QList>
#include <QString>

#include <vector>

#include "CLIPipeline.h"

// Coverage tests for CLIPipeline::cmdGenerate3d (image-to-3D, epic #764) —
// specifically the argument parsing added for the TripoSG backend
// (--backend / --flow-steps / --guidance) plus the existing --quality/
// --resolution validators.
//
// The whole argument-parse loop (every usage-error `return 2`) executes and
// returns BEFORE the input-file existence check, the ONNX-availability guard,
// and any Ogre init — so these branches are fully testable headlessly, and
// the results are independent of whether the binary was built with
// ENABLE_ONNX. A fully-valid argument set with a nonexistent input lands on a
// runtime-error `return 1` (either "image not found" or the "rebuild with
// -DENABLE_ONNX" message — both 1) before any Ogre work.
//
// Return-code contract: 0 = success, 1 = runtime error, 2 = usage error.
//
// Distinct suite name avoids ODR clashes with other CLIPipeline suites.

namespace {

int callGenerate3d(const QList<QByteArray>& tokens)
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

    return CLIPipeline::cmdGenerate3d(static_cast<int>(argv.size()), argv.data());
}

QByteArray nonexistentInput()
{
    return QByteArray("/nonexistent/__qtmesh_cmdgen3d_cov__/no_such_image.png");
}

} // namespace

// ---------------------------------------------------------------------------
// Required-argument checks.
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdGenerate3dCoverageTest, NoInputImageReturnsUsageError)
{
    EXPECT_EQ(2, callGenerate3d({"generate3d"}));
}

// ---------------------------------------------------------------------------
// --backend validation (triposr | triposg).
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdGenerate3dCoverageTest, BackendInvalidValueRejected)
{
    EXPECT_EQ(2, callGenerate3d({"generate3d", nonexistentInput(),
                                 "--backend", "foo", "-o", "out.glb"}));
}

TEST(CLIPipelineCmdGenerate3dCoverageTest, BackendMissingValueRejected)
{
    // --backend as the last token: no value follows.
    EXPECT_EQ(2, callGenerate3d({"generate3d", nonexistentInput(), "--backend"}));
}

TEST(CLIPipelineCmdGenerate3dCoverageTest, BackendTriposrAcceptedThenFileNotFound)
{
    EXPECT_EQ(1, callGenerate3d({"generate3d", nonexistentInput(),
                                 "--backend", "triposr", "-o", "out.glb"}));
}

TEST(CLIPipelineCmdGenerate3dCoverageTest, BackendTriposgAcceptedThenFileNotFound)
{
    EXPECT_EQ(1, callGenerate3d({"generate3d", nonexistentInput(),
                                 "--backend", "triposg", "-o", "out.glb"}));
}

// ---------------------------------------------------------------------------
// --flow-steps validation (integer in [1..200]).
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdGenerate3dCoverageTest, FlowStepsZeroRejected)
{
    EXPECT_EQ(2, callGenerate3d({"generate3d", nonexistentInput(),
                                 "--flow-steps", "0", "-o", "out.glb"}));
}

TEST(CLIPipelineCmdGenerate3dCoverageTest, FlowStepsAboveRangeRejected)
{
    EXPECT_EQ(2, callGenerate3d({"generate3d", nonexistentInput(),
                                 "--flow-steps", "500", "-o", "out.glb"}));
}

TEST(CLIPipelineCmdGenerate3dCoverageTest, FlowStepsNonIntegerRejected)
{
    EXPECT_EQ(2, callGenerate3d({"generate3d", nonexistentInput(),
                                 "--flow-steps", "many", "-o", "out.glb"}));
}

TEST(CLIPipelineCmdGenerate3dCoverageTest, FlowStepsBoundaryValuesAcceptedThenFileNotFound)
{
    EXPECT_EQ(1, callGenerate3d({"generate3d", nonexistentInput(),
                                 "--backend", "triposg", "--flow-steps", "1",
                                 "-o", "out.glb"}));
    EXPECT_EQ(1, callGenerate3d({"generate3d", nonexistentInput(),
                                 "--backend", "triposg", "--flow-steps", "200",
                                 "-o", "out.glb"}));
}

// ---------------------------------------------------------------------------
// --guidance validation (number in [0..30]).
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdGenerate3dCoverageTest, GuidanceNegativeRejected)
{
    EXPECT_EQ(2, callGenerate3d({"generate3d", nonexistentInput(),
                                 "--guidance", "-1", "-o", "out.glb"}));
}

TEST(CLIPipelineCmdGenerate3dCoverageTest, GuidanceAboveRangeRejected)
{
    EXPECT_EQ(2, callGenerate3d({"generate3d", nonexistentInput(),
                                 "--guidance", "31", "-o", "out.glb"}));
}

TEST(CLIPipelineCmdGenerate3dCoverageTest, GuidanceNonNumericRejected)
{
    EXPECT_EQ(2, callGenerate3d({"generate3d", nonexistentInput(),
                                 "--guidance", "strong", "-o", "out.glb"}));
}

TEST(CLIPipelineCmdGenerate3dCoverageTest, GuidanceZeroAcceptedThenFileNotFound)
{
    // 0 is valid (disables CFG) and must be accepted before the file check.
    EXPECT_EQ(1, callGenerate3d({"generate3d", nonexistentInput(),
                                 "--backend", "triposg", "--guidance", "0",
                                 "-o", "out.glb"}));
}

// ---------------------------------------------------------------------------
// --quality validation (fp32 | int8) — existing arg, still parses.
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdGenerate3dCoverageTest, QualityInvalidValueRejected)
{
    EXPECT_EQ(2, callGenerate3d({"generate3d", nonexistentInput(),
                                 "--quality", "fp16", "-o", "out.glb"}));
}

// ---------------------------------------------------------------------------
// Combined valid args + nonexistent file -> runtime error (1).
// ---------------------------------------------------------------------------

TEST(CLIPipelineCmdGenerate3dCoverageTest, AllNewFlagsValidNonexistentFileReturnsRuntimeError)
{
    EXPECT_EQ(1, callGenerate3d({"generate3d", nonexistentInput(),
                                 "--backend", "triposg",
                                 "--flow-steps", "25",
                                 "--guidance", "7",
                                 "--resolution", "128",
                                 "-o", "out.glb"}));
}
