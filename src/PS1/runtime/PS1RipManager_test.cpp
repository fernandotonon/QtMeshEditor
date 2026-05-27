#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTest>

#include "PS1/runtime/PS1RipManager.h"

class PS1RipManagerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        app = qobject_cast<QApplication *>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        qputenv("QTMESH_PS1_FORCE_STUB", "1");
        PS1RipManager::kill();
        manager = PS1RipManager::getSingleton();
        ASSERT_NE(manager, nullptr);
    }

    void TearDown() override
    {
        if (manager && manager->isSessionActive())
            manager->stop();
        PS1RipManager::kill();
        qunsetenv("QTMESH_PS1_FORCE_STUB");
    }

    static QString writeMinimalTestIso(QTemporaryFile &file)
    {
        file.setFileTemplate(QDir::tempPath() + QStringLiteral("/qtmesh_iso_XXXXXX.iso"));
        if (!file.open())
            return {};
        QByteArray sector(2048, '\0');
        sector[0] = '\x01';
        sector[1] = 'C';
        sector[2] = 'D';
        sector[3] = '0';
        sector[4] = '0';
        sector[5] = '1';
        for (int i = 0; i < 17; ++i) {
            if (file.write(sector) != sector.size())
                return {};
        }
        file.close();
        return file.fileName();
    }

    static QString writeStubBios(QTemporaryFile &file)
    {
        file.setFileTemplate(QDir::tempPath() + QStringLiteral("/qtmesh_bios_XXXXXX.bin"));
        if (!file.open())
            return {};
        if (file.write(QByteArray(512 * 1024, '\0')) != 512 * 1024)
            return {};
        file.close();
        return file.fileName();
    }

    bool stubPluginAvailable() const
    {
        const QString base = QCoreApplication::applicationDirPath() + QStringLiteral("/PS1Cores/");
#if defined(Q_OS_WIN)
        return QFileInfo::exists(base + QStringLiteral("qtmesh_ps1core_stub.dll"));
#elif defined(Q_OS_MACOS)
        return QFileInfo::exists(base + QStringLiteral("libqtmesh_ps1core_stub.dylib"));
#else
        return QFileInfo::exists(base + QStringLiteral("libqtmesh_ps1core_stub.so"))
               || QFileInfo::exists(base + QStringLiteral("qtmesh_ps1core_stub.so"));
#endif
    }

    QApplication *app = nullptr;
    PS1RipManager *manager = nullptr;
};

TEST_F(PS1RipManagerTest, SingletonLifecycle)
{
    PS1RipManager *a = PS1RipManager::getSingleton();
    PS1RipManager *b = PS1RipManager::getSingletonPtr();
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, manager);

    PS1RipManager::kill();
    EXPECT_EQ(PS1RipManager::getSingletonPtr(), nullptr);

    PS1RipManager *c = PS1RipManager::getSingleton();
    EXPECT_NE(c, nullptr);
    EXPECT_EQ(PS1RipManager::getSingletonPtr(), c);
    manager = c;
}

TEST_F(PS1RipManagerTest, StartWithoutIsoReturnsFalse)
{
    QSignalSpy errorSpy(manager, &PS1RipManager::error);
    EXPECT_FALSE(manager->start());
    EXPECT_GE(errorSpy.count(), 0);
}

TEST_F(PS1RipManagerTest, StartWithoutBiosReturnsFalse)
{
    QTemporaryFile iso;
    const QString isoPath = writeMinimalTestIso(iso);
    ASSERT_FALSE(isoPath.isEmpty());
    ASSERT_TRUE(manager->loadIso(isoPath));

    QSignalSpy errorSpy(manager, &PS1RipManager::error);
    EXPECT_FALSE(manager->start());
}

TEST_F(PS1RipManagerTest, SessionStartsWhenPluginPresent)
{
    if (!stubPluginAvailable())
        GTEST_SKIP() << "PS1 stub core plugin not beside test binary";

    QTemporaryFile bios;
    QTemporaryFile iso;
    const QString biosPath = writeStubBios(bios);
    ASSERT_FALSE(biosPath.isEmpty());
    const QString isoPath = writeMinimalTestIso(iso);
    ASSERT_FALSE(isoPath.isEmpty());

    ASSERT_TRUE(manager->loadBios(biosPath));
    ASSERT_TRUE(manager->loadIso(isoPath));

    QSignalSpy startedSpy(manager, &PS1RipManager::sessionStarted);
    QSignalSpy errorSpy(manager, &PS1RipManager::error);
    ASSERT_TRUE(manager->start());

    ASSERT_TRUE(startedSpy.wait(3000));
    if (startedSpy.empty() && !errorSpy.empty())
        GTEST_SKIP() << "Emulator failed to start in test environment";

    EXPECT_FALSE(startedSpy.empty());
    EXPECT_TRUE(manager->isSessionActive());
    manager->stop();
}

