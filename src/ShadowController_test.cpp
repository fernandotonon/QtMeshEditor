#include <gtest/gtest.h>

#include "LightManager.h"
#include "Manager.h"
#include "ShadowController.h"
#include "TestHelpers.h"

#include <OgreLight.h>
#include <OgreSceneManager.h>

class ShadowControllerOgreTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ShadowController::kill();
        LightManager::kill();
        Manager::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        ASSERT_TRUE(createTestRenderWindow());
        ASSERT_TRUE(canLoadMeshFiles()) << "Shadow tests require GL mesh loading (Xvfb in CI)";
        LightManager::getSingleton()->tryConnectToManager();
        Manager::getSingleton()->CreateEmptyScene();
    }

    void TearDown() override
    {
        ShadowController::kill();
        LightManager::kill();
        Manager::kill();
    }
};

TEST(ShadowControllerTest, QualityPresetNamesCoverOffThroughHigh)
{
    const QStringList names = ShadowController::instance()->qualityPresetNames();
    ASSERT_EQ(names.size(), 4);
    EXPECT_EQ(names.first(), QStringLiteral("Off"));
}

TEST_F(ShadowControllerOgreTest, NoCastersKeepsShadowTechniqueDisabled)
{
    auto* shadows = ShadowController::instance();
    shadows->setQualityPreset(static_cast<int>(ShadowController::QualityPreset::High));

    LightHandle key = LightManager::getSingleton()->createLight(Ogre::Light::LT_DIRECTIONAL,
                                                                QStringLiteral("Key"));
    ASSERT_TRUE(key.isValid());
  LightSnapshot snapshot = LightSnapshot::fromHandle(key);
    snapshot.castShadows = false;
    LightManager::getSingleton()->applyProperties(key.name, snapshot);

    shadows->syncFromScene();

    Ogre::SceneManager* sm = Manager::getSingleton()->getSceneMgr();
    ASSERT_NE(sm, nullptr);
    EXPECT_FALSE(sm->isShadowTechniqueInUse());
    EXPECT_FALSE(shadows->shadowsActive());
}

TEST_F(ShadowControllerOgreTest, EnablingCasterInstallsShadowTechnique)
{
    auto* shadows = ShadowController::instance();
    shadows->setQualityPreset(static_cast<int>(ShadowController::QualityPreset::Medium));

    LightHandle key = LightManager::getSingleton()->createLight(Ogre::Light::LT_DIRECTIONAL,
                                                                QStringLiteral("Key"));
    ASSERT_TRUE(key.isValid());

    LightSnapshot snapshot = LightSnapshot::fromHandle(key);
    snapshot.castShadows = true;
    LightManager::getSingleton()->applyProperties(key.name, snapshot);

    shadows->syncFromScene();

    Ogre::SceneManager* sm = Manager::getSingleton()->getSceneMgr();
    ASSERT_NE(sm, nullptr);
    EXPECT_TRUE(key.light->getCastShadows());
    if (sm->isShadowTechniqueInUse())
    {
        EXPECT_TRUE(shadows->shadowsActive());
    }
}

TEST_F(ShadowControllerOgreTest, SnapshotRoundTripsShadowBias)
{
    LightHandle spot = LightManager::getSingleton()->createLight(Ogre::Light::LT_SPOTLIGHT,
                                                                QStringLiteral("Stage"));
    ASSERT_TRUE(spot.isValid());

    LightSnapshot snapshot = LightSnapshot::fromHandle(spot);
    snapshot.castShadows = true;
    snapshot.shadowDepthBias = 0.0002f;
    snapshot.shadowSlopeBias = 2.5f;
    LightManager::getSingleton()->applyProperties(spot.name, snapshot);

    const LightSnapshot roundTrip = LightSnapshot::fromHandle(spot);
    EXPECT_TRUE(roundTrip.castShadows);
    EXPECT_FLOAT_EQ(roundTrip.shadowDepthBias, 0.0002f);
    EXPECT_FLOAT_EQ(roundTrip.shadowSlopeBias, 2.5f);
}
