#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "Manager.h"
#include "SkeletonDebug.h"
#include "MeshImporterExporter.h"
#include <OgreException.h>
#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>

// Helper function to create required OGRE materials for tests
static void createOGREMaterials()
{
    Ogre::MaterialPtr baseWhiteMat = Ogre::MaterialManager::getSingleton().getByName(
        "BaseWhiteNoLighting", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!baseWhiteMat)
    {
        baseWhiteMat = Ogre::MaterialManager::getSingleton().create(
            "BaseWhiteNoLighting", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        baseWhiteMat->getTechnique(0)->getPass(0)->setDiffuse(1, 1, 1, 1);
        baseWhiteMat->getTechnique(0)->getPass(0)->setAmbient(1, 1, 1);
        baseWhiteMat->getTechnique(0)->getPass(0)->setSelfIllumination(1, 1, 1);
        baseWhiteMat->getTechnique(0)->setLightingEnabled(false);
    }

    Ogre::MaterialPtr baseWhiteMat2 = Ogre::MaterialManager::getSingleton().getByName(
        "BaseWhite", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!baseWhiteMat2)
    {
        baseWhiteMat2 = Ogre::MaterialManager::getSingleton().create(
            "BaseWhite", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        baseWhiteMat2->getTechnique(0)->getPass(0)->setDiffuse(1, 1, 1, 1);
        baseWhiteMat2->getTechnique(0)->getPass(0)->setAmbient(1, 1, 1);
    }
}

class SkeletonDebugTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    std::unique_ptr<SkeletonDebug> skeletonDebug;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        try {
            Manager::getSingleton();  // headless — no render window needed
        } catch (const Ogre::Exception& e) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
        }
        createOGREMaterials();

        // Import a mesh with skeleton
        QStringList validUri{"./media/models/robot.mesh"};
        try {
            MeshImporterExporter::importer(validUri);
        } catch (const Ogre::Exception& e) {
            GTEST_SKIP() << "Skipping SkeletonDebug tests: failed to import mesh ("
                         << e.getFullDescription() << ")";
        }

        Ogre::Entity* entity = Manager::getSingleton()->getEntities().isEmpty()
                                   ? nullptr
                                   : Manager::getSingleton()->getEntities().last();
        if (!entity) {
            GTEST_SKIP() << "Skipping SkeletonDebug tests: no entity available after import";
        }

        Ogre::SceneManager* sceneManager = Manager::getSingleton()->getSceneMgr();
        skeletonDebug = std::make_unique<SkeletonDebug>(entity, sceneManager);
    }

    void TearDown() override {
        skeletonDebug.reset();

        Manager::kill();

        if (app) {
            app->processEvents();
        }
        QThread::msleep(50);
    }
};

TEST_F(SkeletonDebugTests, ShowAxesTest)
{
    EXPECT_FALSE(skeletonDebug->axesShown());

    skeletonDebug->showAxes(true);
    EXPECT_TRUE(skeletonDebug->axesShown());

    skeletonDebug->showAxes(false);
    EXPECT_FALSE(skeletonDebug->axesShown());
}

TEST_F(SkeletonDebugTests, ShowNamesTest)
{
    EXPECT_FALSE(skeletonDebug->namesShown());

    skeletonDebug->showNames(true);
    EXPECT_TRUE(skeletonDebug->namesShown());

    skeletonDebug->showNames(false);
    EXPECT_FALSE(skeletonDebug->namesShown());
}

TEST_F(SkeletonDebugTests, ShowBonesTest)
{
    EXPECT_FALSE(skeletonDebug->bonesShown());

    skeletonDebug->showBones(true);
    EXPECT_TRUE(skeletonDebug->bonesShown());

    skeletonDebug->showBones(false);
    EXPECT_FALSE(skeletonDebug->bonesShown());
}

TEST_F(SkeletonDebugTests, SetAndGetAxesScaleTest)
{
    skeletonDebug->setAxesScale(0.5f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 0.5f);

    skeletonDebug->setAxesScale(1.0f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 1.0f);
}