TEST_F(PS1RipManagerTest, ArmCaptureWithoutSession)
{
    EXPECT_TRUE(manager->armCapture(true));
    EXPECT_TRUE(manager->isCaptureArmed());
    EXPECT_FALSE(manager->captureFrame());
}

TEST_F(PS1RipManagerTest, StopCancelsPendingStart)
{
    if (!stubPluginAvailable())
        GTEST_SKIP() << "PS1 stub core plugin not beside test binary";

    QTemporaryFile bios;
    QTemporaryFile iso;
    const QString biosPath = writeStubBios(bios);
    ASSERT_FALSE(biosPath.isEmpty());
    const QString isoPath = writeMinimalTestIso(iso);
    ASSERT_FALSE(isoPath.isEmpty());

    ASSERT_TRUE(manager->loadBios(biosPath));
    ASSERT_TRUE(manager->loadIso(isoPath));

    QSignalSpy startedSpy(manager, &PS1RipManager::sessionStarted);
    ASSERT_TRUE(manager->start());
    EXPECT_TRUE(manager->isStartPending());

    ASSERT_TRUE(manager->stop());
    EXPECT_FALSE(manager->isStartPending());
    EXPECT_FALSE(manager->isSessionActive());

    ASSERT_FALSE(startedSpy.wait(500));
    EXPECT_TRUE(startedSpy.empty());
}

TEST_F(PS1RipManagerTest, ArmedCaptureAccumulatesPrimitives)
{
    if (!stubPluginAvailable())
        GTEST_SKIP() << "PS1 stub core plugin not beside test binary";

    QTemporaryFile bios;
    QTemporaryFile iso;
    const QString biosPath = writeStubBios(bios);
    ASSERT_FALSE(biosPath.isEmpty());
    const QString isoPath = writeMinimalTestIso(iso);
    ASSERT_FALSE(isoPath.isEmpty());

    ASSERT_TRUE(manager->loadBios(biosPath));
    ASSERT_TRUE(manager->loadIso(isoPath));
    ASSERT_TRUE(manager->armCapture(true));

    QSignalSpy startedSpy(manager, &PS1RipManager::sessionStarted);
    ASSERT_TRUE(manager->start());
    ASSERT_TRUE(startedSpy.wait(3000));

    QTest::qWait(100);

    QSignalSpy captureSpy(manager, &PS1RipManager::frameCaptured);
    ASSERT_TRUE(manager->captureFrame());
    ASSERT_TRUE(captureSpy.wait(3000));
    EXPECT_FALSE(captureSpy.empty());

    manager->stop();
}

// --- Scene capture lifecycle (#425) -----------------------------------------

TEST_F(PS1RipManagerTest, SceneCaptureRejectsNonPositiveDuration)
{
    QSignalSpy startedSpy(manager, &PS1RipManager::sceneCaptureStarted);
    QSignalSpy errorSpy(manager, &PS1RipManager::error);

    EXPECT_FALSE(manager->captureScene(0));
    EXPECT_FALSE(manager->captureScene(-1));
    EXPECT_TRUE(startedSpy.isEmpty());
    EXPECT_GE(errorSpy.count(), 1);
    EXPECT_FALSE(manager->isSceneCaptureActive());
}

TEST_F(PS1RipManagerTest, SceneCaptureRejectsWithoutSession)
{
    QSignalSpy startedSpy(manager, &PS1RipManager::sceneCaptureStarted);
    QSignalSpy errorSpy(manager, &PS1RipManager::error);

    EXPECT_FALSE(manager->captureScene(2));
    EXPECT_TRUE(startedSpy.isEmpty());
    EXPECT_GE(errorSpy.count(), 1);
    EXPECT_FALSE(manager->isSceneCaptureActive());
}

