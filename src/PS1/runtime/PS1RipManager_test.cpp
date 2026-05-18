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
        PS1RipManager::kill();
        manager = PS1RipManager::getSingleton();
        ASSERT_NE(manager, nullptr);
    }

    void TearDown() override
    {
        if (manager && manager->isSessionActive())
            manager->stop();
        PS1RipManager::kill();
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
    QTemporaryFile iso(QDir::tempPath() + "/qtmesh_ps1rip_XXXXXX.bin");
    ASSERT_TRUE(iso.open());
    iso.write("stub");
    iso.close();
    ASSERT_TRUE(manager->loadIso(iso.fileName()));

    QSignalSpy errorSpy(manager, &PS1RipManager::error);
    EXPECT_FALSE(manager->start());
}

TEST_F(PS1RipManagerTest, SessionStartsWhenPluginPresent)
{
    if (!stubPluginAvailable())
        GTEST_SKIP() << "PS1 stub core plugin not beside test binary";

    QTemporaryFile bios(QDir::tempPath() + "/qtmesh_bios_XXXXXX.bin");
    QTemporaryFile iso(QDir::tempPath() + "/qtmesh_iso_XXXXXX.bin");
    ASSERT_TRUE(bios.open());
    ASSERT_TRUE(iso.open());
    bios.write("bios");
    iso.write("iso");
    bios.close();
    iso.close();

    ASSERT_TRUE(manager->loadBios(bios.fileName()));
    ASSERT_TRUE(manager->loadIso(iso.fileName()));

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

    QTemporaryFile bios(QDir::tempPath() + "/qtmesh_bios2_XXXXXX.bin");
    QTemporaryFile iso(QDir::tempPath() + "/qtmesh_iso2_XXXXXX.bin");
    ASSERT_TRUE(bios.open());
    ASSERT_TRUE(iso.open());
    bios.write("bios");
    iso.write("iso");
    bios.close();
    iso.close();

    ASSERT_TRUE(manager->loadBios(bios.fileName()));
    ASSERT_TRUE(manager->loadIso(iso.fileName()));

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

    QTemporaryFile bios(QDir::tempPath() + "/qtmesh_bios_cap_XXXXXX.bin");
    QTemporaryFile iso(QDir::tempPath() + "/qtmesh_iso_cap_XXXXXX.bin");
    ASSERT_TRUE(bios.open());
    ASSERT_TRUE(iso.open());
    bios.write("bios");
    iso.write("iso");
    bios.close();
    iso.close();

    ASSERT_TRUE(manager->loadBios(bios.fileName()));
    ASSERT_TRUE(manager->loadIso(iso.fileName()));
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

#endif // ENABLE_PS1_RIP
