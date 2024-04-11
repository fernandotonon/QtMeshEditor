#include <gtest/gtest.h>
#include "Manager.h"
#include "mainwindow.h"
#include "SkeletonDebug.h"

class SkeletonDebugTests : public ::testing::Test {
protected:
    std::unique_ptr<SkeletonDebug> skeletonDebug;
    Ogre::Entity* entity;
    Ogre::SceneManager* sceneManager;

    void SetUp() override {
        // Create a QApplication instance for testing
        QStringList validUri{"./media/models/robot.mesh"};
        Manager::getSingleton()->getMainWindow()->importMeshs(validUri);
        Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
        Ogre::SceneManager* sceneManager = Manager::getSingleton()->getSceneMgr();
        skeletonDebug = std::make_unique<SkeletonDebug>(entity,sceneManager);
    }

    void TearDown() override {
        delete entity;
    }
};

TEST_F(SkeletonDebugTests, ShowAxesTest)
{
    // Initially, axes should not be shown
    EXPECT_FALSE(skeletonDebug->axesShown());

    // Show axes
    skeletonDebug->showAxes(true);
    EXPECT_TRUE(skeletonDebug->axesShown());

    // Hide axes
    skeletonDebug->showAxes(false);
    EXPECT_FALSE(skeletonDebug->axesShown());
}

TEST_F(SkeletonDebugTests, ShowNamesTest)
{
    // Initially, names should not be shown
    EXPECT_FALSE(skeletonDebug->namesShown());

    // Show names
    skeletonDebug->showNames(true);
    EXPECT_TRUE(skeletonDebug->namesShown());

    // Hide names
    skeletonDebug->showNames(false);
    EXPECT_FALSE(skeletonDebug->namesShown());
}

TEST_F(SkeletonDebugTests, ShowBonesTest)
{
    // Initially, bones should not be shown
    EXPECT_FALSE(skeletonDebug->bonesShown());

    // Show bones
    skeletonDebug->showBones(true);
    EXPECT_TRUE(skeletonDebug->bonesShown());

    // Hide bones
    skeletonDebug->showBones(false);
    EXPECT_FALSE(skeletonDebug->bonesShown());
}

TEST_F(SkeletonDebugTests, SetAndGetAxesScaleTest)
{
    // Set axes scale
    skeletonDebug->setAxesScale(0.5f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 0.5f);

    // Set axes scale to a different value
    skeletonDebug->setAxesScale(1.0f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 1.0f);
}
