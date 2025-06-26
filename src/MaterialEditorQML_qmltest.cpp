#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <QApplication>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
#include <QTest>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QTemporaryDir>
#include <QDir>
#include "MaterialEditorQML.h"

class MaterialEditorQMLIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Ensure QApplication exists
        if (!QApplication::instance()) {
            int argc = 0;
            char* argv[] = { nullptr };
            app = std::make_unique<QApplication>(argc, argv);
        }
        
        // Create QML engine
        engine = std::make_unique<QQmlEngine>();
        
        // Register MaterialEditorQML type
        qmlRegisterSingletonType<MaterialEditorQML>("MaterialEditorQML", 1, 0, "MaterialEditorQML",
            [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject * {
                Q_UNUSED(engine)
                Q_UNUSED(scriptEngine)
                return MaterialEditorQML::qmlInstance(engine, scriptEngine);
            }
        );
        
        // Create MaterialEditorQML instance
        materialEditor = MaterialEditorQML::qmlInstance(engine.get(), nullptr);
        ASSERT_NE(materialEditor, nullptr);
        
        // Set up context
        engine->rootContext()->setContextProperty("MaterialEditorQML", materialEditor);
    }

    void TearDown() override {
        engine.reset();
        materialEditor = nullptr;
    }

    // Helper to create QML component from string
    std::unique_ptr<QQmlComponent> createComponent(const QString& qmlSource) {
        auto component = std::make_unique<QQmlComponent>(engine.get());
        component->setData(qmlSource.toUtf8(), QUrl());
        return component;
    }

    // Helper to create QML object from string
    QObject* createQmlObject(const QString& qmlSource) {
        auto component = createComponent(qmlSource);
        if (component->isError()) {
            qDebug() << "QML Errors:" << component->errors();
            return nullptr;
        }
        return component->create();
    }

private:
    std::unique_ptr<QApplication> app;
    std::unique_ptr<QQmlEngine> engine;
    MaterialEditorQML* materialEditor;

protected:
    QQmlEngine* getEngine() { return engine.get(); }
    MaterialEditorQML* getMaterialEditor() { return materialEditor; }
};

// Test QML Property Bindings
class QMLPropertyBindingTest : public MaterialEditorQMLIntegrationTest {};

TEST_F(QMLPropertyBindingTest, BasicPropertyBinding) {
    QString qmlSource = R"(
        import QtQuick 2.15
        import MaterialEditorQML 1.0
        
        Item {
            property alias materialName: internal.materialName
            property alias lightingEnabled: internal.lightingEnabled
            property alias ambientColor: internal.ambientColor
            
            QtObject {
                id: internal
                property string materialName: MaterialEditorQML.materialName
                property bool lightingEnabled: MaterialEditorQML.lightingEnabled
                property color ambientColor: MaterialEditorQML.ambientColor
            }
        }
    )";

    QObject* qmlObject = createQmlObject(qmlSource);
    ASSERT_NE(qmlObject, nullptr);

    // Test initial values
    EXPECT_EQ(qmlObject->property("materialName").toString(), getMaterialEditor()->materialName());
    EXPECT_EQ(qmlObject->property("lightingEnabled").toBool(), getMaterialEditor()->lightingEnabled());

    // Test property changes from C++
    getMaterialEditor()->setMaterialName("QMLTestMaterial");
    QTest::qWait(10); // Allow for binding updates
    EXPECT_EQ(qmlObject->property("materialName").toString(), "QMLTestMaterial");

    getMaterialEditor()->setLightingEnabled(false);
    QTest::qWait(10);
    EXPECT_EQ(qmlObject->property("lightingEnabled").toBool(), false);

    delete qmlObject;
}

TEST_F(QMLPropertyBindingTest, ColorPropertyBinding) {
    QString qmlSource = R"(
        import QtQuick 2.15
        import MaterialEditorQML 1.0
        
        Rectangle {
            id: colorRect
            color: MaterialEditorQML.ambientColor
            property alias rectColor: colorRect.color
        }
    )";

    QObject* qmlObject = createQmlObject(qmlSource);
    ASSERT_NE(qmlObject, nullptr);

    // Test color binding
    QColor testColor(255, 128, 64);
    getMaterialEditor()->setAmbientColor(testColor);
    QTest::qWait(10);
    
    QColor boundColor = qmlObject->property("rectColor").value<QColor>();
    EXPECT_EQ(boundColor, testColor);

    delete qmlObject;
}

