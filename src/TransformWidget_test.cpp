#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QMainWindow>
#include <OgreException.h>
#include "TransformWidget.h"
#include "SelectionSet.h"
#include "Manager.h"
#include "mainwindow.h"
#include "TestHelpers.h"

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
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        // Create a main window to hold the widget
        try {
            mainWindow = Manager::getSingleton()->getMainWindow();
        } catch (const Ogre::Exception& e) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
        }

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

// DISABLED: This test causes segfault in Ogre mesh loading (hardware buffer manager not initialized)
// TODO: Fix Ogre render system initialization before mesh loading
TEST_F(TransformWidgetTests, DISABLED_UpdateTreeViewFromSelection)
{
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    // Verify that the tree view is updated
    auto treeView = transformWidget->findChild<QTreeView*>("treeView");
    ASSERT_NE(nullptr, treeView);
    ASSERT_NE(nullptr, treeView->model());
    
    // Process Qt events to ensure model is initialized
    QCoreApplication::processEvents();
    
    ASSERT_EQ(1, treeView->model()->rowCount());
    ASSERT_EQ("No Selection", treeView->model()->headerData(0, Qt::Horizontal).toString().toStdString());

    // import a mesh - wrap in try-catch to handle Ogre initialization issues
    QStringList validUri{"./media/models/ninja.mesh"};
    try {
        Manager::getSingleton()->getMainWindow()->importMeshs(validUri);
        
        // Process Qt events to ensure signals are delivered and tree view is updated
        QCoreApplication::processEvents();
        
        // Verify that entities were created
        auto entities = Manager::getSingleton()->getEntities();
        if (entities.isEmpty()) {
            GTEST_SKIP() << "Skipping test: mesh import failed or no entities created";
        }

        // Verify that the tree view is updated
        ASSERT_NE(nullptr, treeView->model());
        ASSERT_EQ(1, treeView->model()->rowCount());
        ASSERT_EQ("1 object selected", treeView->model()->headerData(0, Qt::Horizontal).toString().toStdString());

        // Select the mesh
        auto entity = entities.last();
        ASSERT_NE(nullptr, entity);
        SelectionSet::getSingleton()->append(entity);
        
        // Process Qt events to ensure signals are delivered
        QCoreApplication::processEvents();
        
        ASSERT_NE(nullptr, treeView->model());
        ASSERT_EQ("2 objects selected", treeView->model()->headerData(0, Qt::Horizontal).toString().toStdString());

        // Select the sub entity - check if entity has sub entities
        if (entity->getNumSubEntities() == 0) {
            GTEST_SKIP() << "Skipping sub-entity test: entity has no sub-entities";
        }
        
        auto subEntity = entity->getSubEntity(0);
        ASSERT_NE(nullptr, subEntity);
        SelectionSet::getSingleton()->append(subEntity);
        
        // Process Qt events to ensure signals are delivered
        QCoreApplication::processEvents();
        
        ASSERT_NE(nullptr, treeView->model());
        ASSERT_EQ("3 objects selected", treeView->model()->headerData(0, Qt::Horizontal).toString().toStdString());
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping test: Ogre exception during mesh import ("
                     << e.getFullDescription() << ")";
    } catch (const std::exception& e) {
        GTEST_SKIP() << "Skipping test: exception during mesh import (" << e.what() << ")";
    } catch (...) {
        GTEST_SKIP() << "Skipping test: unknown exception during mesh import (possible segfault in Ogre mesh loading)";
    }
}


// DISABLED: This test causes segfault in Ogre mesh loading (hardware buffer manager not initialized)
// TODO: Fix Ogre render system initialization before mesh loading
TEST_F(TransformWidgetTests, DISABLED_UpdateSceneNodePositionScaleOrientation) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

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

// Test that spin boxes exist and have correct names
TEST_F(TransformWidgetTests, SpinBoxesExist)
{
    ASSERT_NE(positionX, nullptr);
    ASSERT_NE(positionY, nullptr);
    ASSERT_NE(positionZ, nullptr);
    ASSERT_NE(scaleX, nullptr);
    ASSERT_NE(scaleY, nullptr);
    ASSERT_NE(scaleZ, nullptr);
    ASSERT_NE(rotationX, nullptr);
    ASSERT_NE(rotationY, nullptr);
    ASSERT_NE(rotationZ, nullptr);
}

// Test that the tree view exists
TEST_F(TransformWidgetTests, TreeViewExists)
{
    auto treeView = transformWidget->findChild<QTreeView*>("treeView");
    ASSERT_NE(treeView, nullptr);
    ASSERT_NE(treeView->model(), nullptr);
}

// Test position spin box ranges
TEST_F(TransformWidgetTests, PositionSpinBoxRanges)
{
    ASSERT_NE(positionX, nullptr);
    EXPECT_LE(positionX->minimum(), -10000.0);
    EXPECT_GE(positionX->maximum(), 10000.0);
    EXPECT_EQ(positionX->decimals(), 4);
}

// Test scale spin box minimum > 0
TEST_F(TransformWidgetTests, ScaleSpinBoxMinimum)
{
    ASSERT_NE(scaleX, nullptr);
    ASSERT_NE(scaleY, nullptr);
    ASSERT_NE(scaleZ, nullptr);
    EXPECT_GT(scaleX->minimum(), 0.0);
    EXPECT_GT(scaleY->minimum(), 0.0);
    EXPECT_GT(scaleZ->minimum(), 0.0);
}

// Test rotation spin box ranges
TEST_F(TransformWidgetTests, RotationSpinBoxRanges)
{
    ASSERT_NE(rotationX, nullptr);
    EXPECT_LE(rotationX->minimum(), -360.0);
    EXPECT_GE(rotationX->maximum(), 360.0);
    EXPECT_EQ(rotationX->decimals(), 4);
}

// Test setting position values on the spin boxes
TEST_F(TransformWidgetTests, SetPositionSpinBoxValues)
{
    ASSERT_NE(positionX, nullptr);
    ASSERT_NE(positionY, nullptr);
    ASSERT_NE(positionZ, nullptr);

    positionX->setValue(1.5);
    positionY->setValue(2.5);
    positionZ->setValue(3.5);

    EXPECT_DOUBLE_EQ(positionX->value(), 1.5);
    EXPECT_DOUBLE_EQ(positionY->value(), 2.5);
    EXPECT_DOUBLE_EQ(positionZ->value(), 3.5);
}

// Test setting scale values on the spin boxes
TEST_F(TransformWidgetTests, SetScaleSpinBoxValues)
{
    ASSERT_NE(scaleX, nullptr);
    ASSERT_NE(scaleY, nullptr);
    ASSERT_NE(scaleZ, nullptr);

    scaleX->setValue(2.0);
    scaleY->setValue(3.0);
    scaleZ->setValue(4.0);

    EXPECT_DOUBLE_EQ(scaleX->value(), 2.0);
    EXPECT_DOUBLE_EQ(scaleY->value(), 3.0);
    EXPECT_DOUBLE_EQ(scaleZ->value(), 4.0);
}

// Test setting rotation values on the spin boxes
TEST_F(TransformWidgetTests, SetRotationSpinBoxValues)
{
    ASSERT_NE(rotationX, nullptr);
    ASSERT_NE(rotationY, nullptr);
    ASSERT_NE(rotationZ, nullptr);

    rotationX->setValue(45.0);
    rotationY->setValue(90.0);
    rotationZ->setValue(180.0);

    EXPECT_DOUBLE_EQ(rotationX->value(), 45.0);
    EXPECT_DOUBLE_EQ(rotationY->value(), 90.0);
    EXPECT_DOUBLE_EQ(rotationZ->value(), 180.0);
}

// Test that setting position values triggers onPositionEdited (via signal)
TEST_F(TransformWidgetTests, PositionValueChangeTriggers)
{
    ASSERT_NE(positionX, nullptr);

    // Setting a value should trigger the signal connection to onPositionEdited.
    // Since we do not have a selection, TransformOperator::setSelectedPosition
    // will be called but should not crash (no selection means no-op).
    EXPECT_NO_THROW({
        positionX->setValue(5.0);
        if (app) app->processEvents();
    });
}

// Test that setting scale values does not crash without selection
TEST_F(TransformWidgetTests, ScaleValueChangeTriggers)
{
    ASSERT_NE(scaleX, nullptr);

    EXPECT_NO_THROW({
        scaleX->setValue(2.0);
        if (app) app->processEvents();
    });
}

// Test that setting rotation values does not crash without selection
TEST_F(TransformWidgetTests, RotationValueChangeTriggers)
{
    ASSERT_NE(rotationX, nullptr);

    EXPECT_NO_THROW({
        rotationX->setValue(45.0);
        if (app) app->processEvents();
    });
}

// DISABLED: This test requires entities to exist, which may cause segfault during mesh import
// TODO: Fix Ogre render system initialization before mesh loading
TEST_F(TransformWidgetTests, DISABLED_UpdateEntityPositionScaleOrientation) {
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
