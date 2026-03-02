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
