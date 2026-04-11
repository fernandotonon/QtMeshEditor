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
    EXPECT_EQ(mScaleGizmo->getXAxis().getQueryFlags(), GIZMO_QUERY_FLAGS);
    EXPECT_EQ(mScaleGizmo->getYAxis().getQueryFlags(), GIZMO_QUERY_FLAGS);
    EXPECT_EQ(mScaleGizmo->getZAxis().getQueryFlags(), GIZMO_QUERY_FLAGS);
}

TEST_F(ScaleGizmoTests, CreateAxis) {
    mScaleGizmo->createAxis();
    ASSERT_NE(&mScaleGizmo->getXAxis(), nullptr);
    ASSERT_NE(&mScaleGizmo->getYAxis(), nullptr);
    ASSERT_NE(&mScaleGizmo->getZAxis(), nullptr);
}

TEST_F(ScaleGizmoTests, SetFading) {
    mScaleGizmo->setFading(0.8f);
    EXPECT_FLOAT_EQ(mScaleGizmo->getFading(), 0.8f);

    mScaleGizmo->setFading(0.0f);
    EXPECT_FLOAT_EQ(mScaleGizmo->getFading(), 0.0f);

    mScaleGizmo->setFading(1.0f);
    EXPECT_FLOAT_EQ(mScaleGizmo->getFading(), 1.0f);
}

TEST_F(ScaleGizmoTests, DefaultColorsAreRGB) {
    // X axis should default to red
    EXPECT_FLOAT_EQ(mScaleGizmo->getXaxisColour().r, 1.0f);
    EXPECT_FLOAT_EQ(mScaleGizmo->getXaxisColour().g, 0.0f);
    EXPECT_FLOAT_EQ(mScaleGizmo->getXaxisColour().b, 0.0f);

    // Y axis should default to green
    EXPECT_FLOAT_EQ(mScaleGizmo->getYaxisColour().r, 0.0f);
    EXPECT_FLOAT_EQ(mScaleGizmo->getYaxisColour().g, 1.0f);
    EXPECT_FLOAT_EQ(mScaleGizmo->getYaxisColour().b, 0.0f);

    // Z axis should default to blue
    EXPECT_FLOAT_EQ(mScaleGizmo->getZaxisColour().r, 0.0f);
    EXPECT_FLOAT_EQ(mScaleGizmo->getZaxisColour().g, 0.0f);
    EXPECT_FLOAT_EQ(mScaleGizmo->getZaxisColour().b, 1.0f);
}

TEST_F(ScaleGizmoTests, UnnamedGizmoCreation) {
    // Create a gizmo with empty name
    auto emptyNameGizmo = std::make_unique<ScaleGizmo>(mLinkNode, "");
    EXPECT_FALSE(emptyNameGizmo->isHighlighted());
    EXPECT_FLOAT_EQ(emptyNameGizmo->getScale(), 1.0f);
}

TEST_F(ScaleGizmoTests, HighlightNullReturnsZero) {
    auto result = mScaleGizmo->highlightAxis(nullptr);
    EXPECT_EQ(result, Ogre::Vector3::ZERO);
    EXPECT_FALSE(mScaleGizmo->isHighlighted());
}

TEST_F(ScaleGizmoTests, HighlightUnknownObjectReturnsZeroAndClearsHighlight) {
    mScaleGizmo->highlightAxis(&mScaleGizmo->getXAxis());
    EXPECT_TRUE(mScaleGizmo->isHighlighted());

    Ogre::ManualObject* unrelated = mSceneMgr->createManualObject("ScaleGizmoUnrelated");
    ASSERT_NE(unrelated, nullptr);
    auto result = mScaleGizmo->highlightAxis(unrelated);
    EXPECT_EQ(result, Ogre::Vector3::ZERO);
    EXPECT_FALSE(mScaleGizmo->isHighlighted());
    mSceneMgr->destroyManualObject(unrelated);
}

TEST_F(ScaleGizmoTests, SetScaleMultipleTimes) {
    mScaleGizmo->setScale(0.5f);
    EXPECT_FLOAT_EQ(mScaleGizmo->getScale(), 0.5f);

    mScaleGizmo->setScale(3.0f);
    EXPECT_FLOAT_EQ(mScaleGizmo->getScale(), 3.0f);

    mScaleGizmo->setScale(0.1f);
    EXPECT_FLOAT_EQ(mScaleGizmo->getScale(), 0.1f);
}

TEST_F(ScaleGizmoTests, SetCustomColours) {
    Ogre::ColourValue cyan(0.0f, 1.0f, 1.0f);
    Ogre::ColourValue magenta(1.0f, 0.0f, 1.0f);
    Ogre::ColourValue yellow(1.0f, 1.0f, 0.0f);

    mScaleGizmo->setXaxisColour(cyan);
    mScaleGizmo->setYaxisColour(magenta);
    mScaleGizmo->setZaxisColour(yellow);

    EXPECT_EQ(mScaleGizmo->getXaxisColour(), cyan);
    EXPECT_EQ(mScaleGizmo->getYaxisColour(), magenta);
    EXPECT_EQ(mScaleGizmo->getZaxisColour(), yellow);
}

TEST_F(ScaleGizmoTests, VisibilityToggle) {
    mScaleGizmo->setVisible(true);
    EXPECT_TRUE(mScaleGizmo->getXAxis().isVisible());

    mScaleGizmo->setVisible(false);
    EXPECT_FALSE(mScaleGizmo->getXAxis().isVisible());

    mScaleGizmo->setVisible(true);
    EXPECT_TRUE(mScaleGizmo->getXAxis().isVisible());
}

TEST_F(ScaleGizmoTests, HighlightEachAxisSequentially) {
    // Highlight X then Y then Z then clear
    auto r1 = mScaleGizmo->highlightAxis(&mScaleGizmo->getXAxis());
    EXPECT_EQ(r1, Ogre::Vector3::UNIT_X);
    EXPECT_TRUE(mScaleGizmo->isHighlighted());

    auto r2 = mScaleGizmo->highlightAxis(&mScaleGizmo->getYAxis());
    EXPECT_EQ(r2, Ogre::Vector3::UNIT_Y);
    EXPECT_TRUE(mScaleGizmo->isHighlighted());

    auto r3 = mScaleGizmo->highlightAxis(&mScaleGizmo->getZAxis());
    EXPECT_EQ(r3, Ogre::Vector3::UNIT_Z);
    EXPECT_TRUE(mScaleGizmo->isHighlighted());

    auto r4 = mScaleGizmo->highlightAxis(nullptr);
    EXPECT_EQ(r4, Ogre::Vector3::ZERO);
    EXPECT_FALSE(mScaleGizmo->isHighlighted());
}

TEST_F(ScaleGizmoTests, ConstructWithCustomScale) {
    auto customGizmo = std::make_unique<ScaleGizmo>(mLinkNode, "CustomScaleGizmo", 2.5f);
    EXPECT_FLOAT_EQ(customGizmo->getScale(), 2.5f);
}
