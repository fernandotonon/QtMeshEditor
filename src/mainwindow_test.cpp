#include <gtest/gtest.h>
#include <QApplication>
#include <QToolBar>
#include <QStatusBar>
#include <QSettings>
#include <QDockWidget>
#include <QMimeData>
#include <QDropEvent>
#include <QMouseEvent>
#include "PrimitiveObject.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "mainwindow.h"

class MainWindowTest : public ::testing::Test {
protected:
    QApplication* app;
    MainWindow* mainWindow;

    void SetUp() override {
        // Create a QApplication instance for testing
        int argc = 0;
        char* argv[] = { nullptr };
        app = new QApplication(argc, argv);

        mainWindow = new MainWindow();
    }

    void TearDown() override {
        delete mainWindow;
        delete app;
    }
};

TEST_F(MainWindowTest, ChooseDarkPalette) {
    auto paletteAction = mainWindow->findChild<QAction*>("actionDark");
    ASSERT_TRUE(paletteAction != nullptr);

    // Trigger the action
    paletteAction->toggle();

    // There's no unchecking
    paletteAction->toggle();
    ASSERT_TRUE(paletteAction->isChecked());

    QSettings settings;
    auto selectedFalette = settings.value("palette");
    EXPECT_EQ(selectedFalette, "dark");
}

TEST_F(MainWindowTest, ChooseLightPalette) {
    auto paletteAction = mainWindow->findChild<QAction*>("actionLight");
    ASSERT_TRUE(paletteAction != nullptr);

    // Trigger the action
    paletteAction->toggle();

    // There's no unchecking
    paletteAction->toggle();
    ASSERT_TRUE(paletteAction->isChecked());

    QSettings settings;
    auto selectedFalette = settings.value("palette");
    EXPECT_EQ(selectedFalette, "light");
}
/*
TEST_F(MainWindowTest, ChooseCustomPalette) {
    auto paletteAction = mainWindow->findChild<QAction*>("actionCustom");
    ASSERT_TRUE(paletteAction != nullptr);

    // Trigger the action
    paletteAction->toggle();

    // Check if the color dialog is open
    auto colorDialog = mainWindow->findChild<QColorDialog*>("Custom Color Dialog");
    ASSERT_TRUE(colorDialog != nullptr);
    ASSERT_TRUE(colorDialog->isVisible());

    colorDialog->setCurrentColor(QColor(125,122,123));
    colorDialog->accept();

    QSettings settings;
    auto selectedPalette = settings.value("palette");
    EXPECT_EQ(selectedPalette, "custom");

    auto customPaletteColor = settings.value("customPalette");
    EXPECT_EQ(customPaletteColor, QColor(125,122,123));

    // There's no unchecking
    paletteAction->toggle();
    ASSERT_TRUE(paletteAction->isChecked());
} this is causing GHActions to fail*/

