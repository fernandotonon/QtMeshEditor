#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <OgreMaterialManager.h>
#include <OgreException.h>
#include "BoneWeightOverlay.h"
#include "MeshImporterExporter.h"
#include "SelectionSet.h"
#include "Manager.h"
#include "TestHelpers.h"

// ===========================================================================
// Integration test: verify BoneWeightOverlay material (requires mesh loading)
// ===========================================================================

class BoneWeightOverlayIntegrationTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    Ogre::Entity* entity = nullptr;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createStandardOgreMaterials();

        if (!canLoadMeshFiles())
            GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

        // Remove leftover material so createMaterial() runs fresh
        auto existing = Ogre::MaterialManager::getSingleton().getByName(
            "BoneWeightOverlay/Material");
        if (existing)
            Ogre::MaterialManager::getSingleton().remove(existing);

        QStringList uris{"./media/models/robot.mesh"};
        try { MeshImporterExporter::importer(uris); }
        catch (...) { GTEST_SKIP() << "Skipping: failed to import robot.mesh"; }

        if (Manager::getSingleton()->getEntities().isEmpty())
            GTEST_SKIP() << "Skipping: no entity after import";

        entity = Manager::getSingleton()->getEntities().last();
        ASSERT_NE(entity, nullptr);
    }

    void TearDown() override {
        if (!Manager::getSingletonPtr())
            return;
        auto existing = Ogre::MaterialManager::getSingleton().getByName(
            "BoneWeightOverlay/Material");
        if (existing)
            Ogre::MaterialManager::getSingleton().remove(existing);
        SelectionSet::getSingleton()->clear();
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(50);
    }
};

// Verify that BoneWeightOverlay creates its material with TVC_DIFFUSE.
// TVC_AMBIENT does NOT work with lighting disabled on RTSS (macOS Metal/GL3+),
// causing the overlay to display a solid colour instead of vertex-coloured heat map.
TEST_F(BoneWeightOverlayIntegrationTest, MaterialUsesDiffuseVertexColourTracking)
{
    BoneWeightOverlay overlay(entity, Manager::getSingleton()->getSceneMgr());

    auto mat = Ogre::MaterialManager::getSingleton().getByName(
        "BoneWeightOverlay/Material");
    ASSERT_TRUE(mat) << "BoneWeightOverlay should create its material on construction";

    Ogre::Pass* p = mat->getTechnique(0)->getPass(0);
    ASSERT_NE(p, nullptr);

    EXPECT_FALSE(p->getLightingEnabled());
    EXPECT_TRUE(p->getVertexColourTracking() & Ogre::TVC_DIFFUSE)
        << "Vertex colour tracking MUST include TVC_DIFFUSE for colours "
           "to display with lighting disabled (RTSS requirement)";
}

// ===========================================================================
// weightToColor tests (no Ogre required)
// ===========================================================================

TEST(BoneWeightOverlayTest, WeightToColorAtZeroIsBlue)
{
    auto color = BoneWeightOverlay::weightToColor(0.0f);
    EXPECT_NEAR(color.r, 0.0f, 1e-5f);
    EXPECT_NEAR(color.g, 0.0f, 1e-5f);
    EXPECT_NEAR(color.b, 1.0f, 1e-5f);
    EXPECT_NEAR(color.a, 0.7f, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorAtQuarterIsCyan)
{
    auto color = BoneWeightOverlay::weightToColor(0.25f);
    EXPECT_NEAR(color.r, 0.0f, 1e-5f);
    EXPECT_NEAR(color.g, 1.0f, 1e-5f);
    EXPECT_NEAR(color.b, 1.0f, 1e-5f);
    EXPECT_NEAR(color.a, 0.7f, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorAtHalfIsGreen)
{
    auto color = BoneWeightOverlay::weightToColor(0.5f);
    EXPECT_NEAR(color.r, 0.0f, 1e-5f);
    EXPECT_NEAR(color.g, 1.0f, 1e-5f);
    EXPECT_NEAR(color.b, 0.0f, 1e-5f);
    EXPECT_NEAR(color.a, 0.7f, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorAtThreeQuartersIsYellow)
{
    auto color = BoneWeightOverlay::weightToColor(0.75f);
    EXPECT_NEAR(color.r, 1.0f, 1e-5f);
    EXPECT_NEAR(color.g, 1.0f, 1e-5f);
    EXPECT_NEAR(color.b, 0.0f, 1e-5f);
    EXPECT_NEAR(color.a, 0.7f, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorAtOneIsRed)
{
    auto color = BoneWeightOverlay::weightToColor(1.0f);
    EXPECT_NEAR(color.r, 1.0f, 1e-5f);
    EXPECT_NEAR(color.g, 0.0f, 1e-5f);
    EXPECT_NEAR(color.b, 0.0f, 1e-5f);
    EXPECT_NEAR(color.a, 0.7f, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorClampsNegative)
{
    auto color = BoneWeightOverlay::weightToColor(-0.5f);
    auto colorZero = BoneWeightOverlay::weightToColor(0.0f);
    EXPECT_NEAR(color.r, colorZero.r, 1e-5f);
    EXPECT_NEAR(color.g, colorZero.g, 1e-5f);
    EXPECT_NEAR(color.b, colorZero.b, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorClampsAboveOne)
{
    auto color = BoneWeightOverlay::weightToColor(1.5f);
    auto colorOne = BoneWeightOverlay::weightToColor(1.0f);
    EXPECT_NEAR(color.r, colorOne.r, 1e-5f);
    EXPECT_NEAR(color.g, colorOne.g, 1e-5f);
    EXPECT_NEAR(color.b, colorOne.b, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorMidpointsInterpolate)
{
    // At 0.125 (halfway between blue and cyan), green should be 0.5
    auto color = BoneWeightOverlay::weightToColor(0.125f);
    EXPECT_NEAR(color.r, 0.0f, 1e-5f);
    EXPECT_NEAR(color.g, 0.5f, 1e-5f);
    EXPECT_NEAR(color.b, 1.0f, 1e-5f);

    // At 0.625 (halfway between green and yellow), red should be 0.5
    auto color2 = BoneWeightOverlay::weightToColor(0.625f);
    EXPECT_NEAR(color2.r, 0.5f, 1e-5f);
    EXPECT_NEAR(color2.g, 1.0f, 1e-5f);
    EXPECT_NEAR(color2.b, 0.0f, 1e-5f);
}
