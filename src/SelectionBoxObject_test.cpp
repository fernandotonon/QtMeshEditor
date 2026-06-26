#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "SelectionBoxObject.h"
#include "Manager.h"
#include "GlobalDefinitions.h"
#include "TestHelpers.h"

class SelectionBoxObjectTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";

        createStandardOgreMaterials();
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

TEST_F(SelectionBoxObjectTest, Construction)
{
    SelectionBoxObject box("TestSelBox");
    // Constructor should set query flags to 0
    EXPECT_EQ(box.getQueryFlags(), 0x00u);
}

TEST_F(SelectionBoxObjectTest, DefaultBoxColour)
{
    SelectionBoxObject box("TestSelBoxColour");
    Ogre::ColourValue expected(0.8f, 0.8f, 0.8f, 0.8f);
    EXPECT_EQ(box.getBoxColour(), expected);
}

TEST_F(SelectionBoxObjectTest, SetBoxColour)
{
    SelectionBoxObject box("TestSelBoxSetColour");
    Ogre::ColourValue red(1.0f, 0.0f, 0.0f, 1.0f);
    box.setBoxColour(red);
    EXPECT_EQ(box.getBoxColour(), red);
}

TEST_F(SelectionBoxObjectTest, DrawBox_VectorOverload)
{
    SelectionBoxObject box("TestSelBoxDrawVec");
    Ogre::Vector2 topLeft(-0.5f, 0.5f);
    Ogre::Vector2 bottomRight(0.5f, -0.5f);
    // drawBox with Vector2 should not crash
    box.drawBox(topLeft, bottomRight);
}