TEST_F(MainWindowTest, ChooseAmbientLight) {
    auto actionButton = mainWindow->findChild<QAction*>("actionChange_Ambient_Light");
    ASSERT_TRUE(actionButton != nullptr);

    // Trigger the action
    actionButton->trigger();

    auto colorDialog = mainWindow->findChild<QColorDialog*>("Ambient Light Color Dialog");
    ASSERT_TRUE(colorDialog != nullptr);

    auto testColor = QColor(125,122,123);
    colorDialog->setCurrentColor(testColor);
    colorDialog->accept();

    Ogre::ColourValue ambientLightColour = Manager::getSingleton()->getSceneMgr()->getAmbientLight();
    EXPECT_EQ(ambientLightColour.r, testColor.redF());
    EXPECT_EQ(ambientLightColour.g, testColor.greenF());
    EXPECT_EQ(ambientLightColour.b, testColor.blueF());
}
/*
TEST_F(MainWindowTest, ChooseViewportOptions) {
    auto actionSingle = mainWindow->findChild<QAction*>("actionSingle");
    auto actionSideBySide = mainWindow->findChild<QAction*>("action1x1_Side_by_Side");
    auto actionUpperLower = mainWindow->findChild<QAction*>("action1x1_Upper_and_Lower");
    ASSERT_TRUE(actionSingle != nullptr);
    ASSERT_TRUE(actionSideBySide != nullptr);
    ASSERT_TRUE(actionUpperLower != nullptr);

    actionSingle->toggle();

    // There's no unchecking
    actionSingle->toggle();
    ASSERT_TRUE(actionSingle->isChecked());

    ASSERT_TRUE(actionSingle->isChecked());
    ASSERT_FALSE(actionSideBySide->isChecked());
    ASSERT_FALSE(actionUpperLower->isChecked());

    actionSideBySide->toggle();

    // There's no unchecking
    actionSideBySide->toggle();
    ASSERT_TRUE(actionSideBySide->isChecked());

    ASSERT_FALSE(actionSingle->isChecked());
    ASSERT_TRUE(actionSideBySide->isChecked());
    ASSERT_FALSE(actionUpperLower->isChecked());

    actionUpperLower->toggle();

    // There's no unchecking
    actionUpperLower->toggle();
    ASSERT_TRUE(actionUpperLower->isChecked());

    ASSERT_FALSE(actionSingle->isChecked());
    ASSERT_FALSE(actionSideBySide->isChecked());
    ASSERT_TRUE(actionUpperLower->isChecked());
} failing on GH Actions */

TEST_F(MainWindowTest, AddViewport) {
    auto actionAddViewport = mainWindow->findChild<QAction*>("actionAdd_Viewport");
    auto actionSingle = mainWindow->findChild<QAction*>("actionSingle");
    auto actionSideBySide = mainWindow->findChild<QAction*>("action1x1_Side_by_Side");
    auto actionUpperLower = mainWindow->findChild<QAction*>("action1x1_Upper_and_Lower");
    ASSERT_TRUE(actionAddViewport != nullptr);
    ASSERT_TRUE(actionSingle != nullptr);
    ASSERT_TRUE(actionSideBySide != nullptr);
    ASSERT_TRUE(actionUpperLower != nullptr);

    actionAddViewport->toggle();


    ASSERT_FALSE(actionSingle->isChecked());
    ASSERT_FALSE(actionSideBySide->isChecked());
    ASSERT_FALSE(actionUpperLower->isChecked());
}

TEST_F(MainWindowTest, SelectTranslateRotate) {
    auto actionSelect_Object = mainWindow->findChild<QAction*>("actionSelect_Object");
    auto actionTranslate_Object = mainWindow->findChild<QAction*>("actionTranslate_Object");
    auto actionRotate_Object = mainWindow->findChild<QAction*>("actionRotate_Object");
    ASSERT_TRUE(actionSelect_Object != nullptr);
    ASSERT_TRUE(actionTranslate_Object != nullptr);
    ASSERT_TRUE(actionRotate_Object != nullptr);

    // SELECT
    actionSelect_Object->trigger();

    ASSERT_TRUE(actionSelect_Object->isChecked());
    ASSERT_FALSE(actionTranslate_Object->isChecked());
    ASSERT_FALSE(actionRotate_Object->isChecked());

    // There's no unchecking
    actionSelect_Object->trigger();
    ASSERT_TRUE(actionSelect_Object->isChecked());

    // TRANSLATE
    actionTranslate_Object->trigger();

    ASSERT_FALSE(actionSelect_Object->isChecked());
    ASSERT_TRUE(actionTranslate_Object->isChecked());
    ASSERT_FALSE(actionRotate_Object->isChecked());

    // There's no unchecking
    actionTranslate_Object->trigger();
    ASSERT_TRUE(actionTranslate_Object->isChecked());

    // ROTATE
    actionRotate_Object->trigger();

    ASSERT_FALSE(actionSelect_Object->isChecked());
    ASSERT_FALSE(actionTranslate_Object->isChecked());
    ASSERT_TRUE(actionRotate_Object->isChecked());

    // There's no unchecking
    actionRotate_Object->trigger();
    ASSERT_TRUE(actionRotate_Object->isChecked());
}

