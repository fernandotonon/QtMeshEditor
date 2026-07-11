#ifdef ENABLE_PS1_RIP

#if defined(QTMESH_PS1_LIBRETRO_INTEGRATION_TESTS)

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTest>

#include "PS1/runtime/PS1RipManager.h"

namespace {

QString requireTestBiosPath()
{
    const QString path = qEnvironmentVariable("QTMESH_PS1_TEST_BIOS");
    if (path.isEmpty() || !QFileInfo::exists(path))
        return {};
    return path;
}

QString requireTestIsoPath()
{
    const QString path = qEnvironmentVariable("QTMESH_PS1_TEST_ISO");
    if (path.isEmpty() || !QFileInfo::exists(path))
        return {};
    return path;
}

bool libretroPluginAvailable()
{
    const QString base = QCoreApplication::applicationDirPath() + QStringLiteral("/PS1Cores/");
    return QFileInfo::exists(base + QStringLiteral("qtmesh_ps1core_libretro.so"))
           || QFileInfo::exists(base + QStringLiteral("libqtmesh_ps1core_libretro.so"));
}

void skipUnlessLibretroAssetsReady()
{
    if (!libretroPluginAvailable())
        GTEST_SKIP() << "libretro PS1 plugin not beside test binary";
    if (requireTestBiosPath().isEmpty() || requireTestIsoPath().isEmpty())
        GTEST_SKIP() << "Set QTMESH_PS1_TEST_BIOS and QTMESH_PS1_TEST_ISO to existing files";
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
    skipUnlessLibretroAssetsReady();
    // GTEST_SKIP() inside the helper only returns from the helper, not from
    // this test body — re-check the skip state here so the assertions below
    // don't run (and fail) when the libretro plugin / BIOS aren't present.
    if (IsSkipped())
        return;

    const QString biosPath = requireTestBiosPath();
    const QString isoPath = requireTestIsoPath();

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
    skipUnlessLibretroAssetsReady();
    if (IsSkipped())
        return;

    const QString biosPath = requireTestBiosPath();
    const QString isoPath = requireTestIsoPath();

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

#endif // QTMESH_PS1_LIBRETRO_INTEGRATION_TESTS

#endif // ENABLE_PS1_RIP
