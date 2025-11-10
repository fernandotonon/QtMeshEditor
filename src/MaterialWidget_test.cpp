#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QTableWidget>
#include <QHeaderView>
#include "MaterialWidget.h"

// Test case for MaterialWidget
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

// Test the constructor of MaterialWidget
TEST_F(MaterialWidgetTest, Constructor)
{
    // Create a MaterialWidget instance
    MaterialWidget materialWidget;

    // Check the initial state of the MaterialWidget
    EXPECT_EQ(materialWidget.columnCount(), 3);
    EXPECT_EQ(materialWidget.horizontalHeader()->sectionResizeMode(QHeaderView::Stretch), QHeaderView::Stretch);
    EXPECT_TRUE(materialWidget.verticalHeader()->isHidden());
}
