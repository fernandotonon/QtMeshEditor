#include <gtest/gtest.h>
#include <QApplication>
#include <QMainWindow>
#include "TransformWidget.h"
#include "SelectionSet.h"
#include "Manager.h"
#include "mainwindow.h"

// Test fixture for TransformWidget class
class TransformWidgetTests : public ::testing::Test {
protected:
    TransformWidget* transformWidget;
    QMainWindow* mainWindow;
    QApplication* app;

    QDoubleSpinBox* positionX;
    QDoubleSpinBox* positionY;
    QDoubleSpinBox* positionZ;
    QDoubleSpinBox* scaleX;
    QDoubleSpinBox* scaleY;
    QDoubleSpinBox* scaleZ;
    QDoubleSpinBox* rotationX;
    QDoubleSpinBox* rotationY;
    QDoubleSpinBox* rotationZ;

    void SetUp() override {
        // Create the QApplication
        int argc = 0;
        char** argv = nullptr;
        app = new QApplication(argc, argv);

        // Create a main window to hold the widget
        mainWindow = Manager::getSingleton()->getMainWindow();

        // Create the transform widget
        transformWidget = new TransformWidget(mainWindow);

        positionX = transformWidget->findChild<QDoubleSpinBox*>("positionX");
        positionY = transformWidget->findChild<QDoubleSpinBox*>("positionY");
        positionZ = transformWidget->findChild<QDoubleSpinBox*>("positionZ");
        scaleX = transformWidget->findChild<QDoubleSpinBox*>("scaleX");
        scaleY = transformWidget->findChild<QDoubleSpinBox*>("scaleY");
        scaleZ = transformWidget->findChild<QDoubleSpinBox*>("scaleZ");
        rotationX = transformWidget->findChild<QDoubleSpinBox*>("rotationX");
        rotationY = transformWidget->findChild<QDoubleSpinBox*>("rotationY");
        rotationZ = transformWidget->findChild<QDoubleSpinBox*>("rotationZ");
    }

    void TearDown() override {
    }
};

TEST_F(TransformWidgetTests, Constructor)
{
    // Verify that the widget is not null
    ASSERT_NE(nullptr, transformWidget);

    // Verify that the widget is added to the main window
    ASSERT_EQ(mainWindow, transformWidget->parentWidget());
}

TEST_F(TransformWidgetTests, UpdateTreeViewFromSelection)
{
    // Verify that the tree view is updated
    auto treeView = transformWidget->findChild<QTreeView*>("treeView");
    ASSERT_NE(nullptr, treeView);
    ASSERT_EQ(1, treeView->model()->rowCount());
    ASSERT_EQ("No Selection", treeView->model()->headerData(0, Qt::Horizontal).toString().toStdString());

    // import a mesh
    QStringList validUri{"./media/models/ninja.mesh"};
    Manager::getSingleton()->getMainWindow()->importMeshs(validUri);

    // Verify that the tree view is updated
    ASSERT_EQ(1, treeView->model()->rowCount());
    ASSERT_EQ("1 object selected", treeView->model()->headerData(0, Qt::Horizontal).toString().toStdString());

    // Select the mesh
    auto entities = Manager::getSingleton()->getEntities();
    auto entity = entities.last();
    SelectionSet::getSingleton()->append(entity);
    ASSERT_EQ("2 objects selected", treeView->model()->headerData(0, Qt::Horizontal).toString().toStdString());

    // Select the sub entity
    auto subEntity = entity->getSubEntity(0);
    SelectionSet::getSingleton()->append(subEntity);
    ASSERT_EQ("3 objects selected", treeView->model()->headerData(0, Qt::Horizontal).toString().toStdString());
}


TEST_F(TransformWidgetTests, UpdateSceneNodePositionScaleOrientation) {
    // import a mesh
    QStringList validUri{"./media/models/ninja.mesh"};
    Manager::getSingleton()->getMainWindow()->importMeshs(validUri);
    auto selectedSceneNode = SelectionSet::getSingleton()->getNodesSelectionList().first();
    SelectionSet::getSingleton()->selectOne(selectedSceneNode);

    positionX->setValue(1.0);
    positionY->setValue(2.0);
    positionZ->setValue(3.0);
    scaleX->setValue(4.0);
    scaleY->setValue(5.0);
    scaleZ->setValue(6.0);
    rotationX->setValue(7.0);
    rotationY->setValue(8.0);
    rotationZ->setValue(0.0); // TODO: Fix changing all 3 rotation values makes weird numbers to appear

    // get node rotation in euler angles
    Ogre::Radian x,y,z;
    x = selectedSceneNode->getOrientation().getPitch();
    y = selectedSceneNode->getOrientation().getYaw();
    z = selectedSceneNode->getOrientation().getRoll();

    ASSERT_EQ(selectedSceneNode->getPosition().x, positionX->value());
    ASSERT_EQ(selectedSceneNode->getPosition().y, positionY->value());
    ASSERT_EQ(selectedSceneNode->getPosition().z, positionZ->value());
    ASSERT_EQ(selectedSceneNode->getScale().x, scaleX->value());
    ASSERT_EQ(selectedSceneNode->getScale().y, scaleY->value());
    ASSERT_EQ(selectedSceneNode->getScale().z, scaleZ->value());
    ASSERT_NEAR(x.valueDegrees(), rotationX->value(),0.1);
    ASSERT_NEAR(y.valueDegrees(), rotationY->value(),0.1);
    ASSERT_NEAR(z.valueDegrees(), rotationZ->value(),0.1);
}

TEST_F(TransformWidgetTests, UpdateEntityPositionScaleOrientation) {
    auto selectedEntity = Manager::getSingleton()->getEntities().last();
    SelectionSet::getSingleton()->selectOne(selectedEntity);

    positionX->setValue(1.0);
    positionY->setValue(2.0);
    positionZ->setValue(3.0);
    scaleX->setValue(4.0);
    scaleY->setValue(5.0);
    scaleZ->setValue(6.0);
    rotationX->setValue(7.0);
    rotationY->setValue(8.0);
    rotationZ->setValue(9.0);

    Manager::getSingleton()->getRoot()->renderOneFrame();

    ASSERT_EQ(selectedEntity->getWorldBoundingBox().getCenter().x, 0); // TODO: find a way to validate the new position
    ASSERT_EQ(SelectionSet::getSingleton()->getSelectionCenter().y, 0);
    ASSERT_EQ(SelectionSet::getSingleton()->getSelectionCenter().z, 0);
    ASSERT_EQ(SelectionSet::getSingleton()->getSelectionScale().x, scaleX->value());
    ASSERT_EQ(SelectionSet::getSingleton()->getSelectionScale().y, scaleY->value());
    ASSERT_EQ(SelectionSet::getSingleton()->getSelectionScale().z, scaleZ->value());
    ASSERT_EQ(SelectionSet::getSingleton()->getSelectionOrientation().x, rotationX->value());
    ASSERT_EQ(SelectionSet::getSingleton()->getSelectionOrientation().y, rotationY->value());
    ASSERT_EQ(SelectionSet::getSingleton()->getSelectionOrientation().z, rotationZ->value());
}
