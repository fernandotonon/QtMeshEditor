#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "SpaceCamera.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

using ::testing::Mock;

// Mock class for SpaceCamera
class MockSpaceCamera : public SpaceCamera
{
public:
    // Mock constructor
    MockSpaceCamera():SpaceCamera(){}
    virtual ~MockSpaceCamera() = default;
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
