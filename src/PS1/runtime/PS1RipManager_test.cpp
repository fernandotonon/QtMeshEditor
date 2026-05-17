#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QDir>
#include <QTemporaryFile>

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
        PS1RipManager::kill();
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
}

TEST_F(PS1RipManagerTest, StartWithoutIsoReturnsFalse)
{
    QSignalSpy errorSpy(manager, &PS1RipManager::error);
    EXPECT_FALSE(manager->start());
    EXPECT_TRUE(manager->isoPath().isEmpty());
    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(PS1RipManagerTest, LoadIsoThenStartStillStub)
{
    QTemporaryFile iso(QDir::tempPath() + "/qtmesh_ps1rip_XXXXXX.bin");
    ASSERT_TRUE(iso.open());
    iso.write("stub");
    iso.close();

    ASSERT_TRUE(manager->loadIso(iso.fileName()));
    EXPECT_TRUE(manager->hasIso());
    EXPECT_FALSE(manager->start());
    EXPECT_FALSE(manager->isSessionActive());
}

TEST_F(PS1RipManagerTest, ArmCaptureWithoutSession)
{
    EXPECT_TRUE(manager->armCapture(true));
    EXPECT_TRUE(manager->isCaptureArmed());
    EXPECT_FALSE(manager->captureFrame());
}

TEST_F(PS1RipManagerTest, ErrorSignalWired)
{
    QSignalSpy errorSpy(manager, &PS1RipManager::error);
    manager->captureScene(0);
    EXPECT_GE(errorSpy.count(), 1);
}

#endif // ENABLE_PS1_RIP
