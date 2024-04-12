#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <QApplication>
#include <QPalette>
#include <QDebug>
#include <QTimer>
#include <QStyleFactory>
#include <QSettings>
#include "mainwindow.h"
#include "Manager.h"

using ::testing::Mock;

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
    
    // Clear the mock object
    Mock::VerifyAndClear(&mockQApplication);
}

//Test creating mainwindow passing a list of URIs to import as startup arguments
TEST(MainTest, ImportMeshs) {
    int before = Manager::getSingleton()->getEntities().count();
    int argc = 2;
    char* argv[] = { "./media/models/ninja.mesh", "./media/models/robot.mesh" };
    QApplication app(argc, argv);
    MainWindow mainWindow;
    Manager::getSingleton()->getRoot()->renderOneFrame();
    int after = Manager::getSingleton()->getEntities().count();
    ASSERT_EQ(after, before+3);
}
