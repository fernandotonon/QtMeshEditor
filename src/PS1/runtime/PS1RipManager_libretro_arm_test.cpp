#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include <atomic>

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QSignalSpy>
#include <QThread>
#include <QTest>

#include "PS1/runtime/PS1RipManager.h"

namespace {

QString defaultBiosPath()
{
    const QString env = qEnvironmentVariable("QTMESH_PS1_TEST_BIOS");
    if (!env.isEmpty())
        return env;
    return QStringLiteral("/home/fernando/Downloads/scph5501.bin");
}

QString defaultIsoPath()
{
    const QString env = qEnvironmentVariable("QTMESH_PS1_TEST_ISO");
    if (!env.isEmpty())
        return env;
    return QStringLiteral(
        "/home/fernando/Downloads/Crash Bandicoot - Warped (USA)/Crash Bandicoot - Warped (USA)/"
        "Crash Bandicoot - Warped (USA).cue");
}

bool libretroPluginAvailable()
{
    const QString base = QCoreApplication::applicationDirPath() + QStringLiteral("/PS1Cores/");
    return QFileInfo::exists(base + QStringLiteral("qtmesh_ps1core_libretro.so"))
           || QFileInfo::exists(base + QStringLiteral("libqtmesh_ps1core_libretro.so"));
}

} // namespace

class PS1RipManagerLibretroArmTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_NE(qobject_cast<QApplication *>(QCoreApplication::instance()), nullptr);
        qunsetenv("QTMESH_PS1_FORCE_STUB");
        PS1RipManager::kill();
        manager = PS1RipManager::getSingleton();
        ASSERT_NE(manager, nullptr);
    }

    void TearDown() override
    {
        if (manager && manager->isSessionActive())
            manager->stop();
        PS1RipManager::kill();
        qputenv("QTMESH_PS1_FORCE_STUB", "1");
    }

    PS1RipManager *manager = nullptr;
};

TEST_F(PS1RipManagerLibretroArmTest, ArmCaptureWhileLibretroSessionRuns)
{
    if (!libretroPluginAvailable())
        GTEST_SKIP() << "libretro PS1 plugin not beside test binary";

    const QString biosPath = defaultBiosPath();
    const QString isoPath = defaultIsoPath();
    ASSERT_TRUE(QFileInfo::exists(biosPath)) << biosPath.toStdString();
    ASSERT_TRUE(QFileInfo::exists(isoPath)) << isoPath.toStdString();

    ASSERT_TRUE(manager->loadBios(biosPath));
    ASSERT_TRUE(manager->loadIso(isoPath));

    QSignalSpy startedSpy(manager, &PS1RipManager::sessionStarted);
    ASSERT_TRUE(manager->start());
    ASSERT_TRUE(startedSpy.wait(60000)) << "libretro boot timed out";

    QTest::qWait(2000);

    for (int i = 0; i < 30; ++i) {
        EXPECT_TRUE(manager->armCapture(true));
        QTest::qWait(30);
        EXPECT_TRUE(manager->armCapture(false));
        QTest::qWait(30);
    }
    EXPECT_TRUE(manager->armCapture(true));

    QTest::qWait(3000);
    ASSERT_TRUE(manager->isSessionActive());
    manager->stop();
}

TEST_F(PS1RipManagerLibretroArmTest, ArmThenCaptureFrame)
{
    if (!libretroPluginAvailable())
        GTEST_SKIP() << "libretro PS1 plugin not beside test binary";

    const QString biosPath = defaultBiosPath();
    const QString isoPath = defaultIsoPath();
    ASSERT_TRUE(QFileInfo::exists(biosPath));
    ASSERT_TRUE(QFileInfo::exists(isoPath));

    ASSERT_TRUE(manager->loadBios(biosPath));
    ASSERT_TRUE(manager->loadIso(isoPath));

    QSignalSpy startedSpy(manager, &PS1RipManager::sessionStarted);
    ASSERT_TRUE(manager->start());
    ASSERT_TRUE(startedSpy.wait(60000));

    QTest::qWait(3000);

    ASSERT_TRUE(manager->armCapture(true));
    QTest::qWait(500);

    QSignalSpy captureSpy(manager, &PS1RipManager::frameCaptured);
    ASSERT_TRUE(manager->captureFrame());
    ASSERT_TRUE(captureSpy.wait(120000)) << "capture frame timed out";

    manager->stop();
}

#endif // ENABLE_PS1_RIP
