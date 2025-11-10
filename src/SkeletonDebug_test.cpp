#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "Manager.h"
#include "mainwindow.h"
#include "SkeletonDebug.h"

class SkeletonDebugTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    MainWindow* mainWindow = nullptr;
    std::unique_ptr<SkeletonDebug> skeletonDebug;

    void SetUp() override {
        // Ensure fresh OGRE state
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        try {
            mainWindow = new MainWindow();
            Manager::getSingleton(mainWindow);
        } catch (const Ogre::RenderingAPIException& e) {
            GTEST_SKIP() << "Skipping SkeletonDebug tests: unable to create OGRE render window ("
                         << e.getFullDescription() << ")";
        } catch (const std::exception& e) {
            GTEST_SKIP() << "Skipping SkeletonDebug tests: " << e.what();
        }

        QStringList validUri{"./media/models/robot.mesh"};
        try {
            Manager::getSingleton()->getMainWindow()->importMeshs(validUri);
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

        delete mainWindow;
        mainWindow = nullptr;

        Manager::kill();

        if (app) {
            app->processEvents();
        }
        QThread::msleep(50);
    }
};

TEST_F(SkeletonDebugTests, ShowAxesTest)
{
    // Initially, axes should not be shown
    EXPECT_FALSE(skeletonDebug->axesShown());

    // Show axes
    skeletonDebug->showAxes(true);
    EXPECT_TRUE(skeletonDebug->axesShown());

    // Hide axes
    skeletonDebug->showAxes(false);
    EXPECT_FALSE(skeletonDebug->axesShown());
}

TEST_F(SkeletonDebugTests, ShowNamesTest)
{
    // Initially, names should not be shown
    EXPECT_FALSE(skeletonDebug->namesShown());

    // Show names
    skeletonDebug->showNames(true);
    EXPECT_TRUE(skeletonDebug->namesShown());

    // Hide names
    skeletonDebug->showNames(false);
    EXPECT_FALSE(skeletonDebug->namesShown());
}

TEST_F(SkeletonDebugTests, ShowBonesTest)
{
    // Initially, bones should not be shown
    EXPECT_FALSE(skeletonDebug->bonesShown());

    // Show bones
    skeletonDebug->showBones(true);
    EXPECT_TRUE(skeletonDebug->bonesShown());

    // Hide bones
    skeletonDebug->showBones(false);
    EXPECT_FALSE(skeletonDebug->bonesShown());
}

TEST_F(SkeletonDebugTests, SetAndGetAxesScaleTest)
{
    // Set axes scale
    skeletonDebug->setAxesScale(0.5f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 0.5f);

    // Set axes scale to a different value
    skeletonDebug->setAxesScale(1.0f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 1.0f);
}