TEST_F(MainWindowTest, SelectTranslateRotateShortcut) {
    auto actionSelect_Object = mainWindow->findChild<QAction*>("actionSelect_Object");
    auto actionTranslate_Object = mainWindow->findChild<QAction*>("actionTranslate_Object");
    auto actionRotate_Object = mainWindow->findChild<QAction*>("actionRotate_Object");
    ASSERT_TRUE(actionSelect_Object != nullptr);
    ASSERT_TRUE(actionTranslate_Object != nullptr);
    ASSERT_TRUE(actionRotate_Object != nullptr);

    // ROTATE
    // mock pressing R key in mainwindow
    auto event = new QKeyEvent(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier);
    mainWindow->keyPressEvent(event);

    ASSERT_FALSE(actionSelect_Object->isChecked());
    ASSERT_FALSE(actionTranslate_Object->isChecked());
    ASSERT_TRUE(actionRotate_Object->isChecked());

    // There's no unchecking
    mainWindow->keyPressEvent(event);
    ASSERT_TRUE(actionRotate_Object->isChecked());

    // SELECT
    event = new QKeyEvent(QEvent::KeyPress, Qt::Key_Y, Qt::NoModifier);
    mainWindow->keyPressEvent(event);

    ASSERT_TRUE(actionSelect_Object->isChecked());
    ASSERT_FALSE(actionTranslate_Object->isChecked());
    ASSERT_FALSE(actionRotate_Object->isChecked());

    // There's no unchecking
    mainWindow->keyPressEvent(event);
    ASSERT_TRUE(actionSelect_Object->isChecked());

    // TRANSLATE
    event = new QKeyEvent(QEvent::KeyPress, Qt::Key_T, Qt::NoModifier);
    mainWindow->keyPressEvent(event);

    ASSERT_FALSE(actionSelect_Object->isChecked());
    ASSERT_TRUE(actionTranslate_Object->isChecked());
    ASSERT_FALSE(actionRotate_Object->isChecked());

    // There's no unchecking
    mainWindow->keyPressEvent(event);
    ASSERT_TRUE(actionTranslate_Object->isChecked());

    // Other key
    event = new QKeyEvent(QEvent::KeyPress, Qt::Key_P, Qt::NoModifier);
    mainWindow->keyPressEvent(event);

    // Keeps the previous status
    ASSERT_FALSE(actionSelect_Object->isChecked());
    ASSERT_TRUE(actionTranslate_Object->isChecked());
    ASSERT_FALSE(actionRotate_Object->isChecked());
}

TEST_F(MainWindowTest, RemoveEmptySelection) {
    auto actionRemove_Object = mainWindow->findChild<QAction*>("actionRemove_Object");
    ASSERT_TRUE(actionRemove_Object != nullptr);

    SelectionSet::getSingleton()->clear();

    auto countBefore = Manager::getSingleton()->getEntities().count();

    actionRemove_Object->trigger();

    auto countAfter = Manager::getSingleton()->getEntities().count();

    ASSERT_EQ(countBefore,countAfter);
}

