#include <gtest/gtest.h>
#include <GlobalDefinitions.h>
#include "RotationGizmo.h"
#include "Manager.h"

// Test fixture for RotationGizmo class
class RotationGizmoTests : public ::testing::Test {
protected:
    Ogre::SceneManager* mSceneMgr;
    Ogre::SceneNode* mLinkNode;
    RotationGizmo* mRotationGizmo;

    void SetUp() override {
        // Set up the scene manager and link node
        mSceneMgr = Manager::getSingleton()->getSceneMgr();
        mLinkNode = mSceneMgr->createSceneNode();

        // Create an instance of RotationGizmo
        mRotationGizmo = new RotationGizmo(mLinkNode, "TestRotationGizmo");
    }

    void TearDown() override {
        // Clean up the RotationGizmo and scene manager
        delete mRotationGizmo;
        delete mLinkNode;
    }
};

// Test case for unamed gizmo
TEST_F(RotationGizmoTests, CreateUnamedGizmo) {
    RotationGizmo unamedGizmo{mLinkNode,""};

    ASSERT_EQ(unamedGizmo.getQueryFlags(), 0);
    ASSERT_EQ(unamedGizmo.getXCircle().getName(), "X");
    ASSERT_EQ(unamedGizmo.getYCircle().getName(), "Y");
    ASSERT_EQ(unamedGizmo.getZCircle().getName(), "Z");
}

// Test case for createCircles() method
TEST_F(RotationGizmoTests, CreateCircles) {
    // Call the createCircles() method
    mRotationGizmo->createCircles();

    // Assert that the X, Y, and Z circles are not null
    ASSERT_NE(&mRotationGizmo->getXCircle(), nullptr);
    ASSERT_NE(&mRotationGizmo->getYCircle(), nullptr);
    ASSERT_NE(&mRotationGizmo->getZCircle(), nullptr);
}

// Test case for setScale() method
TEST_F(RotationGizmoTests, SetScale) {
    // Set the scale to 2.0
    mRotationGizmo->setScale(2.0);

    // Assert that the scale is set correctly
    EXPECT_EQ(mRotationGizmo->getScale(), 2.0);
}

// Test case for setFading() method
TEST_F(RotationGizmoTests, SetFading) {
    // Set the fading to 0.5
    mRotationGizmo->setFading(0.5);

    // Assert that the fading is set correctly
    EXPECT_EQ(mRotationGizmo->getFading(), 0.5);
}

// Test case for isHighlighted() method
TEST_F(RotationGizmoTests, IsHighlighted) {
    // By default, the gizmo should not be highlighted
    EXPECT_FALSE(mRotationGizmo->isHighlighted());

    // Highlight the X Circle
    mRotationGizmo->highlightCircle(&mRotationGizmo->getXCircle());
    EXPECT_TRUE(mRotationGizmo->isHighlighted());

    //Return to default value
    mRotationGizmo->highlightCircle(nullptr);
    EXPECT_FALSE(mRotationGizmo->isHighlighted());

    // Highlight the Y Circle
    mRotationGizmo->highlightCircle(&mRotationGizmo->getYCircle());
    EXPECT_TRUE(mRotationGizmo->isHighlighted());

    //Return to default value
    mRotationGizmo->highlightCircle(nullptr);
    EXPECT_FALSE(mRotationGizmo->isHighlighted());

    // Highlight the Z Circle
    mRotationGizmo->highlightCircle(&mRotationGizmo->getZCircle());
    EXPECT_TRUE(mRotationGizmo->isHighlighted());
}

// Test case for setVisible() method
TEST_F(RotationGizmoTests, SetVisible) {
    // Set the gizmo to be visible
    mRotationGizmo->setVisible(true);

    // Assert that the gizmo is visible
    EXPECT_TRUE(mRotationGizmo->getXCircle().isVisible());
    EXPECT_TRUE(mRotationGizmo->getYCircle().isVisible());
    EXPECT_TRUE(mRotationGizmo->getZCircle().isVisible());

    // Set the gizmo to be invisible
    mRotationGizmo->setVisible(false);

    // Assert that the gizmo is invisible
    EXPECT_FALSE(mRotationGizmo->getXCircle().isVisible());
    EXPECT_FALSE(mRotationGizmo->getYCircle().isVisible());
    EXPECT_FALSE(mRotationGizmo->getZCircle().isVisible());
}

// Test case for setColour() method
TEST_F(RotationGizmoTests, SetColour) {
    // Set the X axis colour to red
    mRotationGizmo->setXaxisColour(Ogre::ColourValue::Red);

    // Assert that the X axis colour is set correctly
    EXPECT_EQ(mRotationGizmo->getXaxisColour(), Ogre::ColourValue::Red);

    // Set the Y axis colour to green
    mRotationGizmo->setYaxisColour(Ogre::ColourValue::Green);

    // Assert that the Y axis colour is set correctly
    EXPECT_EQ(mRotationGizmo->getYaxisColour(), Ogre::ColourValue::Green);

    // Set the Z axis colour to blue
    mRotationGizmo->setZaxisColour(Ogre::ColourValue::Blue);

    // Assert that the Z axis colour is set correctly
    EXPECT_EQ(mRotationGizmo->getZaxisColour(), Ogre::ColourValue::Blue);
}
