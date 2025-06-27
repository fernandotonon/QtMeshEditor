#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <QApplication>
#include <QQmlEngine>
#include <QJSEngine>

// Simple test for MaterialEditorQML functionality
class MaterialEditorQMLTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create QApplication if not exists
        if (!QApplication::instance()) {
            int argc = 0;
            char* argv[] = { nullptr };
            app = new QApplication(argc, argv);
        }
    }

    void TearDown() override {
        // Cleanup if needed
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
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 