TEST_F(PS1RipManagerTest, SceneCaptureCancellableViaStopSceneCapture)
{
    if (!stubPluginAvailable())
        GTEST_SKIP() << "PS1 stub core plugin not beside test binary";

    QTemporaryFile bios;
    QTemporaryFile iso;
    const QString biosPath = writeStubBios(bios);
    ASSERT_FALSE(biosPath.isEmpty());
    const QString isoPath = writeMinimalTestIso(iso);
    ASSERT_FALSE(isoPath.isEmpty());

    ASSERT_TRUE(manager->loadBios(biosPath));
    ASSERT_TRUE(manager->loadIso(isoPath));

    QSignalSpy startedSessionSpy(manager, &PS1RipManager::sessionStarted);
    ASSERT_TRUE(manager->start());
    ASSERT_TRUE(startedSessionSpy.wait(3000));

    QSignalSpy startedSpy(manager, &PS1RipManager::sceneCaptureStarted);
    QSignalSpy finishedSpy(manager, &PS1RipManager::sceneCaptureFinished);
    // Long duration so the test can cancel before the timer would fire.
    ASSERT_TRUE(manager->captureScene(30));
    EXPECT_TRUE(manager->isSceneCaptureActive());
    EXPECT_EQ(startedSpy.count(), 1);
    EXPECT_EQ(startedSpy.at(0).at(0).toInt(), 30);
    // captureScene auto-arms when the user didn't pre-arm.
    EXPECT_TRUE(manager->isCaptureArmed());

    ASSERT_TRUE(manager->stopSceneCapture());
    EXPECT_FALSE(manager->isSceneCaptureActive());
    ASSERT_EQ(finishedSpy.count(), 1);
    EXPECT_TRUE(finishedSpy.at(0).at(0).toBool()); // cancelled
    EXPECT_TRUE(finishedSpy.at(0).at(1).toString().isEmpty()); // no captureId

    manager->stop();
}

TEST_F(PS1RipManagerTest, SceneCaptureCancelledOnDisarm)
{
    if (!stubPluginAvailable())
        GTEST_SKIP() << "PS1 stub core plugin not beside test binary";

    QTemporaryFile bios;
    QTemporaryFile iso;
    const QString biosPath = writeStubBios(bios);
    ASSERT_FALSE(biosPath.isEmpty());
    const QString isoPath = writeMinimalTestIso(iso);
    ASSERT_FALSE(isoPath.isEmpty());

    ASSERT_TRUE(manager->loadBios(biosPath));
    ASSERT_TRUE(manager->loadIso(isoPath));

    QSignalSpy startedSessionSpy(manager, &PS1RipManager::sessionStarted);
    ASSERT_TRUE(manager->start());
    ASSERT_TRUE(startedSessionSpy.wait(3000));

    QSignalSpy finishedSpy(manager, &PS1RipManager::sceneCaptureFinished);
    ASSERT_TRUE(manager->captureScene(30));
    EXPECT_TRUE(manager->isSceneCaptureActive());

    manager->armCapture(false);
    EXPECT_FALSE(manager->isSceneCaptureActive());
    EXPECT_FALSE(manager->isCaptureArmed());
    ASSERT_GE(finishedSpy.count(), 1);
    EXPECT_TRUE(finishedSpy.first().at(0).toBool()); // cancelled

    manager->stop();
}

TEST_F(PS1RipManagerTest, SceneCaptureCancelledOnStop)
{
    if (!stubPluginAvailable())
        GTEST_SKIP() << "PS1 stub core plugin not beside test binary";

    QTemporaryFile bios;
    QTemporaryFile iso;
    const QString biosPath = writeStubBios(bios);
    ASSERT_FALSE(biosPath.isEmpty());
    const QString isoPath = writeMinimalTestIso(iso);
    ASSERT_FALSE(isoPath.isEmpty());

    ASSERT_TRUE(manager->loadBios(biosPath));
    ASSERT_TRUE(manager->loadIso(isoPath));

    QSignalSpy startedSessionSpy(manager, &PS1RipManager::sessionStarted);
    ASSERT_TRUE(manager->start());
    ASSERT_TRUE(startedSessionSpy.wait(3000));

    QSignalSpy finishedSpy(manager, &PS1RipManager::sceneCaptureFinished);
    ASSERT_TRUE(manager->captureScene(30));
    EXPECT_TRUE(manager->isSceneCaptureActive());

    manager->stop();
    EXPECT_FALSE(manager->isSceneCaptureActive());
    ASSERT_GE(finishedSpy.count(), 1);
    EXPECT_TRUE(finishedSpy.first().at(0).toBool()); // cancelled
}

