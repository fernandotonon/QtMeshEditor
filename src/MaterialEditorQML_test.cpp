#include <gtest/gtest.h>
#include <memory>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>
#include "MaterialEditorQML.h"
#include "Manager.h"
#include <OgreException.h>
#include "TestHelpers.h"

// Ensure a QApplication exists for the process lifetime.
// gtest_main does not create one, so we lazily create it here.
static QApplication* ensureQApplication()
{
    QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!app) {
        static int argc = 1;
        static char appName[] = "MaterialEditorQML_test";
        static char* argv[] = {appName, nullptr};
        app = new QApplication(argc, argv); // NOSONAR - intentional: QApplication must outlive all tests
    }
    return app;
}

// ---------------------------------------------------------------------------
// Basic fixture (no Ogre needed)
// ---------------------------------------------------------------------------
class MaterialEditorQMLTest : public ::testing::Test {
protected:
    void SetUp() override {
        app = ensureQApplication();
        ASSERT_NE(app, nullptr);
        editor = std::make_unique<MaterialEditorQML>();
    }

    void TearDown() override {
        editor.reset();
    }

    QApplication* app = nullptr;
    std::unique_ptr<MaterialEditorQML> editor;
};

// ---------------------------------------------------------------------------
// Ogre fixture
// ---------------------------------------------------------------------------
class MaterialEditorQMLWithOgreTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = ensureQApplication();
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createStandardOgreMaterials();

        editor = std::make_unique<MaterialEditorQML>();
    }

    void TearDown() override {
        editor.reset();

        if (app) {
            app->processEvents();
        }
    }

    QApplication* app = nullptr;
    std::unique_ptr<MaterialEditorQML> editor;
};

// ===========================================================================
// Basic fixture tests -- file system helpers
// ===========================================================================

TEST_F(MaterialEditorQMLTest, FileSystem_IsDirectory) {
    // A known directory should return true
    EXPECT_TRUE(editor->isDirectory("/tmp"));
    // A non-existent or file path should return false
    EXPECT_FALSE(editor->isDirectory("/tmp/nonexistent_path_xyz_12345"));
}

TEST_F(MaterialEditorQMLTest, FileSystem_GetParentDirectory) {
    QString parent = editor->getParentDirectory("/tmp/somefile.txt");
    EXPECT_EQ(parent, "/tmp");

    QString parent2 = editor->getParentDirectory("/usr/local/bin");
    EXPECT_EQ(parent2, "/usr/local");
}

TEST_F(MaterialEditorQMLTest, FileSystem_GetFileName) {
    QString name = editor->getFileName("/some/path/texture.png");
    EXPECT_EQ(name, "texture.png");

    QString name2 = editor->getFileName("/another/directory/file.material");
    EXPECT_EQ(name2, "file.material");
}

TEST_F(MaterialEditorQMLTest, FileSystem_PathExists) {
    EXPECT_TRUE(editor->pathExists("/tmp"));
    EXPECT_FALSE(editor->pathExists("/nonexistent_path_xyz_99999"));
}

TEST_F(MaterialEditorQMLTest, FileSystem_GetFileSize) {
    // /tmp always exists as a directory; its size is platform-dependent
    // but should be non-negative
    qint64 size = editor->getFileSize("/tmp");
    EXPECT_GE(size, 0);
}

TEST_F(MaterialEditorQMLTest, FileSystem_GetFileSizeString) {
    // The format should contain B, KB, or MB
    QString sizeStr = editor->getFileSizeString("/tmp");
    EXPECT_TRUE(sizeStr.contains("B") || sizeStr.contains("KB") || sizeStr.contains("MB"));
}

TEST_F(MaterialEditorQMLTest, FileSystem_ListDirectory) {
    // /tmp should have some entries (or at least be a valid call)
    QVariantList entries = editor->listDirectory("/tmp");
    // We simply verify the call succeeds. /tmp might have image files or not,
    // but the result type should be a list.
    // listDirectory filters for image files + dirs, so we just check it doesn't crash.
    EXPECT_GE(entries.size(), 0);

    // Non-existent directory should return empty list
    QVariantList empty = editor->listDirectory("/nonexistent_path_xyz_99999");
    EXPECT_EQ(empty.size(), 0);
}

// ===========================================================================
// Basic fixture tests -- enum name getters
// ===========================================================================

TEST_F(MaterialEditorQMLTest, GetPolygonModeNames) {
    QStringList names = editor->getPolygonModeNames();
    EXPECT_EQ(names.size(), 3);
    EXPECT_EQ(names[0], "Points");
    EXPECT_EQ(names[1], "Wireframe");
    EXPECT_EQ(names[2], "Solid");
}

TEST_F(MaterialEditorQMLTest, GetBlendFactorNames) {
    QStringList names = editor->getBlendFactorNames();
    EXPECT_FALSE(names.isEmpty());
    // Should contain known entries
    EXPECT_TRUE(names.contains("One"));
    EXPECT_TRUE(names.contains("Zero"));
}

