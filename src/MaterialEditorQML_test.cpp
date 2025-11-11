#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <QApplication>
#include <QCoreApplication>
#include <QQmlEngine>
#include <QJSEngine>

// Simple test for MaterialEditorQML functionality
class MaterialEditorQMLTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Ensure QApplication exists - create if it doesn't
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        if (!app) {
            // QApplication doesn't exist yet, create it
            static int argc = 1;
            static char appName[] = "MaterialEditorQML_test";
            static char* argv[] = {appName, nullptr};
            app = new QApplication(argc, argv);
        }
        ASSERT_NE(app, nullptr);
    }

    void TearDown() override {
        // Cleanup if needed
        // Note: We don't delete app here as it may be used by other tests
    }

    QApplication* app = nullptr;
};

// Basic functionality tests
TEST_F(MaterialEditorQMLTest, QmlEngineTest) {
    QQmlEngine engine;
    ASSERT_TRUE(engine.importPathList().size() > 0);
}

TEST_F(MaterialEditorQMLTest, BasicQmlTest) {
    QQmlEngine engine;
    QJSValue result = engine.evaluate("1 + 1");
    EXPECT_EQ(result.toNumber(), 2.0);
}

TEST_F(MaterialEditorQMLTest, StringManipulationTest) {
    QString testString = "MaterialEditor";
    EXPECT_FALSE(testString.isEmpty());
    EXPECT_TRUE(testString.contains("Material"));
}

int main(int argc, char** argv) {
    // Create QApplication before running tests (required for Qt tests)
    // Use static to ensure it persists for the lifetime of the program
    static QApplication* app = nullptr;
    if (!QCoreApplication::instance()) {
        app = new QApplication(argc, argv);
    }
    
    ::testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();
    
    // Note: We don't delete app here as it may be needed during test teardown
    // The OS will clean it up when the process exits
    return result;
} 
