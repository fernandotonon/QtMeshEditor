#include <gtest/gtest.h>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSignalSpy>
#include <QKeyEvent>
#include <QInputDialog>
#include "PrimitivesWidget.h"
#include "Manager.h"

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

private:
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

TEST_F(PrimitivesWidgetTest, RemoveAndRecreateCube) {
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createCube("Cube");

    auto countBefore = Manager::getSingleton()->getSceneNodes().count();

    Manager::getSingleton()->destroySceneNode("Cube");

    auto countAfter = Manager::getSingleton()->getSceneNodes().count();

    ASSERT_EQ(countBefore-1,countAfter);

    PrimitiveObject::createCube("Cube");
    Manager::getSingleton()->destroySceneNode("Cube");
    ASSERT_EQ(countBefore-1,countAfter);
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

TEST_F(PrimitivesWidgetTest, CreateSpring)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createSpring("Spring");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Spring");
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

TEST_F(PrimitivesWidgetTest, UpdateParamsFromUi)
{
    PrimitivesWidget widget;
    auto sizeXLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeX");
    ASSERT_TRUE(sizeXLineEdit != nullptr);

    auto sizeYLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeY");
    ASSERT_TRUE(sizeYLineEdit != nullptr);

    auto sizeZLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeZ");
    ASSERT_TRUE(sizeZLineEdit != nullptr);

    auto edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    ASSERT_TRUE(edit_radius != nullptr);

    auto edit_radius2 = widget.findChild<QDoubleSpinBox*>("edit_radius2");
    ASSERT_TRUE(edit_radius2 != nullptr);

    auto edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");
    ASSERT_TRUE(edit_height != nullptr);

    auto edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    ASSERT_TRUE(edit_numSegX != nullptr);

    auto edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");
    ASSERT_TRUE(edit_numSegY != nullptr);

    auto edit_numSegZ = widget.findChild<QSpinBox*>("edit_numSegZ");
    ASSERT_TRUE(edit_numSegZ != nullptr);

    auto edit_UTile = widget.findChild<QDoubleSpinBox*>("edit_UTile");
    ASSERT_TRUE(edit_UTile != nullptr);

    auto edit_VTile = widget.findChild<QDoubleSpinBox*>("edit_VTile");
    ASSERT_TRUE(edit_VTile != nullptr);

    auto pb_switchUV = widget.findChild<QPushButton*>("pb_switchUV");
    ASSERT_TRUE(pb_switchUV != nullptr);

    sizeXLineEdit->setValue(1.0);
    sizeYLineEdit->setValue(2.0);
    sizeZLineEdit->setValue(3.0);
    edit_radius->setValue(1.0);
    edit_radius2->setValue(2.0);
    edit_height->setValue(3.0);
    edit_numSegX->setValue(4);
    edit_numSegY->setValue(5);
    edit_numSegZ->setValue(6);
    edit_UTile->setValue(7.0);
    edit_VTile->setValue(8.0);
    pb_switchUV->click();

    ASSERT_EQ(sizeXLineEdit->value(), 1.0);
    ASSERT_EQ(sizeYLineEdit->value(), 2.0);
    ASSERT_EQ(sizeZLineEdit->value(), 3.0);
    ASSERT_EQ(edit_radius->value(), 1.0);
    ASSERT_EQ(edit_radius2->value(), 2.0);
    ASSERT_EQ(edit_height->value(), 3.0);
    ASSERT_EQ(edit_numSegX->value(), 4);
    ASSERT_EQ(edit_numSegY->value(), 5);
    ASSERT_EQ(edit_numSegZ->value(), 6);
    ASSERT_EQ(edit_UTile->value(), 7.0);
    ASSERT_EQ(edit_VTile->value(), 8.0);
    ASSERT_TRUE(pb_switchUV->isChecked());
}

TEST_F(PrimitivesWidgetTest, UpdateParamsFromUiForACube)
{
    PrimitivesWidget widget;
    PrimitiveObject::createCube("Cube");
    auto cube = widget.getSelectedPrimitiveList()[0];

    auto sizeXLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeX");
    auto sizeYLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeY");
    auto sizeZLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeZ");
    auto edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto edit_radius2 = widget.findChild<QDoubleSpinBox*>("edit_radius2");
    auto edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");
    auto edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    auto edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");
    auto edit_numSegZ = widget.findChild<QSpinBox*>("edit_numSegZ");
    auto edit_UTile = widget.findChild<QDoubleSpinBox*>("edit_UTile");
    auto edit_VTile = widget.findChild<QDoubleSpinBox*>("edit_VTile");
    auto pb_switchUV = widget.findChild<QPushButton*>("pb_switchUV");

    sizeXLineEdit->setValue(1.0);
    sizeYLineEdit->setValue(2.0);
    sizeZLineEdit->setValue(3.0);
    edit_radius->setValue(1.0);
    edit_radius2->setValue(2.0);
    edit_height->setValue(3.0);
    edit_numSegX->setValue(4);
    edit_numSegY->setValue(5);
    edit_numSegZ->setValue(6);
    edit_UTile->setValue(7.0);
    edit_VTile->setValue(8.0);
    pb_switchUV->click();

    ASSERT_EQ(cube->getType(), PrimitiveObject::PrimitiveType::AP_CUBE);
    ASSERT_EQ(cube->getSizeX(), 1.0);
    ASSERT_EQ(cube->getSizeY(), 2.0);
    ASSERT_EQ(cube->getSizeZ(), 3.0);
    ASSERT_EQ(cube->getRadius(), 1.0);
    ASSERT_EQ(cube->getSectionRadius(), 0);
    ASSERT_EQ(cube->getHeight(), 3.0);
    ASSERT_EQ(cube->getNumSegX(), 4);
    ASSERT_EQ(cube->getNumSegY(), 5);
    ASSERT_EQ(cube->getNumSegZ(), 6);
    ASSERT_EQ(cube->getUTile(), 7.0);
    ASSERT_EQ(cube->getVTile(), 8.0);
    ASSERT_TRUE(cube->hasUVSwitched());

    Manager::getSingleton()->destroySceneNode("Cube");
}