TEST_F(MaterialEditorQMLTest, GetShadingModeNames) {
    QStringList names = editor->getShadingModeNames();
    EXPECT_EQ(names.size(), 3);
    EXPECT_EQ(names[0], "Flat");
    EXPECT_EQ(names[1], "Gouraud");
    EXPECT_EQ(names[2], "Phong");
}

TEST_F(MaterialEditorQMLTest, GetCullModeNames) {
    QStringList names = editor->getCullModeNames();
    EXPECT_EQ(names.size(), 3);
    EXPECT_TRUE(names.contains("None"));
    EXPECT_TRUE(names.contains("Clockwise"));
    EXPECT_TRUE(names.contains("Counter-Clockwise"));
}

TEST_F(MaterialEditorQMLTest, GetDepthFunctionNames) {
    QStringList names = editor->getDepthFunctionNames();
    EXPECT_EQ(names.size(), 8);
    EXPECT_TRUE(names.contains("Less"));
    EXPECT_TRUE(names.contains("Greater"));
}

TEST_F(MaterialEditorQMLTest, GetAlphaRejectionFunctionNames) {
    QStringList names = editor->getAlphaRejectionFunctionNames();
    EXPECT_EQ(names.size(), 8);
    EXPECT_TRUE(names.contains("Always Pass"));
}

TEST_F(MaterialEditorQMLTest, GetSceneBlendOperationNames) {
    QStringList names = editor->getSceneBlendOperationNames();
    EXPECT_EQ(names.size(), 5);
    EXPECT_TRUE(names.contains("Add"));
    EXPECT_TRUE(names.contains("Max"));
}

TEST_F(MaterialEditorQMLTest, GetFogModeNames) {
    QStringList names = editor->getFogModeNames();
    EXPECT_EQ(names.size(), 4);
    EXPECT_TRUE(names.contains("None"));
    EXPECT_TRUE(names.contains("Linear"));
}

TEST_F(MaterialEditorQMLTest, GetTextureAddressModeNames) {
    QStringList names = editor->getTextureAddressModeNames();
    EXPECT_EQ(names.size(), 4);
    EXPECT_TRUE(names.contains("Wrap"));
    EXPECT_TRUE(names.contains("Border"));
}

TEST_F(MaterialEditorQMLTest, GetTextureFilteringNames) {
    QStringList names = editor->getTextureFilteringNames();
    EXPECT_EQ(names.size(), 4);
    EXPECT_TRUE(names.contains("Bilinear"));
    EXPECT_TRUE(names.contains("Anisotropic"));
}

TEST_F(MaterialEditorQMLTest, GetEnvironmentMappingNames) {
    QStringList names = editor->getEnvironmentMappingNames();
    EXPECT_EQ(names.size(), 2);
    EXPECT_TRUE(names.contains("None"));
    EXPECT_TRUE(names.contains("Enabled"));
}

// ===========================================================================
// Basic fixture tests -- createNewMaterial (no Ogre)
// ===========================================================================

TEST_F(MaterialEditorQMLTest, CreateNewMaterial_DefaultName) {
    QSignalSpy nameSpy(editor.get(), &MaterialEditorQML::materialNameChanged);
    QSignalSpy textSpy(editor.get(), &MaterialEditorQML::materialTextChanged);

    editor->createNewMaterial();

    EXPECT_EQ(editor->materialName(), "new_material");
    EXPECT_TRUE(editor->materialText().contains("material new_material"));
    EXPECT_GE(nameSpy.count(), 1);
    EXPECT_GE(textSpy.count(), 1);
}

TEST_F(MaterialEditorQMLTest, CreateNewMaterial_CustomName) {
    editor->createNewMaterial("MyTestMat");

    EXPECT_EQ(editor->materialName(), "MyTestMat");
    EXPECT_TRUE(editor->materialText().contains("material MyTestMat"));
}

// ===========================================================================
// Basic fixture tests -- validate material script (no Ogre)
// ===========================================================================

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_Valid) {
    QString validScript =
        "material TestMaterial\n"
        "{\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\tpass\n"
        "\t\t{\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    EXPECT_TRUE(editor->validateMaterialScript(validScript));
}

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_Empty) {
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::errorOccurred);
    EXPECT_FALSE(editor->validateMaterialScript(""));
    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_MissingTechnique) {
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::errorOccurred);
    QString script =
        "material TestMaterial\n"
        "{\n"
        "}";
    EXPECT_FALSE(editor->validateMaterialScript(script));
    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_MismatchedBraces) {
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::errorOccurred);
    QString script =
        "material TestMaterial\n"
        "{\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\tpass\n"
        "\t\t{\n"
        "\t}\n"
        "}";
    EXPECT_FALSE(editor->validateMaterialScript(script));
    EXPECT_GE(errorSpy.count(), 1);
}

