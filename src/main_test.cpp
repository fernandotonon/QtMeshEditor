#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <QApplication>
#include <QPalette>
#include <QDebug>
#include <QTimer>
#include <QStyleFactory>
#include <QSettings>
#include "mainwindow.h"

// Mock class for MainWindow
class MockMainWindow : public MainWindow
{
public:
    MOCK_METHOD(void, show, ());
};

// Mock class for QApplication
class MockQApplication : public QApplication
{
public:
    MockQApplication(int& argc, char** argv) : QApplication(argc, argv) {}

    MOCK_METHOD(int, exec, ());
};

// Test case using the mocked MainWindow and QApplication
TEST(MainTest, QApplicationAndMainWindowMock)
{
    int argc = 0;
    char* argv[] = { nullptr };

    // Create mock objects
    MockQApplication mockQApplication(argc, argv);
   // MockMainWindow mockMainWindow;

    // Set expectations
   // EXPECT_CALL(mockMainWindow, show()).Times(0);
    EXPECT_CALL(mockQApplication, exec()).Times(1);

    // Run the code under test
    int result = mockQApplication.exec();

    // Assertions on the result if needed
    EXPECT_EQ(result, 0);
}

int main(int argc, char* argv[])
{
    ::testing::InitGoogleMock(&argc, argv);
    return RUN_ALL_TESTS();
}