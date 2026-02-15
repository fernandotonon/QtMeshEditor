#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QTableWidget>
#include <QHeaderView>
#include <QThread>
#include "MaterialWidget.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "PrimitiveObject.h"
#include "TestHelpers.h"

class MaterialWidgetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }

    void TearDown() override
    {
    }

    QApplication* app;
};

TEST_F(MaterialWidgetTest, Constructor)
{
    MaterialWidget materialWidget;
    EXPECT_EQ(materialWidget.columnCount(), 3);
    EXPECT_EQ(materialWidget.horizontalHeader()->sectionResizeMode(QHeaderView::Stretch), QHeaderView::Stretch);
    EXPECT_TRUE(materialWidget.verticalHeader()->isHidden());
}

TEST_F(MaterialWidgetTest, InitialRowCountIsZero)
{
    MaterialWidget materialWidget;
    EXPECT_EQ(materialWidget.rowCount(), 0);
}

TEST_F(MaterialWidgetTest, HeaderLabels)
{
    MaterialWidget materialWidget;
    EXPECT_EQ(materialWidget.horizontalHeaderItem(0)->text(), "Entity");
    EXPECT_EQ(materialWidget.horizontalHeaderItem(1)->text(), "Sub");
    EXPECT_EQ(materialWidget.horizontalHeaderItem(2)->text(), "Material");
}

TEST_F(MaterialWidgetTest, EditTriggersEnabled)
{
    MaterialWidget materialWidget;
    EXPECT_NE(materialWidget.editTriggers(), QAbstractItemView::NoEditTriggers);
}

// Tests that require Manager/Ogre
class MaterialWidgetOgreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        try {
            Manager::getSingleton();
        } catch (const Ogre::Exception& e) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
        }
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

TEST_F(MaterialWidgetOgreTest, EmptySelectionKeepsEmptyTable)
{
    MaterialWidget materialWidget;
    // With no selection, table should remain empty
    EXPECT_EQ(materialWidget.rowCount(), 0);
}

TEST_F(MaterialWidgetOgreTest, ConstructorWithOgre)
{
    MaterialWidget materialWidget;
    EXPECT_EQ(materialWidget.columnCount(), 3);
    EXPECT_EQ(materialWidget.rowCount(), 0);
}
