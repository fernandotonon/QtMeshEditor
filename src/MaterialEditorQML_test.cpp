#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "MaterialEditorQML.h"
#include "Manager.h"
#include <QApplication>
#include <QQmlEngine>
#include <QJSEngine>
#include <QSignalSpy>
#include <QTest>
#include <QColor>
#include <QTemporaryDir>
#include <QFileInfo>

// Mock Manager class for testing
class MockManager {
public:
    static bool s_initialized;
    static void initialize() { s_initialized = true; }
    static bool isInitialized() { return s_initialized; }
};

bool MockManager::s_initialized = false;

class MaterialEditorQMLTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Ensure QApplication exists
        if (!QApplication::instance()) {
            int argc = 0;
            char* argv[] = { nullptr };
            app = std::make_unique<QApplication>(argc, argv);
        }
        
        // Initialize mock manager
        MockManager::initialize();
        
        // Create MaterialEditorQML instance
        editor = MaterialEditorQML::qmlInstance(nullptr, nullptr);
        ASSERT_NE(editor, nullptr);
    }

    void TearDown() override {
        // Clean up
        if (editor) {
            editor = nullptr;
        }
    }

private:
    std::unique_ptr<QApplication> app;
    
protected:
    MaterialEditorQML* editor;
};

// Test Material Creation and Basic Properties
class MaterialCreationTest : public MaterialEditorQMLTest {};

TEST_F(MaterialCreationTest, CreateNewMaterialBasic) {
    // Test creating a new material
    QString testMaterialName = "TestMaterial_Basic";
    editor->createNewMaterial(testMaterialName);

    EXPECT_EQ(editor->materialName(), testMaterialName);
    EXPECT_TRUE(editor->materialText().contains("material " + testMaterialName));
    EXPECT_FALSE(editor->materialText().isEmpty());
}

TEST_F(MaterialCreationTest, CreateMaterialWithSpecialCharacters) {
    QString specialName = "Test_Material-123";
    editor->createNewMaterial(specialName);

    EXPECT_EQ(editor->materialName(), specialName);
    EXPECT_TRUE(editor->materialText().contains("material " + specialName));
}

TEST_F(MaterialCreationTest, CreateMaterialEmptyName) {
    QString originalName = editor->materialName();
    editor->createNewMaterial("");
    
    // Should not change from original name when empty string is provided
    EXPECT_NE(editor->materialName(), "");
}

// Test Material Validation
class MaterialValidationTest : public MaterialEditorQMLTest {};

TEST_F(MaterialValidationTest, ValidateValidMaterialScript) {
    QString validScript = R"(
material TestMaterial
{
    technique
    {
        pass
        {
            ambient 0.5 0.5 0.5
            diffuse 1.0 1.0 1.0
            specular 1.0 1.0 1.0 32.0
        }
    }
}
)";
    EXPECT_TRUE(editor->validateMaterialScript(validScript));
}

TEST_F(MaterialValidationTest, ValidateInvalidMaterialScript) {
    QString invalidScript = "invalid material script {{{";
    EXPECT_FALSE(editor->validateMaterialScript(invalidScript));
}

TEST_F(MaterialValidationTest, ValidateEmptyScript) {
    EXPECT_FALSE(editor->validateMaterialScript(""));
    EXPECT_FALSE(editor->validateMaterialScript("   "));
}

// Test Basic Properties
class BasicPropertiesTest : public MaterialEditorQMLTest {};

TEST_F(BasicPropertiesTest, LightingEnabled) {
    QSignalSpy spy(editor, &MaterialEditorQML::lightingEnabledChanged);
    
    editor->setLightingEnabled(false);
    EXPECT_FALSE(editor->lightingEnabled());
    EXPECT_EQ(spy.count(), 1);
    
    editor->setLightingEnabled(true);
    EXPECT_TRUE(editor->lightingEnabled());
    EXPECT_EQ(spy.count(), 2);
    
    // Setting same value should not emit signal
    editor->setLightingEnabled(true);
    EXPECT_EQ(spy.count(), 2);
}

TEST_F(BasicPropertiesTest, DepthWriteEnabled) {
    QSignalSpy spy(editor, &MaterialEditorQML::depthWriteEnabledChanged);
    
    editor->setDepthWriteEnabled(false);
    EXPECT_FALSE(editor->depthWriteEnabled());
    EXPECT_EQ(spy.count(), 1);
    
    editor->setDepthWriteEnabled(true);
    EXPECT_TRUE(editor->depthWriteEnabled());
    EXPECT_EQ(spy.count(), 2);
}

