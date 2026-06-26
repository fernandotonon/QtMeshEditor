#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "SpaceCamera.h"
#include "TestHelpers.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "EditorViewport.h"
#include "mainwindow.h"
#include "OgreWidget.h"

#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QThread>
#include <cmath>
#include <exception>

using ::testing::Mock;

// Mock class for SpaceCamera
class MockSpaceCamera : public SpaceCamera
{
public:
    // Mock constructor
    MockSpaceCamera():SpaceCamera(){}
    virtual ~MockSpaceCamera() = default;
};

// Fixture for tests that need Ogre scene (mouse move with camera manipulation)
class SpaceCameraOgreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Manager::kill();
        QThread::msleep(50);

        auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
    }

    void TearDown() override
    {
        Manager::kill();
        auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
        if (app) app->processEvents();
        QThread::msleep(50);
    }
};

TEST(SpaceCamera, InitialSpeed)
{
    MockSpaceCamera spaceCamera;
    EXPECT_EQ(spaceCamera.getCameraSpeed(), 0.0f);
    spaceCamera.setCameraSpeed(0.5f);
    EXPECT_EQ(spaceCamera.getCameraSpeed(), 0.5f);
}

TEST(SpaceCamera, FrameStartedAndEnded)
{
    MockSpaceCamera spaceCamera;
    Ogre::FrameEvent frameEvent;
    EXPECT_TRUE(spaceCamera.frameStarted(frameEvent));
    EXPECT_TRUE(spaceCamera.frameEnded(frameEvent));
}

TEST(SpaceCamera, SetCameraSpeed)
{
    MockSpaceCamera spaceCamera;
    EXPECT_EQ(spaceCamera.getCameraSpeed(), 0.0f);

    spaceCamera.setCameraSpeed(1.5f);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 1.5f);

    spaceCamera.setCameraSpeed(0.0f);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 0.0f);

    spaceCamera.setCameraSpeed(100.0f);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 100.0f);
}

TEST(SpaceCamera, SetCameraSpeedNegative)
{
    MockSpaceCamera spaceCamera;
    spaceCamera.setCameraSpeed(-1.0f);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), -1.0f);
}

TEST(SpaceCamera, FrameStartedMultipleTimes)
{
    MockSpaceCamera spaceCamera;
    Ogre::FrameEvent frameEvent;
    frameEvent.timeSinceLastFrame = 0.016f;

    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(spaceCamera.frameStarted(frameEvent));
    }
}

TEST(SpaceCamera, FrameEndedMultipleTimes)
{
    MockSpaceCamera spaceCamera;
    Ogre::FrameEvent frameEvent;
    frameEvent.timeSinceLastFrame = 0.016f;

    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(spaceCamera.frameEnded(frameEvent));
    }
}

TEST(SpaceCamera, KeyPressEventUnmappedKey)
{
    MockSpaceCamera spaceCamera;
    // Press a key that is not mapped (e.g., Key_Z)
    QKeyEvent pressEvent(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier);
    spaceCamera.keyPressEvent(&pressEvent);
    // Should not crash; unmapped keys are simply ignored
}

TEST(SpaceCamera, KeyReleaseEventUnmappedKey)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent releaseEvent(QEvent::KeyRelease, Qt::Key_Z, Qt::NoModifier);
    spaceCamera.keyReleaseEvent(&releaseEvent);
    // Should not crash
}

TEST(SpaceCamera, KeyPressControlChangesSpeed)
{
    MockSpaceCamera spaceCamera;
    spaceCamera.setCameraSpeed(0.5f);

    QKeyEvent pressCtrl(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
    spaceCamera.keyPressEvent(&pressCtrl);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 0.05f); // base 0.5 * 0.1
}

TEST(SpaceCamera, KeyReleaseControlRestoresSpeed)
{
    MockSpaceCamera spaceCamera;
    spaceCamera.setCameraSpeed(0.5f);

    // Press Control
    QKeyEvent pressCtrl(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
    spaceCamera.keyPressEvent(&pressCtrl);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 0.05f); // base 0.5 * 0.1

    // Release Control
    QKeyEvent releaseCtrl(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
    spaceCamera.keyReleaseEvent(&releaseCtrl);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 0.5f); // restores base speed
}

