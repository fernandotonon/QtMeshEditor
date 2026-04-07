#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>
#include <QStyleFactory>
#include <exception>
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

        constexpr int kMaxMainWindowInitAttempts = 5;
        for (int attempt = 1; attempt <= kMaxMainWindowInitAttempts && !mainWindow; ++attempt) {
            try {
                mainWindow = new MainWindow();
            } catch (const std::exception& e) {
                GTEST_LOG_(WARNING) << "MainWindow init attempt " << attempt
                                    << " failed: " << e.what();
                mainWindow = nullptr;
                app->processEvents();
                QThread::msleep(400);
            } catch (...) {
                GTEST_LOG_(WARNING) << "MainWindow init attempt " << attempt
                                    << " failed with unknown exception";
                mainWindow = nullptr;
                app->processEvents();
                QThread::msleep(400);
            }
        }
        ASSERT_NE(mainWindow, nullptr) << "Failed to initialize MainWindow for EditorViewport tests";
    }

    static void TearDownTestSuite() {
        delete mainWindow;
        mainWindow = nullptr;
        if (app) {
            app->processEvents();
        }
        Manager::kill();
        QThread::msleep(100);
        app = nullptr;
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
    EXPECT_EQ(spy.count(), 1);
    delete viewport;
}