// Test QML Method Invocation
class QMLMethodInvocationTest : public MaterialEditorQMLIntegrationTest {};

TEST_F(QMLMethodInvocationTest, InvokeBasicMethods) {
    QString qmlSource = R"(
        import QtQuick 2.15
        import MaterialEditorQML 1.0
        
        Item {
            Component.onCompleted: {
                MaterialEditorQML.createNewMaterial("QMLInvokedMaterial")
                MaterialEditorQML.setLightingEnabled(false)
                MaterialEditorQML.setDiffuseAlpha(0.7)
            }
        }
    )";

    QObject* qmlObject = createQmlObject(qmlSource);
    ASSERT_NE(qmlObject, nullptr);
    
    QTest::qWait(50); // Allow for Component.onCompleted to execute

    // Verify methods were called
    EXPECT_EQ(getMaterialEditor()->materialName(), "QMLInvokedMaterial");
    EXPECT_FALSE(getMaterialEditor()->lightingEnabled());
    EXPECT_FLOAT_EQ(getMaterialEditor()->diffuseAlpha(), 0.7f);

    delete qmlObject;
}

TEST_F(QMLMethodInvocationTest, InvokeUtilityMethods) {
    QString qmlSource = R"(
        import QtQuick 2.15
        import MaterialEditorQML 1.0
        
        Item {
            property var polygonModes: MaterialEditorQML.getPolygonModeNames()
            property var blendFactors: MaterialEditorQML.getBlendFactorNames()
            property string connectionTest: MaterialEditorQML.testConnection()
        }
    )";

    QObject* qmlObject = createQmlObject(qmlSource);
    ASSERT_NE(qmlObject, nullptr);

    // Test utility method results
    QVariant polygonModes = qmlObject->property("polygonModes");
    EXPECT_TRUE(polygonModes.canConvert<QStringList>());
    QStringList modeList = polygonModes.toStringList();
    EXPECT_GT(modeList.size(), 0);
    EXPECT_TRUE(modeList.contains("Solid"));

    QVariant blendFactors = qmlObject->property("blendFactors");
    EXPECT_TRUE(blendFactors.canConvert<QStringList>());
    QStringList factorList = blendFactors.toStringList();
    EXPECT_GT(factorList.size(), 0);

    QString connectionResult = qmlObject->property("connectionTest").toString();
    EXPECT_EQ(connectionResult, "C++ method called successfully!");

    delete qmlObject;
}

// Test QML Signal Handling
class QMLSignalTest : public MaterialEditorQMLIntegrationTest {};

TEST_F(QMLSignalTest, MaterialNameChangedSignal) {
    QString qmlSource = R"(
        import QtQuick 2.15
        import MaterialEditorQML 1.0
        
        Item {
            property int signalCount: 0
            property string lastMaterialName: ""
            
            Connections {
                target: MaterialEditorQML
                function onMaterialNameChanged() {
                    signalCount++
                    lastMaterialName = MaterialEditorQML.materialName
                }
            }
        }
    )";

    QObject* qmlObject = createQmlObject(qmlSource);
    ASSERT_NE(qmlObject, nullptr);

    // Change material name and check signal
    getMaterialEditor()->setMaterialName("SignalTestMaterial1");
    QTest::qWait(10);
    EXPECT_EQ(qmlObject->property("signalCount").toInt(), 1);
    EXPECT_EQ(qmlObject->property("lastMaterialName").toString(), "SignalTestMaterial1");

    getMaterialEditor()->setMaterialName("SignalTestMaterial2");
    QTest::qWait(10);
    EXPECT_EQ(qmlObject->property("signalCount").toInt(), 2);
    EXPECT_EQ(qmlObject->property("lastMaterialName").toString(), "SignalTestMaterial2");

    delete qmlObject;
}

