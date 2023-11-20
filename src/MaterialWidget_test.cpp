#include <gtest/gtest.h>
#include <QApplication>
#include <QTableWidget>
#include <QHeaderView>
#include "MaterialWidget.h"

// Test case for MaterialWidget
class MaterialWidgetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create a QApplication instance for testing
        int argc = 0;
        char* argv[] = { nullptr };
        app = new QApplication(argc, argv);
    }

    void TearDown() override
    {
        // Clean up the QApplication instance
        delete app;
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