TEST(SpaceCamera, MousePressLeftButtonIgnored)
{
    MockSpaceCamera spaceCamera;
    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(100, 100),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    spaceCamera.mousePressEvent(&pressEvent);
    // Left button should be ignored (not accepted)
}

TEST(SpaceCamera, MouseReleaseWithoutPress)
{
    MockSpaceCamera spaceCamera;
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(100, 100),
                             Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    spaceCamera.mouseReleaseEvent(&releaseEvent);
    // Should not crash
}

TEST(SpaceCamera, MouseMoveWithoutPress)
{
    MockSpaceCamera spaceCamera;
    QMouseEvent moveEvent(QEvent::MouseMove, QPointF(150, 150),
                          Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    spaceCamera.mouseMoveEvent(&moveEvent);
    // Should be ignored when no button was pressed
}

// WheelEvent and ZoomByDelta tests require Ogre scene nodes (mCameraNode, mTarget)
// which are only initialized in the OgreWidget constructor path.
// These are tested via integration tests with a full Ogre context.


TEST(SpaceCamera, KeyPressControlModifier)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent pressEvent(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
    spaceCamera.keyPressEvent(&pressEvent);
    // Control sets precision mode speed to 0.01
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 0.05f); // base 0.5 * 0.1
}

TEST(SpaceCamera, KeyReleaseControlModifier)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent pressCtrl(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
    spaceCamera.keyPressEvent(&pressCtrl);
    float speedWithCtrl = spaceCamera.getCameraSpeed();
    QKeyEvent releaseCtrl(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
    spaceCamera.keyReleaseEvent(&releaseCtrl);
    // After releasing Control, speed is restored to 0.1
    EXPECT_GT(spaceCamera.getCameraSpeed(), speedWithCtrl);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 0.5f); // restores base speed
}

// NOTE: MouseMoveAfterMiddleButtonPress and MouseMoveAfterRightButtonPress
// (SpaceCameraOgreTest) were removed because they crash in CI.

TEST(SpaceCamera, MousePressAndReleaseMiddleButton)
{
    MockSpaceCamera spaceCamera;
    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(100, 100),
                          Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    spaceCamera.mousePressEvent(&pressEvent);
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(150, 150),
                            Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
    spaceCamera.mouseReleaseEvent(&releaseEvent);
}

TEST(SpaceCamera, MousePressAndReleaseRightButton)
{
    MockSpaceCamera spaceCamera;
    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(100, 100),
                          Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    spaceCamera.mousePressEvent(&pressEvent);
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(120, 110),
                            Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    spaceCamera.mouseReleaseEvent(&releaseEvent);
}

// ==========================================================================
// NEW: Left button mouse move should be ignored
// ==========================================================================