// ===========================================================================
// Basic fixture tests -- undo/redo (no Ogre)
// ===========================================================================

TEST_F(MaterialEditorQMLTest, UndoRedo_InitialState) {
    EXPECT_FALSE(editor->canUndo());
    EXPECT_FALSE(editor->canRedo());
}

TEST_F(MaterialEditorQMLTest, UndoRedo_AfterChanges) {
    // Set initial text
    editor->setMaterialText("first text");
    // After first set, undo stack should be empty (no previous text)
    EXPECT_FALSE(editor->canUndo());

    // Change text -- now "first text" goes to undo stack
    editor->setMaterialText("second text");
    EXPECT_TRUE(editor->canUndo());
    EXPECT_FALSE(editor->canRedo());

    // Undo should restore "first text"
    editor->undo();
    EXPECT_EQ(editor->materialText(), "first text");
    EXPECT_FALSE(editor->canUndo());
    EXPECT_TRUE(editor->canRedo());

    // Redo should restore "second text"
    editor->redo();
    EXPECT_EQ(editor->materialText(), "second text");
    EXPECT_TRUE(editor->canUndo());
    EXPECT_FALSE(editor->canRedo());
}

TEST_F(MaterialEditorQMLTest, UndoRedo_ClearHistory) {
    editor->setMaterialText("first");
    editor->setMaterialText("second");
    EXPECT_TRUE(editor->canUndo());

    editor->clearUndoHistory();
    EXPECT_FALSE(editor->canUndo());
    EXPECT_FALSE(editor->canRedo());
}

// ===========================================================================
// Basic fixture tests -- property setters emit signals (no Ogre pass)
// ===========================================================================

TEST_F(MaterialEditorQMLTest, PropertySettersAndSignals_Lighting) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::lightingEnabledChanged);
    editor->setLightingEnabled(false);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FALSE(editor->lightingEnabled());
}

TEST_F(MaterialEditorQMLTest, PropertySettersAndSignals_DepthWrite) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::depthWriteEnabledChanged);
    editor->setDepthWriteEnabled(false);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FALSE(editor->depthWriteEnabled());
}

TEST_F(MaterialEditorQMLTest, PropertySettersAndSignals_AmbientColor) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::ambientColorChanged);
    QColor newColor(255, 0, 0);
    editor->setAmbientColor(newColor);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->ambientColor(), newColor);
}

TEST_F(MaterialEditorQMLTest, PropertySettersAndSignals_DiffuseColor) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::diffuseColorChanged);
    QColor newColor(0, 255, 0);
    editor->setDiffuseColor(newColor);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->diffuseColor(), newColor);
}

TEST_F(MaterialEditorQMLTest, PropertySettersAndSignals_Shininess) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::shininessChanged);
    editor->setShininess(42.0f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FLOAT_EQ(editor->shininess(), 42.0f);
}

TEST_F(MaterialEditorQMLTest, PropertySettersAndSignals_PolygonMode) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::polygonModeChanged);
    editor->setPolygonMode(0); // Points
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->polygonMode(), 0);
}

TEST_F(MaterialEditorQMLTest, PropertySettersAndSignals_NoSignalOnSameValue) {
    // Setting the same default value should not emit a signal
    QSignalSpy spy(editor.get(), &MaterialEditorQML::lightingEnabledChanged);
    editor->setLightingEnabled(true); // default is true
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(MaterialEditorQMLTest, PropertySettersAndSignals_FogOverride) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::fogOverrideChanged);
    editor->setFogOverride(true);
    EXPECT_GE(spy.count(), 1);
    EXPECT_TRUE(editor->fogOverride());
}

TEST_F(MaterialEditorQMLTest, PropertySettersAndSignals_PointSize) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::pointSizeChanged);
    editor->setPointSize(5.0f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FLOAT_EQ(editor->pointSize(), 5.0f);
}

// ===========================================================================
// Basic fixture tests -- theme colors (constants)
// ===========================================================================

TEST_F(MaterialEditorQMLTest, ThemeColors_AreValid) {
    EXPECT_TRUE(editor->backgroundColor().isValid());
    EXPECT_TRUE(editor->panelColor().isValid());
    EXPECT_TRUE(editor->textColor().isValid());
    EXPECT_TRUE(editor->borderColor().isValid());
    EXPECT_TRUE(editor->highlightColor().isValid());
    EXPECT_TRUE(editor->buttonColor().isValid());
    EXPECT_TRUE(editor->buttonTextColor().isValid());
    EXPECT_TRUE(editor->accentColor().isValid());
}

// ===========================================================================
// Basic fixture tests -- testConnection
// ===========================================================================

TEST_F(MaterialEditorQMLTest, TestConnection) {
    QString result = editor->testConnection();
    EXPECT_FALSE(result.isEmpty());
    EXPECT_TRUE(result.contains("successfully"));
}

