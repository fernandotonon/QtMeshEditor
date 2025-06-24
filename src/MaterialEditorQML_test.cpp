#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "MaterialEditorQML.h"
#include "Manager.h"
#include <QApplication>
#include <QQmlEngine>
#include <QJSEngine>

class MaterialEditorQMLTest : public ::testing::Test {
protected:
    void SetUp() override {
        int argc{0};
        char* argv[] = { nullptr };
        app = std::make_unique<QApplication>(argc, argv);
        editor = MaterialEditorQML::qmlInstance(nullptr, nullptr);
    }
private:
    std::unique_ptr<QApplication> app;
    MaterialEditorQML* editor;
};

TEST_F(MaterialEditorQMLTest, CreateNewMaterialTest) {
    auto editor = MaterialEditorQML::qmlInstance(nullptr, nullptr);
    editor->createNewMaterial("TestMaterial");

    ASSERT_EQ(editor->materialName(), "TestMaterial");
    ASSERT_TRUE(editor->materialText().contains("material TestMaterial"));
}

TEST_F(MaterialEditorQMLTest, SetPropertiesTest) {
    auto editor = MaterialEditorQML::qmlInstance(nullptr, nullptr);
    
    // Test basic property setters
    editor->setLightingEnabled(false);
    ASSERT_FALSE(editor->lightingEnabled());
    
    editor->setDepthWriteEnabled(false);
    ASSERT_FALSE(editor->depthWriteEnabled());
    
    QColor testColor(255, 128, 64);
    editor->setAmbientColor(testColor);
    ASSERT_EQ(editor->ambientColor(), testColor);
    
    editor->setDiffuseAlpha(0.5f);
    ASSERT_FLOAT_EQ(editor->diffuseAlpha(), 0.5f);
    
    editor->setShininess(64.0f);
    ASSERT_FLOAT_EQ(editor->shininess(), 64.0f);
}

TEST_F(MaterialEditorQMLTest, TexturePropertiesTest) {
    auto editor = MaterialEditorQML::qmlInstance(nullptr, nullptr);
    
    editor->setTextureName("test_texture.png");
    ASSERT_EQ(editor->textureName(), "test_texture.png");
    
    editor->setScrollAnimUSpeed(1.5);
    ASSERT_DOUBLE_EQ(editor->scrollAnimUSpeed(), 1.5);
    
    editor->setScrollAnimVSpeed(-0.5);
    ASSERT_DOUBLE_EQ(editor->scrollAnimVSpeed(), -0.5);
}

TEST_F(MaterialEditorQMLTest, VertexColorTrackingTest) {
    auto editor = MaterialEditorQML::qmlInstance(nullptr, nullptr);
    
    editor->setUseVertexColorToAmbient(true);
    ASSERT_TRUE(editor->useVertexColorToAmbient());
    
    editor->setUseVertexColorToDiffuse(true);
    ASSERT_TRUE(editor->useVertexColorToDiffuse());
    
    editor->setUseVertexColorToSpecular(true);
    ASSERT_TRUE(editor->useVertexColorToSpecular());
    
    editor->setUseVertexColorToEmissive(true);
    ASSERT_TRUE(editor->useVertexColorToEmissive());
}

TEST_F(MaterialEditorQMLTest, BlendingTest) {
    auto editor = MaterialEditorQML::qmlInstance(nullptr, nullptr);
    
    editor->setSourceBlendFactor(1);
    ASSERT_EQ(editor->sourceBlendFactor(), 1);
    
    editor->setDestBlendFactor(2);
    ASSERT_EQ(editor->destBlendFactor(), 2);
    
    editor->setPolygonMode(1); // Wireframe
    ASSERT_EQ(editor->polygonMode(), 1);
}

TEST_F(MaterialEditorQMLTest, UtilityFunctionsTest) {
    auto editor = MaterialEditorQML::qmlInstance(nullptr, nullptr);
    
    QStringList polygonModes = editor->getPolygonModeNames();
    ASSERT_TRUE(polygonModes.contains("Points"));
    ASSERT_TRUE(polygonModes.contains("Wireframe"));
    ASSERT_TRUE(polygonModes.contains("Solid"));
    
    QStringList blendFactors = editor->getBlendFactorNames();
    ASSERT_TRUE(blendFactors.contains("None"));
    ASSERT_TRUE(blendFactors.contains("Add"));
    ASSERT_TRUE(blendFactors.contains("One"));
}

TEST_F(MaterialEditorQMLTest, ValidationTest) {
    auto editor = MaterialEditorQML::qmlInstance(nullptr, nullptr);
    
    // Test valid script
    QString validScript = "material TestMaterial\n{\n\ttechnique\n\t{\n\t\tpass\n\t\t{\n\t\t}\n\t}\n}";
    ASSERT_TRUE(editor->validateMaterialScript(validScript));
    
    // Test empty script (should be considered invalid)
    ASSERT_FALSE(editor->validateMaterialScript(""));
} 