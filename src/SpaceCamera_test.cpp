#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "SpaceCamera.h"
#include "TestHelpers.h"
#include "Manager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QThread>

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

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
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
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 0.01f);
}

TEST(SpaceCamera, KeyReleaseControlRestoresSpeed)
{
    MockSpaceCamera spaceCamera;
    spaceCamera.setCameraSpeed(0.5f);

    // Press Control
    QKeyEvent pressCtrl(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
    spaceCamera.keyPressEvent(&pressCtrl);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 0.01f);

    // Release Control
    QKeyEvent releaseCtrl(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
    spaceCamera.keyReleaseEvent(&releaseCtrl);
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 0.1f);
}

TEST(SpaceCamera, MousePressLeftButtonIgnored)
{
    MockSpaceCamera spaceCamera;
    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(100, 100),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    spaceCamera.mousePressEvent(&pressEvent);
    // Left button should be ignored (not accepted)
}

TEST(SpaceCamera, MousePressRightButton)
{
    MockSpaceCamera spaceCamera;
    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(100, 100),
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    spaceCamera.mousePressEvent(&pressEvent);
    // Should store the position for panning
}

TEST(SpaceCamera, MousePressMiddleButton)
{
    MockSpaceCamera spaceCamera;
    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(100, 100),
                           Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    spaceCamera.mousePressEvent(&pressEvent);
    // Should store the position for arc ball
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


TEST(SpaceCamera, KeyPressW)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent pressEvent(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
    spaceCamera.keyPressEvent(&pressEvent);
}

TEST(SpaceCamera, KeyPressReleaseW)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent pressEvent(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
    spaceCamera.keyPressEvent(&pressEvent);
    QKeyEvent releaseEvent(QEvent::KeyRelease, Qt::Key_W, Qt::NoModifier);
    spaceCamera.keyReleaseEvent(&releaseEvent);
}

TEST(SpaceCamera, KeyPressA)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent pressEvent(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
    spaceCamera.keyPressEvent(&pressEvent);
}

TEST(SpaceCamera, KeyPressS)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent pressEvent(QEvent::KeyPress, Qt::Key_S, Qt::NoModifier);
    spaceCamera.keyPressEvent(&pressEvent);
}

TEST(SpaceCamera, KeyPressD)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent pressEvent(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier);
    spaceCamera.keyPressEvent(&pressEvent);
}

TEST(SpaceCamera, KeyPressControlModifier)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent pressEvent(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
    spaceCamera.keyPressEvent(&pressEvent);
    // Control sets precision mode speed to 0.01
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 0.01f);
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
    EXPECT_FLOAT_EQ(spaceCamera.getCameraSpeed(), 0.1f);
}

