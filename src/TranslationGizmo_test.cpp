#include <gtest/gtest.h>
#include <GlobalDefinitions.h>
#include "TranslationGizmo.h"
#include "Manager.h"

// Test fixture for TranslationGizmo class
class TranslationGizmoTests : public ::testing::Test {
protected:
    Ogre::SceneManager* mSceneMgr;
    Ogre::SceneNode* mLinkNode;
    TranslationGizmo* mTranslationGizmo;

    void SetUp() override {
        // Set up the scene manager and link node
        mSceneMgr = Manager::getSingleton()->getSceneMgr();
        mLinkNode = mSceneMgr->createSceneNode();

        // Create an instance of RotationGizmo
        mTranslationGizmo = new TranslationGizmo(mLinkNode, "TestRotationGizmo");
    }

    void TearDown() override {
        // Clean up the RotationGizmo and scene manager
        delete mTranslationGizmo;
        delete mLinkNode;
    }
};

TEST_F(TranslationGizmoTests, TestIsLeftHandCS) {
    // Test the default value
    EXPECT_FALSE(mTranslationGizmo->isLeftHandCS());

    // Test after setting to true
    // Assuming there's a method to set this value
    mTranslationGizmo->setLeftHandCS(true);
    EXPECT_TRUE(mTranslationGizmo->isLeftHandCS());

    // Test after setting to false
    mTranslationGizmo->setLeftHandCS(false);
    EXPECT_FALSE(mTranslationGizmo->isLeftHandCS());
}

TEST_F(TranslationGizmoTests, TestIsHighlighted) {
    // By default, the gizmo should not be highlighted
    EXPECT_FALSE(mTranslationGizmo->isHighlighted());

    // Highlight the X Circle
    mTranslationGizmo->highlightAxis(&mTranslationGizmo->getXAxis());
    EXPECT_TRUE(mTranslationGizmo->isHighlighted());

    //Return to default value
    mTranslationGizmo->highlightAxis(nullptr);
    EXPECT_FALSE(mTranslationGizmo->isHighlighted());

    // Highlight the Y Circle
    mTranslationGizmo->highlightAxis(&mTranslationGizmo->getYAxis());
    EXPECT_TRUE(mTranslationGizmo->isHighlighted());

    //Return to default value
    mTranslationGizmo->highlightAxis(nullptr);
    EXPECT_FALSE(mTranslationGizmo->isHighlighted());

    // Highlight the Z Circle
    mTranslationGizmo->highlightAxis(&mTranslationGizmo->getZAxis());
    EXPECT_TRUE(mTranslationGizmo->isHighlighted());
}

// Test case for setVisible() method
TEST_F(TranslationGizmoTests, SetVisible) {
    // Set the gizmo to be visible
    mTranslationGizmo->setVisible(true);

    // Assert that the gizmo is visible
    EXPECT_TRUE(mTranslationGizmo->getXAxis().isVisible());
    EXPECT_TRUE(mTranslationGizmo->getYAxis().isVisible());
    EXPECT_TRUE(mTranslationGizmo->getZAxis().isVisible());

    // Set the gizmo to be invisible
    mTranslationGizmo->setVisible(false);

    // Assert that the gizmo is invisible
    EXPECT_FALSE(mTranslationGizmo->getXAxis().isVisible());
    EXPECT_FALSE(mTranslationGizmo->getYAxis().isVisible());
    EXPECT_FALSE(mTranslationGizmo->getZAxis().isVisible());
}

// Test case for setColour() method
TEST_F(TranslationGizmoTests, SetColour) {
    // Set the X axis colour to red
    mTranslationGizmo->setXaxisColour(Ogre::ColourValue::Red);

    // Assert that the X axis colour is set correctly
    EXPECT_EQ(mTranslationGizmo->getXaxisColour(), Ogre::ColourValue::Red);

    // Set the Y axis colour to green
    mTranslationGizmo->setYaxisColour(Ogre::ColourValue::Green);

    // Assert that the Y axis colour is set correctly
    EXPECT_EQ(mTranslationGizmo->getYaxisColour(), Ogre::ColourValue::Green);

    // Set the Z axis colour to blue
    mTranslationGizmo->setZaxisColour(Ogre::ColourValue::Blue);

    // Assert that the Z axis colour is set correctly
    EXPECT_EQ(mTranslationGizmo->getZaxisColour(), Ogre::ColourValue::Blue);
}

// Test case for unamed gizmo
TEST_F(TranslationGizmoTests, CreateUnamedGizmo) {
    TranslationGizmo unamedGizmo{mLinkNode,""};

    ASSERT_EQ(unamedGizmo.getQueryFlags(), 0);
    ASSERT_EQ(unamedGizmo.getXAxis().getName(), "X");
    ASSERT_EQ(unamedGizmo.getYAxis().getName(), "Y");
    ASSERT_EQ(unamedGizmo.getZAxis().getName(), "Z");
}

// Test case for createCircles() method
TEST_F(TranslationGizmoTests, CreateAxis) {
    // Call the createCircles() method
    mTranslationGizmo->createAxis();

    // Assert that the X, Y, and Z circles are not null
    ASSERT_NE(&mTranslationGizmo->getXAxis(), nullptr);
    ASSERT_NE(&mTranslationGizmo->getYAxis(), nullptr);
    ASSERT_NE(&mTranslationGizmo->getZAxis(), nullptr);
}

// Test case for setScale() method
TEST_F(TranslationGizmoTests, SetScale) {
    // Set the scale to 2.0
    mTranslationGizmo->setScale(2.0);

    // Assert that the scale is set correctly
    EXPECT_EQ(mTranslationGizmo->getScale(), 2.0);
}

// Test case for setFading() method
TEST_F(TranslationGizmoTests, SetFading) {
    // Set the fading to 0.5
    mTranslationGizmo->setFading(0.5);

    // Assert that the fading is set correctly
    EXPECT_EQ(mTranslationGizmo->getFading(), 0.5);
}