// ===========================================================================
// Ogre fixture tests
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, CreateNewMaterial) {
    QSignalSpy nameSpy(editor.get(), &MaterialEditorQML::materialNameChanged);

    editor->createNewMaterial("OgreTestMat");
    EXPECT_EQ(editor->materialName(), "OgreTestMat");
    EXPECT_TRUE(editor->materialText().contains("material OgreTestMat"));
    EXPECT_GE(nameSpy.count(), 1);
}

TEST_F(MaterialEditorQMLWithOgreTest, LoadMaterial_BaseWhite) {
    QSignalSpy nameSpy(editor.get(), &MaterialEditorQML::materialNameChanged);

    editor->loadMaterial("BaseWhite");

    EXPECT_EQ(editor->materialName(), "BaseWhite");
    EXPECT_GE(nameSpy.count(), 1);
    // The material text should contain the material name
    EXPECT_TRUE(editor->materialText().contains("BaseWhite"));
    // After loading, technique list should be populated
    EXPECT_FALSE(editor->techniqueList().isEmpty());
}

TEST_F(MaterialEditorQMLWithOgreTest, LoadMaterial_NonExistent) {
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::errorOccurred);

    editor->loadMaterial("NonExistentMaterial_XYZ_12345");

    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(MaterialEditorQMLWithOgreTest, LoadMaterial_Empty) {
    // Loading empty name should create a new material
    editor->loadMaterial("");

    EXPECT_EQ(editor->materialName(), "new_material");
}

TEST_F(MaterialEditorQMLWithOgreTest, ValidateMaterialScript_ValidWithOgre) {
    editor->setMaterialName("TestValidation");
    QString validScript =
        "material TestValidation\n"
        "{\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\tpass\n"
        "\t\t{\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    EXPECT_TRUE(editor->validateMaterialScript(validScript));
}

TEST_F(MaterialEditorQMLWithOgreTest, ValidateMaterialScript_InvalidWithOgre) {
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::errorOccurred);
    EXPECT_FALSE(editor->validateMaterialScript("not a valid material script at all"));
    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(MaterialEditorQMLWithOgreTest, MaterialList_NonEmpty) {
    QStringList materials = editor->getMaterialList();
    EXPECT_FALSE(materials.isEmpty());
    // Should at least contain BaseWhite and BaseWhiteNoLighting
    EXPECT_TRUE(materials.contains("BaseWhite"));
    EXPECT_TRUE(materials.contains("BaseWhiteNoLighting"));
}

TEST_F(MaterialEditorQMLWithOgreTest, CreateTechniquePassTextureUnit) {
    // Load BaseWhite so we have a valid Ogre material
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->techniqueList().isEmpty());

    int originalTechCount = editor->techniqueList().size();

    // Create a new technique
    editor->createNewTechnique("TestTechnique");
    EXPECT_EQ(editor->techniqueList().size(), originalTechCount + 1);

    // Select the new technique
    editor->setSelectedTechniqueIndex(editor->techniqueList().size() - 1);

    // The new technique should have no passes initially
    // (createNewTechnique does NOT auto-create a pass)
    // Actually, after selecting technique the passList updates
    int passCount = editor->passList().size();

    // Create a new pass
    editor->createNewPass("TestPass");
    EXPECT_GT(editor->passList().size(), passCount);

    // Select the pass
    editor->setSelectedPassIndex(editor->passList().size() - 1);

    // Create a texture unit
    int texUnitCount = editor->textureUnitList().size();
    editor->createNewTextureUnit("TestTexUnit");
    EXPECT_GT(editor->textureUnitList().size(), texUnitCount);
}

TEST_F(MaterialEditorQMLWithOgreTest, PropertySettersWithOgrePass) {
    // Load a material so we have a valid pass
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->techniqueList().isEmpty());
    ASSERT_FALSE(editor->passList().isEmpty());

    // Set various properties and verify they take effect
    QSignalSpy lightingSpy(editor.get(), &MaterialEditorQML::lightingEnabledChanged);
    editor->setLightingEnabled(false);
    EXPECT_GE(lightingSpy.count(), 1);
    EXPECT_FALSE(editor->lightingEnabled());

    QSignalSpy depthWriteSpy(editor.get(), &MaterialEditorQML::depthWriteEnabledChanged);
    editor->setDepthWriteEnabled(false);
    EXPECT_GE(depthWriteSpy.count(), 1);

    QSignalSpy depthCheckSpy(editor.get(), &MaterialEditorQML::depthCheckEnabledChanged);
    editor->setDepthCheckEnabled(false);
    EXPECT_GE(depthCheckSpy.count(), 1);

    QSignalSpy shininessSpy(editor.get(), &MaterialEditorQML::shininessChanged);
    editor->setShininess(64.0f);
    EXPECT_GE(shininessSpy.count(), 1);
    EXPECT_FLOAT_EQ(editor->shininess(), 64.0f);

    // Material text should update after property changes
    EXPECT_FALSE(editor->materialText().isEmpty());
}