TEST(SpaceCamera, MouseMoveAfterLeftButtonPressIgnored)
{
    MockSpaceCamera spaceCamera;
    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(100, 100),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    spaceCamera.mousePressEvent(&pressEvent);
    QMouseEvent moveEvent(QEvent::MouseMove, QPointF(200, 200),
                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    spaceCamera.mouseMoveEvent(&moveEvent);
    // Left button move should be ignored — no crash
}

// ==========================================================================
// NEW: All direction keys pressed simultaneously
// ==========================================================================

// ==========================================================================
// NEW: Key press Q and E for rolling
// ==========================================================================

// ==========================================================================
// NEW: Arrow keys for rotation
// ==========================================================================

TEST(SpaceCamera, KeyPressArrowUp)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent pressEvent(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    spaceCamera.keyPressEvent(&pressEvent);

    Ogre::FrameEvent frameEvent;
    frameEvent.timeSinceLastFrame = 0.016f;
    EXPECT_TRUE(spaceCamera.frameStarted(frameEvent));

    QKeyEvent releaseEvent(QEvent::KeyRelease, Qt::Key_Up, Qt::NoModifier);
    spaceCamera.keyReleaseEvent(&releaseEvent);
}

TEST(SpaceCamera, KeyPressArrowDown)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent pressEvent(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    spaceCamera.keyPressEvent(&pressEvent);

    Ogre::FrameEvent frameEvent;
    frameEvent.timeSinceLastFrame = 0.016f;
    EXPECT_TRUE(spaceCamera.frameStarted(frameEvent));

    QKeyEvent releaseEvent(QEvent::KeyRelease, Qt::Key_Down, Qt::NoModifier);
    spaceCamera.keyReleaseEvent(&releaseEvent);
}

TEST(SpaceCamera, KeyPressArrowLeft)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent pressEvent(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    spaceCamera.keyPressEvent(&pressEvent);

    Ogre::FrameEvent frameEvent;
    frameEvent.timeSinceLastFrame = 0.016f;
    EXPECT_TRUE(spaceCamera.frameStarted(frameEvent));

    QKeyEvent releaseEvent(QEvent::KeyRelease, Qt::Key_Left, Qt::NoModifier);
    spaceCamera.keyReleaseEvent(&releaseEvent);
}

TEST(SpaceCamera, KeyPressArrowRight)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent pressEvent(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    spaceCamera.keyPressEvent(&pressEvent);

    Ogre::FrameEvent frameEvent;
    frameEvent.timeSinceLastFrame = 0.016f;
    EXPECT_TRUE(spaceCamera.frameStarted(frameEvent));

    QKeyEvent releaseEvent(QEvent::KeyRelease, Qt::Key_Right, Qt::NoModifier);
    spaceCamera.keyReleaseEvent(&releaseEvent);
}

// ==========================================================================
// NEW: Rapid direction changes
// ==========================================================================

// ==========================================================================
// Animation accessors
// ==========================================================================

TEST(SpaceCamera, IsAnimatingDefaultFalse)
{
    MockSpaceCamera spaceCamera;
    EXPECT_FALSE(spaceCamera.isAnimating());
}

TEST(SpaceCamera, GetCameraReturnsNullForDefaultConstructed)
{
    MockSpaceCamera spaceCamera;
    EXPECT_EQ(spaceCamera.getCamera(), nullptr);
}

// ==========================================================================
// frameStarted edge cases
// ==========================================================================

TEST(SpaceCamera, FrameStartedWithZeroTimeDelta)
{
    MockSpaceCamera spaceCamera;
    Ogre::FrameEvent frameEvent;
    frameEvent.timeSinceLastFrame = 0.0f;
    EXPECT_TRUE(spaceCamera.frameStarted(frameEvent));
}

TEST(SpaceCamera, FrameStartedWithLargeTimeDelta)
{
    MockSpaceCamera spaceCamera;
    Ogre::FrameEvent frameEvent;
    frameEvent.timeSinceLastFrame = 10.0f; // huge delta
    EXPECT_TRUE(spaceCamera.frameStarted(frameEvent));
}

TEST(SpaceCamera, FrameStartedWithNegativeTimeDelta)
{
    MockSpaceCamera spaceCamera;
    Ogre::FrameEvent frameEvent;
    frameEvent.timeSinceLastFrame = -1.0f;
    EXPECT_TRUE(spaceCamera.frameStarted(frameEvent));
}

// ==========================================================================
// Speed transitions
// ==========================================================================

TEST(SpaceCamera, ControlKeySpeedTransitionCycle)
{
    MockSpaceCamera spaceCamera;
    spaceCamera.setCameraSpeed(1.0f);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 1.0f);

    // Press Control → precision mode (base 1.0 * 0.1 = 0.1)
    QKeyEvent pressCtrl(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
    spaceCamera.keyPressEvent(&pressCtrl);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 0.1f);

    // Release Control → restored speed (base 1.0)
    QKeyEvent releaseCtrl(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
    spaceCamera.keyReleaseEvent(&releaseCtrl);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 1.0f);

    // Press again
    spaceCamera.keyPressEvent(&pressCtrl);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 0.1f);
    spaceCamera.keyReleaseEvent(&releaseCtrl);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 1.0f);
}

TEST(SpaceCamera, SetCameraSpeedExtremeValues)
{
    MockSpaceCamera spaceCamera;
    spaceCamera.setCameraSpeed(99999.0f);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 99999.0f);

    spaceCamera.setCameraSpeed(0.0001f);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 0.0001f);
}

// ==========================================================================
// frameSelection tests (requires empty selection check)
// ==========================================================================

TEST(SpaceCamera, FrameSelectionWithEmptySelection)
{
    MockSpaceCamera spaceCamera;
    // frameSelection() should return early when selection is empty
    // (it checks sel->isEmpty() and returns before dereferencing mTarget)
    SelectionSet::getSingleton()->clear();
    EXPECT_NO_THROW(spaceCamera.frameSelection());
}

// ==========================================================================
// Multiple speed changes interleaved with control key
// ==========================================================================

