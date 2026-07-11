#include <gtest/gtest.h>

#include "LightLinking.h"
#include "LightManager.h"
#include "Manager.h"
#include "SceneLightsIO.h"
#include "TestHelpers.h"

#include <OgreEntity.h>
#include <OgreLight.h>
#include <OgreMeshManager.h>
#include <OgreSceneManager.h>

class LightLinkingOgreTest : public ::testing::Test
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

    Ogre::Entity* createTestEntity(const QString& nodeName)
    {
        auto* mgr = Manager::getSingletonPtr();
        Ogre::SceneNode* node = mgr->addSceneNode(nodeName);
        Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual(
            (nodeName + QStringLiteral("_mesh")).toStdString(),
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        return mgr->createEntity(node, mesh);
    }
};

TEST_F(LightLinkingOgreTest, IncludeOnlyAffectsListedEntity)
{
    Ogre::Entity* hero = createTestEntity(QStringLiteral("Hero"));
    Ogre::Entity* floor = createTestEntity(QStringLiteral("Floor"));

    LightHandle light =
        LightManager::getSingleton()->createLight(Ogre::Light::LT_POINT, QStringLiteral("Key"));
    ASSERT_TRUE(light.isValid());

    LightSnapshot snapshot = LightSnapshot::fromHandle(light);
    snapshot.linkMode = LightLinking::Mode::Include;
    snapshot.linkedEntityNames = {QStringLiteral("Hero")};
    LightManager::getSingleton()->applyProperties(light.name, snapshot);

    snapshot = LightSnapshot::fromHandle(light);
    EXPECT_EQ(snapshot.linkMode, LightLinking::Mode::Include);
    EXPECT_NE(snapshot.linkChannelBit, 0u);
    EXPECT_NE(light.light->getLightMask() & hero->getLightMask(), 0u);
    EXPECT_EQ(light.light->getLightMask() & floor->getLightMask(), 0u);
}

TEST_F(LightLinkingOgreTest, ExcludeSkipsListedEntity)
{
    Ogre::Entity* hero = createTestEntity(QStringLiteral("Hero"));
    Ogre::Entity* floor = createTestEntity(QStringLiteral("Floor"));

    LightHandle light =
        LightManager::getSingleton()->createLight(Ogre::Light::LT_POINT, QStringLiteral("Key"));
    ASSERT_TRUE(light.isValid());

    LightSnapshot snapshot = LightSnapshot::fromHandle(light);
    snapshot.linkMode = LightLinking::Mode::Exclude;
    snapshot.linkedEntityNames = {QStringLiteral("Floor")};
    LightManager::getSingleton()->applyProperties(light.name, snapshot);

    EXPECT_NE(light.light->getLightMask() & hero->getLightMask(), 0u);
    EXPECT_EQ(light.light->getLightMask() & floor->getLightMask(), 0u);
}

TEST_F(LightLinkingOgreTest, JsonRoundTripPreservesLinkFields)
{
    LightSnapshot original;
    original.name = QStringLiteral("L1");
    original.type = Ogre::Light::LT_POINT;
    original.linkMode = LightLinking::Mode::Include;
    original.linkedEntityNames = {QStringLiteral("Hero")};
    original.linkChannelBit = 4;

    SceneLightsIO::SceneLightsDocument doc;
    doc.standaloneLights.append(original);
    const QByteArray json = SceneLightsIO::documentToJson(doc);
    SceneLightsIO::SceneLightsDocument restored;
    ASSERT_TRUE(SceneLightsIO::documentFromJson(json, restored));
    ASSERT_EQ(restored.standaloneLights.size(), 1);
    EXPECT_EQ(restored.standaloneLights.first().linkMode, LightLinking::Mode::Include);
    EXPECT_EQ(restored.standaloneLights.first().linkedEntityNames,
              QStringList{QStringLiteral("Hero")});
    EXPECT_EQ(restored.standaloneLights.first().linkChannelBit, 4u);
}

TEST_F(LightLinkingOgreTest, RestoreSnapshotAppliesLinkingBeforeRegistration)
{
    Ogre::Entity* hero = createTestEntity(QStringLiteral("Hero"));
    Ogre::Entity* floor = createTestEntity(QStringLiteral("Floor"));

    LightSnapshot snapshot;
    snapshot.name = QStringLiteral("SavedKey");
    snapshot.type = Ogre::Light::LT_POINT;
    snapshot.linkMode = LightLinking::Mode::Include;
    snapshot.linkedEntityNames = {QStringLiteral("Hero")};

    const LightHandle restored = LightManager::getSingleton()->restoreSnapshot(snapshot);
    ASSERT_TRUE(restored.isValid());
    EXPECT_NE(restored.light->getLightMask() & hero->getLightMask(), 0u);
    EXPECT_EQ(restored.light->getLightMask() & floor->getLightMask(), 0u);
}

TEST_F(LightLinkingOgreTest, DuplicateLightAllocatesFreshLinkChannel)
{
    Ogre::Entity* hero = createTestEntity(QStringLiteral("Hero"));

    const LightHandle source =
        LightManager::getSingleton()->createLight(Ogre::Light::LT_POINT, QStringLiteral("Key"));
    ASSERT_TRUE(source.isValid());

    LightSnapshot sourceSnap = LightSnapshot::fromHandle(source);
    sourceSnap.linkMode = LightLinking::Mode::Include;
    sourceSnap.linkedEntityNames = {QStringLiteral("Hero")};
    LightManager::getSingleton()->applyProperties(source.name, sourceSnap);

    const uint32_t sourceBit = LightSnapshot::fromHandle(source).linkChannelBit;
    ASSERT_NE(sourceBit, 0u);

    const LightHandle clone = LightManager::getSingleton()->duplicateLight(source.name);
    ASSERT_TRUE(clone.isValid());

    const uint32_t cloneBit = LightSnapshot::fromHandle(clone).linkChannelBit;
    EXPECT_NE(sourceBit, cloneBit);
    EXPECT_NE(source.light->getLightMask() & hero->getLightMask(), 0u);
    EXPECT_NE(clone.light->getLightMask() & hero->getLightMask(), 0u);

    LightSnapshot cloneSnap = LightSnapshot::fromHandle(clone);
    cloneSnap.linkedEntityNames.clear();
    LightManager::getSingleton()->applyProperties(clone.name, cloneSnap);

    EXPECT_EQ(LightSnapshot::fromHandle(source).linkChannelBit, sourceBit);
    EXPECT_EQ(LightSnapshot::fromHandle(clone).linkChannelBit, cloneBit);
    EXPECT_NE(source.light->getLightMask() & hero->getLightMask(), 0u);
}
