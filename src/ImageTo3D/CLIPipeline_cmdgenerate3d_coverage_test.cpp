// Coverage tests for CLIPipeline::cmdGenerate3d (#764, image-to-3D). Mirrors the
// cmdRig/cmdSegment coverage-test pattern: exercise the argument-validation and
// graceful-degradation paths that don't need the (not-yet-hosted, #769) model or a
// GL context. No GTEST_SKIP — CI's zero-skip policy rejects skips; every assertion
// runs on every runner.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QList>
#include <QString>
#include <QTemporaryDir>
#include <initializer_list>

#include "CLIPipeline.h"

namespace {

// RAII argc/argv builder (own anon-namespace name to avoid ODR clashes).
class Gen3dArgv {
public:
    Gen3dArgv(std::initializer_list<const char*> args)
    {
        for (auto* a : args) m_storage.push_back(QByteArray(a));
        for (auto& ba : m_storage) m_argv.push_back(ba.data());
        m_argc = static_cast<int>(m_argv.size());
    }
    int argc() const { return m_argc; }
    char** argv() { return m_argv.data(); }
private:
    QList<QByteArray> m_storage;
    QList<char*> m_argv;
    int m_argc = 0;
};

const char* kMissingImage = "/nonexistent_qtmesh_gen3d_input_zzz.png";

} // namespace

// ── Required-argument / usage errors (return 2) ─────────────────────────────

