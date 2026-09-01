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