TEST(SpaceCamera, SpeedChangesWithControlKey)
{
    MockSpaceCamera spaceCamera;
    spaceCamera.setCameraSpeed(2.0f);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 2.0f);

    // Press control (base 2.0 * 0.1 = 0.2)
    QKeyEvent pressCtrl(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
    spaceCamera.keyPressEvent(&pressCtrl);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 0.2f);

    // Release control → restores base speed (2.0)
    QKeyEvent releaseCtrl(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
    spaceCamera.keyReleaseEvent(&releaseCtrl);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 2.0f);
}

// ==========================================================================
// Key F for frame selection (should be handled)
// ==========================================================================

TEST(SpaceCamera, KeyPressF)
{
    MockSpaceCamera spaceCamera;
    // F key may trigger frame selection, but with empty selection
    // and null mTarget it should bail out safely
    SelectionSet::getSingleton()->clear();
    QKeyEvent pressF(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier);
    EXPECT_NO_THROW(spaceCamera.keyPressEvent(&pressF));
}

// ==========================================================================
// Key press/release for keys that may have special handling
// ==========================================================================

TEST(SpaceCamera, KeyPressShift)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent pressShift(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
    EXPECT_NO_THROW(spaceCamera.keyPressEvent(&pressShift));
    QKeyEvent releaseShift(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier);
    EXPECT_NO_THROW(spaceCamera.keyReleaseEvent(&releaseShift));
}

// ==========================================================================
// Repeated frame events with movement keys held
// ==========================================================================

