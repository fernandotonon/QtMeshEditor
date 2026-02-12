#include <gtest/gtest.h>
#include <GlobalDefinitions.h>
#include "TranslationGizmo.h"
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

// Test fixture for TranslationGizmo class
class TranslationGizmoTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    Ogre::SceneManager* mSceneMgr = nullptr;
    Ogre::SceneNode* mLinkNode = nullptr;
    TranslationGizmo* mTranslationGizmo = nullptr;

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

        mTranslationGizmo = new TranslationGizmo(mLinkNode, "TestTranslationGizmo");
    }

    void TearDown() override {
        if (mTranslationGizmo)
        {
            delete mTranslationGizmo;
            mTranslationGizmo = nullptr;
        }

        if (mLinkNode && mSceneMgr)
        {
            mLinkNode->detachAllObjects();
            mSceneMgr->destroySceneNode(mLinkNode);
            mLinkNode = nullptr;
        }

        Manager::kill();

        if (app)
        {
            app->processEvents();
        }
        QThread::msleep(50);
    }
};

TEST_F(TranslationGizmoTests, TestIsLeftHandCS) {
    EXPECT_FALSE(mTranslationGizmo->isLeftHandCS());

    mTranslationGizmo->setLeftHandCS(true);
    EXPECT_TRUE(mTranslationGizmo->isLeftHandCS());

    mTranslationGizmo->setLeftHandCS(false);
    EXPECT_FALSE(mTranslationGizmo->isLeftHandCS());
}

TEST_F(TranslationGizmoTests, TestIsHighlighted) {
    EXPECT_FALSE(mTranslationGizmo->isHighlighted());

    mTranslationGizmo->highlightAxis(&mTranslationGizmo->getXAxis());
    EXPECT_TRUE(mTranslationGizmo->isHighlighted());

    mTranslationGizmo->highlightAxis(nullptr);
    EXPECT_FALSE(mTranslationGizmo->isHighlighted());

    mTranslationGizmo->highlightAxis(&mTranslationGizmo->getYAxis());
    EXPECT_TRUE(mTranslationGizmo->isHighlighted());

    mTranslationGizmo->highlightAxis(nullptr);
    EXPECT_FALSE(mTranslationGizmo->isHighlighted());

    mTranslationGizmo->highlightAxis(&mTranslationGizmo->getZAxis());
    EXPECT_TRUE(mTranslationGizmo->isHighlighted());
}

TEST_F(TranslationGizmoTests, SetVisible) {
    mTranslationGizmo->setVisible(true);

    EXPECT_TRUE(mTranslationGizmo->getXAxis().isVisible());
    EXPECT_TRUE(mTranslationGizmo->getYAxis().isVisible());
    EXPECT_TRUE(mTranslationGizmo->getZAxis().isVisible());

    mTranslationGizmo->setVisible(false);

    EXPECT_FALSE(mTranslationGizmo->getXAxis().isVisible());
    EXPECT_FALSE(mTranslationGizmo->getYAxis().isVisible());
    EXPECT_FALSE(mTranslationGizmo->getZAxis().isVisible());
}

TEST_F(TranslationGizmoTests, SetColour) {
    mTranslationGizmo->setXaxisColour(Ogre::ColourValue::Red);
    EXPECT_EQ(mTranslationGizmo->getXaxisColour(), Ogre::ColourValue::Red);

    mTranslationGizmo->setYaxisColour(Ogre::ColourValue::Green);
    EXPECT_EQ(mTranslationGizmo->getYaxisColour(), Ogre::ColourValue::Green);

    mTranslationGizmo->setZaxisColour(Ogre::ColourValue::Blue);
    EXPECT_EQ(mTranslationGizmo->getZaxisColour(), Ogre::ColourValue::Blue);
}

TEST_F(TranslationGizmoTests, CreateUnamedGizmo) {
    TranslationGizmo unamedGizmo{mLinkNode,""};

    unamedGizmo.createAxis();

    ASSERT_EQ(unamedGizmo.getQueryFlags(), 0);
    ASSERT_EQ(unamedGizmo.getXAxis().getName(), "X");
    ASSERT_EQ(unamedGizmo.getYAxis().getName(), "Y");
    ASSERT_EQ(unamedGizmo.getZAxis().getName(), "Z");
}

TEST_F(TranslationGizmoTests, CreateAxis) {
    mTranslationGizmo->createAxis();

    ASSERT_NE(&mTranslationGizmo->getXAxis(), nullptr);
    ASSERT_NE(&mTranslationGizmo->getYAxis(), nullptr);
    ASSERT_NE(&mTranslationGizmo->getZAxis(), nullptr);
}

TEST_F(TranslationGizmoTests, SetScale) {
    mTranslationGizmo->setScale(2.0);
    EXPECT_EQ(mTranslationGizmo->getScale(), 2.0);
}

TEST_F(TranslationGizmoTests, SetFading) {
    mTranslationGizmo->setFading(0.5);
    EXPECT_EQ(mTranslationGizmo->getFading(), 0.5);
}
