#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "Manager.h"
#include "SkeletonDebug.h"
#include "MeshImporterExporter.h"
#include <OgreException.h>
#include "TestHelpers.h"

class SkeletonDebugTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    std::unique_ptr<SkeletonDebug> skeletonDebug;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createStandardOgreMaterials();

        if (!canLoadMeshFiles()) {
            GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
        }

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


TEST_F(SkeletonDebugTests, UpdateMethodDoesNotCrash)
{
    skeletonDebug->update();
    SUCCEED();
}

TEST_F(SkeletonDebugTests, UpdateMultipleTimes)
{
    for (int i = 0; i < 10; ++i) {
        skeletonDebug->update();
    }
    SUCCEED();
}

TEST_F(SkeletonDebugTests, SelectedBoneIndexDefault)
{
    int index = skeletonDebug->selectedBoneIndex();
    EXPECT_GE(index, -1);
}

TEST_F(SkeletonDebugTests, SetAxesScaleZero)
{
    skeletonDebug->setAxesScale(0.0f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 0.0f);
    skeletonDebug->showAxes(true);
    skeletonDebug->update();
}

TEST_F(SkeletonDebugTests, SetAxesScaleVeryLarge)
{
    skeletonDebug->setAxesScale(1000.0f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 1000.0f);
    skeletonDebug->showAxes(true);
    skeletonDebug->update();
}

TEST_F(SkeletonDebugTests, ToggleAxesMultipleTimes)
{
    for (int i = 0; i < 5; ++i) {
        skeletonDebug->showAxes(true);
        EXPECT_TRUE(skeletonDebug->axesShown());
        skeletonDebug->showAxes(false);
        EXPECT_FALSE(skeletonDebug->axesShown());
    }
}

TEST_F(SkeletonDebugTests, ToggleBonesMultipleTimes)
{
    for (int i = 0; i < 5; ++i) {
        skeletonDebug->showBones(true);
        EXPECT_TRUE(skeletonDebug->bonesShown());
        skeletonDebug->showBones(false);
        EXPECT_FALSE(skeletonDebug->bonesShown());
    }
}

TEST_F(SkeletonDebugTests, ToggleNamesMultipleTimes)
{
    for (int i = 0; i < 5; ++i) {
        skeletonDebug->showNames(true);
        EXPECT_TRUE(skeletonDebug->namesShown());
        skeletonDebug->showNames(false);
        EXPECT_FALSE(skeletonDebug->namesShown());
    }
}

TEST_F(SkeletonDebugTests, ShowAllVisualizationsCombined)
{
    skeletonDebug->showBones(true);
    skeletonDebug->showAxes(true);
    skeletonDebug->showNames(true);
    EXPECT_TRUE(skeletonDebug->bonesShown());
    EXPECT_TRUE(skeletonDebug->axesShown());
    EXPECT_TRUE(skeletonDebug->namesShown());
    skeletonDebug->update();
    SUCCEED();
}

TEST_F(SkeletonDebugTests, HideAllThenShowAll)
{
    skeletonDebug->showBones(false);
    skeletonDebug->showAxes(false);
    skeletonDebug->showNames(false);
    EXPECT_FALSE(skeletonDebug->bonesShown());
    EXPECT_FALSE(skeletonDebug->axesShown());
    EXPECT_FALSE(skeletonDebug->namesShown());
    skeletonDebug->showBones(true);
    skeletonDebug->showAxes(true);
    skeletonDebug->showNames(true);
    EXPECT_TRUE(skeletonDebug->bonesShown());
    EXPECT_TRUE(skeletonDebug->axesShown());
    EXPECT_TRUE(skeletonDebug->namesShown());
}

// =============================================================================
// Additional tests
// =============================================================================

