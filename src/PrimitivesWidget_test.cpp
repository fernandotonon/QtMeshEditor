#include <gtest/gtest.h>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSignalSpy>
#include <QKeyEvent>
#include <QInputDialog>
#include "PrimitivesWidget.h"

// Test case for MaterialWidget
class PrimitivesWidgetTest : public ::testing::Test
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

TEST_F(PrimitivesWidgetTest, CreateCube)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createCube("Cube");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Cube");
}

TEST_F(PrimitivesWidgetTest, UpdateUiFromParams)
{
    PrimitivesWidget widget;
    QDoubleSpinBox* sizeXLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeX");
    ASSERT_TRUE(sizeXLineEdit != nullptr);

    QDoubleSpinBox* sizeYLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeY");
    ASSERT_TRUE(sizeYLineEdit != nullptr);

    QDoubleSpinBox* sizeZLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeZ");
    ASSERT_TRUE(sizeZLineEdit != nullptr);

    widget.updateUiFromParams();

    ASSERT_EQ(sizeXLineEdit->value(), sizeXLineEdit->minimum());
    ASSERT_EQ(sizeYLineEdit->value(), sizeXLineEdit->minimum());
    ASSERT_EQ(sizeZLineEdit->value(), sizeXLineEdit->minimum());
}
