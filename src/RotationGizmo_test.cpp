#include <gtest/gtest.h>
#include <GlobalDefinitions.h>
#include "RotationGizmo.h"
#include "Manager.h"
#include <OgreException.h>
#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>

// Helper function to create required OGRE materials for tests
static void createOGREMaterials()
{
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
    Ogre::SceneManager* mSceneMgr = nullptr;
    Ogre::SceneNode* mLinkNode = nullptr;
    RotationGizmo* mRotationGizmo = nullptr;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        try {
            Manager::getSingleton();  // headless — no render window needed
        } catch (const Ogre::Exception& e) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
        }
        createOGREMaterials();

        Manager* manager = Manager::getSingleton();
        ASSERT_NE(manager, nullptr);
        mSceneMgr = manager->getSceneMgr();
        ASSERT_NE(mSceneMgr, nullptr);
        mLinkNode = mSceneMgr->createSceneNode();
        ASSERT_NE(mLinkNode, nullptr);

        mRotationGizmo = new RotationGizmo(mLinkNode, "TestRotationGizmo");
        ASSERT_NE(mRotationGizmo, nullptr);
    }

    void TearDown() override {
        if (mRotationGizmo)
        {
            delete mRotationGizmo;
            mRotationGizmo = nullptr;
        }

        if (mLinkNode && mSceneMgr)
        {
            mLinkNode->detachAllObjects();
            mSceneMgr->destroySceneNode(mLinkNode);
            mLinkNode = nullptr;
        }

        Manager::kill();

        if(app)
        {
            app->processEvents();
        }

        QThread::msleep(50);
    }
};

TEST_F(RotationGizmoTests, CreateUnamedGizmo) {
    RotationGizmo unamedGizmo{mLinkNode,""};

    unamedGizmo.createCircles();

    ASSERT_EQ(unamedGizmo.getQueryFlags(), 0);
    ASSERT_EQ(unamedGizmo.getXCircle().getName(), "X");
    ASSERT_EQ(unamedGizmo.getYCircle().getName(), "Y");
    ASSERT_EQ(unamedGizmo.getZCircle().getName(), "Z");
}

TEST_F(RotationGizmoTests, CreateCircles) {
    mRotationGizmo->createCircles();

    ASSERT_NE(&mRotationGizmo->getXCircle(), nullptr);
    ASSERT_NE(&mRotationGizmo->getYCircle(), nullptr);
    ASSERT_NE(&mRotationGizmo->getZCircle(), nullptr);
}

TEST_F(RotationGizmoTests, SetScale) {
    mRotationGizmo->setScale(2.0);
    EXPECT_EQ(mRotationGizmo->getScale(), 2.0);
}

TEST_F(RotationGizmoTests, SetFading) {
    mRotationGizmo->setFading(0.5);
    EXPECT_EQ(mRotationGizmo->getFading(), 0.5);
}

TEST_F(RotationGizmoTests, IsHighlighted) {
    EXPECT_FALSE(mRotationGizmo->isHighlighted());

    mRotationGizmo->highlightCircle(&mRotationGizmo->getXCircle());
    EXPECT_TRUE(mRotationGizmo->isHighlighted());

    mRotationGizmo->highlightCircle(nullptr);
    EXPECT_FALSE(mRotationGizmo->isHighlighted());

    mRotationGizmo->highlightCircle(&mRotationGizmo->getYCircle());
    EXPECT_TRUE(mRotationGizmo->isHighlighted());

    mRotationGizmo->highlightCircle(nullptr);
    EXPECT_FALSE(mRotationGizmo->isHighlighted());

    mRotationGizmo->highlightCircle(&mRotationGizmo->getZCircle());
    EXPECT_TRUE(mRotationGizmo->isHighlighted());
}

TEST_F(RotationGizmoTests, SetVisible) {
    mRotationGizmo->setVisible(true);

    EXPECT_TRUE(mRotationGizmo->getXCircle().isVisible());
    EXPECT_TRUE(mRotationGizmo->getYCircle().isVisible());
    EXPECT_TRUE(mRotationGizmo->getZCircle().isVisible());

    mRotationGizmo->setVisible(false);

    EXPECT_FALSE(mRotationGizmo->getXCircle().isVisible());
    EXPECT_FALSE(mRotationGizmo->getYCircle().isVisible());
    EXPECT_FALSE(mRotationGizmo->getZCircle().isVisible());
}

TEST_F(RotationGizmoTests, SetColour) {
    mRotationGizmo->setXaxisColour(Ogre::ColourValue::Red);
    EXPECT_EQ(mRotationGizmo->getXaxisColour(), Ogre::ColourValue::Red);

    mRotationGizmo->setYaxisColour(Ogre::ColourValue::Green);
    EXPECT_EQ(mRotationGizmo->getYaxisColour(), Ogre::ColourValue::Green);

    mRotationGizmo->setZaxisColour(Ogre::ColourValue::Blue);
    EXPECT_EQ(mRotationGizmo->getZaxisColour(), Ogre::ColourValue::Blue);
}
