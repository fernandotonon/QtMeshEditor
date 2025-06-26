#include <QApplication>
#include <QQmlEngine>
#include <QQuickView>
#include <QTest>
// #include <QtQuickTest/quicktest.h> // Not needed for our custom test runner
#include <QQmlContext>
#include <QDir>
#include <QDebug>
#include <gtest/gtest.h>
#include "MaterialEditorQML.h"

// QML Test Environment Setup
class QMLTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        // Initialize QML environment
        qmlRegisterSingletonType<MaterialEditorQML>("MaterialEditorQML", 1, 0, "MaterialEditorQML",
            [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject * {
                Q_UNUSED(engine)
                Q_UNUSED(scriptEngine)
                return MaterialEditorQML::qmlInstance(engine, scriptEngine);
            }
        );
    }

    void TearDown() override {
        // Cleanup if needed
    }
};

// QML Test Fixture
class QMLTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        if (!QApplication::instance()) {
            int argc = 0;
            char* argv[] = { nullptr };
            app = std::make_unique<QApplication>(argc, argv);
        }

        engine = std::make_unique<QQmlEngine>();
        
        // Register MaterialEditorQML if not already registered
        static bool registered = false;
        if (!registered) {
            qmlRegisterSingletonType<MaterialEditorQML>("MaterialEditorQML", 1, 0, "MaterialEditorQML",
                [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject * {
                    Q_UNUSED(engine)
                    Q_UNUSED(scriptEngine)
                    return MaterialEditorQML::qmlInstance(engine, scriptEngine);
                }
            );
            registered = true;
        }

        materialEditor = MaterialEditorQML::qmlInstance(engine.get(), nullptr);
        engine->rootContext()->setContextProperty("MaterialEditorQML", materialEditor);
    }

    void TearDown() override {
        engine.reset();
        materialEditor = nullptr;
    }

protected:
    std::unique_ptr<QApplication> app;
    std::unique_ptr<QQmlEngine> engine;
    MaterialEditorQML* materialEditor;
};

// Test that QML test environment can be set up
TEST_F(QMLTestFixture, QMLEnvironmentSetup) {
    EXPECT_NE(engine.get(), nullptr);
    EXPECT_NE(materialEditor, nullptr);
    
    // Test that MaterialEditorQML is accessible in QML context
    QObject* contextProperty = engine->rootContext()->contextProperty("MaterialEditorQML").value<QObject*>();
    EXPECT_EQ(contextProperty, materialEditor);
}

// Test loading and running QML test component
TEST_F(QMLTestFixture, LoadQMLTestComponent) {
    QString qmlTestCode = R"(
        import QtQuick 2.15
        import QtTest 1.15
        import MaterialEditorQML 1.0
        
        TestCase {
            name: "BasicQMLTest"
            
            function test_materialEditorAccess() {
                verify(MaterialEditorQML !== undefined, "MaterialEditorQML should be available")
                verify(typeof MaterialEditorQML.materialName === "string", "materialName should be string")
                verify(typeof MaterialEditorQML.testConnection === "function", "testConnection should be function")
            }
            
            function test_basicFunctionality() {
                var result = MaterialEditorQML.testConnection()
                compare(result, "C++ method called successfully!", "Connection test should work")
                
                MaterialEditorQML.createNewMaterial("QMLTestMaterial")
                compare(MaterialEditorQML.materialName, "QMLTestMaterial", "Material creation should work")
            }
        }
    )";

    QQmlComponent component(engine.get());
    component.setData(qmlTestCode.toUtf8(), QUrl("qrc:/test.qml"));
    
    EXPECT_FALSE(component.isError()) << "QML component should compile without errors";
    
    if (!component.isError()) {
        QObject* testObject = component.create();
        EXPECT_NE(testObject, nullptr) << "QML test object should be created";
        
        if (testObject) {
            // The TestCase will automatically run its test functions
            QTest::qWait(100); // Give it time to run
            delete testObject;
        }
    } else {
        qDebug() << "QML Compilation Errors:" << component.errors();
    }
}

