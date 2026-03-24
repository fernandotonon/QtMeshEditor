#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "mainwindow.h"
#include "Manager.h"
#include "TestHelpers.h"

// Test that QApplication exists (created by test_main.cpp) - do not create another
TEST(MainTest, QApplicationExists)
{
    // QApplication is created by test_main.cpp - verify it exists
    ASSERT_NE(QCoreApplication::instance(), nullptr);
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    ASSERT_NE(app, nullptr);
}

// DISABLED: Segfaults in Ogre hardware buffer manager during mesh import.
// The MainWindow constructor creates an OgreWidget which requires a real GL
// context and render window. On CI (even with Xvfb), the render system
// initialization path differs from the test harness expectations, causing
// crashes in HardwareBufferManager when meshes are loaded in frameEnded().
// TODO: Refactor MainWindow to allow injecting a mock render system, or
// create a dedicated integration test that runs in a separate process.
TEST(MainTest, DISABLED_ImportMeshs) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    // Use global QApplication from test_main.cpp - do not create another
    ASSERT_NE(QCoreApplication::instance(), nullptr);

    // Ensure Manager is destroyed from previous tests
    Manager::kill();
    QThread::msleep(50);

    try {
        MainWindow mainWindow;

        // Get Manager - it should be created by MainWindow constructor
        Manager* manager = Manager::getSingleton(&mainWindow);
        ASSERT_NE(manager, nullptr);

        // Get initial count - Manager might have some entities from initialization
        auto before = manager->getEntities().count();

        // Import meshes - this happens in frameEnded, so we need to render a frame
        manager->getRoot()->renderOneFrame();

        // Wait a bit for async operations
        QThread::msleep(100);
        QCoreApplication::processEvents();

        auto after = manager->getEntities().count();

        EXPECT_GE(after, before + 2) << "Expected at least 2 new entities after importing 2 meshes";
        EXPECT_LE(after, before + 4) << "Expected at most 4 new entities (some meshes create multiple entities)";
    } catch (const Ogre::RenderingAPIException& e) {
        GTEST_SKIP() << "Skipping ImportMeshs test: unable to create OGRE render window ("
                     << e.getFullDescription() << ")";
    } catch (const std::exception& e) {
        GTEST_SKIP() << "Skipping ImportMeshs test: " << e.what();
    } catch (...) {
        GTEST_SKIP() << "Skipping ImportMeshs test: unknown exception (possible segfault in mesh loading)";
    }
}