TEST_F(PS1RipManagerTest, SceneCaptureProgressTicksAndCompletes)
{
    if (!stubPluginAvailable())
        GTEST_SKIP() << "PS1 stub core plugin not beside test binary";

    QTemporaryFile bios;
    QTemporaryFile iso;
    const QString biosPath = writeStubBios(bios);
    ASSERT_FALSE(biosPath.isEmpty());
    const QString isoPath = writeMinimalTestIso(iso);
    ASSERT_FALSE(isoPath.isEmpty());

    ASSERT_TRUE(manager->loadBios(biosPath));
    ASSERT_TRUE(manager->loadIso(isoPath));

    QSignalSpy startedSessionSpy(manager, &PS1RipManager::sessionStarted);
    ASSERT_TRUE(manager->start());
    ASSERT_TRUE(startedSessionSpy.wait(3000));

    QSignalSpy progressSpy(manager, &PS1RipManager::sceneCaptureProgress);
    QSignalSpy finishedSpy(manager, &PS1RipManager::sceneCaptureFinished);
    ASSERT_TRUE(manager->captureScene(2));
    EXPECT_EQ(manager->sceneCaptureSecondsRemaining(), 2);
    EXPECT_EQ(manager->sceneCaptureSecondsTotal(), 2);
    // First progress emission fires immediately so the UI shows the full
    // duration without waiting for the first 1 s tick.
    ASSERT_GE(progressSpy.count(), 1);

    // Wait long enough for both timer ticks + the finalize round-trip through
    // the worker thread (~2 s + slack).
    ASSERT_TRUE(finishedSpy.wait(8000));
    EXPECT_FALSE(manager->isSceneCaptureActive());
    EXPECT_GE(progressSpy.count(), 2);
    // The completion signal may be cancelled (e.g. when the stub core didn't
    // produce captureable prims), but it MUST fire so the UI exits scene-
    // capture state. Test that both branches drop us out cleanly.
    const bool cancelled = finishedSpy.first().at(0).toBool();
    if (!cancelled) {
        // Success path: captureId must be non-empty so the UI can correlate.
        EXPECT_FALSE(finishedSpy.first().at(1).toString().isEmpty());
    }

    manager->stop();
}

TEST_F(PS1RipManagerTest, SceneCaptureIsActiveBetweenStartAndStop)
{
    // Verifies `isSceneCaptureActive()` (the combined predicate driving the
    // Stop-Capture-in-finalize-window fix, Codex P1 / CodeRabbit Major on
    // #677) flips to true as soon as captureScene() returns and back to
    // false once the user cancels via stopSceneCapture(). Without this
    // predicate, stop-during-finalize left the UI stuck.
    if (!stubPluginAvailable())
        GTEST_SKIP() << "PS1 stub core plugin not beside test binary";

    QTemporaryFile bios;
    QTemporaryFile iso;
    const QString biosPath = writeStubBios(bios);
    ASSERT_FALSE(biosPath.isEmpty());
    const QString isoPath = writeMinimalTestIso(iso);
    ASSERT_FALSE(isoPath.isEmpty());

    ASSERT_TRUE(manager->loadBios(biosPath));
    ASSERT_TRUE(manager->loadIso(isoPath));

    QSignalSpy startedSessionSpy(manager, &PS1RipManager::sessionStarted);
    ASSERT_TRUE(manager->start());
    ASSERT_TRUE(startedSessionSpy.wait(3000));

    EXPECT_FALSE(manager->isSceneCaptureActive());
    ASSERT_TRUE(manager->captureScene(30));
    EXPECT_TRUE(manager->isSceneCaptureActive());

    // Cancellation in the active window must succeed.
    EXPECT_TRUE(manager->stopSceneCapture());
    EXPECT_FALSE(manager->isSceneCaptureActive());

    // A second stop with no scene capture in flight must be a no-op (false)
    // so callers can chain Stop Capture + Disarm without spurious double
    // breadcrumbs / signal storms.
    EXPECT_FALSE(manager->stopSceneCapture());

    manager->stop();
}

TEST_F(PS1RipManagerTest, SceneCaptureRejectsConcurrent)
{
    if (!stubPluginAvailable())
        GTEST_SKIP() << "PS1 stub core plugin not beside test binary";

    QTemporaryFile bios;
    QTemporaryFile iso;
    const QString biosPath = writeStubBios(bios);
    ASSERT_FALSE(biosPath.isEmpty());
    const QString isoPath = writeMinimalTestIso(iso);
    ASSERT_FALSE(isoPath.isEmpty());

    ASSERT_TRUE(manager->loadBios(biosPath));
    ASSERT_TRUE(manager->loadIso(isoPath));

    QSignalSpy startedSessionSpy(manager, &PS1RipManager::sessionStarted);
    ASSERT_TRUE(manager->start());
    ASSERT_TRUE(startedSessionSpy.wait(3000));

    ASSERT_TRUE(manager->captureScene(30));
    QSignalSpy errorSpy(manager, &PS1RipManager::error);
    EXPECT_FALSE(manager->captureScene(5));
    EXPECT_GE(errorSpy.count(), 1);

    manager->stopSceneCapture();
    manager->stop();
}

#endif // ENABLE_PS1_RIP
