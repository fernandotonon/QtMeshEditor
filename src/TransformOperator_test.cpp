#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <Ogre.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "TransformOperator.h"
#include "Manager.h"
#include "GlobalDefinitions.h"
#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>

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