TEST_F(MaterialEditorQMLWithOgreTest, ColorSettersWithOgrePass) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->techniqueList().isEmpty());
    ASSERT_FALSE(editor->passList().isEmpty());

    QColor red(255, 0, 0);
    QColor green(0, 255, 0);
    QColor blue(0, 0, 255);

    QSignalSpy ambientSpy(editor.get(), &MaterialEditorQML::ambientColorChanged);
    editor->setAmbientColor(red);
    EXPECT_GE(ambientSpy.count(), 1);

    QSignalSpy diffuseSpy(editor.get(), &MaterialEditorQML::diffuseColorChanged);
    editor->setDiffuseColor(green);
    EXPECT_GE(diffuseSpy.count(), 1);

    QSignalSpy specularSpy(editor.get(), &MaterialEditorQML::specularColorChanged);
    editor->setSpecularColor(blue);
    EXPECT_GE(specularSpy.count(), 1);

    QSignalSpy emissiveSpy(editor.get(), &MaterialEditorQML::emissiveColorChanged);
    editor->setEmissiveColor(red);
    EXPECT_GE(emissiveSpy.count(), 1);
}

TEST_F(MaterialEditorQMLWithOgreTest, AdvancedPassProperties) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->techniqueList().isEmpty());
    ASSERT_FALSE(editor->passList().isEmpty());

    QSignalSpy shadingSpy(editor.get(), &MaterialEditorQML::shadingModeChanged);
    editor->setShadingMode(2); // Phong
    EXPECT_GE(shadingSpy.count(), 1);
    EXPECT_EQ(editor->shadingMode(), 2);

    QSignalSpy cullHwSpy(editor.get(), &MaterialEditorQML::cullHardwareChanged);
    editor->setCullHardware(0); // None
    EXPECT_GE(cullHwSpy.count(), 1);

    QSignalSpy cullSwSpy(editor.get(), &MaterialEditorQML::cullSoftwareChanged);
    editor->setCullSoftware(2); // Front
    EXPECT_GE(cullSwSpy.count(), 1);

    QSignalSpy blendOpSpy(editor.get(), &MaterialEditorQML::sceneBlendOperationChanged);
    editor->setSceneBlendOperation(1); // Subtract
    EXPECT_GE(blendOpSpy.count(), 1);
}

TEST_F(MaterialEditorQMLWithOgreTest, UndoRedo_WithOgreMaterial) {
    editor->loadMaterial("BaseWhite");
    QString originalText = editor->materialText();

    // Change a property which updates material text
    editor->setLightingEnabled(false);
    QString modifiedText = editor->materialText();
    EXPECT_NE(originalText, modifiedText);

    // Undo should restore the previous material text
    EXPECT_TRUE(editor->canUndo());
    editor->undo();
    EXPECT_EQ(editor->materialText(), originalText);

    // Redo should restore the modified text
    EXPECT_TRUE(editor->canRedo());
    editor->redo();
    EXPECT_EQ(editor->materialText(), modifiedText);
}

TEST_F(MaterialEditorQMLWithOgreTest, SelectedIndices_Update) {
    editor->loadMaterial("BaseWhite");

    // After loading, technique 0 and pass 0 should be auto-selected
    EXPECT_EQ(editor->selectedTechniqueIndex(), 0);
    EXPECT_EQ(editor->selectedPassIndex(), 0);

    // Setting invalid indices should still work without crashing
    editor->setSelectedTechniqueIndex(-1);
    EXPECT_EQ(editor->selectedTechniqueIndex(), -1);

    editor->setSelectedPassIndex(-1);
    EXPECT_EQ(editor->selectedPassIndex(), -1);

    editor->setSelectedTextureUnitIndex(-1);
    EXPECT_EQ(editor->selectedTextureUnitIndex(), -1);
}

TEST_F(MaterialEditorQMLWithOgreTest, ApplyMaterial_Valid) {
    editor->loadMaterial("BaseWhite");
    QSignalSpy appliedSpy(editor.get(), &MaterialEditorQML::materialApplied);

    // Apply the current (valid) material text
    bool result = editor->applyMaterial();
    EXPECT_TRUE(result);
    EXPECT_GE(appliedSpy.count(), 1);
}

TEST_F(MaterialEditorQMLWithOgreTest, VertexColorTracking) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    QSignalSpy spy(editor.get(), &MaterialEditorQML::useVertexColorToAmbientChanged);
    editor->setUseVertexColorToAmbient(true);
    EXPECT_GE(spy.count(), 1);
    EXPECT_TRUE(editor->useVertexColorToAmbient());

    editor->setUseVertexColorToDiffuse(true);
    EXPECT_TRUE(editor->useVertexColorToDiffuse());

    editor->setUseVertexColorToSpecular(true);
    EXPECT_TRUE(editor->useVertexColorToSpecular());

    editor->setUseVertexColorToEmissive(true);
    EXPECT_TRUE(editor->useVertexColorToEmissive());
}