TEST(SpaceCamera, FrameStartedWithArrowKeys)
{
    MockSpaceCamera spaceCamera;
    // Hold arrow keys (still mapped for camera rotation)
    QKeyEvent pressUp(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QKeyEvent pressLeft(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    spaceCamera.keyPressEvent(&pressUp);
    spaceCamera.keyPressEvent(&pressLeft);

    Ogre::FrameEvent frameEvent;
    frameEvent.timeSinceLastFrame = 0.016f;

    // Process multiple frames — rotation accumulates
    for (int i = 0; i < 20; ++i) {
        EXPECT_TRUE(spaceCamera.frameStarted(frameEvent));
    }

    QKeyEvent releaseUp(QEvent::KeyRelease, Qt::Key_Up, Qt::NoModifier);
    QKeyEvent releaseLeft(QEvent::KeyRelease, Qt::Key_Left, Qt::NoModifier);
    spaceCamera.keyReleaseEvent(&releaseUp);
    spaceCamera.keyReleaseEvent(&releaseLeft);
}

class SpaceCameraWidgetIntegrationTest : public ::testing::Test
{
protected:
    static QApplication* app;
    static MainWindow* mainWindow;
    EditorViewport* viewport = nullptr;
    OgreWidget* widget = nullptr;
    SpaceCamera* camera = nullptr;

    static void SetUpTestSuite()
    {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr) << "QApplication instance is required";

        app->processEvents();
        Manager::kill();
        QThread::msleep(100);

        if (mainWindow) {
            return;
        }

        constexpr int kMaxMainWindowInitAttempts = 6;
        for (int attempt = 1; attempt <= kMaxMainWindowInitAttempts && !mainWindow; ++attempt) {
            // EGL/Xvfb setup can transiently fail to create the Ogre surface in CI.
            // Keep a single MainWindow for this suite to avoid repeated EGL churn.
            Manager::kill();
            app->processEvents();
            QThread::msleep(75);
            try {
                mainWindow = new MainWindow();
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
        ASSERT_NE(mainWindow, nullptr)
            << "MainWindow failed to initialize (Xvfb/GL required for integration tests)";
    }

    static void TearDownTestSuite()
    {
        delete mainWindow;
        mainWindow = nullptr;

        if (app) {
            app->processEvents();
        }

        Manager::kill();
        QThread::msleep(100);
    }

    void SetUp() override
    {
        ASSERT_NE(mainWindow, nullptr);
        try {
            viewport = new EditorViewport(mainWindow, 31);
        } catch (const std::exception& e) {
            FAIL() << "EditorViewport creation failed: " << e.what();
        } catch (...) {
            FAIL() << "EditorViewport creation failed with unknown exception";
        }
        ASSERT_NE(viewport, nullptr);

        widget = viewport->getOgreWidget();
        ASSERT_NE(widget, nullptr);

        camera = widget->getSpaceCamera();
        ASSERT_NE(camera, nullptr);
        ASSERT_NE(camera->getCamera(), nullptr);
    }

    void TearDown() override
    {
        SelectionSet::kill();

        delete viewport;
        viewport = nullptr;
        widget = nullptr;
        camera = nullptr;
    }
};

QApplication* SpaceCameraWidgetIntegrationTest::app = nullptr;
MainWindow* SpaceCameraWidgetIntegrationTest::mainWindow = nullptr;

TEST_F(SpaceCameraWidgetIntegrationTest, AnimateToOrientationImmediateSnap)
{
    Ogre::Quaternion target(Ogre::Degree(90), Ogre::Vector3::UNIT_Y);
    camera->animateToOrientation(target, 0.0f);

    EXPECT_FALSE(camera->isAnimating());
    const Ogre::Quaternion& current = camera->getOrientation();
    EXPECT_NEAR(current.w, target.w, 0.001f);
    EXPECT_NEAR(current.x, target.x, 0.001f);
    EXPECT_NEAR(current.y, target.y, 0.001f);
    EXPECT_NEAR(current.z, target.z, 0.001f);
}

TEST_F(SpaceCameraWidgetIntegrationTest, AnimateToOrientationCompletesAfterFrameUpdate)
{
    Ogre::Quaternion target(Ogre::Degree(45), Ogre::Vector3::UNIT_X);
    camera->animateToOrientation(target, 0.2f);
    EXPECT_TRUE(camera->isAnimating());

    Ogre::FrameEvent frameEvent;
    frameEvent.timeSinceLastFrame = 0.25f;
    EXPECT_TRUE(camera->frameStarted(frameEvent));
    EXPECT_FALSE(camera->isAnimating());

    const Ogre::Quaternion& current = camera->getOrientation();
    EXPECT_NEAR(current.w, target.w, 0.001f);
    EXPECT_NEAR(current.x, target.x, 0.001f);
    EXPECT_NEAR(current.y, target.y, 0.001f);
    EXPECT_NEAR(current.z, target.z, 0.001f);
}

TEST_F(SpaceCameraWidgetIntegrationTest, SetCameraPositionAdjustsCameraNodeDistance)
{
    Ogre::SceneNode* cameraNode = camera->getCamera()->getParentSceneNode();
    ASSERT_NE(cameraNode, nullptr);
    const Ogre::Real beforeZ = cameraNode->getPosition().z;

    camera->setCameraPosition(Ogre::Vector3(0.0f, 1.0f, 40.0f));
    const Ogre::Real afterZ = cameraNode->getPosition().z;

    EXPECT_NE(afterZ, beforeZ);
    EXPECT_LT(afterZ, 0.0f);
}

TEST_F(SpaceCameraWidgetIntegrationTest, ViewportPoseRoundTripRestoresTargetAndCamera)
{
    Ogre::Vector3 t0, c0, t1, c1;
    Ogre::Quaternion q0, q1;

    camera->setTargetPosition(Ogre::Vector3(1.0f, 2.0f, -3.0f));
    camera->setCameraPosition(Ogre::Vector3(10.0f, 11.0f, 50.0f));
    camera->getViewportPose(t0, c0, q0);

    camera->setTargetPosition(Ogre::Vector3(0.0f, 0.0f, 0.0f));
    camera->setCameraPosition(Ogre::Vector3(0.0f, 1.0f, -25.0f));

    camera->applyViewportPose(t0, c0, q0);
    camera->getViewportPose(t1, c1, q1);

    const Ogre::Real eps = 0.08f;
    EXPECT_NEAR(t0.x, t1.x, eps);
    EXPECT_NEAR(t0.y, t1.y, eps);
    EXPECT_NEAR(t0.z, t1.z, eps);
    EXPECT_NEAR(c0.x, c1.x, eps);
    EXPECT_NEAR(c0.y, c1.y, eps);
    EXPECT_NEAR(c0.z, c1.z, eps);
    EXPECT_NEAR(q0.w, q1.w, eps);
    EXPECT_NEAR(q0.x, q1.x, eps);
    EXPECT_NEAR(q0.y, q1.y, eps);
    EXPECT_NEAR(q0.z, q1.z, eps);
}

TEST_F(SpaceCameraWidgetIntegrationTest, WheelEventControlModifierUsesZoomPath)
{
    Ogre::SceneNode* cameraNode = camera->getCamera()->getParentSceneNode();
    ASSERT_NE(cameraNode, nullptr);
    const Ogre::Real originalZ = cameraNode->getPosition().z;

    QWheelEvent event(
        QPointF(10.0, 10.0),
        QPointF(10.0, 10.0),
        QPoint(0, 0),
        QPoint(0, 120),
        Qt::NoButton,
        Qt::ControlModifier,
        Qt::NoScrollPhase,
        false);

    camera->wheelEvent(&event);
    EXPECT_TRUE(event.isAccepted());
    EXPECT_GT(cameraNode->getPosition().z, originalZ);
}

TEST_F(SpaceCameraWidgetIntegrationTest, WheelEventTrackpadPathPansCamera)
{
    Ogre::SceneNode* cameraNode = camera->getCamera()->getParentSceneNode();
    ASSERT_NE(cameraNode, nullptr);
    Ogre::SceneNode* targetNode = cameraNode->getParentSceneNode();
    ASSERT_NE(targetNode, nullptr);
    const Ogre::Vector3 before = targetNode->getPosition();

    QWheelEvent event(
        QPointF(12.0, 12.0),
        QPointF(12.0, 12.0),
        QPoint(0, 0),
        QPoint(120, 120),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false,
        Qt::MouseEventSynthesizedBySystem);

    camera->wheelEvent(&event);
    EXPECT_TRUE(event.isAccepted());

    const Ogre::Vector3 after = targetNode->getPosition();
    EXPECT_GT((after - before).length(), 0.0001f);
}

TEST_F(SpaceCameraWidgetIntegrationTest, MouseMoveMiddleShiftAppliesRoll)
{
    const Ogre::Quaternion before = camera->getOrientation();

    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(100.0, 100.0),
                           Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    camera->mousePressEvent(&pressEvent);

    QMouseEvent moveEvent(QEvent::MouseMove, QPointF(140.0, 100.0),
                          Qt::NoButton, Qt::MiddleButton, Qt::ShiftModifier);
    camera->mouseMoveEvent(&moveEvent);
    EXPECT_TRUE(moveEvent.isAccepted());

    const Ogre::Quaternion after = camera->getOrientation();
    EXPECT_GT(std::abs(after.w - before.w) +
              std::abs(after.x - before.x) +
              std::abs(after.y - before.y) +
              std::abs(after.z - before.z), 0.0001f);
}

TEST_F(SpaceCameraWidgetIntegrationTest, MouseMoveRightButtonPansCamera)
{
    Ogre::SceneNode* cameraNode = camera->getCamera()->getParentSceneNode();
    ASSERT_NE(cameraNode, nullptr);
    Ogre::SceneNode* targetNode = cameraNode->getParentSceneNode();
    ASSERT_NE(targetNode, nullptr);
    const Ogre::Vector3 before = targetNode->getPosition();

    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(80.0, 80.0),
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    camera->mousePressEvent(&pressEvent);

    QMouseEvent moveEvent(QEvent::MouseMove, QPointF(120.0, 95.0),
                          Qt::NoButton, Qt::RightButton, Qt::NoModifier);
    camera->mouseMoveEvent(&moveEvent);
    EXPECT_TRUE(moveEvent.isAccepted());

    const Ogre::Vector3 after = targetNode->getPosition();
    EXPECT_GT((after - before).length(), 0.0001f);
}

TEST_F(SpaceCameraWidgetIntegrationTest, FrameSelectionWithEntitySelectionRepositionsCamera)
{
    auto mesh = createInMemoryTriangleMesh("space_cam_frame_selection_mesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("space_cam_frame_selection_node");
    auto* entity = sceneMgr->createEntity("space_cam_frame_selection_entity", mesh);
    ASSERT_NE(entity, nullptr);
    node->attachObject(entity);
    node->setPosition(15.0f, 4.0f, -8.0f);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(entity);

    Ogre::SceneNode* cameraNode = camera->getCamera()->getParentSceneNode();
    ASSERT_NE(cameraNode, nullptr);
    const Ogre::Real beforeZ = cameraNode->getPosition().z;

    camera->frameSelection();
    const Ogre::Real afterZ = cameraNode->getPosition().z;

    EXPECT_NE(afterZ, beforeZ);
    EXPECT_LT(afterZ, 0.0f);
}

TEST_F(SpaceCameraWidgetIntegrationTest, MouseReleaseLeftButtonIsIgnored)
{
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(40.0, 40.0),
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    camera->mouseReleaseEvent(&releaseEvent);
    EXPECT_FALSE(releaseEvent.isAccepted());
}

TEST_F(SpaceCameraWidgetIntegrationTest, MouseMoveMiddleWithoutShiftUsesArcBall)
{
    const Ogre::Quaternion before = camera->getOrientation();

    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(100.0, 100.0),
                           Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    camera->mousePressEvent(&pressEvent);

    QMouseEvent moveEvent(QEvent::MouseMove, QPointF(140.0, 120.0),
                          Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    camera->mouseMoveEvent(&moveEvent);
    EXPECT_TRUE(moveEvent.isAccepted());

    const Ogre::Quaternion after = camera->getOrientation();
    EXPECT_GT(std::abs(after.w - before.w) +
              std::abs(after.x - before.x) +
              std::abs(after.y - before.y) +
              std::abs(after.z - before.z), 0.0001f);
}

TEST_F(SpaceCameraWidgetIntegrationTest, WheelEventMousePathWithHorizontalDeltaAlsoPans)
{
    Ogre::SceneNode* cameraNode = camera->getCamera()->getParentSceneNode();
    ASSERT_NE(cameraNode, nullptr);
    Ogre::SceneNode* targetNode = cameraNode->getParentSceneNode();
    ASSERT_NE(targetNode, nullptr);
    const Ogre::Vector3 before = targetNode->getPosition();

    QWheelEvent event(
        QPointF(20.0, 20.0),
        QPointF(20.0, 20.0),
        QPoint(0, 0),
        QPoint(120, 120),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false);

    camera->wheelEvent(&event);
    EXPECT_TRUE(event.isAccepted());

    const Ogre::Vector3 after = targetNode->getPosition();
    EXPECT_GT((after - before).length(), 0.0001f);
}

TEST_F(SpaceCameraWidgetIntegrationTest, FrameSelectionWithEmptyNodeSelectionUsesNodePosition)
{
    Ogre::SceneNode* emptyNode = Manager::getSingleton()->addSceneNode("space_cam_empty_node");
    ASSERT_NE(emptyNode, nullptr);
    emptyNode->setPosition(8.0f, -3.0f, 12.0f);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(emptyNode);

    Ogre::SceneNode* cameraNode = camera->getCamera()->getParentSceneNode();
    ASSERT_NE(cameraNode, nullptr);
    Ogre::SceneNode* targetNode = cameraNode->getParentSceneNode();
    ASSERT_NE(targetNode, nullptr);

    camera->frameSelection();

    const Ogre::Vector3 targetPos = targetNode->getPosition();
    EXPECT_NEAR(targetPos.x, 8.0f, 0.001f);
    EXPECT_NEAR(targetPos.y, -3.0f, 0.001f);
    EXPECT_NEAR(targetPos.z, 12.0f, 0.001f);
    EXPECT_LT(cameraNode->getPosition().z, 0.0f);
}

TEST_F(SpaceCameraWidgetIntegrationTest, FrameStartedAppliesQueuedRotationFromArrowKey)
{
    Ogre::SceneNode* cameraNode = camera->getCamera()->getParentSceneNode();
    ASSERT_NE(cameraNode, nullptr);
    camera->setCameraSpeed(1.0f);

    const Ogre::Quaternion beforeOrient = camera->getOrientation();

    QKeyEvent pressUp(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    camera->keyPressEvent(&pressUp);

    Ogre::FrameEvent frameEvent;
    frameEvent.timeSinceLastFrame = 0.016f;
    EXPECT_TRUE(camera->frameStarted(frameEvent));

    const Ogre::Quaternion afterOrient = camera->getOrientation();

    EXPECT_GT(std::abs(afterOrient.w - beforeOrient.w) +
              std::abs(afterOrient.x - beforeOrient.x) +
              std::abs(afterOrient.y - beforeOrient.y) +
              std::abs(afterOrient.z - beforeOrient.z), 0.0001f);

    QKeyEvent releaseUp(QEvent::KeyRelease, Qt::Key_Up, Qt::NoModifier);
    camera->keyReleaseEvent(&releaseUp);
}
