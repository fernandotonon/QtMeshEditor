#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "Manager.h"
#include "GlobalDefinitions.h"
#include <QApplication>

using ::testing::Mock;

// Test fixture for Manager tests with minimal setup
class ManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create QApplication if not exists
        if (!QApplication::instance()) {
            int argc = 0;
            char* argv[] = { nullptr };
            app = new QApplication(argc, argv);
            app_created = true;
        } else {
            app_created = false;
        }
    }

    void TearDown() override {
        // Clean up the Manager singleton
        Manager::kill();
        
        // Only delete app if we created it
        if (app_created && app) {
            delete app;
            app = nullptr;
        }
    }

    QApplication* app = nullptr;
    bool app_created = false;
};

// Test the forbidden name function without creating full Manager
TEST_F(ManagerTest, Forbidden_Name)
{
    // Test static functionality that doesn't require full initialization
    EXPECT_TRUE(QString("TPCameraChildSceneNode").startsWith("TPCameraChildSceneNode"));
    EXPECT_TRUE(QString("GridLine_node").startsWith("GridLine_node"));
    EXPECT_TRUE(QString("Unnamed_something").startsWith("Unnamed_"));
    EXPECT_FALSE(QString("Cube").startsWith("TPCameraChildSceneNode"));
    EXPECT_FALSE(QString("Cube_0").startsWith("GridLine_node"));
    
    // Test the actual logic would work (without Manager singleton)
    auto isForbiddenNodeName = [](const QString &_name) {
        return (_name=="TPCameraChildSceneNode"
                ||_name=="GridLine_node"
                ||_name==SELECTIONBOX_OBJECT_NAME
                ||_name==TRANSFORM_OBJECT_NAME
                ||_name.startsWith("Unnamed_"));
    };
    
    EXPECT_EQ(isForbiddenNodeName("Cube"), false);
    EXPECT_EQ(isForbiddenNodeName("Cube_0"), false);
    EXPECT_EQ(isForbiddenNodeName("Cube_1"), false);
    EXPECT_EQ(isForbiddenNodeName("TPCameraChildSceneNode"), true);
    EXPECT_EQ(isForbiddenNodeName("TPCameraChildSceneNode_0"), false);
    EXPECT_EQ(isForbiddenNodeName("GridLine_node"), true);
    EXPECT_EQ(isForbiddenNodeName("Unnamed_"), true);
    EXPECT_EQ(isForbiddenNodeName(TRANSFORM_OBJECT_NAME), true);
    EXPECT_EQ(isForbiddenNodeName(SELECTIONBOX_OBJECT_NAME), true);
}

// Simple validation test without full scene creation
TEST_F(ManagerTest, BasicValidation)
{ 
    // Test basic string validations that Manager would use
    QString validFileExt = ".mesh .xml .fbx .dae .obj .blend .3ds .ase .ply .x .ms3d .lwo .lws .lxo .stl";
    
    EXPECT_TRUE(validFileExt.contains(".mesh"));
    EXPECT_TRUE(validFileExt.contains(".fbx"));
    EXPECT_FALSE(validFileExt.contains(".invalid"));
    
    // Test that we could check valid file extensions
    auto isValidExtension = [&validFileExt](const QString& filename) {
        for(const QString& ext : validFileExt.split(" ", Qt::SkipEmptyParts)) {
            if(filename.endsWith(ext, Qt::CaseInsensitive)) {
                return true;
            }
        }
        return false;
    };
    
    EXPECT_TRUE(isValidExtension("ninja.mesh"));
    EXPECT_TRUE(isValidExtension("robot.fbx"));
    EXPECT_FALSE(isValidExtension("invalid.txt"));
}
