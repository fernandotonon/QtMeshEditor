#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <Ogre.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "TransformOperator.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "GlobalDefinitions.h"
#include "TestHelpers.h"

// Helper function to create required OGRE materials for tests
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

// Test fixture for TransformOperator tests that require Manager
class TransformOperatorTestFixture : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override {
        Manager::kill();
        TransformOperator::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        try {
            Manager::getSingleton();  // headless — no render window needed
        } catch (const Ogre::Exception& e) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
        }
        createOGREMaterials();
    }

    void TearDown() override {
        TransformOperator::kill();
        Manager::kill();

        if (app) {
            app->processEvents();
        }
        QThread::msleep(50);
    }
};

// Test if getSingleton returns a valid pointer
TEST_F(TransformOperatorTestFixture, GetSingleton) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NE(instance, nullptr);
}

// Test if getSingleton always returns the same instance
TEST_F(TransformOperatorTestFixture, SingletonInstance) {
    TransformOperator* instance1 = TransformOperator::getSingleton();
    TransformOperator* instance2 = TransformOperator::getSingleton();
    EXPECT_EQ(instance1, instance2);
}

// Test if setTransformState sets the state correctly
TEST_F(TransformOperatorTestFixture, SetSelectionBoxColour) {
    TransformOperator* instance = TransformOperator::getSingleton();
    instance->setSelectionBoxColour(Ogre::ColourValue(0.5, 0.5, 0.5, 1.0));
    EXPECT_EQ(instance->getSelectionBoxColour(), Ogre::ColourValue(0.5, 0.5, 0.5, 1.0));
}

// Swap test doesn't need Manager, so it can be standalone
TEST(TransformOperatorTest, Swap) {
    int x = 1;
    int y = 2;
    TransformOperator::swap(x, y);
    EXPECT_EQ(x, 2);
    EXPECT_EQ(y, 1);
}

TEST_F(TransformOperatorTestFixture, RayFromScreenPoint) {
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::Ray ray = instance->rayFromScreenPoint(QPoint(0, 0));
    EXPECT_EQ(ray.getOrigin(), Ogre::Vector3::ZERO);
    EXPECT_EQ(ray.getDirection(), Ogre::Vector3::UNIT_Z);
}

// ---------- New tests ----------

// Test onTransformStateChange with TS_SELECT: gizmos should be hidden, no crash
TEST_F(TransformOperatorTestFixture, OnTransformStateChange_Select) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_SELECT));
}

// Test onTransformStateChange with TS_TRANSLATE: no crash without selection
TEST_F(TransformOperatorTestFixture, OnTransformStateChange_Translate) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_TRANSLATE));
}

// Test onTransformStateChange with TS_ROTATE: no crash without selection
TEST_F(TransformOperatorTestFixture, OnTransformStateChange_Rotate) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_ROTATE));
}

// Test onTransformStateChange with TS_NONE: default/reset state, no crash
TEST_F(TransformOperatorTestFixture, OnTransformStateChange_None) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_NONE));
}

// Test removeSelected when nothing is selected: should not crash
TEST_F(TransformOperatorTestFixture, RemoveSelectedEmpty) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->removeSelected());
}

// Test setSelectedPosition when nothing is selected: should not crash or modify anything
TEST_F(TransformOperatorTestFixture, SetSelectedPositionNoSelection) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->setSelectedPosition(Ogre::Vector3(10.0f, 20.0f, 30.0f)));
}

// Test translateSelected when nothing is selected: should not crash
TEST_F(TransformOperatorTestFixture, TranslateSelectedNoSelection) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->translateSelected(Ogre::Vector3(5.0f, 5.0f, 5.0f)));
}

// Test setSelectedScale when nothing is selected: should not crash
TEST_F(TransformOperatorTestFixture, SetSelectedScaleNoSelection) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->setSelectedScale(Ogre::Vector3(2.0f, 2.0f, 2.0f)));
}

// Test setSelectedOrientation when nothing is selected: should not crash
TEST_F(TransformOperatorTestFixture, SetSelectedOrientationNoSelection) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->setSelectedOrientation(Ogre::Vector3(45.0f, 90.0f, 0.0f)));
}

// Test scaleSelected when nothing is selected: should not crash
TEST_F(TransformOperatorTestFixture, ScaleSelectedNoSelection) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->scaleSelected(Ogre::Vector3(1.5f, 1.5f, 1.5f)));
}

// Test rotateSelected(Quaternion) when nothing is selected: should not crash
TEST_F(TransformOperatorTestFixture, RotateSelectedNoSelection) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    Ogre::Quaternion rotation(Ogre::Degree(45), Ogre::Vector3::UNIT_Y);
    EXPECT_NO_THROW(instance->rotateSelected(rotation));
}

// Test rotateSelected(Vector3) when nothing is selected: should not crash
TEST_F(TransformOperatorTestFixture, RotateSelectedVectorNoSelection) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->rotateSelected(Ogre::Vector3(15.0f, 30.0f, 45.0f)));
}

// Test setActiveWidget with nullptr: should not crash
TEST_F(TransformOperatorTestFixture, SetActiveWidgetNull) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->setActiveWidget(nullptr));
}

// Test onSelectionChanged when nothing is selected: should reset grid position and hide gizmos
TEST_F(TransformOperatorTestFixture, OnSelectionChangedEmpty) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->onSelectionChanged());
}
