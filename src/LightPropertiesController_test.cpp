#include <gtest/gtest.h>

#include "LightManager.h"
#include "LightPropertiesController.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "UndoManager.h"
#include "Manager.h"

class LightPropertiesControllerOgreTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        Manager::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        LightManager::getSingleton()->tryConnectToManager();
        UndoManager::getSingleton()->clear();
        LightPropertiesController::instance()->refreshFromSelection();
    }

    void TearDown() override
    {
        UndoManager::kill();
        LightPropertiesController::kill();
        LightManager::kill();
        Manager::kill();
    }

    void selectLight(const QString& name)
    {
        Ogre::SceneNode* node = Manager::getSingleton()->getSceneNode(name);
        ASSERT_NE(node, nullptr);
        SelectionSet::getSingleton()->selectOne(node);
        LightPropertiesController::instance()->refreshFromSelection();
    }
};

TEST_F(LightPropertiesControllerOgreTest, RoundTripAllProperties)
{
    LightHandle spot = LightManager::getSingleton()->createLight(Ogre::Light::LT_SPOTLIGHT,
                                                                 QStringLiteral("PropSpot"));
    ASSERT_TRUE(spot.isValid());
    selectLight(spot.name);

    auto* controller = LightPropertiesController::instance();

    controller->setEnabled(false);
    controller->setIntensity(4.5);
    controller->setDiffuseColor(QColor::fromRgbF(0.2, 0.4, 0.8));
    controller->setColorsLinked(false);
    controller->setSpecularColor(QColor::fromRgbF(0.1, 0.2, 0.3));
    controller->setRange(25.0);
    controller->setAttenuationPreset(1);
    controller->setSpotInnerAngle(20.0);
    controller->setSpotOuterAngle(35.0);
    controller->setSpotFalloff(2.0);
    controller->setLightType(static_cast<int>(Ogre::Light::LT_POINT));

    const LightSnapshot snapshot = LightSnapshot::fromHandle(
        *LightManager::getSingleton()->findLight(spot.name));

    EXPECT_FALSE(snapshot.enabled);
    EXPECT_NEAR(snapshot.powerScale, 4.5f, 1e-3f);
    EXPECT_NEAR(snapshot.diffuse.r, 0.2f, 1e-3f);
    EXPECT_NEAR(snapshot.diffuse.g, 0.4f, 1e-3f);
    EXPECT_NEAR(snapshot.diffuse.b, 0.8f, 1e-3f);
    EXPECT_NEAR(snapshot.specular.r, 0.1f, 1e-3f);
    EXPECT_NEAR(snapshot.attenuationRange, 25.0f, 1e-3f);
    EXPECT_NEAR(snapshot.attenuationConstant, 1.0f, 1e-3f);
    EXPECT_NEAR(snapshot.attenuationQuadratic, 1.0f, 1e-3f);
    EXPECT_EQ(snapshot.type, Ogre::Light::LT_POINT);

    controller->setLightType(static_cast<int>(Ogre::Light::LT_SPOTLIGHT));
    controller->setSpotInnerAngle(15.0);
    controller->setSpotOuterAngle(45.0);
    EXPECT_LE(LightSnapshot::fromHandle(*LightManager::getSingleton()->findLight(spot.name))
                  .spotlightInnerAngleDeg,
              LightSnapshot::fromHandle(*LightManager::getSingleton()->findLight(spot.name))
                  .spotlightOuterAngleDeg);
}

TEST_F(LightPropertiesControllerOgreTest, SliderEditPushesSingleUndoCommand)
{
    LightHandle point = LightManager::getSingleton()->createLight(Ogre::Light::LT_POINT,
                                                                QStringLiteral("SliderPoint"));
    ASSERT_TRUE(point.isValid());
    selectLight(point.name);

    auto* controller = LightPropertiesController::instance();
    controller->beginSliderEdit(static_cast<int>(LightPropertyClass::Intensity));
    controller->setIntensity(2.0);
    controller->setIntensity(5.0);
    controller->endSliderEdit(static_cast<int>(LightPropertyClass::Intensity));

    EXPECT_NEAR(LightManager::getSingleton()->findLight(point.name)->light->getPowerScale(),
                5.0f,
                1e-3f);
    EXPECT_TRUE(UndoManager::getSingleton()->canUndo());

    UndoManager::getSingleton()->undo();
    EXPECT_NEAR(LightManager::getSingleton()->findLight(point.name)->light->getPowerScale(),
                1.0f,
                1e-3f);
}

TEST_F(LightPropertiesControllerOgreTest, MultiEditAppliesToAll)
{
    LightHandle a = LightManager::getSingleton()->createLight(Ogre::Light::LT_POINT,
                                                            QStringLiteral("LightA"));
    LightHandle b = LightManager::getSingleton()->createLight(Ogre::Light::LT_POINT,
                                                            QStringLiteral("LightB"));
    ASSERT_TRUE(a.isValid());
    ASSERT_TRUE(b.isValid());

    LightSnapshot snapA = LightSnapshot::fromHandle(a);
    snapA.powerScale = 1.0f;
    LightManager::getSingleton()->applyProperties(a.name, snapA);

    LightSnapshot snapB = LightSnapshot::fromHandle(b);
    snapB.powerScale = 2.0f;
    LightManager::getSingleton()->applyProperties(b.name, snapB);

    SelectionSet::getSingleton()->clearList();
    SelectionSet::getSingleton()->append(a.sceneNode);
    SelectionSet::getSingleton()->append(b.sceneNode);
    LightPropertiesController::instance()->refreshFromSelection();

    EXPECT_TRUE(LightPropertiesController::instance()->mixedIntensity());
    LightPropertiesController::instance()->setIntensity(3.0);

    EXPECT_NEAR(LightManager::getSingleton()->findLight(a.name)->light->getPowerScale(), 3.0f, 1e-3f);
    EXPECT_NEAR(LightManager::getSingleton()->findLight(b.name)->light->getPowerScale(), 3.0f, 1e-3f);
}
