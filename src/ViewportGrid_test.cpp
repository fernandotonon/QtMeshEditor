#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "ViewportGrid.h"
#include "Manager.h"
#include "GlobalDefinitions.h"
#include "TestHelpers.h"

class ViewportGridTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";

        ensureMaterialManagerInitialised();

        // Create the GUI material needed by ViewportGrid
        Ogre::MaterialPtr guiMat = Ogre::MaterialManager::getSingleton().getByName(
            GUI_MATERIAL_NAME, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        if (!guiMat) {
            guiMat = Ogre::MaterialManager::getSingleton().create(
                GUI_MATERIAL_NAME, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
            guiMat->getTechnique(0)->setLightingEnabled(false);
            guiMat->getTechnique(0)->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
            guiMat->getTechnique(0)->setDepthCheckEnabled(false);
        }
    }

    void TearDown() override
    {
        Manager::kill();
        if (app)
            app->processEvents();
        QThread::msleep(50);
    }

    QApplication* app = nullptr;
};

TEST_F(ViewportGridTest, Constructor)
{
    ViewportGrid grid;
    EXPECT_EQ(grid.getColour(), Ogre::ColourValue::White);
    EXPECT_EQ(grid.getScale(), 10u);
}

TEST_F(ViewportGridTest, ConstructorCustomColor)
{
    Ogre::ColourValue red(1.0f, 0.0f, 0.0f, 1.0f);
    ViewportGrid grid(red, 5);
    EXPECT_EQ(grid.getColour(), red);
    EXPECT_EQ(grid.getScale(), 5u);
}

TEST_F(ViewportGridTest, GetFading)
{
    ViewportGrid grid;
    EXPECT_FLOAT_EQ(grid.getFading(), 0.3f);
}

TEST_F(ViewportGridTest, SetPosition)
{
    ViewportGrid grid;
    Ogre::Vector3 pos(1.0f, 2.0f, 3.0f);
    grid.setPosition(pos);
    EXPECT_EQ(grid.getPosition(), pos);
}

TEST_F(ViewportGridTest, GetDefaultPosition)
{
    ViewportGrid grid;
    EXPECT_EQ(grid.getPosition(), Ogre::Vector3::ZERO);
}

TEST_F(ViewportGridTest, SetVisible)
{
    ViewportGrid grid;
    // Should not crash
    grid.setVisible(true);
    grid.setVisible(false);
}

TEST_F(ViewportGridTest, SetQueryFlags)
{
    ViewportGrid grid;
    grid.setQueryFlags(0x00);
    EXPECT_EQ(grid.getQueryFlags(), 0x00u);

    grid.setQueryFlags(0xFF);
    EXPECT_EQ(grid.getQueryFlags(), 0xFFu);
}

TEST_F(ViewportGridTest, DefaultQueryFlags)
{
    ViewportGrid grid;
    // Constructor calls setQueryFlags(0)
    EXPECT_EQ(grid.getQueryFlags(), 0x00u);
}