TEST_F(QMLSignalTest, ColorChangedSignals) {
    QString qmlSource = R"(
        import QtQuick 2.15
        import MaterialEditorQML 1.0
        
        Item {
            property int ambientSignalCount: 0
            property int diffuseSignalCount: 0
            property color lastAmbientColor
            property color lastDiffuseColor
            
            Connections {
                target: MaterialEditorQML
                function onAmbientColorChanged() {
                    ambientSignalCount++
                    lastAmbientColor = MaterialEditorQML.ambientColor
                }
                function onDiffuseColorChanged() {
                    diffuseSignalCount++
                    lastDiffuseColor = MaterialEditorQML.diffuseColor
                }
            }
        }
    )";

    QObject* qmlObject = createQmlObject(qmlSource);
    ASSERT_NE(qmlObject, nullptr);

    // Test ambient color signal
    QColor testAmbient(100, 150, 200);
    getMaterialEditor()->setAmbientColor(testAmbient);
    QTest::qWait(10);
    EXPECT_EQ(qmlObject->property("ambientSignalCount").toInt(), 1);
    EXPECT_EQ(qmlObject->property("lastAmbientColor").value<QColor>(), testAmbient);

    // Test diffuse color signal
    QColor testDiffuse(200, 100, 50);
    getMaterialEditor()->setDiffuseColor(testDiffuse);
    QTest::qWait(10);
    EXPECT_EQ(qmlObject->property("diffuseSignalCount").toInt(), 1);
    EXPECT_EQ(qmlObject->property("lastDiffuseColor").value<QColor>(), testDiffuse);

    delete qmlObject;
}

// Test Complex QML Interactions
class QMLComplexInteractionTest : public MaterialEditorQMLIntegrationTest {};

TEST_F(QMLComplexInteractionTest, MaterialEditorWorkflow) {
    QString qmlSource = R"(
        import QtQuick 2.15
        import MaterialEditorQML 1.0
        
        Item {
            property bool workflowCompleted: false
            property var materialStates: []
            
            function runWorkflow() {
                // Step 1: Create new material
                MaterialEditorQML.createNewMaterial("WorkflowTestMaterial")
                materialStates.push({
                    step: "created",
                    name: MaterialEditorQML.materialName
                })
                
                // Step 2: Set basic properties
                MaterialEditorQML.setLightingEnabled(true)
                MaterialEditorQML.setDepthWriteEnabled(false)
                materialStates.push({
                    step: "basicProps",
                    lighting: MaterialEditorQML.lightingEnabled,
                    depthWrite: MaterialEditorQML.depthWriteEnabled
                })
                
                // Step 3: Set colors
                MaterialEditorQML.setAmbientColor(Qt.rgba(0.2, 0.3, 0.4, 1.0))
                MaterialEditorQML.setDiffuseColor(Qt.rgba(0.8, 0.6, 0.4, 1.0))
                materialStates.push({
                    step: "colors",
                    ambient: MaterialEditorQML.ambientColor,
                    diffuse: MaterialEditorQML.diffuseColor
                })
                
                // Step 4: Set material parameters
                MaterialEditorQML.setShininess(32.0)
                MaterialEditorQML.setDiffuseAlpha(0.9)
                materialStates.push({
                    step: "parameters",
                    shininess: MaterialEditorQML.shininess,
                    alpha: MaterialEditorQML.diffuseAlpha
                })
                
                workflowCompleted = true
            }
            
            Component.onCompleted: runWorkflow()
        }
    )";

    QObject* qmlObject = createQmlObject(qmlSource);
    ASSERT_NE(qmlObject, nullptr);
    
    QTest::qWait(100); // Allow workflow to complete

    EXPECT_TRUE(qmlObject->property("workflowCompleted").toBool());
    
    // Verify final state
    EXPECT_EQ(getMaterialEditor()->materialName(), "WorkflowTestMaterial");
    EXPECT_TRUE(getMaterialEditor()->lightingEnabled());
    EXPECT_FALSE(getMaterialEditor()->depthWriteEnabled());
    EXPECT_FLOAT_EQ(getMaterialEditor()->shininess(), 32.0f);
    EXPECT_FLOAT_EQ(getMaterialEditor()->diffuseAlpha(), 0.9f);

    // Check that colors were set correctly
    QColor ambient = getMaterialEditor()->ambientColor();
    QColor diffuse = getMaterialEditor()->diffuseColor();
    EXPECT_NEAR(ambient.redF(), 0.2, 0.01);
    EXPECT_NEAR(diffuse.redF(), 0.8, 0.01);

    delete qmlObject;
}

