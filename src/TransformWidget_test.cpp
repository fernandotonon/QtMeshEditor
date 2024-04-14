#include <gtest/gtest.h>
#include <QApplication>
#include <QMainWindow>
#include "TransformWidget.h"

TEST(TransformWidgetTests, Constructor)
{
    // Create the QApplication
    int argc = 0;
    char** argv = nullptr;
    QApplication app(argc, argv);
    
    // Create a main window to hold the widget
    QMainWindow mainWindow;

    // Create the transform widget
    TransformWidget transformWidget(&mainWindow);

    // Verify that the widget is not null
    ASSERT_NE(nullptr, &transformWidget);

    // Verify that the widget is added to the main window
    ASSERT_EQ(&mainWindow, transformWidget.parentWidget());
}
