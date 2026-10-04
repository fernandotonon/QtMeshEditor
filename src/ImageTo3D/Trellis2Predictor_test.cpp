// Unit tests for the TRELLIS.2 provider's runtime-discovery + failure paths.
// The real generation needs a Linux + NVIDIA CUDA runtime (and the mock path
// needs a Python interpreter), so these tests cover exactly what CI can:
// clean "runtime not installed" behaviour — the project's "no crash when
// unavailable" convention — and option plumbing.
#include "Trellis2Predictor.h"

#include <gtest/gtest.h>

#include <QImage>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <thread>

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

// ---- one generation at a time ---------------------------------------------
// A run holds several GB for minutes; two at once drove a 24 GB Mac into
// sustained swap with both trellis-cli processes blocked in Metal, right
// before watchdogd restarted the graphical session. The second request must be
// REFUSED (not queued — a silent multi-minute wait reads as a hang).
//
// Driving this needs a runtime that resolves, so the guard is actually
// reached: a stub trellis-cli plus the .gguf files runtimeKind() probes for.
// The stub blocks until released, so the first call is guaranteed to still
// hold the slot while the second one tries.
namespace {

struct FakeTrellisCli {
    QTemporaryDir dir;
    QString releaseFile;

    FakeTrellisCli()
    {
        const QDir d(dir.path());
        releaseFile = d.filePath(QStringLiteral("release"));
        // trellisCliModelsDir() looks for a `models/` dir NEXT TO the binary,
        // and trellisCliAvailable() requires this exact 512-pipeline set.
        d.mkpath(QStringLiteral("models"));
        const QDir m(d.filePath(QStringLiteral("models")));
        for (const char* n : {"ss_flow.gguf",
                              "shape_flow_512.gguf", "tex_flow_512.gguf",
                              "shape_dec.gguf", "tex_dec.gguf",
                              "ss_dec.gguf", "dinov3.gguf"}) {
            QFile f(m.filePath(QString::fromLatin1(n)));
            f.open(QIODevice::WriteOnly);
            f.write("stub");
            f.close();
        }
        const QString cli = d.filePath(QStringLiteral("trellis-cli"));
        QFile s(cli);
        s.open(QIODevice::WriteOnly);
        // Bounded wait: if the guard ever regresses, the second call reaches
        // the stub too and BOTH would block on a release that only arrives
        // after join() — a permanent CI hang. The self-timeout turns that
        // deadlock into a normal test failure.
        s.write("#!/bin/sh\ni=0\nwhile [ ! -f \"");
        s.write(releaseFile.toUtf8());
        s.write("\" ] && [ $i -lt 600 ]; do sleep 0.05; i=$((i+1)); done\nexit 1\n");
        s.close();
        s.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("QTMESH_TRELLIS2_CLI", cli.toUtf8());
        qputenv("QTMESH_TRELLIS2_CLI_MODELS", m.absolutePath().toUtf8());
    }
    ~FakeTrellisCli()
    {
        release();
        qunsetenv("QTMESH_TRELLIS2_CLI");
        qunsetenv("QTMESH_TRELLIS2_CLI_MODELS");
    }
    void release() const { QFile f(releaseFile); f.open(QIODevice::WriteOnly); f.close(); }
};

} // namespace

TEST(Trellis2PredictorTest, SecondConcurrentGenerationIsRefused)
{
    // The stub is a /bin/sh script. runtimeAvailable() only checks that the
    // files EXIST, so on Windows it would pass the gate and then fail to
    // launch — skip there rather than assert on a process that cannot start.
#ifdef Q_OS_WIN
    GTEST_SKIP() << "the stub trellis-cli is a /bin/sh script; needs a native "
                    "fake executable on Windows";
#else
    FakeTrellisCli fake;
    if (!Trellis2Predictor::runtimeAvailable())
        GTEST_SKIP() << "stub runtime not picked up on this platform";

    QImage img(8, 8, QImage::Format_RGB888);
    img.fill(Qt::blue);

    // Hold the slot on a worker: block inside the stub CLI until released.
    std::atomic<bool> inFlight{false};
    std::thread first([&] {
        Trellis2Predictor::Options o;
        Trellis2Predictor::predict(
            img, o, [&](MeshGenPredictor::Stage, int, int) {
                inFlight.store(true);
                return true;
            });
    });

    // Wait until the first run is genuinely inside the guarded region.
    for (int i = 0; i < 400 && !inFlight.load(); ++i)
        QThread::msleep(10);
    // Release + join BEFORE asserting. ASSERT_* returns from the function, so
    // a failure here would destroy a still-joinable std::thread, and that
    // calls std::terminate — aborting the whole binary instead of reporting
    // one failed test.
    const bool started = inFlight.load();
    if (!started) {
        fake.release();
        first.join();
    }
    ASSERT_TRUE(started) << "first generation never started";

    const auto second = Trellis2Predictor::predict(img, {});

    // Same reasoning: finish the worker before any EXPECT can bail out early
    // on a future edit, and so the assertions below run with no live thread.
    fake.release();
    first.join();

    EXPECT_FALSE(second.ok);
    EXPECT_TRUE(second.error.contains(QStringLiteral("already running")))
        << second.error.toStdString();

    // The slot is released afterwards, so a later run is admitted (it fails
    // for its own reason — the stub exits 1 — never with "already running").
    const auto later = Trellis2Predictor::predict(img, {});
    EXPECT_FALSE(later.error.contains(QStringLiteral("already running")))
        << later.error.toStdString();
#endif   // !Q_OS_WIN
}