TEST_F(QMLComplexInteractionTest, TextureManagement) {
    QString qmlSource = R"(
        import QtQuick 2.15
        import MaterialEditorQML 1.0
        
        Item {
            property string selectedTexture: ""
            property var availableTextures: MaterialEditorQML.getAvailableTextures()
            property string previewPath: MaterialEditorQML.getTexturePreviewPath()
            
            function selectTexture(textureName) {
                MaterialEditorQML.setTextureName(textureName)
                selectedTexture = MaterialEditorQML.textureName
                previewPath = MaterialEditorQML.getTexturePreviewPath()
            }
            
            function setTextureAnimation(uSpeed, vSpeed) {
                MaterialEditorQML.setScrollAnimUSpeed(uSpeed)
                MaterialEditorQML.setScrollAnimVSpeed(vSpeed)
            }
            
            Component.onCompleted: {
                selectTexture("test_texture.png")
                setTextureAnimation(0.5, -0.3)
            }
        }
    )";

    QObject* qmlObject = createQmlObject(qmlSource);
    ASSERT_NE(qmlObject, nullptr);
    
    QTest::qWait(50);

    // Verify texture selection
    EXPECT_EQ(qmlObject->property("selectedTexture").toString(), "test_texture.png");
    EXPECT_EQ(getMaterialEditor()->textureName(), "test_texture.png");

    // Verify animation settings
    EXPECT_DOUBLE_EQ(getMaterialEditor()->scrollAnimUSpeed(), 0.5);
    EXPECT_DOUBLE_EQ(getMaterialEditor()->scrollAnimVSpeed(), -0.3);

    // Test texture list retrieval
    QVariant textureList = qmlObject->property("availableTextures");
    EXPECT_TRUE(textureList.canConvert<QStringList>());

    delete qmlObject;
}

// Test QML Error Handling
class QMLErrorHandlingTest : public MaterialEditorQMLIntegrationTest {};

TEST_F(QMLErrorHandlingTest, InvalidMethodParameters) {
    QString qmlSource = R"(
        import QtQuick 2.15
        import MaterialEditorQML 1.0
        
        Item {
            property bool errorsHandled: true
            
            Component.onCompleted: {
                try {
                    // Test with invalid values
                    MaterialEditorQML.setPolygonMode(-1)
                    MaterialEditorQML.setDiffuseAlpha(-0.5)
                    MaterialEditorQML.setShininess(-10.0)
                    
                    // These should not crash the application
                    MaterialEditorQML.setTextureName("")
                    MaterialEditorQML.createNewMaterial("")
                } catch (e) {
                    console.log("Caught error:", e)
                    errorsHandled = false
                }
            }
        }
    )";

    QObject* qmlObject = createQmlObject(qmlSource);
    ASSERT_NE(qmlObject, nullptr);
    
    QTest::qWait(50);

    // Application should still be stable
    EXPECT_TRUE(qmlObject->property("errorsHandled").toBool());
    
    // MaterialEditor should still be functional
    getMaterialEditor()->setMaterialName("ErrorTestMaterial");
    EXPECT_EQ(getMaterialEditor()->materialName(), "ErrorTestMaterial");

    delete qmlObject;
}

// Test QML Component Loading
class QMLComponentLoadingTest : public MaterialEditorQMLIntegrationTest {};

TEST_F(QMLComponentLoadingTest, DynamicComponentCreation) {
    QString qmlSource = R"(
        import QtQuick 2.15
        import MaterialEditorQML 1.0
        
        Item {
            property var dynamicComponent: null
            property bool componentLoaded: false
            
            function createDynamicComponent() {
                var componentText = '
                    import QtQuick 2.15;
                    import MaterialEditorQML 1.0;
                    Rectangle {
                        color: MaterialEditorQML.diffuseColor;
                        Component.onCompleted: MaterialEditorQML.setDiffuseColor("red")
                    }'
                
                var component = Qt.createQmlObject(componentText, this, "dynamicComponent")
                if (component) {
                    dynamicComponent = component
                    componentLoaded = true
                }
            }
            
            Component.onCompleted: createDynamicComponent()
        }
    )";

    QObject* qmlObject = createQmlObject(qmlSource);
    ASSERT_NE(qmlObject, nullptr);
    
    QTest::qWait(100);

    EXPECT_TRUE(qmlObject->property("componentLoaded").toBool());
    
    // Verify the dynamic component affected the material editor
    QColor diffuseColor = getMaterialEditor()->diffuseColor();
    EXPECT_EQ(diffuseColor, QColor(Qt::red));

    delete qmlObject;
} 