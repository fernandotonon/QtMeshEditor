#include <gtest/gtest.h>
#include <memory>
#include <GlobalDefinitions.h>
#include "ScaleGizmo.h"
#include "Manager.h"
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "TestHelpers.h"

static void createOGREMaterials()
{
    ensureMaterialManagerInitialised();
    Ogre::MaterialPtr guiMat = Ogre::MaterialManager::getSingleton().getByName(GUI_MATERIAL_NAME, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!guiMat)
    {
        guiMat = Ogre::MaterialManager::getSingleton().create(GUI_MATERIAL_NAME, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        guiMat->getTechnique(0)->setLightingEnabled(false);
        guiMat->getTechnique(0)->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        guiMat->getTechnique(0)->setDepthCheckEnabled(false);
    }
}

class ScaleGizmoTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    Ogre::SceneManager* mSceneMgr = nullptr;
    Ogre::SceneNode* mLinkNode = nullptr;
    std::unique_ptr<ScaleGizmo> mScaleGizmo;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createOGREMaterials();
        if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

        Manager* manager = Manager::getSingleton();
        ASSERT_NE(manager, nullptr);
        mSceneMgr = manager->getSceneMgr();
        ASSERT_NE(mSceneMgr, nullptr);
        mLinkNode = mSceneMgr->createSceneNode();
        ASSERT_NE(mLinkNode, nullptr);

        mScaleGizmo = std::make_unique<ScaleGizmo>(mLinkNode, "TestScaleGizmo");
    }

    void TearDown() override {
        mScaleGizmo.reset();
        mLinkNode = nullptr;
        if (app) app->processEvents();
    }
};

TEST_F(ScaleGizmoTests, InitialState) {
    EXPECT_FALSE(mScaleGizmo->isHighlighted());
    EXPECT_EQ(mScaleGizmo->getQueryFlags(), 0u);
    EXPECT_FLOAT_EQ(mScaleGizmo->getScale(), 1.0f);
    EXPECT_FLOAT_EQ(mScaleGizmo->getFading(), 0.4f);
}

TEST_F(ScaleGizmoTests, HighlightAxis) {
    EXPECT_FALSE(mScaleGizmo->isHighlighted());

    auto result = mScaleGizmo->highlightAxis(&mScaleGizmo->getXAxis());
    EXPECT_EQ(result, Ogre::Vector3::UNIT_X);
    EXPECT_TRUE(mScaleGizmo->isHighlighted());

    result = mScaleGizmo->highlightAxis(&mScaleGizmo->getYAxis());
    EXPECT_EQ(result, Ogre::Vector3::UNIT_Y);

    result = mScaleGizmo->highlightAxis(&mScaleGizmo->getZAxis());
    EXPECT_EQ(result, Ogre::Vector3::UNIT_Z);

    result = mScaleGizmo->highlightAxis(nullptr);
    EXPECT_EQ(result, Ogre::Vector3::ZERO);
    EXPECT_FALSE(mScaleGizmo->isHighlighted());
}

TEST_F(ScaleGizmoTests, SetVisible) {
    mScaleGizmo->setVisible(true);
    EXPECT_TRUE(mScaleGizmo->getXAxis().isVisible());
    EXPECT_TRUE(mScaleGizmo->getYAxis().isVisible());
    EXPECT_TRUE(mScaleGizmo->getZAxis().isVisible());

    mScaleGizmo->setVisible(false);
    EXPECT_FALSE(mScaleGizmo->getXAxis().isVisible());
    EXPECT_FALSE(mScaleGizmo->getYAxis().isVisible());
    EXPECT_FALSE(mScaleGizmo->getZAxis().isVisible());
}

TEST_F(ScaleGizmoTests, SetScale) {
    mScaleGizmo->setScale(2.0);
    EXPECT_FLOAT_EQ(mScaleGizmo->getScale(), 2.0f);
}

TEST_F(ScaleGizmoTests, SetColour) {
    mScaleGizmo->setXaxisColour(Ogre::ColourValue::Red);
    EXPECT_EQ(mScaleGizmo->getXaxisColour(), Ogre::ColourValue::Red);

    mScaleGizmo->setYaxisColour(Ogre::ColourValue::Green);
    EXPECT_EQ(mScaleGizmo->getYaxisColour(), Ogre::ColourValue::Green);

    mScaleGizmo->setZaxisColour(Ogre::ColourValue::Blue);
    EXPECT_EQ(mScaleGizmo->getZaxisColour(), Ogre::ColourValue::Blue);
}

TEST_F(ScaleGizmoTests, SetQueryFlags) {
    mScaleGizmo->setQueryFlags(GIZMO_QUERY_FLAGS);
    EXPECT_EQ(mScaleGizmo->getQueryFlags(), GIZMO_QUERY_FLAGS);
}

TEST_F(ScaleGizmoTests, CreateAxis) {
    mScaleGizmo->createAxis();
    ASSERT_NE(&mScaleGizmo->getXAxis(), nullptr);
    ASSERT_NE(&mScaleGizmo->getYAxis(), nullptr);
    ASSERT_NE(&mScaleGizmo->getZAxis(), nullptr);
}
