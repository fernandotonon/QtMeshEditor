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

TEST_F(PrimitivesWidgetTest, CreateSphere)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createSphere("Sphere");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Sphere");
}

TEST_F(PrimitivesWidgetTest, CreatePlane)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createPlane("Plane");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Plane");
}

TEST_F(PrimitivesWidgetTest, CreateCylinder)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createCylinder("Cylinder");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Cylinder");
}

TEST_F(PrimitivesWidgetTest, CreateCone)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createCone("Cone");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Cone");
}

TEST_F(PrimitivesWidgetTest, CreateTorus)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createTorus("Torus");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Torus");
}

TEST_F(PrimitivesWidgetTest, CreateTube)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createTube("Tube");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Tube");
}

TEST_F(PrimitivesWidgetTest, CreateCapsule)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createCapsule("Capsule");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Capsule");
}

TEST_F(PrimitivesWidgetTest, CreateIcoSphere)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createIcoSphere("IcoSphere");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "IcoSphere");
}

TEST_F(PrimitivesWidgetTest, CreateRoundedBox)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createRoundedBox("Rounded Box");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Rounded Box");
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