TEST_F(MaterialEditorQMLWithOgreTest, BlendFactors) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    QSignalSpy srcSpy(editor.get(), &MaterialEditorQML::sourceBlendFactorChanged);
    editor->setSourceBlendFactor(7); // One
    EXPECT_GE(srcSpy.count(), 1);

    QSignalSpy dstSpy(editor.get(), &MaterialEditorQML::destBlendFactorChanged);
    // BaseWhite default dest blend is SBF_ZERO (enum 1) loaded as 1+1=2, so use a different value
    editor->setDestBlendFactor(7); // SBF_SOURCE_ALPHA + 1
    EXPECT_GE(dstSpy.count(), 1);
}

TEST_F(MaterialEditorQMLWithOgreTest, FogProperties) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    editor->setFogOverride(true);
    EXPECT_TRUE(editor->fogOverride());

    QSignalSpy fogModeSpy(editor.get(), &MaterialEditorQML::fogModeChanged);
    editor->setFogMode(3); // Linear
    EXPECT_GE(fogModeSpy.count(), 1);

    editor->setFogDensity(0.5f);
    EXPECT_FLOAT_EQ(editor->fogDensity(), 0.5f);

    editor->setFogStart(10.0f);
    EXPECT_FLOAT_EQ(editor->fogStart(), 10.0f);

    editor->setFogEnd(100.0f);
    EXPECT_FLOAT_EQ(editor->fogEnd(), 100.0f);

    QColor fogCol(128, 64, 32);
    editor->setFogColor(fogCol);
    EXPECT_EQ(editor->fogColor(), fogCol);
}

TEST_F(MaterialEditorQMLWithOgreTest, DiffuseAlphaAndSpecularAlpha) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    QSignalSpy diffAlphaSpy(editor.get(), &MaterialEditorQML::diffuseAlphaChanged);
    editor->setDiffuseAlpha(0.5f);
    EXPECT_GE(diffAlphaSpy.count(), 1);
    EXPECT_FLOAT_EQ(editor->diffuseAlpha(), 0.5f);

    QSignalSpy specAlphaSpy(editor.get(), &MaterialEditorQML::specularAlphaChanged);
    editor->setSpecularAlpha(0.3f);
    EXPECT_GE(specAlphaSpy.count(), 1);
    EXPECT_FLOAT_EQ(editor->specularAlpha(), 0.3f);
}

TEST_F(MaterialEditorQMLWithOgreTest, TextureUnitOperations) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    // Create a texture unit
    editor->createNewTextureUnit("TestTU");
    EXPECT_FALSE(editor->textureUnitList().isEmpty());

    // Select the texture unit
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);
    EXPECT_GE(editor->selectedTextureUnitIndex(), 0);

    // Remove texture should reset texture name
    editor->removeTexture();
    EXPECT_EQ(editor->textureName(), "*Select a texture*");
}

TEST_F(MaterialEditorQMLWithOgreTest, DepthBias) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    QSignalSpy constSpy(editor.get(), &MaterialEditorQML::depthBiasConstantChanged);
    editor->setDepthBiasConstant(1.5f);
    EXPECT_GE(constSpy.count(), 1);
    EXPECT_FLOAT_EQ(editor->depthBiasConstant(), 1.5f);

    QSignalSpy slopeSpy(editor.get(), &MaterialEditorQML::depthBiasSlopeScaleChanged);
    editor->setDepthBiasSlopeScale(2.0f);
    EXPECT_GE(slopeSpy.count(), 1);
    EXPECT_FLOAT_EQ(editor->depthBiasSlopeScale(), 2.0f);
}

TEST_F(MaterialEditorQMLWithOgreTest, AlphaRejection) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    editor->setAlphaRejectionEnabled(true);
    EXPECT_TRUE(editor->alphaRejectionEnabled());

    editor->setAlphaRejectionFunction(2); // Less
    EXPECT_EQ(editor->alphaRejectionFunction(), 2);

    editor->setAlphaRejectionValue(128);
    EXPECT_EQ(editor->alphaRejectionValue(), 128);
}

TEST_F(MaterialEditorQMLWithOgreTest, ColourWriteChannels) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    editor->setColourWriteRed(false);
    EXPECT_FALSE(editor->colourWriteRed());

    editor->setColourWriteGreen(false);
    EXPECT_FALSE(editor->colourWriteGreen());

    editor->setColourWriteBlue(false);
    EXPECT_FALSE(editor->colourWriteBlue());

    editor->setColourWriteAlpha(false);
    EXPECT_FALSE(editor->colourWriteAlpha());
}

TEST_F(MaterialEditorQMLWithOgreTest, PointAndLineProperties) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    editor->setPointSize(3.0f);
    EXPECT_FLOAT_EQ(editor->pointSize(), 3.0f);

    editor->setLineWidth(2.0f);
    EXPECT_FLOAT_EQ(editor->lineWidth(), 2.0f);

    editor->setPointSpritesEnabled(true);
    EXPECT_TRUE(editor->pointSpritesEnabled());
}

