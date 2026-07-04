#include <gtest/gtest.h>

#include "AppSettingsKeys.h"
#include "LightManager.h"
#include "LightRigLibrary.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <OgreSceneManager.h>

#include <QSettings>

class LightRigLibraryOgreTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        LightManager::kill();
        Manager::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        LightManager::getSingleton()->tryConnectToManager();
    }

    void TearDown() override
    {
        LightManager::kill();
        Manager::kill();
    }
};

TEST_F(LightRigLibraryOgreTest, CatalogListsSixRigs)
{
    EXPECT_EQ(LightRigLibrary::rigIds().size(), 6);
    EXPECT_GE(LightRigLibrary::indexOfRig(QStringLiteral("three_point_studio")), 0);
    EXPECT_EQ(LightRigLibrary::displayNameForId(QStringLiteral("single_key")),
              QStringLiteral("Single Key"));
}

TEST_F(LightRigLibraryOgreTest, ApplyThreePointStudio_CreatesGroupedLights)
{
    Manager::getSingletonPtr()->CreateEmptyScene();

    const LightRigApplyResult result =
        LightRigLibrary::apply(QStringLiteral("three_point_studio"), true);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_FALSE(result.rigGroupNodeName.isEmpty());
    EXPECT_EQ(result.addedLights.size(), 3);
    EXPECT_EQ(result.suggestedHdri, QStringLiteral("studio_neutral.hdr"));
    EXPECT_EQ(LightManager::getSingleton()->lights().size(), 3);

    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr->getSceneMgr(), nullptr);
    EXPECT_EQ(mgr->getSceneMgr()->getAmbientLight(),
              Ogre::ColourValue(0.15f, 0.15f, 0.18f));
}

TEST_F(LightRigLibraryOgreTest, ApplySingleKey_MatchesLegacyDefault)
{
    const LightRigApplyResult result =
        LightRigLibrary::apply(QStringLiteral("single_key"), true);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.addedLights.size(), 1);
    EXPECT_EQ(result.addedLights.first().name, QStringLiteral("KeyLight"));
    EXPECT_EQ(result.ambientAfter, Ogre::ColourValue(0.3f, 0.3f, 0.3f));
}

TEST_F(LightRigLibraryOgreTest, OutdoorSunset_SuggestsHdri)
{
    const LightRigApplyResult result =
        LightRigLibrary::apply(QStringLiteral("outdoor_sunset"), true);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.suggestedHdri, QStringLiteral("sunset_outdoor.hdr"));
}

TEST_F(LightRigLibraryOgreTest, OutdoorOvercast_SuggestsHdri)
{
    const LightRigApplyResult result =
        LightRigLibrary::apply(QStringLiteral("outdoor_overcast"), true);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.suggestedHdri, QStringLiteral("overcast_outdoor.hdr"));
}

TEST_F(LightRigLibraryOgreTest, DefaultRigSetting_RoundTrips)
{
    QSettings settings;
    settings.setValue(AppSettingsKeys::lightingDefaultRig(), QStringLiteral("single_key"));
    EXPECT_EQ(LightRigLibrary::readDefaultRigId(), QStringLiteral("single_key"));
    settings.setValue(AppSettingsKeys::lightingDefaultRig(), QStringLiteral("three_point_studio"));
}

TEST_F(LightRigLibraryOgreTest, ApplyWithoutReplace_KeepsExistingLights)
{
    LightManager::getSingleton()->createLight(Ogre::Light::LT_POINT, QStringLiteral("Extra"));
    const LightRigApplyResult result =
        LightRigLibrary::apply(QStringLiteral("single_key"), false);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(LightManager::getSingleton()->lights().size(), 2);
}

TEST_F(LightRigLibraryOgreTest, ReapplyWithoutReplace_DoesNotStackRigLights)
{
    const LightRigApplyResult first =
        LightRigLibrary::apply(QStringLiteral("three_point_studio"), false);
    ASSERT_TRUE(first.ok) << first.error.toStdString();
    ASSERT_EQ(LightManager::getSingleton()->lights().size(), 3);

    const LightRigApplyResult second =
        LightRigLibrary::apply(QStringLiteral("three_point_studio"), false);
    ASSERT_TRUE(second.ok) << second.error.toStdString();
    EXPECT_EQ(LightManager::getSingleton()->lights().size(), 3);
}
