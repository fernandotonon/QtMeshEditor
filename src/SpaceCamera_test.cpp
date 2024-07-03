#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "SpaceCamera.h"

#include <QApplication>

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

