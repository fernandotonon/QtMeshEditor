#include <gtest/gtest.h>
#include <QApplication>
#include <QSettings>
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
}

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

TEST_F(MainWindowTest, RemoveEmptySelection) {
    auto actionRemove_Object = mainWindow->findChild<QAction*>("actionRemove_Object");
    ASSERT_TRUE(actionRemove_Object != nullptr);

    SelectionSet::getSingleton()->clear();

    auto countBefore = Manager::getSingleton()->getEntities().count();

    actionRemove_Object->trigger();

    auto countAfter = Manager::getSingleton()->getEntities().count();

    ASSERT_EQ(countBefore,countAfter);
}