TEST_F(MaterialEditorQMLWithOgreTest, LightLimits) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    editor->setMaxLights(4);
    EXPECT_EQ(editor->maxLights(), 4);

    editor->setStartLight(1);
    EXPECT_EQ(editor->startLight(), 1);
}

TEST_F(MaterialEditorQMLWithOgreTest, DepthFunction) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    QSignalSpy spy(editor.get(), &MaterialEditorQML::depthFunctionChanged);
    editor->setDepthFunction(2); // Less
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->depthFunction(), 2);
}

TEST_F(MaterialEditorQMLWithOgreTest, AlphaToCoverage) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    editor->setAlphaToCoverageEnabled(true);
    EXPECT_TRUE(editor->alphaToCoverageEnabled());
}

// ===========================================================================
// Texture transform setters (with Ogre)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, TextureUOffset) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());
    editor->createNewTextureUnit("TexOffsetTU");
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::textureUOffsetChanged);
    editor->setTextureUOffset(0.5f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FLOAT_EQ(editor->textureUOffset(), 0.5f);
}

TEST_F(MaterialEditorQMLWithOgreTest, TextureVOffset) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());
    editor->createNewTextureUnit("TexVOffsetTU");
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::textureVOffsetChanged);
    editor->setTextureVOffset(0.3f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FLOAT_EQ(editor->textureVOffset(), 0.3f);
}

TEST_F(MaterialEditorQMLWithOgreTest, TextureUScale) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());
    editor->createNewTextureUnit("TexUScaleTU");
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::textureUScaleChanged);
    editor->setTextureUScale(2.0f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FLOAT_EQ(editor->textureUScale(), 2.0f);
}

TEST_F(MaterialEditorQMLWithOgreTest, TextureVScale) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());
    editor->createNewTextureUnit("TexVScaleTU");
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::textureVScaleChanged);
    editor->setTextureVScale(3.0f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FLOAT_EQ(editor->textureVScale(), 3.0f);
}

TEST_F(MaterialEditorQMLWithOgreTest, TextureRotation) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());
    editor->createNewTextureUnit("TexRotTU");
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::textureRotationChanged);
    editor->setTextureRotation(45.0f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FLOAT_EQ(editor->textureRotation(), 45.0f);
}

// ===========================================================================
// UV animation setters (with Ogre)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, ScrollAnimUSpeed) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());
    editor->createNewTextureUnit("ScrollUTU");
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::scrollAnimUSpeedChanged);
    editor->setScrollAnimUSpeed(1.5);
    EXPECT_GE(spy.count(), 1);
    EXPECT_DOUBLE_EQ(editor->scrollAnimUSpeed(), 1.5);
}

TEST_F(MaterialEditorQMLWithOgreTest, ScrollAnimVSpeed) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());
    editor->createNewTextureUnit("ScrollVTU");
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::scrollAnimVSpeedChanged);
    editor->setScrollAnimVSpeed(2.0);
    EXPECT_GE(spy.count(), 1);
    EXPECT_DOUBLE_EQ(editor->scrollAnimVSpeed(), 2.0);
}

TEST_F(MaterialEditorQMLWithOgreTest, RotateAnimSpeed) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());
    editor->createNewTextureUnit("RotAnimTU");
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::rotateAnimSpeedChanged);
    editor->setRotateAnimSpeed(0.5);
    EXPECT_GE(spy.count(), 1);
    EXPECT_DOUBLE_EQ(editor->rotateAnimSpeed(), 0.5);
}

// ===========================================================================
// Environment mapping (with Ogre)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, EnvironmentMapping) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());
    editor->createNewTextureUnit("EnvMapTU");
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::environmentMappingChanged);

    // Test None
    editor->setEnvironmentMapping(0);
    EXPECT_EQ(editor->environmentMapping(), 0);

    // Test Planar
    editor->setEnvironmentMapping(1);
    EXPECT_EQ(editor->environmentMapping(), 1);

    // Test Curved
    editor->setEnvironmentMapping(2);
    EXPECT_EQ(editor->environmentMapping(), 2);

    EXPECT_GE(spy.count(), 2); // at least 2 changes (0->1, 1->2)
}

// ===========================================================================
// Export material (with Ogre)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, ExportMaterial_Valid) {
    editor->loadMaterial("BaseWhite");

    editor->exportMaterial("/tmp/test_matexport.material");

    // Verify file was created
    EXPECT_TRUE(QFile::exists("/tmp/test_matexport.material"));
    QFile::remove("/tmp/test_matexport.material");
}

