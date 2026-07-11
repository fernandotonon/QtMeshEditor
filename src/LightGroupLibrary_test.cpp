#include <gtest/gtest.h>

#include "LightGroupLibrary.h"
#include "LightLinking.h"
#include "LightManager.h"
#include "Manager.h"
#include "SceneLightsIO.h"
#include "TestHelpers.h"

#include <OgreSceneNode.h>

class LightGroupLibraryOgreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Manager::kill();
        ASSERT_TRUE(tryInitOgre());
        createStandardOgreMaterials();
        LightManager::getSingleton()->tryConnectToManager();
        LightLinking::clearAllRules();
    }

    void TearDown() override
    {
        LightLinking::clearAllRules();
        LightManager::kill();
        Manager::kill();
    }
};

TEST_F(LightGroupLibraryOgreTest, CreateAndDissolveGroup)
{
    const LightHandle a =
        LightManager::getSingleton()->createLight(Ogre::Light::LT_POINT, QStringLiteral("A"));
    const LightHandle b =
        LightManager::getSingleton()->createLight(Ogre::Light::LT_POINT, QStringLiteral("B"));
    ASSERT_TRUE(a.isValid());
    ASSERT_TRUE(b.isValid());

    const LightGroupCreateResult result =
        LightGroupLibrary::createGroup(QStringLiteral("HeroRig"), {a.name, b.name});
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.movedLightNames.size(), 2);

    auto* mgr = Manager::getSingletonPtr();
    Ogre::SceneNode* groupNode = mgr->getSceneNode(result.groupNodeName);
    ASSERT_NE(groupNode, nullptr);
    EXPECT_TRUE(LightGroupLibrary::sceneNodeIsLightGroup(groupNode));
    EXPECT_EQ(groupNode->numChildren(), 2u);

    EXPECT_TRUE(LightGroupLibrary::dissolveGroup(result.groupNodeName));
    EXPECT_FALSE(mgr->hasSceneNode(result.groupNodeName));
}

TEST_F(LightGroupLibraryOgreTest, CollectionRoundTripsInSceneJson)
{
    const LightHandle light =
        LightManager::getSingleton()->createLight(Ogre::Light::LT_POINT, QStringLiteral("Key"));
    ASSERT_TRUE(light.isValid());

    LightGroupLibrary::createGroup(QStringLiteral("Set"), {light.name});

    const SceneLightsIO::SceneLightsDocument captured = SceneLightsIO::captureFromScene();
    ASSERT_EQ(captured.rigGroups.size(), 1);
    EXPECT_TRUE(captured.rigGroups.first().isUserCollection);
    EXPECT_EQ(captured.rigGroups.first().lights.size(), 1);

    const QByteArray json = SceneLightsIO::documentToJson(captured);
    SceneLightsIO::SceneLightsDocument restored;
    ASSERT_TRUE(SceneLightsIO::documentFromJson(json, restored));
    ASSERT_EQ(restored.rigGroups.size(), 1);
    EXPECT_TRUE(restored.rigGroups.first().isUserCollection);
}
