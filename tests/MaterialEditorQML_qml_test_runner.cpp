#include <QApplication>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickView>
#include <QTest>
// #include <QtQuickTest/quicktest.h> // Not needed for our custom test runner
#include <QQmlContext>
#include <QDir>
#include <QDebug>
#include <gtest/gtest.h>

// Simple QML Test Environment Setup without MaterialEditorQML dependencies
class QMLTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        // Set platform to offscreen if not already set (for CI environments)
        if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
    }

    void TearDown() override {
        // Cleanup if needed
    }
};

// Simple QML Test Fixture for basic QML functionality
class QMLTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        if (!QApplication::instance()) {
            int argc = 0;
            char* argv[] = { nullptr };
            app = std::make_unique<QApplication>(argc, argv);
        }

        engine = std::make_unique<QQmlEngine>();
    }

    void TearDown() override {
        engine.reset();
    }

protected:
    std::unique_ptr<QApplication> app;
    std::unique_ptr<QQmlEngine> engine;
};

// Test that basic QML environment can be set up
TEST_F(QMLTestFixture, QMLEngineBasic) {
    EXPECT_NE(engine.get(), nullptr);
    
    // Test basic QML evaluation
    QString qmlCode = R"(
        import QtQuick 2.15
        
        Item {
            property string testProperty: "QML Test Success"
            property int numberProperty: 42
            property bool boolProperty: true
        }
    )";

    QQmlComponent component(engine.get());
    component.setData(qmlCode.toUtf8(), QUrl("qrc:/basictest.qml"));
    
    EXPECT_FALSE(component.isError()) << "Basic QML component should compile without errors";
    
    if (!component.isError()) {
        QObject* item = component.create();
        EXPECT_NE(item, nullptr) << "QML item should be created";
        
        if (item) {
            EXPECT_EQ(item->property("testProperty").toString(), "QML Test Success");
            EXPECT_EQ(item->property("numberProperty").toInt(), 42);
            EXPECT_EQ(item->property("boolProperty").toBool(), true);
            delete item;
        }
    } else {
        qDebug() << "QML Compilation Errors:" << component.errors();
    }
}

// Test QML with JavaScript evaluation
TEST_F(QMLTestFixture, QMLJavaScriptEvaluation) {
    QString qmlCode = R"(
        import QtQuick 2.15
        
        Item {
            property string result: {
                var x = 10;
                var y = 20;
                return "Calculated: " + (x + y);
            }
            
            function calculate(a, b) {
                return a * b;
            }
        }
    )";

    QQmlComponent component(engine.get());
    component.setData(qmlCode.toUtf8(), QUrl("qrc:/jstest.qml"));
    
    EXPECT_FALSE(component.isError()) << "JavaScript QML component should compile";
    
    if (!component.isError()) {
        QObject* item = component.create();
        EXPECT_NE(item, nullptr);
        
        if (item) {
            EXPECT_EQ(item->property("result").toString(), "Calculated: 30");
            
            // Test calling QML function from C++
            QVariant result;
            QMetaObject::invokeMethod(item, "calculate", 
                                    Q_RETURN_ARG(QVariant, result),
                                    Q_ARG(QVariant, 5),
                                    Q_ARG(QVariant, 6));
            EXPECT_EQ(result.toInt(), 30);
            
            delete item;
        }
    }
}

// Test QML Timer (basic QtQuick functionality)
TEST_F(QMLTestFixture, QMLTimerTest) {
    QString qmlCode = R"(
        import QtQuick 2.15
        
        Item {
            property int timerCount: 0
            property bool timerTriggered: false
            
            Timer {
                interval: 10
                running: true
                onTriggered: {
                    parent.timerCount++;
                    parent.timerTriggered = true;
                }
            }
        }
    )";

    QQmlComponent component(engine.get());
    component.setData(qmlCode.toUtf8(), QUrl("qrc:/timertest.qml"));
    
    EXPECT_FALSE(component.isError()) << "Timer QML component should compile";
    
    if (!component.isError()) {
        QObject* item = component.create();
        EXPECT_NE(item, nullptr);
        
        if (item) {
            // Wait for timer to trigger
            QTest::qWait(50);
            
            EXPECT_GT(item->property("timerCount").toInt(), 0);
            EXPECT_TRUE(item->property("timerTriggered").toBool());
            
            delete item;
        }
    }
}

// Main function for standalone execution
int main(int argc, char *argv[])
{
    // Set platform to offscreen if not already set (for CI environments)
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    
    QApplication app(argc, argv);
    
    // Add our custom environment setup
    ::testing::AddGlobalTestEnvironment(new QMLTestEnvironment);
    
    // Initialize Google Test
    ::testing::InitGoogleTest(&argc, argv);
    
    // Run the tests
    int result = RUN_ALL_TESTS();
    
    return result;
} 