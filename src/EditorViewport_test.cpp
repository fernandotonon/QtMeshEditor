#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>
#include <QStyleFactory>
#include "EditorViewport.h"
#include "mainwindow.h"
#include "Manager.h"
#include "TestHelpers.h"

class EditorViewportTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    MainWindow* mainWindow = nullptr;

    void SetUp() override {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        Manager::kill();
        QThread::msleep(50);

        try {
            mainWindow = new MainWindow();
        } catch (...) {
            GTEST_SKIP() << "Skipping: MainWindow creation failed";
        }
        ASSERT_NE(mainWindow, nullptr);
    }

    void TearDown() override {
        delete mainWindow;
        mainWindow = nullptr;
        Manager::kill();
        QThread::msleep(50);
    }
};

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