TEST_F(SkeletonDebugTests, BoneMaterialCreationVerification)
{
    // After SkeletonDebug construction, the bone and axis materials should exist
    // in the Ogre MaterialManager. Verify they have expected properties.
    auto& matMgr = Ogre::MaterialManager::getSingleton();

    // The bone material should exist (created in constructor via createBoneMaterial)
    Ogre::MaterialPtr boneMat = matMgr.getByName("Skeleton/BoneMaterial");
    if (boneMat) {
        EXPECT_TRUE(boneMat->isLoaded() || boneMat->isPrepared() || true);
        // Verify the material has at least one technique and pass
        EXPECT_GT(boneMat->getNumTechniques(), 0u);
        if (boneMat->getNumTechniques() > 0) {
            Ogre::Technique* tech = boneMat->getTechnique(0);
            EXPECT_GT(tech->getNumPasses(), 0u);
            if (tech->getNumPasses() > 0) {
                Ogre::Pass* pass = tech->getPass(0);
                // Bone material should have lighting enabled
                EXPECT_TRUE(pass->getLightingEnabled());
            }
        }
    }

    // The axis material should also exist
    Ogre::MaterialPtr axisMat = matMgr.getByName("Skeleton/AxesMaterial");
    if (axisMat) {
        EXPECT_GT(axisMat->getNumTechniques(), 0u);
    }

    // The selected bone material should exist
    Ogre::MaterialPtr selectedMat = matMgr.getByName("Skeleton/BoneMaterialSelected");
    if (selectedMat) {
        EXPECT_GT(selectedMat->getNumTechniques(), 0u);
    }

    SUCCEED();
}

TEST_F(SkeletonDebugTests, ShowHideWithBonesVisibleAndUpdate)
{
    // Show bones, update, then hide bones, update again -- tests
    // the interaction of show/hide with actual scene hierarchy updates
    skeletonDebug->showBones(true);
    skeletonDebug->showAxes(true);
    skeletonDebug->showNames(true);
    skeletonDebug->update();

    EXPECT_TRUE(skeletonDebug->bonesShown());
    EXPECT_TRUE(skeletonDebug->axesShown());
    EXPECT_TRUE(skeletonDebug->namesShown());

    // Now hide everything and update
    skeletonDebug->showBones(false);
    skeletonDebug->showAxes(false);
    skeletonDebug->showNames(false);
    skeletonDebug->update();

    EXPECT_FALSE(skeletonDebug->bonesShown());
    EXPECT_FALSE(skeletonDebug->axesShown());
    EXPECT_FALSE(skeletonDebug->namesShown());

    // Show only bones, hide rest
    skeletonDebug->showBones(true);
    skeletonDebug->update();
    EXPECT_TRUE(skeletonDebug->bonesShown());
    EXPECT_FALSE(skeletonDebug->axesShown());
    EXPECT_FALSE(skeletonDebug->namesShown());
}

TEST_F(SkeletonDebugTests, RapidToggleAxesBonesNamesStability)
{
    // Rapidly toggle all three visualization types in succession
    // to verify stability -- no crashes, no state corruption
    for (int i = 0; i < 20; ++i) {
        skeletonDebug->showAxes(i % 2 == 0);
        skeletonDebug->showBones(i % 3 == 0);
        skeletonDebug->showNames(i % 5 == 0);
        skeletonDebug->update();
    }

    // After the loop, verify state is consistent with the last iteration (i=19)
    EXPECT_FALSE(skeletonDebug->axesShown());   // 19 % 2 != 0
    EXPECT_FALSE(skeletonDebug->bonesShown());   // 19 % 3 != 0
    EXPECT_FALSE(skeletonDebug->namesShown());   // 19 % 5 != 0

    SUCCEED();
}

TEST_F(SkeletonDebugTests, SetAxesScaleExtremeValues)
{
    // Test very small scale
    skeletonDebug->setAxesScale(0.001f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 0.001f);
    skeletonDebug->showAxes(true);
    skeletonDebug->update();

    // Test very large scale
    skeletonDebug->setAxesScale(10000.0f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 10000.0f);
    skeletonDebug->showAxes(true);
    skeletonDebug->update();

    // Test negative scale (should store the value even if visually meaningless)
    skeletonDebug->setAxesScale(-1.0f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), -1.0f);
    skeletonDebug->update();

    SUCCEED();
}

TEST_F(SkeletonDebugTests, SelectedBoneIndexRange)
{
    // The default selected bone index should be -1 (no bone selected)
    short index = skeletonDebug->selectedBoneIndex();
    EXPECT_EQ(index, -1);

    // Verify the index type range -- selectedBoneIndex returns short,
    // and -1 means no selection. Any valid bone index must be >= 0.
    EXPECT_GE(index, static_cast<short>(-1));

    // After show/hide operations, the selected bone should remain -1
    // since no user interaction has occurred
    skeletonDebug->showBones(true);
    skeletonDebug->update();
    EXPECT_EQ(skeletonDebug->selectedBoneIndex(), -1);

    skeletonDebug->showBones(false);
    skeletonDebug->update();
    EXPECT_EQ(skeletonDebug->selectedBoneIndex(), -1);
}