TEST_F(MainWindowTest, RemoveEmptySelectionShortcut) {
    SelectionSet::getSingleton()->clear();

    auto countBefore = Manager::getSingleton()->getEntities().count();

    auto event = new QKeyEvent(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    mainWindow->keyPressEvent(event);

    auto countAfter = Manager::getSingleton()->getEntities().count();

    ASSERT_EQ(countBefore,countAfter);
}

TEST_F(MainWindowTest, RemoveSelectedSceneNodeShortcut) {
    auto sceneNodeName = "TestSceneNode";
    auto sceneNode = Manager::getSingleton()->addSceneNode(sceneNodeName);
    auto countBefore = Manager::getSingleton()->getSceneNodes().count();

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(sceneNode);

    auto event = new QKeyEvent(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    mainWindow->keyPressEvent(event);

    auto countAfter = Manager::getSingleton()->getSceneNodes().count();

    ASSERT_EQ(countBefore-1,countAfter);

    for (auto node : Manager::getSingleton()->getSceneNodes()) {
        ASSERT_NE(node->getName(), sceneNodeName);
    }
}

TEST_F(MainWindowTest, RemoveAndRecreateSceneNode) {
    auto actionRemove_Object = mainWindow->findChild<QAction*>("actionRemove_Object");
    ASSERT_TRUE(actionRemove_Object != nullptr);

    auto sceneNodeName = "TestSceneNode";
    auto sceneNode = Manager::getSingleton()->addSceneNode(sceneNodeName);
    auto countBefore = Manager::getSingleton()->getSceneNodes().count();

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(sceneNode);

    actionRemove_Object->trigger();

    auto countAfter = Manager::getSingleton()->getSceneNodes().count();

    ASSERT_EQ(countBefore-1,countAfter);

    for (auto node : Manager::getSingleton()->getSceneNodes()) {
        ASSERT_NE(node->getName(), sceneNodeName);
    }

    sceneNode = Manager::getSingleton()->addSceneNode(sceneNodeName);
    SelectionSet::getSingleton()->selectOne(sceneNode);
    actionRemove_Object->trigger();
    ASSERT_EQ(countBefore-1,countAfter);
}

TEST_F(MainWindowTest, ShowHideObjectsToolbar) {
    mainWindow->setVisible(true);
    auto actionObjectsToolbar = mainWindow->findChild<QAction*>("actionObjects_Toolbar");
    ASSERT_TRUE(actionObjectsToolbar != nullptr);

    auto objectsToolbar = mainWindow->findChild<QToolBar*>("objectsToolbar");
    ASSERT_TRUE(objectsToolbar != nullptr);

    actionObjectsToolbar->toggle();

    ASSERT_FALSE(actionObjectsToolbar->isChecked());
    ASSERT_FALSE(objectsToolbar->isVisible());

    actionObjectsToolbar->toggle();

    ASSERT_TRUE(actionObjectsToolbar->isChecked());
    ASSERT_TRUE(objectsToolbar->isVisible());
}

TEST_F(MainWindowTest, ShowHideToolsToolbar) {
    mainWindow->setVisible(true);
    auto actionToolsToolbar = mainWindow->findChild<QAction*>("actionTools_Toolbar");
    ASSERT_TRUE(actionToolsToolbar != nullptr);

    auto toolsToolbar = mainWindow->findChild<QToolBar*>("toolToolbar");
    ASSERT_TRUE(toolsToolbar != nullptr);

    actionToolsToolbar->toggle();

    ASSERT_FALSE(actionToolsToolbar->isChecked());
    ASSERT_FALSE(toolsToolbar->isVisible());

    actionToolsToolbar->toggle();

    ASSERT_TRUE(actionToolsToolbar->isChecked());
    ASSERT_TRUE(toolsToolbar->isVisible());
}

TEST_F(MainWindowTest, ShowHideMeshEditor) {
    mainWindow->setVisible(true);
    auto actionMeshEditor = mainWindow->findChild<QAction*>("actionMeshEditor");
    ASSERT_TRUE(actionMeshEditor != nullptr);
    ASSERT_TRUE(actionMeshEditor->isChecked());

    auto meshEditor = mainWindow->findChild<QDockWidget*>("meshEditorWidget");
    ASSERT_TRUE(meshEditor != nullptr);

    actionMeshEditor->toggle();
    ASSERT_FALSE(actionMeshEditor->isChecked());
    ASSERT_FALSE(meshEditor->isVisible());

    actionMeshEditor->toggle();
    ASSERT_TRUE(actionMeshEditor->isChecked());
    ASSERT_TRUE(meshEditor->isVisible());
}

TEST_F(MainWindowTest, NavigateTabWidget) {
    mainWindow->setVisible(true);
    auto tabWidget = mainWindow->findChild<QTabWidget*>("tabWidget");
    ASSERT_TRUE(tabWidget != nullptr);

    auto transformTab = tabWidget->widget(0);
    auto materialTab = tabWidget->widget(1);
    auto editTab = tabWidget->widget(2);
    auto animationTab = tabWidget->widget(3);
    ASSERT_TRUE(transformTab != nullptr);
    ASSERT_TRUE(materialTab != nullptr);
    ASSERT_TRUE(editTab != nullptr);
    ASSERT_TRUE(animationTab != nullptr);

    ASSERT_FALSE(animationTab->isVisible());
    tabWidget->setCurrentIndex(3);
    ASSERT_TRUE(animationTab->isVisible());
} 

TEST_F(MainWindowTest, FrameRendering) {
    auto statusBar = mainWindow->findChild<QStatusBar*>("statusBar");
    ASSERT_TRUE(statusBar != nullptr);

    auto message = statusBar->currentMessage();
    ASSERT_EQ(message, "");

    Manager::getSingleton()->getRoot()->renderOneFrame();

    message = statusBar->currentMessage();
    ASSERT_TRUE(message.startsWith("Status "));
}

/*
TEST_F(MainWindowTest, OpenMaterialWindow) {
    auto actionMaterial_Editor = mainWindow->findChild<QAction*>("actionMaterial_Editor");
    ASSERT_TRUE(actionMaterial_Editor != nullptr);

    int childrenBefore = mainWindow->children().size();

    actionMaterial_Editor->trigger();

    int childrenAfter = mainWindow->children().size();
    ASSERT_EQ(childrenBefore, childrenAfter-1);
} segfault on GH Actions */

TEST_F(MainWindowTest, OpenAbout) {
    auto actionAbout = mainWindow->findChild<QAction*>("actionAbout");
    ASSERT_TRUE(actionAbout != nullptr);

    int childrenBefore = mainWindow->children().size();

    actionAbout->trigger();

    int childrenAfter = mainWindow->children().size();
    ASSERT_EQ(childrenBefore, childrenAfter-1);
}

TEST_F(MainWindowTest, DropEvent) {
    auto entities = Manager::getSingleton()->getEntities();
    int countBefore = entities.count();
    // Create a QDropEvent instance for testing
    QMimeData* mimeData = new QMimeData();
    QDropEvent* event = new QDropEvent(QPoint(), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);

    // Set the mime data with a valid URI
    QString validUri = "file:///./media/models/ninja.mesh\nfile:///./media/models/robot.mesh\nfile:///./media/models/Rumba%20Dancing.fbx";
    mimeData->setData("text/uri-list", validUri.toUtf8());

    // Call the dropEvent method
    mainWindow->dropEvent(event);

    Manager::getSingleton()->getRoot()->renderOneFrame();

    // Verify that the file is loaded
    entities = Manager::getSingleton()->getEntities();
    ASSERT_EQ(entities.count(), countBefore+3);

    // Set the mime data with an invalid URI
    QString invalidUri = "file:///./UnitTests";
    mimeData->setData("text/uri-list", invalidUri.toUtf8());

    // Call the dropEvent method again
    mainWindow->dropEvent(event);

    Manager::getSingleton()->getRoot()->renderOneFrame();

    // Verify that the file is not loaded
    entities = Manager::getSingleton()->getEntities();
    ASSERT_EQ(entities.count(), countBefore+3);

    // Set the mime data with another type of data
    QString other = "asd";
    mimeData->setData("text", other.toUtf8());

    // Call the dropEvent method again
    mainWindow->dropEvent(event);

    Manager::getSingleton()->getRoot()->renderOneFrame();

    // Verify that the file is not loaded
    entities = Manager::getSingleton()->getEntities();
    ASSERT_EQ(entities.count(), countBefore+3);
}
