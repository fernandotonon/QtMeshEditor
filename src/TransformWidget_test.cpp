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

    void SetUp() override {
        // Create the QApplication
        int argc = 0;
        char** argv = nullptr;
        app = new QApplication(argc, argv);

        // Create a main window to hold the widget
        mainWindow = Manager::getSingleton()->getMainWindow();

        // Create the transform widget
        transformWidget = new TransformWidget(mainWindow);
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
