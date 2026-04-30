#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>
#include <QStyleFactory>
#include <exception>
#include <OgreException.h>
#include "EditorViewport.h"
#include "mainwindow.h"
#include "Manager.h"
#include "TestHelpers.h"

class EditorViewportTest : public ::testing::Test {
protected:
    static QApplication* app;
    static MainWindow* mainWindow;

    static void SetUpTestSuite() {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        app->processEvents();
        Manager::kill();
        QThread::msleep(100);

        constexpr int kMaxMainWindowInitAttempts = 6;
        for (int attempt = 1; attempt <= kMaxMainWindowInitAttempts && !mainWindow; ++attempt) {
            // EGL/Xvfb setup can transiently fail to create the Ogre surface in CI.
            // Reset global manager state between attempts before constructing MainWindow.
            Manager::kill();
            app->processEvents();
            QThread::msleep(75);
            try {
                mainWindow = new MainWindow();
            } catch (const Ogre::Exception& e) {
                GTEST_LOG_(WARNING) << "MainWindow init attempt " << attempt
                                    << " failed: " << e.getFullDescription();
                mainWindow = nullptr;
                Manager::kill();
                app->processEvents();
                QThread::msleep(200 * attempt);
            } catch (const std::exception& e) {
                GTEST_LOG_(WARNING) << "MainWindow init attempt " << attempt
                                    << " failed: " << e.what();
                mainWindow = nullptr;
                Manager::kill();
                app->processEvents();
                QThread::msleep(200 * attempt);
            } catch (...) {
                GTEST_LOG_(WARNING) << "MainWindow init attempt " << attempt
                                    << " failed with unknown exception";
                mainWindow = nullptr;
                Manager::kill();
                app->processEvents();
                QThread::msleep(200 * attempt);
            }
        }
        ASSERT_NE(mainWindow, nullptr) << "Failed to initialize MainWindow for EditorViewport tests";
    }

    static void TearDownTestSuite() {
        // NOTE: We intentionally do NOT delete the MainWindow nor kill Manager here.
        //
        // The overall test suite has multiple fixtures that create/destroy Ogre-backed
        // singletons in different orders. Deleting MainWindow during this suite teardown
        // can crash inside Ogre-backed destructors (e.g. gizmos) if another fixture already
        // tore down parts of Ogre. Leaking the window at process shutdown is acceptable for
        // unit tests; gtest will exit immediately afterwards.
        if (mainWindow) {
            mainWindow->close();
        }
        if (app) app->processEvents();
        mainWindow = nullptr;
    }

    void SetUp() override {
        ASSERT_NE(app, nullptr);
        ASSERT_NE(mainWindow, nullptr);
    }

    void TearDown() override {
        if (app) {
            app->processEvents();
        }
    }
};

QApplication* EditorViewportTest::app = nullptr;
MainWindow* EditorViewportTest::mainWindow = nullptr;

TEST_F(EditorViewportTest, ConstructionWithIndex) {
    EditorViewport viewport(mainWindow, 5);
    EXPECT_EQ(viewport.getIndex(), 5);
}

TEST_F(EditorViewportTest, GetIndexReturnsCorrectValue) {
    EditorViewport viewport1(mainWindow, 0);
    EXPECT_EQ(viewport1.getIndex(), 0);

    EditorViewport viewport2(mainWindow, 42);
    EXPECT_EQ(viewport2.getIndex(), 42);
}

TEST_F(EditorViewportTest, GetMainWindowReturnsParent) {
    EditorViewport viewport(mainWindow, 1);
    EXPECT_EQ(viewport.getMainWindow(), mainWindow);
}

TEST_F(EditorViewportTest, GetOgreWidgetReturnsValid) {
    EditorViewport viewport(mainWindow, 0);
    OgreWidget* ogreWidget = viewport.getOgreWidget();
    EXPECT_NE(ogreWidget, nullptr);
}

TEST_F(EditorViewportTest, WidgetAboutToCloseSignalEmitted) {
    EditorViewport* viewport = new EditorViewport(mainWindow, 0);
    QSignalSpy spy(viewport, &EditorViewport::widgetAboutToClose);
    ASSERT_TRUE(spy.isValid());
    viewport->close();
    // Let Qt deliver close events / deferred deletes before we destroy the viewport.
    if (app) {
        app->processEvents();
    }
    EXPECT_EQ(spy.count(), 1);
    delete viewport;
}