TEST_F(MaterialEditorQMLWithOgreTest, ExportMaterial_NoMaterial) {
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::errorOccurred);
    // No material loaded — should emit error
    editor->exportMaterial("/tmp/test_matexport_none.material");
    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(MaterialEditorQMLWithOgreTest, ExportMaterial_ByName) {
    editor->exportMaterial("/tmp/test_matexport_named.material", "BaseWhite");

    EXPECT_TRUE(QFile::exists("/tmp/test_matexport_named.material"));
    QFile::remove("/tmp/test_matexport_named.material");
}

TEST_F(MaterialEditorQMLWithOgreTest, ExportMaterial_ByNameNotFound) {
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::errorOccurred);
    editor->exportMaterial("/tmp/test.material", "NonExistentMaterial_XYZ");
    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(MaterialEditorQMLWithOgreTest, ExportMaterial_EmptyParams) {
    // Both empty — should silently return
    editor->exportMaterial("", "");
    // No crash is the test
}

// ===========================================================================
// Import material file (with Ogre)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, ImportMaterialFile_Empty) {
    // Empty path should silently return
    editor->importMaterialFile("");
    // No crash is the test
}

TEST_F(MaterialEditorQMLWithOgreTest, ImportMaterialFile_NonExistentPath) {
    // Non-existent path — Ogre may throw, should be caught
    editor->importMaterialFile("/tmp/nonexistent_xyz.material");
    // No crash is the test — error is handled internally
}

// ===========================================================================
// Texture transform setters without Ogre (signal-only)
// ===========================================================================

TEST_F(MaterialEditorQMLTest, TextureTransform_SignalsOnly) {
    QSignalSpy uOffSpy(editor.get(), &MaterialEditorQML::textureUOffsetChanged);
    editor->setTextureUOffset(0.25f);
    EXPECT_GE(uOffSpy.count(), 1);
    EXPECT_FLOAT_EQ(editor->textureUOffset(), 0.25f);

    QSignalSpy vOffSpy(editor.get(), &MaterialEditorQML::textureVOffsetChanged);
    editor->setTextureVOffset(0.75f);
    EXPECT_GE(vOffSpy.count(), 1);
    EXPECT_FLOAT_EQ(editor->textureVOffset(), 0.75f);

    QSignalSpy uScaleSpy(editor.get(), &MaterialEditorQML::textureUScaleChanged);
    editor->setTextureUScale(4.0f);
    EXPECT_GE(uScaleSpy.count(), 1);
    EXPECT_FLOAT_EQ(editor->textureUScale(), 4.0f);

    QSignalSpy vScaleSpy(editor.get(), &MaterialEditorQML::textureVScaleChanged);
    editor->setTextureVScale(5.0f);
    EXPECT_GE(vScaleSpy.count(), 1);
    EXPECT_FLOAT_EQ(editor->textureVScale(), 5.0f);

    QSignalSpy rotSpy(editor.get(), &MaterialEditorQML::textureRotationChanged);
    editor->setTextureRotation(90.0f);
    EXPECT_GE(rotSpy.count(), 1);
    EXPECT_FLOAT_EQ(editor->textureRotation(), 90.0f);
}

TEST_F(MaterialEditorQMLTest, ScrollAnim_SignalsOnly) {
    QSignalSpy uSpeedSpy(editor.get(), &MaterialEditorQML::scrollAnimUSpeedChanged);
    editor->setScrollAnimUSpeed(3.0);
    EXPECT_GE(uSpeedSpy.count(), 1);
    EXPECT_DOUBLE_EQ(editor->scrollAnimUSpeed(), 3.0);

    QSignalSpy vSpeedSpy(editor.get(), &MaterialEditorQML::scrollAnimVSpeedChanged);
    editor->setScrollAnimVSpeed(4.0);
    EXPECT_GE(vSpeedSpy.count(), 1);
    EXPECT_DOUBLE_EQ(editor->scrollAnimVSpeed(), 4.0);
}

TEST_F(MaterialEditorQMLTest, RotateAnimSpeed_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::rotateAnimSpeedChanged);
    editor->setRotateAnimSpeed(1.0);
    EXPECT_GE(spy.count(), 1);
    EXPECT_DOUBLE_EQ(editor->rotateAnimSpeed(), 1.0);
}

TEST_F(MaterialEditorQMLTest, EnvironmentMapping_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::environmentMappingChanged);
    editor->setEnvironmentMapping(1);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->environmentMapping(), 1);
}

TEST_F(MaterialEditorQMLTest, TextureTransform_NoChangeNoSignal) {
    // Set initial values
    editor->setTextureUOffset(0.0f);
    editor->setTextureVOffset(0.0f);

    // Setting same value again should not emit
    QSignalSpy uSpy(editor.get(), &MaterialEditorQML::textureUOffsetChanged);
    editor->setTextureUOffset(0.0f);
    EXPECT_EQ(uSpy.count(), 0);

    QSignalSpy vSpy(editor.get(), &MaterialEditorQML::textureVOffsetChanged);
    editor->setTextureVOffset(0.0f);
    EXPECT_EQ(vSpy.count(), 0);
}