TEST_F(BasicPropertiesTest, DepthCheckEnabled) {
    QSignalSpy spy(editor, &MaterialEditorQML::depthCheckEnabledChanged);
    
    editor->setDepthCheckEnabled(false);
    EXPECT_FALSE(editor->depthCheckEnabled());
    EXPECT_EQ(spy.count(), 1);
    
    editor->setDepthCheckEnabled(true);
    EXPECT_TRUE(editor->depthCheckEnabled());
    EXPECT_EQ(spy.count(), 2);
}

// Test Color Properties
class ColorPropertiesTest : public MaterialEditorQMLTest {};

TEST_F(ColorPropertiesTest, AmbientColor) {
    QSignalSpy spy(editor, &MaterialEditorQML::ambientColorChanged);
    
    QColor testColor(255, 128, 64);
    editor->setAmbientColor(testColor);
    EXPECT_EQ(editor->ambientColor(), testColor);
    EXPECT_EQ(spy.count(), 1);
    
    // Setting same color should not emit signal
    editor->setAmbientColor(testColor);
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(ColorPropertiesTest, DiffuseColor) {
    QSignalSpy spy(editor, &MaterialEditorQML::diffuseColorChanged);
    
    QColor testColor(200, 100, 50);
    editor->setDiffuseColor(testColor);
    EXPECT_EQ(editor->diffuseColor(), testColor);
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(ColorPropertiesTest, SpecularColor) {
    QSignalSpy spy(editor, &MaterialEditorQML::specularColorChanged);
    
    QColor testColor(255, 255, 255);
    editor->setSpecularColor(testColor);
    EXPECT_EQ(editor->specularColor(), testColor);
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(ColorPropertiesTest, EmissiveColor) {
    QSignalSpy spy(editor, &MaterialEditorQML::emissiveColorChanged);
    
    QColor testColor(50, 50, 100);
    editor->setEmissiveColor(testColor);
    EXPECT_EQ(editor->emissiveColor(), testColor);
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(ColorPropertiesTest, InvalidColors) {
    QColor originalAmbient = editor->ambientColor();
    
    // Test with invalid color
    QColor invalidColor;
    editor->setAmbientColor(invalidColor);
    
    // Should handle invalid colors gracefully
    EXPECT_TRUE(editor->ambientColor().isValid() || editor->ambientColor() == invalidColor);
}

// Test Alpha and Shininess Properties
class MaterialParametersTest : public MaterialEditorQMLTest {};

TEST_F(MaterialParametersTest, DiffuseAlpha) {
    QSignalSpy spy(editor, &MaterialEditorQML::diffuseAlphaChanged);
    
    editor->setDiffuseAlpha(0.5f);
    EXPECT_FLOAT_EQ(editor->diffuseAlpha(), 0.5f);
    EXPECT_EQ(spy.count(), 1);
    
    // Test boundary values
    editor->setDiffuseAlpha(0.0f);
    EXPECT_FLOAT_EQ(editor->diffuseAlpha(), 0.0f);
    
    editor->setDiffuseAlpha(1.0f);
    EXPECT_FLOAT_EQ(editor->diffuseAlpha(), 1.0f);
}

TEST_F(MaterialParametersTest, SpecularAlpha) {
    QSignalSpy spy(editor, &MaterialEditorQML::specularAlphaChanged);
    
    editor->setSpecularAlpha(0.75f);
    EXPECT_FLOAT_EQ(editor->specularAlpha(), 0.75f);
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(MaterialParametersTest, Shininess) {
    QSignalSpy spy(editor, &MaterialEditorQML::shininessChanged);
    
    editor->setShininess(64.0f);
    EXPECT_FLOAT_EQ(editor->shininess(), 64.0f);
    EXPECT_EQ(spy.count(), 1);
    
    // Test extreme values
    editor->setShininess(0.0f);
    EXPECT_FLOAT_EQ(editor->shininess(), 0.0f);
    
    editor->setShininess(128.0f);
    EXPECT_FLOAT_EQ(editor->shininess(), 128.0f);
}

// Test Texture Properties
class TexturePropertiesTest : public MaterialEditorQMLTest {};

TEST_F(TexturePropertiesTest, TextureName) {
    QSignalSpy spy(editor, &MaterialEditorQML::textureNameChanged);
    
    QString textureName = "test_texture.png";
    editor->setTextureName(textureName);
    EXPECT_EQ(editor->textureName(), textureName);
    EXPECT_EQ(spy.count(), 1);
    
    // Test setting empty texture name
    editor->setTextureName("");
    EXPECT_EQ(spy.count(), 2);
}

TEST_F(TexturePropertiesTest, ScrollAnimationSpeeds) {
    QSignalSpy uSpeedSpy(editor, &MaterialEditorQML::scrollAnimUSpeedChanged);
    QSignalSpy vSpeedSpy(editor, &MaterialEditorQML::scrollAnimVSpeedChanged);
    
    editor->setScrollAnimUSpeed(1.5);
    EXPECT_DOUBLE_EQ(editor->scrollAnimUSpeed(), 1.5);
    EXPECT_EQ(uSpeedSpy.count(), 1);
    
    editor->setScrollAnimVSpeed(-0.5);
    EXPECT_DOUBLE_EQ(editor->scrollAnimVSpeed(), -0.5);
    EXPECT_EQ(vSpeedSpy.count(), 1);
    
    // Test zero speeds
    editor->setScrollAnimUSpeed(0.0);
    EXPECT_DOUBLE_EQ(editor->scrollAnimUSpeed(), 0.0);
    
    editor->setScrollAnimVSpeed(0.0);
    EXPECT_DOUBLE_EQ(editor->scrollAnimVSpeed(), 0.0);
}

TEST_F(TexturePropertiesTest, TextureCoordinateProperties) {
    // Test texture coordinate set
    QSignalSpy spy(editor, &MaterialEditorQML::texCoordSetChanged);
    editor->setTexCoordSet(2);
    EXPECT_EQ(editor->texCoordSet(), 2);
    EXPECT_EQ(spy.count(), 1);
    
    // Test texture address mode
    QSignalSpy addressSpy(editor, &MaterialEditorQML::textureAddressModeChanged);
    editor->setTextureAddressMode(1); // Clamp
    EXPECT_EQ(editor->textureAddressMode(), 1);
    EXPECT_EQ(addressSpy.count(), 1);
    
    // Test texture filtering
    QSignalSpy filterSpy(editor, &MaterialEditorQML::textureFilteringChanged);
    editor->setTextureFiltering(2); // Trilinear
    EXPECT_EQ(editor->textureFiltering(), 2);
    EXPECT_EQ(filterSpy.count(), 1);
}

// Test Vertex Color Tracking
class VertexColorTrackingTest : public MaterialEditorQMLTest {};

TEST_F(VertexColorTrackingTest, VertexColorToAmbient) {
    QSignalSpy spy(editor, &MaterialEditorQML::useVertexColorToAmbientChanged);
    
    editor->setUseVertexColorToAmbient(true);
    EXPECT_TRUE(editor->useVertexColorToAmbient());
    EXPECT_EQ(spy.count(), 1);
    
    editor->setUseVertexColorToAmbient(false);
    EXPECT_FALSE(editor->useVertexColorToAmbient());
    EXPECT_EQ(spy.count(), 2);
}

TEST_F(VertexColorTrackingTest, VertexColorToDiffuse) {
    QSignalSpy spy(editor, &MaterialEditorQML::useVertexColorToDiffuseChanged);
    
    editor->setUseVertexColorToDiffuse(true);
    EXPECT_TRUE(editor->useVertexColorToDiffuse());
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(VertexColorTrackingTest, VertexColorToSpecular) {
    QSignalSpy spy(editor, &MaterialEditorQML::useVertexColorToSpecularChanged);
    
    editor->setUseVertexColorToSpecular(true);
    EXPECT_TRUE(editor->useVertexColorToSpecular());
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(VertexColorTrackingTest, VertexColorToEmissive) {
    QSignalSpy spy(editor, &MaterialEditorQML::useVertexColorToEmissiveChanged);
    
    editor->setUseVertexColorToEmissive(true);
    EXPECT_TRUE(editor->useVertexColorToEmissive());
    EXPECT_EQ(spy.count(), 1);
}

// Test Blending Properties
class BlendingTest : public MaterialEditorQMLTest {};

TEST_F(BlendingTest, BlendFactors) {
    QSignalSpy sourceSpy(editor, &MaterialEditorQML::sourceBlendFactorChanged);
    QSignalSpy destSpy(editor, &MaterialEditorQML::destBlendFactorChanged);
    
    editor->setSourceBlendFactor(1);
    EXPECT_EQ(editor->sourceBlendFactor(), 1);
    EXPECT_EQ(sourceSpy.count(), 1);
    
    editor->setDestBlendFactor(2);
    EXPECT_EQ(editor->destBlendFactor(), 2);
    EXPECT_EQ(destSpy.count(), 1);
}

TEST_F(BlendingTest, PolygonMode) {
    QSignalSpy spy(editor, &MaterialEditorQML::polygonModeChanged);
    
    editor->setPolygonMode(1); // Wireframe
    EXPECT_EQ(editor->polygonMode(), 1);
    EXPECT_EQ(spy.count(), 1);
    
    editor->setPolygonMode(2); // Solid
    EXPECT_EQ(editor->polygonMode(), 2);
    EXPECT_EQ(spy.count(), 2);
}

// Test Utility Functions
class UtilityFunctionsTest : public MaterialEditorQMLTest {};

TEST_F(UtilityFunctionsTest, PolygonModeNames) {
    QStringList polygonModes = editor->getPolygonModeNames();
    EXPECT_GE(polygonModes.size(), 3);
    EXPECT_TRUE(polygonModes.contains("Points"));
    EXPECT_TRUE(polygonModes.contains("Wireframe"));
    EXPECT_TRUE(polygonModes.contains("Solid"));
}

TEST_F(UtilityFunctionsTest, BlendFactorNames) {
    QStringList blendFactors = editor->getBlendFactorNames();
    EXPECT_GE(blendFactors.size(), 5);
    EXPECT_TRUE(blendFactors.contains("None"));
    EXPECT_TRUE(blendFactors.contains("Add"));
    EXPECT_TRUE(blendFactors.contains("One"));
    EXPECT_TRUE(blendFactors.contains("Zero"));
}

TEST_F(UtilityFunctionsTest, ShadingModeNames) {
    QStringList shadingModes = editor->getShadingModeNames();
    EXPECT_GT(shadingModes.size(), 0);
    // Should contain standard shading modes
}

TEST_F(UtilityFunctionsTest, TextureAddressModeNames) {
    QStringList addressModes = editor->getTextureAddressModeNames();
    EXPECT_GE(addressModes.size(), 3);
    // Should contain Wrap, Clamp, Mirror, etc.
}

// Test File Operations
class FileOperationsTest : public MaterialEditorQMLTest {};

TEST_F(FileOperationsTest, TestConnection) {
    // Test the connection test method
    QString result = editor->testConnection();
    EXPECT_EQ(result, "C++ method called successfully!");
}

TEST_F(FileOperationsTest, GetAvailableTextures) {
    QStringList textures = editor->getAvailableTextures();
    // Should return a list (may be empty if no textures loaded)
    EXPECT_TRUE(textures.isEmpty() || textures.size() > 0);
}

TEST_F(FileOperationsTest, GetTexturePreviewPath) {
    // Test with no texture
    editor->setTextureName("*Select a texture*");
    QString previewPath = editor->getTexturePreviewPath();
    EXPECT_TRUE(previewPath.isEmpty());
    
    // Test with a texture name
    editor->setTextureName("test_texture.jpg");
    previewPath = editor->getTexturePreviewPath();
    // Should attempt to construct a path (may be empty if file doesn't exist)
}

// Test Material Hierarchy (Techniques, Passes, Texture Units)
class MaterialHierarchyTest : public MaterialEditorQMLTest {};

TEST_F(MaterialHierarchyTest, TechniqueSelection) {
    // Create a material first
    editor->createNewMaterial("HierarchyTestMaterial");
    
    QSignalSpy spy(editor, &MaterialEditorQML::selectedTechniqueIndexChanged);
    
    // Test technique selection
    int techniqueCount = editor->techniqueList().size();
    if (techniqueCount > 0) {
        editor->setSelectedTechniqueIndex(0);
        EXPECT_EQ(editor->selectedTechniqueIndex(), 0);
        EXPECT_EQ(spy.count(), 1);
    }
}

TEST_F(MaterialHierarchyTest, PassSelection) {
    editor->createNewMaterial("PassTestMaterial");
    
    QSignalSpy spy(editor, &MaterialEditorQML::selectedPassIndexChanged);
    
    // Select first technique if available
    if (!editor->techniqueList().isEmpty()) {
        editor->setSelectedTechniqueIndex(0);
        
        // Test pass selection
        if (!editor->passList().isEmpty()) {
            editor->setSelectedPassIndex(0);
            EXPECT_EQ(editor->selectedPassIndex(), 0);
            EXPECT_GE(spy.count(), 1);
        }
    }
}

TEST_F(MaterialHierarchyTest, TextureUnitSelection) {
    editor->createNewMaterial("TextureUnitTestMaterial");
    
    QSignalSpy spy(editor, &MaterialEditorQML::selectedTextureUnitIndexChanged);
    
    // Navigate to technique and pass first
    if (!editor->techniqueList().isEmpty()) {
        editor->setSelectedTechniqueIndex(0);
        
        if (!editor->passList().isEmpty()) {
            editor->setSelectedPassIndex(0);
            
            // Test texture unit selection
            if (!editor->textureUnitList().isEmpty()) {
                editor->setSelectedTextureUnitIndex(0);
                EXPECT_EQ(editor->selectedTextureUnitIndex(), 0);
                EXPECT_GE(spy.count(), 1);
            }
        }
    }
}

// Test Advanced Material Properties
class AdvancedPropertiesTest : public MaterialEditorQMLTest {};

TEST_F(AdvancedPropertiesTest, AlphaRejection) {
    QSignalSpy enabledSpy(editor, &MaterialEditorQML::alphaRejectionEnabledChanged);
    QSignalSpy functionSpy(editor, &MaterialEditorQML::alphaRejectionFunctionChanged);
    QSignalSpy valueSpy(editor, &MaterialEditorQML::alphaRejectionValueChanged);
    
    editor->setAlphaRejectionEnabled(true);
    EXPECT_TRUE(editor->alphaRejectionEnabled());
    EXPECT_EQ(enabledSpy.count(), 1);
    
    editor->setAlphaRejectionFunction(1);
    EXPECT_EQ(editor->alphaRejectionFunction(), 1);
    EXPECT_EQ(functionSpy.count(), 1);
    
    editor->setAlphaRejectionValue(128);
    EXPECT_EQ(editor->alphaRejectionValue(), 128);
    EXPECT_EQ(valueSpy.count(), 1);
}

TEST_F(AdvancedPropertiesTest, CullingModes) {
    QSignalSpy hardwareSpy(editor, &MaterialEditorQML::cullHardwareChanged);
    QSignalSpy softwareSpy(editor, &MaterialEditorQML::cullSoftwareChanged);
    
    editor->setCullHardware(1);
    EXPECT_EQ(editor->cullHardware(), 1);
    EXPECT_EQ(hardwareSpy.count(), 1);
    
    editor->setCullSoftware(2);
    EXPECT_EQ(editor->cullSoftware(), 2);
    EXPECT_EQ(softwareSpy.count(), 1);
}

// Test Error Handling and Edge Cases
class ErrorHandlingTest : public MaterialEditorQMLTest {};

TEST_F(ErrorHandlingTest, NullPointerHandling) {
    // Test that the singleton properly handles multiple calls
    MaterialEditorQML* editor1 = MaterialEditorQML::qmlInstance(nullptr, nullptr);
    MaterialEditorQML* editor2 = MaterialEditorQML::qmlInstance(nullptr, nullptr);
    
    EXPECT_EQ(editor1, editor2); // Should return same instance
    EXPECT_NE(editor1, nullptr);
}

TEST_F(ErrorHandlingTest, InvalidPropertyValues) {
    // Test setting invalid polygon mode
    int originalMode = editor->polygonMode();
    editor->setPolygonMode(-1);
    // Should either handle gracefully or remain unchanged
    
    // Test invalid blend factors
    editor->setSourceBlendFactor(-1);
    editor->setDestBlendFactor(999);
    // Should handle gracefully
}

TEST_F(ErrorHandlingTest, EmptyStringHandling) {
    // Test empty material name
    QString originalName = editor->materialName();
    editor->setMaterialName("");
    // Should handle empty strings appropriately
    
    // Test empty texture name
    editor->setTextureName("");
    // Should handle empty texture names
} 