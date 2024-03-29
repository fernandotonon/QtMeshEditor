#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "Manager.h"
#include "GlobalDefinitions.h"
#include <QApplication>
#include "mainwindow.h"

using ::testing::Mock;

// Mock class for QApplication
class MockQApplication : public QApplication
{
public:
    MockQApplication(int& argc, char** argv) : QApplication(argc, argv) {}

    MOCK_METHOD(int, exec, ());
};

// Mock class for MainWindow
class MockMainWindow : public MainWindow
{
public:
    MockMainWindow(): MainWindow() {};
    virtual ~MockMainWindow() {};
    MOCK_METHOD(void, show, ());
};

TEST(ManagerTest, Forbidden_Name)
{
    // Create mock objects
    int argc = 0;
    char* argv[] = { nullptr };
    MockQApplication mockQApplication(argc, argv);

    Manager *manager = Manager::getSingleton(nullptr);
    EXPECT_EQ(manager->isForbiddenNodeName("Cube"), false);
    EXPECT_EQ(manager->isForbiddenNodeName("Cube_0"), false);
    EXPECT_EQ(manager->isForbiddenNodeName("Cube_1"), false);
    EXPECT_EQ(manager->isForbiddenNodeName("TPCameraChildSceneNode"), true);
    EXPECT_EQ(manager->isForbiddenNodeName("TPCameraChildSceneNode_0"), false);
    EXPECT_EQ(manager->isForbiddenNodeName("GridLine_node"), true);
    EXPECT_EQ(manager->isForbiddenNodeName("Unnamed_"), true);
    EXPECT_EQ(manager->isForbiddenNodeName(TRANSFORM_OBJECT_NAME), true);
    EXPECT_EQ(manager->isForbiddenNodeName(SELECTIONBOX_OBJECT_NAME), true);

    // Clear the mock object
    Mock::VerifyAndClear(&mockQApplication);
}

TEST(ManagerTest, CreateEmptyScene)
{ 
    // Create mock objects
    int argc = 0;
    char* argv[] = { nullptr };
    MockQApplication mockQApplication(argc, argv);
    MainWindow* mainWindow = new MainWindow(); // TODO: currently it is not possible to mock it twice, so we need to refactory it to be able to do it to test the other functions

    Manager *manager = Manager::getSingleton(mainWindow);
    manager->CreateEmptyScene();
    EXPECT_EQ(manager->getSceneNodes().size(), 3); // Root and the light
    EXPECT_EQ(manager->getEntities().size(), 2);
 
    // Clear the mock object
    Mock::VerifyAndClear(&mockQApplication);
    delete mainWindow;
}
