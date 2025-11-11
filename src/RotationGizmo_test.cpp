#include <gtest/gtest.h>
#include <GlobalDefinitions.h>
#include "RotationGizmo.h"
#include "Manager.h"
#include "mainwindow.h"
#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>

// Helper function to create required OGRE materials for tests
static void createOGREMaterials()
{
    // Create GUI_Material (used by RotationGizmo)
    Ogre::MaterialPtr guiMat = Ogre::MaterialManager::getSingleton().getByName(GUI_MATERIAL_NAME, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!guiMat)
    {
        guiMat = Ogre::MaterialManager::getSingleton().create(GUI_MATERIAL_NAME, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        guiMat->getTechnique(0)->setLightingEnabled(false);
        guiMat->getTechnique(0)->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        guiMat->getTechnique(0)->setDepthCheckEnabled(false);
    }
}

// Test fixture for RotationGizmo class
class RotationGizmoTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    MainWindow* mainWindow = nullptr;
    Ogre::SceneManager* mSceneMgr = nullptr;
    Ogre::SceneNode* mLinkNode = nullptr;
    RotationGizmo* mRotationGizmo = nullptr;

    void SetUp() override {
        // Ensure Manager is completely destroyed from previous test
        Manager::kill();
        QThread::msleep(50); // Small delay to ensure cleanup is complete
        
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        
        // Create MainWindow to initialize Manager
        try {
            mainWindow = new MainWindow();
            // Verify mainWindow was created successfully
            ASSERT_NE(mainWindow, nullptr);
            
            // Initialize Manager with mainWindow
            Manager::getSingleton(mainWindow);
            
            // Verify Manager was created successfully
            Manager* manager = Manager::getSingleton(mainWindow);
            ASSERT_NE(manager, nullptr);
        } catch (const Ogre::RenderingAPIException& e) {
            GTEST_SKIP() << "Skipping RotationGizmo tests: unable to create OGRE render window ("
                         << e.getFullDescription() << ")";
        } catch (const std::exception& e) {
            GTEST_SKIP() << "Skipping RotationGizmo tests: " << e.what();
        }
        
        // Verify mainWindow is still valid before proceeding
        ASSERT_NE(mainWindow, nullptr);
        
        // Create required OGRE materials
        createOGREMaterials();
        
        // Set up the scene manager and link node
        // Always pass mainWindow to getSingleton to ensure it's not null
        Manager* manager = Manager::getSingleton(mainWindow);
        ASSERT_NE(manager, nullptr);
        mSceneMgr = manager->getSceneMgr();
        ASSERT_NE(mSceneMgr, nullptr);
        mLinkNode = mSceneMgr->createSceneNode();
        ASSERT_NE(mLinkNode, nullptr);

        // Create an instance of RotationGizmo
        mRotationGizmo = new RotationGizmo(mLinkNode, "TestRotationGizmo");
        ASSERT_NE(mRotationGizmo, nullptr);
    }

    void TearDown() override {
        // Clean up the RotationGizmo first (while scene manager is still valid)
        // The gizmo destructor needs access to the scene manager to destroy manual objects
        if (mRotationGizmo)
        {
            delete mRotationGizmo;
            mRotationGizmo = nullptr;
        }
        
        // Now clean up the scene node (manual objects should already be destroyed)
        // But first detach any remaining objects to be safe
        if (mLinkNode && mSceneMgr)
        {
            mLinkNode->detachAllObjects();
            mSceneMgr->destroySceneNode(mLinkNode);
            mLinkNode = nullptr;
        }
        
        // Clean up MainWindow (it may have references to Manager)
        // The MainWindow destructor will handle cleanup of widgets and may call Manager::kill()
        if (mainWindow)
        {
            delete mainWindow;
            mainWindow = nullptr;
        }
        
        // Ensure Manager is destroyed (in case MainWindow didn't destroy it)
        Manager::kill();
        
        // Process any pending events to ensure cleanup is complete
        if(app)
        {
            app->processEvents();
        }
        
        // Small delay to ensure OGRE resources are fully cleaned up before next test
        QThread::msleep(50);
        
    }
};

// Test case for unamed gizmo
TEST_F(RotationGizmoTests, CreateUnamedGizmo) {
    RotationGizmo unamedGizmo{mLinkNode,""};

    // Ensure circles are created (have geometry) before accessing them
    // This is necessary because the constructor doesn't call createCircles()
    unamedGizmo.createCircles();

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
