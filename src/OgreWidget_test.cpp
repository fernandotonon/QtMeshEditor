#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QFocusEvent>
#include <QSignalSpy>
#include <QThread>
#include <QWheelEvent>
#include <exception>

#include "EditorViewport.h"
#include "GlobalDefinitions.h"
#include "Manager.h"
#include "SpaceCamera.h"
#include "mainwindow.h"

// These tests exercise QWidget event handlers and Ogre frame callbacks directly.
#define protected public
#include "OgreWidget.h"
#undef protected

class OgreWidgetTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    MainWindow* mainWindow = nullptr;
    EditorViewport* viewport = nullptr;
    OgreWidget* widget = nullptr;

    void SetUp() override
    {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        app->processEvents();
        Manager::kill();
        QThread::msleep(100);

        constexpr int kMaxMainWindowInitAttempts = 3;
        for (int attempt = 1; attempt <= kMaxMainWindowInitAttempts && !mainWindow; ++attempt) {
            try {
                mainWindow = new MainWindow();
            } catch (const std::exception& e) {
                GTEST_LOG_(WARNING) << "MainWindow init attempt " << attempt
                                    << " failed: " << e.what();
                mainWindow = nullptr;
                app->processEvents();
                QThread::msleep(250);
            } catch (...) {
                GTEST_LOG_(WARNING) << "MainWindow init attempt " << attempt
                                    << " failed with unknown exception";
                mainWindow = nullptr;
                app->processEvents();
                QThread::msleep(250);
            }
        }
        ASSERT_NE(mainWindow, nullptr) << "Failed to initialize MainWindow for OgreWidgetTest";

        viewport = new EditorViewport(mainWindow, 7);
        ASSERT_NE(viewport, nullptr);

        widget = viewport->getOgreWidget();
        ASSERT_NE(widget, nullptr);
    }

    void TearDown() override
    {
        delete viewport;
        viewport = nullptr;
        widget = nullptr;

        delete mainWindow;
        mainWindow = nullptr;

        if (app)
            app->processEvents();

        Manager::kill();
        QThread::msleep(100);
    }
};

TEST_F(OgreWidgetTest, GetIndexMatchesParentViewport)
{
    EXPECT_EQ(widget->getIndex(), 7);
}

TEST_F(OgreWidgetTest, BackgroundColorRoundTripsThroughViewport)
{
    const QColor targetColor(32, 96, 160);

    widget->setBackgroundColor(targetColor);

    EXPECT_EQ(widget->getBackgroundColor(), targetColor);
}

TEST_F(OgreWidgetTest, PaintEngineAndFrameCallbacksReturnExpectedValues)
{
    Ogre::FrameEvent event{};

    EXPECT_EQ(widget->paintEngine(), nullptr);
    EXPECT_TRUE(widget->frameStarted(event));
    EXPECT_TRUE(widget->frameRenderingQueued(event));
    EXPECT_TRUE(widget->frameEnded(event));
}

TEST_F(OgreWidgetTest, ResizeUpdatesCameraAspectRatio)
{
    const QSize oldSize = widget->size();
    const QSize newSize(400, 200);
    QResizeEvent event(newSize, oldSize);
    ASSERT_NE(widget->getSpaceCamera(), nullptr);

    widget->resize(newSize);
    widget->resizeEvent(&event);

    EXPECT_NEAR(widget->getSpaceCamera()->getCamera()->getAspectRatio(), 2.0f, 0.01f);
}

TEST_F(OgreWidgetTest, MousePressAndReleaseUpdateCursorShape)
{
    QMouseEvent middlePress(
        QEvent::MouseButtonPress,
        QPointF(10.0, 10.0),
        Qt::MiddleButton,
        Qt::MiddleButton,
        Qt::NoModifier);
    widget->mousePressEvent(&middlePress);
    EXPECT_TRUE(middlePress.isAccepted());
    EXPECT_EQ(widget->cursor().shape(), Qt::SizeAllCursor);

    QMouseEvent release(
        QEvent::MouseButtonRelease,
        QPointF(10.0, 10.0),
        Qt::MiddleButton,
        Qt::NoButton,
        Qt::NoModifier);
    widget->mouseReleaseEvent(&release);
    EXPECT_EQ(widget->cursor().shape(), Qt::ArrowCursor);

    QMouseEvent leftPress(
        QEvent::MouseButtonPress,
        QPointF(12.0, 12.0),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    widget->mousePressEvent(&leftPress);
    EXPECT_TRUE(leftPress.isAccepted());
    EXPECT_EQ(widget->cursor().shape(), Qt::ClosedHandCursor);
}

TEST_F(OgreWidgetTest, WheelEventDelegatesToCameraZoom)
{
    ASSERT_NE(widget->getSpaceCamera(), nullptr);
    Ogre::Camera* camera = widget->getSpaceCamera()->getCamera();
    ASSERT_NE(camera, nullptr);
    Ogre::SceneNode* cameraNode = camera->getParentSceneNode();
    ASSERT_NE(cameraNode, nullptr);
    const Ogre::Real originalZ = cameraNode->getPosition().z;

    QWheelEvent event(
        QPointF(10.0, 10.0),
        QPointF(10.0, 10.0),
        QPoint(0, 0),
        QPoint(0, 120),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false);

    widget->wheelEvent(&event);

    EXPECT_TRUE(event.isAccepted());
    EXPECT_GT(cameraNode->getPosition().z, originalZ);
}

TEST_F(OgreWidgetTest, FocusEventsEmitSignalAndToggleVisibilityMask)
{
    QSignalSpy spy(widget, &OgreWidget::focusOnWidget);
    ASSERT_TRUE(spy.isValid());

    QFocusEvent focusIn(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
    widget->focusInEvent(&focusIn);
    EXPECT_TRUE(focusIn.isAccepted());
    EXPECT_EQ(spy.count(), 1);
    ASSERT_NE(widget->getViewport(), nullptr);
    EXPECT_EQ(widget->getViewport()->getVisibilityMask(),
              SCENE_VISIBILITY_FLAGS | GUI_VISIBILITY_FLAGS);

    QFocusEvent focusOut(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
    widget->focusOutEvent(&focusOut);
    EXPECT_TRUE(focusOut.isAccepted());
    EXPECT_EQ(widget->getViewport()->getVisibilityMask(), SCENE_VISIBILITY_FLAGS);
}