// Test QML property bindings
TEST_F(QMLTestFixture, QMLPropertyBindings) {
    QString qmlCode = R"(
        import QtQuick 2.15
        import MaterialEditorQML 1.0
        
        Item {
            property string boundMaterialName: MaterialEditorQML.materialName
            property bool boundLightingEnabled: MaterialEditorQML.lightingEnabled
            property real boundDiffuseAlpha: MaterialEditorQML.diffuseAlpha
        }
    )";

    QQmlComponent component(engine.get());
    component.setData(qmlCode.toUtf8(), QUrl("qrc:/bindingtest.qml"));
    
    EXPECT_FALSE(component.isError()) << "Property binding QML should compile";
    
    if (!component.isError()) {
        QObject* item = component.create();
        EXPECT_NE(item, nullptr);
        
        if (item) {
            // Test initial binding values
            EXPECT_EQ(item->property("boundMaterialName").toString(), materialEditor->materialName());
            EXPECT_EQ(item->property("boundLightingEnabled").toBool(), materialEditor->lightingEnabled());
            
            // Change values and test binding updates
            materialEditor->setMaterialName("BindingTestMaterial");
            materialEditor->setLightingEnabled(!materialEditor->lightingEnabled());
            materialEditor->setDiffuseAlpha(0.75f);
            
            QTest::qWait(50); // Allow bindings to update
            
            EXPECT_EQ(item->property("boundMaterialName").toString(), "BindingTestMaterial");
            EXPECT_EQ(item->property("boundLightingEnabled").toBool(), materialEditor->lightingEnabled());
            EXPECT_FLOAT_EQ(item->property("boundDiffuseAlpha").toFloat(), 0.75f);
            
            delete item;
        }
    }
}

// Test QML method invocation
TEST_F(QMLTestFixture, QMLMethodInvocation) {
    QString qmlCode = R"(
        import QtQuick 2.15
        import MaterialEditorQML 1.0
        
        Item {
            property var polygonModes: MaterialEditorQML.getPolygonModeNames()
            property var blendFactors: MaterialEditorQML.getBlendFactorNames()
            property string connectionResult: MaterialEditorQML.testConnection()
            
            Component.onCompleted: {
                MaterialEditorQML.setLightingEnabled(false)
                MaterialEditorQML.setDiffuseAlpha(0.5)
                MaterialEditorQML.createNewMaterial("MethodTestMaterial")
            }
        }
    )";

    QQmlComponent component(engine.get());
    component.setData(qmlCode.toUtf8(), QUrl("qrc:/methodtest.qml"));
    
    EXPECT_FALSE(component.isError()) << "Method invocation QML should compile";
    
    if (!component.isError()) {
        QObject* item = component.create();
        EXPECT_NE(item, nullptr);
        
        if (item) {
            QTest::qWait(50); // Allow Component.onCompleted to execute
            
            // Check method results
            QVariant polygonModes = item->property("polygonModes");
            EXPECT_TRUE(polygonModes.canConvert<QStringList>());
            QStringList modeList = polygonModes.toStringList();
            EXPECT_GT(modeList.size(), 0);
            
            QString connectionResult = item->property("connectionResult").toString();
            EXPECT_EQ(connectionResult, "C++ method called successfully!");
            
            // Check that methods were called
            EXPECT_EQ(materialEditor->materialName(), "MethodTestMaterial");
            EXPECT_FALSE(materialEditor->lightingEnabled());
            EXPECT_FLOAT_EQ(materialEditor->diffuseAlpha(), 0.5f);
            
            delete item;
        }
    }
}

// Main function for standalone execution
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    
    // Add our custom environment setup
    ::testing::AddGlobalTestEnvironment(new QMLTestEnvironment);
    
    // Initialize Google Test
    ::testing::InitGoogleTest(&argc, argv);
    
    // Run the tests
    int result = RUN_ALL_TESTS();
    
    return result;
} 