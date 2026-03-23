#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QMimeData>
#include "mainwindow.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TransformOperator.h"
#include "TestHelpers.h"

class MainWindowTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    MainWindow* window = nullptr;

    void SetUp() override {
        TransformOperator::kill();
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createStandardOgreMaterials();

        window = new MainWindow();
        ASSERT_NE(window, nullptr);
    }

    void TearDown() override {
        delete window;
        window = nullptr;
        if (app) app->processEvents();
    }
};

// ---- setTransformState ----

TEST_F(MainWindowTest, SetTransformStateSelect) {
    EXPECT_NO_THROW(window->keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier)));
}

TEST_F(MainWindowTest, SetTransformStateTranslate) {
    EXPECT_NO_THROW(window->keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier)));
}

TEST_F(MainWindowTest, SetTransformStateRotate) {
    EXPECT_NO_THROW(window->keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_E, Qt::NoModifier)));
}

TEST_F(MainWindowTest, SetTransformStateScale) {
    EXPECT_NO_THROW(window->keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier)));
}

// ---- Key shortcuts ----

TEST_F(MainWindowTest, KeyXTogglesTransformSpace) {
    auto space_before = TransformOperator::getSingleton()->getTransformSpace();
    window->keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier));
    auto space_after = TransformOperator::getSingleton()->getTransformSpace();
    EXPECT_NE(space_before, space_after);

    // Toggle back
    window->keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier));
    EXPECT_EQ(TransformOperator::getSingleton()->getTransformSpace(), space_before);
}

TEST_F(MainWindowTest, KeyDeleteWithEmptySelection) {
    SelectionSet::getSingleton()->clear();
    EXPECT_NO_THROW(window->keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier)));
}

TEST_F(MainWindowTest, KeyFFrameSelectionEmptyDoesNotCrash) {
    SelectionSet::getSingleton()->clear();
    EXPECT_NO_THROW(window->keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier)));
}

// keyReleaseEvent is protected — tested implicitly via keyPressEvent

// ---- setPlaying ----

TEST_F(MainWindowTest, SetPlayingTrue) {
    EXPECT_NO_THROW(window->setPlaying(true));
}

TEST_F(MainWindowTest, SetPlayingFalse) {
    EXPECT_NO_THROW(window->setPlaying(false));
}

// ---- Cycle all transform states via keyboard ----

TEST_F(MainWindowTest, CycleAllStatesViaKeyboard) {
    window->keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier));
    window->keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier));
    window->keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_E, Qt::NoModifier));
    window->keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier));
    window->keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier));
    // Should not crash
}

// ---- Unmapped key ----

TEST_F(MainWindowTest, UnmappedKeyDoesNotCrash) {
    window->keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier));
    window->keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier));
}

// ---- importMeshs with empty list ----

TEST_F(MainWindowTest, ImportMeshsEmptyList) {
    EXPECT_NO_THROW(window->importMeshs(QStringList()));
}

// ---- Drop event ----

TEST_F(MainWindowTest, DropEventNoValidFiles) {
    auto* mimeData = new QMimeData();
    mimeData->setUrls({QUrl::fromLocalFile("/nonexistent/file.txt")});
    QDropEvent event(QPointF(0, 0), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);
    EXPECT_NO_THROW(window->dropEvent(&event));
    delete mimeData;
}

TEST_F(MainWindowTest, DropEventWithFbxFile) {
    auto* mimeData = new QMimeData();
    mimeData->setUrls({QUrl::fromLocalFile("/some/model.fbx")});
    QDropEvent event(QPointF(0, 0), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);
    EXPECT_NO_THROW(window->dropEvent(&event));
    delete mimeData;
}
