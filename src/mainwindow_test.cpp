#include <gtest/gtest.h>
#include <QApplication>
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

TEST_F(MainWindowTest, ChooseCustomPalette) {
    auto customPaletteAction = mainWindow->findChild<QAction*>("actionCustom");
    ASSERT_TRUE(customPaletteAction != nullptr);

    // Trigger the action
    customPaletteAction->toggle();

    // Check if the color dialog is open
    auto colorDialog = mainWindow->getCustomPaletteColorDialog();
    ASSERT_TRUE(colorDialog != nullptr);

    // Check if the color dialog is visible
    ASSERT_TRUE(colorDialog->isVisible());
}