TEST(CLIPipelineCmdGenerate3dCoverage, NoInputIsUsageError)
{
    Gen3dArgv args({"generate3d"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdGenerate3dCoverage, ResolutionRequiresValue)
{
    Gen3dArgv args({"generate3d", kMissingImage, "--resolution"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdGenerate3dCoverage, ResolutionOutOfRange)
{
    Gen3dArgv lo({"generate3d", kMissingImage, "--resolution", "8"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(lo.argc(), lo.argv()), 2);
    Gen3dArgv hi({"generate3d", kMissingImage, "--resolution", "9999"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(hi.argc(), hi.argv()), 2);
    Gen3dArgv nan({"generate3d", kMissingImage, "--resolution", "abc"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(nan.argc(), nan.argv()), 2);
}

TEST(CLIPipelineCmdGenerate3dCoverage, OutputRequiresValue)
{
    Gen3dArgv args({"generate3d", kMissingImage, "-o"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdGenerate3dCoverage, BadQualityIsUsageError)
{
    Gen3dArgv args({"generate3d", kMissingImage, "--quality", "ultra"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdGenerate3dCoverage, QualityRequiresValue)
{
    Gen3dArgv args({"generate3d", kMissingImage, "--quality"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 2);
}

// ── Runtime failures (return 1), no crash ───────────────────────────────────

TEST(CLIPipelineCmdGenerate3dCoverage, MissingImageIsError)
{
    Gen3dArgv args({"generate3d", kMissingImage});
    // Missing input → 1 regardless of ONNX/model state.
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdGenerate3dCoverage, NoModelFlagRejected)
{
    // --no-model is not a supported fallback (TripoSR is generative) → usage error.
    Gen3dArgv args({"generate3d", kMissingImage, "--no-model"});
    const int rc = CLIPipeline::cmdGenerate3d(args.argc(), args.argv());
    EXPECT_TRUE(rc == 1 || rc == 2);   // 2 (rejected) w/ ONNX, 1 (no-onnx) otherwise
}

TEST(CLIPipelineCmdGenerate3dCoverage, ValidImageWithoutModelOrOnnxFailsCleanly)
{
    // A real (tiny) image but no model / no ONNX build → clean exit 1, not a crash.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString img = QDir(dir.path()).filePath("in.png");
    QImage(16, 16, QImage::Format_RGB888).save(img);
    ASSERT_TRUE(QFileInfo::exists(img));

    const QString out = QDir(dir.path()).filePath("out.glb");
    Gen3dArgv args({"generate3d", img.toUtf8().constData(),
                    "-o", out.toUtf8().constData(),
                    "--no-model"});
    // Without the hosted model (CI) / without ONNX, this must fail cleanly.
    const int rc = CLIPipeline::cmdGenerate3d(args.argc(), args.argv());
    EXPECT_NE(rc, 0);
}

// ── TRELLIS.2 backend flags (this integration) ───────────────────────────────

TEST(CLIPipelineCmdGenerate3dCoverage, Trellis2BadPresetIsUsageError)
{
    Gen3dArgv args({"generate3d", kMissingImage, "--preset", "ultra"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 2);
    Gen3dArgv missing({"generate3d", kMissingImage, "--preset"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(missing.argc(), missing.argv()), 2);
}

TEST(CLIPipelineCmdGenerate3dCoverage, Trellis2BadTargetTrisIsUsageError)
{
    Gen3dArgv neg({"generate3d", kMissingImage, "--target-tris", "-5"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(neg.argc(), neg.argv()), 2);
    Gen3dArgv nan({"generate3d", kMissingImage, "--target-tris", "many"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(nan.argc(), nan.argv()), 2);
    Gen3dArgv missing({"generate3d", kMissingImage, "--seed"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(missing.argc(), missing.argv()), 2);
}

TEST(CLIPipelineCmdGenerate3dCoverage, Trellis2BackendAcceptedButUnknownRejected)
{
    // Unknown backend name → usage error.
    Gen3dArgv bad({"generate3d", kMissingImage, "--backend", "dreamfusion"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(bad.argc(), bad.argv()), 2);

    // trellis2 is a valid backend; with a real image but a deliberately
    // nonexistent runtime the command must fail at RUNTIME (1) with the
    // install hint — never crash, never a usage error.
    // All ASSERTs run BEFORE the env override: an ASSERT returns from the
    // test body immediately, and env vars set before a failed ASSERT would
    // leak into every later test in this process.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString png = QDir(tmp.path()).filePath("in.png");
    QImage img(16, 16, QImage::Format_RGB888);
    img.fill(Qt::red);
    ASSERT_TRUE(img.save(png, "PNG"));
    qputenv("QTMESH_TRELLIS2_ENV", "/nonexistent/qtmesh-trellis2-cli-ut");
    qputenv("QTMESH_TRELLIS2_PYTHON", "/nonexistent/python-cli-ut");
    qputenv("QTMESH_TRELLIS2_CLI", "/nonexistent/trellis-cli-cli-ut");
    const QByteArray pngBytes = png.toLocal8Bit();
    Gen3dArgv ok({"generate3d", pngBytes.constData(), "--backend", "trellis2"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(ok.argc(), ok.argv()), 1);
    qunsetenv("QTMESH_TRELLIS2_ENV");
    qunsetenv("QTMESH_TRELLIS2_PYTHON");
    qunsetenv("QTMESH_TRELLIS2_CLI");
}

// ---- TRELLIS.2 texture options ---------------------------------------------
//
// These three flags exist so a simple prop (a pizza box, a floor mat) is not
// forced to a hero asset's budget. The backend always accepted the values;
// only the pickers were capped. Argument validation is what these pin.

TEST(CLIPipelineCmdGenerate3dCoverage, TextureSupersampleRequiresValue)
{
    Gen3dArgv args({"generate3d", kMissingImage, "--texture-supersample"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdGenerate3dCoverage, TextureSupersampleRejectsAnythingButOneOrTwo)
{
    // The bake clamps to [1,2] anyway, but a silently-clamped 4 would read as
    // "the flag did nothing" — reject it at the boundary instead.
    for (const char* bad : {"0", "3", "4", "abc", "-1"}) {
        Gen3dArgv args({"generate3d", kMissingImage, "--texture-supersample", bad});
        EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 2)
            << "should reject --texture-supersample " << bad;
    }
}

TEST(CLIPipelineCmdGenerate3dCoverage, TextureSupersampleAcceptsTheAlias)
{
    // `--supersample` is accepted as a shorthand. It reaches the same
    // validation, so a bad value must fail identically — otherwise the alias
    // would be a way around the check.
    Gen3dArgv bad({"generate3d", kMissingImage, "--supersample", "7"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(bad.argc(), bad.argv()), 2);
    // A GOOD value gets past argument parsing and fails later on the missing
    // image (exit 1), which is how these tests distinguish "rejected the
    // flag" (2) from "accepted the flag" (1).
    Gen3dArgv ok({"generate3d", kMissingImage, "--supersample", "2"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(ok.argc(), ok.argv()), 1);
}

TEST(CLIPipelineCmdGenerate3dCoverage, TexResRequiresValueAndRejectsOffMenuSizes)
{
    Gen3dArgv missing({"generate3d", kMissingImage, "--tex-res"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(missing.argc(), missing.argv()), 2);
    // Only the two sizes trellis-cli actually offers; 2048 is NOT one of them,
    // so it must be refused rather than passed through and ignored.
    for (const char* bad : {"256", "2048", "999", "xyz"}) {
        Gen3dArgv args({"generate3d", kMissingImage, "--tex-res", bad});
        EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 2)
            << "should reject --tex-res " << bad;
    }
    for (const char* good : {"512", "1024"}) {
        Gen3dArgv args({"generate3d", kMissingImage, "--tex-res", good});
        EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 1)
            << "should accept --tex-res " << good;
    }
}

TEST(CLIPipelineCmdGenerate3dCoverage, NoSourceTakesNoValue)
{
    // A bare switch: it must not swallow the next argument, or
    // `--no-source -o out.glb` would silently lose the output path.
    Gen3dArgv args({"generate3d", kMissingImage, "--no-source"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 1);
}

// ---- Pixal3D backend --------------------------------------------------------
//
// Pixal3D is a TRELLIS.2 fork with view-aligned projection conditioning,
// markedly better on humanoids. It rides the SAME trellis-cli runtime, so the
// CLI only has to name it and carry its two camera knobs.

TEST(CLIPipelineCmdGenerate3dCoverage, Pixal3DBackendIsAccepted)
{
    // exit 1 (not 2) = the argument parsed and the run failed later on the
    // missing image, which is how this file distinguishes accept from reject.
    for (const char* name : {"pixal3d", "pixal", "PIXAL3D"}) {
        Gen3dArgv args({"generate3d", kMissingImage, "--backend", name});
        EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 1)
            << "should accept --backend " << name;
    }
}

TEST(CLIPipelineCmdGenerate3dCoverage, UnknownBackendStillRejected)
{
    // Adding a backend must not turn the enum into a catch-all.
    Gen3dArgv args({"generate3d", kMissingImage, "--backend", "pixal4d"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdGenerate3dCoverage, PixalFovRejectsOutOfRangeAngles)
{
    // 0 is reserved for "leave trellis-cli on its own 49.13 default", so an
    // explicit 0 is a caller error rather than a silent no-op — otherwise
    // `--pixal-fov 0` would look like it set something and do nothing.
    for (const char* bad : {"0", "-10", "180", "999", "abc"}) {
        Gen3dArgv args({"generate3d", kMissingImage, "--pixal-fov", bad});
        EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 2)
            << "should reject --pixal-fov " << bad;
    }
    Gen3dArgv missing({"generate3d", kMissingImage, "--pixal-fov"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(missing.argc(), missing.argv()), 2);
    Gen3dArgv ok({"generate3d", kMissingImage, "--pixal-fov", "49.13"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(ok.argc(), ok.argv()), 1);
}

TEST(CLIPipelineCmdGenerate3dCoverage, NoNafTakesNoValue)
{
    // Bare switch: must not swallow the next argument.
    Gen3dArgv args({"generate3d", kMissingImage, "--no-naf"});
    EXPECT_EQ(CLIPipeline::cmdGenerate3d(args.argc(), args.argv()), 1);
}
