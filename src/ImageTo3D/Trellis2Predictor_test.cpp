// Unit tests for the TRELLIS.2 provider's runtime-discovery + failure paths.
// The real generation needs a Linux + NVIDIA CUDA runtime (and the mock path
// needs a Python interpreter), so these tests cover exactly what CI can:
// clean "runtime not installed" behaviour — the project's "no crash when
// unavailable" convention — and option plumbing.
#include "Trellis2Predictor.h"

#include <gtest/gtest.h>

#include <QImage>

namespace {

// Force-resolve to a nonexistent runtime for the duration of a test.
struct NoRuntimeGuard {
    NoRuntimeGuard()
    {
        qputenv("QTMESH_TRELLIS2_ENV", "/nonexistent/qtmesh-trellis2-ut");
        qputenv("QTMESH_TRELLIS2_PYTHON", "/nonexistent/python-ut");
        // Also neutralize the trellis.cpp flavor (#966): an env override that
        // points nowhere beats any PATH-installed trellis-cli.
        qputenv("QTMESH_TRELLIS2_CLI", "/nonexistent/trellis-cli-ut");
    }
    ~NoRuntimeGuard()
    {
        qunsetenv("QTMESH_TRELLIS2_ENV");
        qunsetenv("QTMESH_TRELLIS2_PYTHON");
        qunsetenv("QTMESH_TRELLIS2_CLI");
    }
};

} // namespace

TEST(Trellis2PredictorTest, AlwaysCompiledIn)
{
    // Unlike the ONNX backends, availability is not a build-flag question —
    // the runtime probe is the gate.
    EXPECT_TRUE(Trellis2Predictor::isAvailable());
}

TEST(Trellis2PredictorTest, MissingRuntimeReportsCleanly)
{
    NoRuntimeGuard guard;
    EXPECT_FALSE(Trellis2Predictor::runtimeAvailable());
    EXPECT_TRUE(Trellis2Predictor::pythonPath().isEmpty());
    EXPECT_TRUE(Trellis2Predictor::generateScriptPath().isEmpty());
    const QString desc = Trellis2Predictor::runtimeDescription();
    EXPECT_TRUE(desc.contains(QStringLiteral("install.py")));
}

TEST(Trellis2PredictorTest, PredictWithoutRuntimeFailsWithInstallHint)
{
    NoRuntimeGuard guard;
    QImage img(8, 8, QImage::Format_RGB888);
    img.fill(Qt::red);
    const auto r = Trellis2Predictor::predict(img, {});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("runtime")));
}

TEST(Trellis2PredictorTest, NullImageRejected)
{
    const auto r = Trellis2Predictor::predict(QImage(), {});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("null")));
}

TEST(Trellis2PredictorTest, DispatchThroughMeshGenPredictor)
{
    // Backend::Trellis2 must route through the shared dispatch (both ONNX and
    // non-ONNX builds) and fail with the runtime hint, never the generic
    // "needs ONNX" error.
    NoRuntimeGuard guard;
    QImage img(8, 8, QImage::Format_RGB888);
    img.fill(Qt::blue);
    MeshGenPredictor::Options opts;
    opts.backend = MeshGenPredictor::Backend::Trellis2;
    const auto r = MeshGenPredictor::predict(img, QString(), QString(), opts);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("TRELLIS.2")));

    // And the default-backend resolver falls back to TripoSR without it.
    EXPECT_EQ(MeshGenPredictor::defaultBackend(),
              MeshGenPredictor::Backend::TripoSR);
}