TEST(SpaceCamera, MultipleKeyPressesInSequence)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent pressW(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
    spaceCamera.keyPressEvent(&pressW);
    QKeyEvent pressA(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
    spaceCamera.keyPressEvent(&pressA);
    QKeyEvent releaseW(QEvent::KeyRelease, Qt::Key_W, Qt::NoModifier);
    spaceCamera.keyReleaseEvent(&releaseW);
    QKeyEvent releaseA(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier);
    spaceCamera.keyReleaseEvent(&releaseA);
}

// These tests need Ogre because mouseMoveEvent calls arcBall/pan which dereference mTarget
TEST_F(SpaceCameraOgreTest, MouseMoveAfterMiddleButtonPress)
{
    MockSpaceCamera spaceCamera;
    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(100, 100),
                          Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    spaceCamera.mousePressEvent(&pressEvent);
    QMouseEvent moveEvent(QEvent::MouseMove, QPointF(150, 120),
                         Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    spaceCamera.mouseMoveEvent(&moveEvent);
}

TEST_F(SpaceCameraOgreTest, MouseMoveAfterRightButtonPress)
{
    MockSpaceCamera spaceCamera;
    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(100, 100),
                          Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    spaceCamera.mousePressEvent(&pressEvent);
    QMouseEvent moveEvent(QEvent::MouseMove, QPointF(120, 130),
                         Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    spaceCamera.mouseMoveEvent(&moveEvent);
}

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
// NEW: Middle button + Shift modifier triggers roll branch
// ==========================================================================

TEST_F(SpaceCameraOgreTest, MouseMoveMiddleButtonWithShift)
{
    MockSpaceCamera spaceCamera;
    // Press middle button with Shift modifier
    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(100, 100),
                          Qt::MiddleButton, Qt::MiddleButton, Qt::ShiftModifier);
    spaceCamera.mousePressEvent(&pressEvent);
    // Move with Shift held — should trigger roll branch instead of arc ball
    QMouseEvent moveEvent(QEvent::MouseMove, QPointF(150, 100),
                         Qt::MiddleButton, Qt::MiddleButton, Qt::ShiftModifier);
    spaceCamera.mouseMoveEvent(&moveEvent);
    // Release
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(150, 100),
                            Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
    spaceCamera.mouseReleaseEvent(&releaseEvent);
    // No crash is the test — roll branch was exercised
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
// NEW: Multiple press/release cycles without crash
// ==========================================================================

TEST_F(SpaceCameraOgreTest, MultipleButtonPressReleaseCycles)
{
    MockSpaceCamera spaceCamera;
    for (int i = 0; i < 5; ++i) {
        // Middle button cycle
        QMouseEvent pressMiddle(QEvent::MouseButtonPress, QPointF(100 + i, 100),
                              Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
        spaceCamera.mousePressEvent(&pressMiddle);
        QMouseEvent moveMiddle(QEvent::MouseMove, QPointF(110 + i, 110),
                             Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
        spaceCamera.mouseMoveEvent(&moveMiddle);
        QMouseEvent releaseMiddle(QEvent::MouseButtonRelease, QPointF(110 + i, 110),
                                Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
        spaceCamera.mouseReleaseEvent(&releaseMiddle);

        // Right button cycle
        QMouseEvent pressRight(QEvent::MouseButtonPress, QPointF(200 + i, 200),
                             Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        spaceCamera.mousePressEvent(&pressRight);
        QMouseEvent moveRight(QEvent::MouseMove, QPointF(210 + i, 210),
                            Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        spaceCamera.mouseMoveEvent(&moveRight);
        QMouseEvent releaseRight(QEvent::MouseButtonRelease, QPointF(210 + i, 210),
                               Qt::RightButton, Qt::NoButton, Qt::NoModifier);
        spaceCamera.mouseReleaseEvent(&releaseRight);
    }
}

// ==========================================================================
// NEW: All direction keys pressed simultaneously
// ==========================================================================

TEST(SpaceCamera, KeyPressAllDirectionKeys)
{
    MockSpaceCamera spaceCamera;
    QKeyEvent pressW(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
    QKeyEvent pressA(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
    QKeyEvent pressS(QEvent::KeyPress, Qt::Key_S, Qt::NoModifier);
    QKeyEvent pressD(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier);

    spaceCamera.keyPressEvent(&pressW);
    spaceCamera.keyPressEvent(&pressA);
    spaceCamera.keyPressEvent(&pressS);
    spaceCamera.keyPressEvent(&pressD);

    // Process a frame
    Ogre::FrameEvent frameEvent;
    frameEvent.timeSinceLastFrame = 0.016f;
    EXPECT_TRUE(spaceCamera.frameStarted(frameEvent));

    QKeyEvent releaseW(QEvent::KeyRelease, Qt::Key_W, Qt::NoModifier);
    QKeyEvent releaseA(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier);
    QKeyEvent releaseS(QEvent::KeyRelease, Qt::Key_S, Qt::NoModifier);
    QKeyEvent releaseD(QEvent::KeyRelease, Qt::Key_D, Qt::NoModifier);

    spaceCamera.keyReleaseEvent(&releaseW);
    spaceCamera.keyReleaseEvent(&releaseA);
    spaceCamera.keyReleaseEvent(&releaseS);
    spaceCamera.keyReleaseEvent(&releaseD);
}

// ==========================================================================
// NEW: Rapid direction changes
// ==========================================================================

TEST(SpaceCamera, RapidDirectionChanges)
{
    MockSpaceCamera spaceCamera;
    Ogre::FrameEvent frameEvent;
    frameEvent.timeSinceLastFrame = 0.016f;

    // Rapidly alternate W and S
    for (int i = 0; i < 10; ++i) {
        QKeyEvent pressW(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
        spaceCamera.keyPressEvent(&pressW);
        spaceCamera.frameStarted(frameEvent);
        QKeyEvent releaseW(QEvent::KeyRelease, Qt::Key_W, Qt::NoModifier);
        spaceCamera.keyReleaseEvent(&releaseW);

        QKeyEvent pressS(QEvent::KeyPress, Qt::Key_S, Qt::NoModifier);
        spaceCamera.keyPressEvent(&pressS);
        spaceCamera.frameStarted(frameEvent);
        QKeyEvent releaseS(QEvent::KeyRelease, Qt::Key_S, Qt::NoModifier);
        spaceCamera.keyReleaseEvent(&releaseS);
    }
}
