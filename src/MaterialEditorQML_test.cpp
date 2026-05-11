#include <gtest/gtest.h>
#include <memory>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include "MaterialEditorQML.h"
#include "Manager.h"
#include <OgreException.h>
#include "TestHelpers.h"
#ifdef ENABLE_LOCAL_LLM
#include "LLMManager.h"
#endif

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
        // Ensure no Ogre state leaks from prior test suites so that
        // isOgreAvailable() reliably returns false for these tests.
        Manager::kill();
        QThread::msleep(50);

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

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
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

TEST_F(MaterialEditorQMLTest, FileSystem_ListDirectoryFiltersOnlyImagesAndDirectories) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    const QString subdirPath = tempDir.filePath("textures");
    ASSERT_TRUE(QDir().mkpath(subdirPath));

    QFile imageFile(tempDir.filePath("albedo.png"));
    ASSERT_TRUE(imageFile.open(QIODevice::WriteOnly));
    imageFile.write("png");
    imageFile.close();

    QFile textFile(tempDir.filePath("notes.txt"));
    ASSERT_TRUE(textFile.open(QIODevice::WriteOnly));
    textFile.write("txt");
    textFile.close();

    QVariantList entries = editor->listDirectory(tempDir.path());
    ASSERT_GE(entries.size(), 2);

    bool foundDir = false;
    bool foundImage = false;
    bool foundText = false;
    for (const QVariant& v : entries) {
        const QVariantMap item = v.toMap();
        const QString name = item.value("name").toString();
        const QString type = item.value("type").toString();
        if (name == "textures" && type == "dir") foundDir = true;
        if (name == "albedo.png" && type == "file") foundImage = true;
        if (name == "notes.txt") foundText = true;
    }

    EXPECT_TRUE(foundDir);
    EXPECT_TRUE(foundImage);
    EXPECT_FALSE(foundText);
}

TEST_F(MaterialEditorQMLTest, FileSystem_GetFileSizeStringCoversKbAndMbBranches) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    const QString kbPath = tempDir.filePath("kb.bin");
    QFile kbFile(kbPath);
    ASSERT_TRUE(kbFile.open(QIODevice::WriteOnly));
    kbFile.write(QByteArray(2048, 'k'));
    kbFile.close();

    const QString mbPath = tempDir.filePath("mb.bin");
    QFile mbFile(mbPath);
    ASSERT_TRUE(mbFile.open(QIODevice::WriteOnly));
    mbFile.write(QByteArray(2 * 1024 * 1024, 'm'));
    mbFile.close();

    const QString kbSize = editor->getFileSizeString(kbPath);
    const QString mbSize = editor->getFileSizeString(mbPath);

    EXPECT_TRUE(kbSize.contains("KB"));
    EXPECT_TRUE(mbSize.contains("MB"));
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

// ===========================================================================
// Texture Unit properties signal-only tests (no Ogre)
// ===========================================================================

TEST_F(MaterialEditorQMLTest, TexCoordSet_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::texCoordSetChanged);
    editor->setTexCoordSet(1);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->texCoordSet(), 1);
}

TEST_F(MaterialEditorQMLTest, TextureAddressMode_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::textureAddressModeChanged);
    editor->setTextureAddressMode(2);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->textureAddressMode(), 2);
}

TEST_F(MaterialEditorQMLTest, TextureBorderColor_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::textureBorderColorChanged);
    QColor borderColor(255, 128, 64);
    editor->setTextureBorderColor(borderColor);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->textureBorderColor(), borderColor);
}

TEST_F(MaterialEditorQMLTest, TextureFiltering_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::textureFilteringChanged);
    editor->setTextureFiltering(3);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->textureFiltering(), 3);
}

TEST_F(MaterialEditorQMLTest, MaxAnisotropy_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::maxAnisotropyChanged);
    editor->setMaxAnisotropy(16);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->maxAnisotropy(), 16);
}

TEST_F(MaterialEditorQMLTest, ThemeColors_DisabledTextColorIsValid) {
    EXPECT_TRUE(editor->disabledTextColor().isValid());
}

TEST_F(MaterialEditorQMLTest, ColorSetters_NoChangeNoSignal) {
    QColor red(255, 0, 0);
    editor->setAmbientColor(red);

    QSignalSpy ambientSpy(editor.get(), &MaterialEditorQML::ambientColorChanged);
    editor->setAmbientColor(red);
    EXPECT_EQ(ambientSpy.count(), 0);

    QColor green(0, 255, 0);
    editor->setDiffuseColor(green);
    QSignalSpy diffuseSpy(editor.get(), &MaterialEditorQML::diffuseColorChanged);
    editor->setDiffuseColor(green);
    EXPECT_EQ(diffuseSpy.count(), 0);

    QColor blue(0, 0, 255);
    editor->setSpecularColor(blue);
    QSignalSpy specularSpy(editor.get(), &MaterialEditorQML::specularColorChanged);
    editor->setSpecularColor(blue);
    EXPECT_EQ(specularSpy.count(), 0);

    QColor white(255, 255, 255);
    editor->setEmissiveColor(white);
    QSignalSpy emissiveSpy(editor.get(), &MaterialEditorQML::emissiveColorChanged);
    editor->setEmissiveColor(white);
    EXPECT_EQ(emissiveSpy.count(), 0);
}

TEST_F(MaterialEditorQMLTest, FogColor_NoChangeNoSignal) {
    QColor fogCol(64, 128, 192);
    editor->setFogColor(fogCol);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::fogColorChanged);
    editor->setFogColor(fogCol);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(MaterialEditorQMLTest, TextureBorderColor_NoChangeNoSignal) {
    QColor borderCol(32, 64, 96);
    editor->setTextureBorderColor(borderCol);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::textureBorderColorChanged);
    editor->setTextureBorderColor(borderCol);
    EXPECT_EQ(spy.count(), 0);
}

// Texture unit properties with Ogre pass

TEST_F(MaterialEditorQMLWithOgreTest, TexCoordSet) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());
    editor->createNewTextureUnit("TexCoordTU");
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::texCoordSetChanged);
    editor->setTexCoordSet(2);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->texCoordSet(), 2);
}

TEST_F(MaterialEditorQMLWithOgreTest, TextureAddressMode) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());
    editor->createNewTextureUnit("TexAddrTU");
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::textureAddressModeChanged);
    editor->setTextureAddressMode(1);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->textureAddressMode(), 1);
}

TEST_F(MaterialEditorQMLWithOgreTest, TextureBorderColor) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());
    editor->createNewTextureUnit("TexBorderTU");
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::textureBorderColorChanged);
    QColor borderColor(128, 64, 32);
    editor->setTextureBorderColor(borderColor);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->textureBorderColor(), borderColor);
}

TEST_F(MaterialEditorQMLWithOgreTest, TextureFiltering) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());
    editor->createNewTextureUnit("TexFilterTU");
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::textureFilteringChanged);
    editor->setTextureFiltering(2);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->textureFiltering(), 2);
}

TEST_F(MaterialEditorQMLWithOgreTest, MaxAnisotropy) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());
    editor->createNewTextureUnit("TexAnisoTU");
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::maxAnisotropyChanged);
    editor->setMaxAnisotropy(8);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->maxAnisotropy(), 8);
}

TEST_F(MaterialEditorQMLWithOgreTest, GetAvailableTextures) {
    QStringList textures = editor->getAvailableTextures();
    EXPECT_GE(textures.size(), 0);
}

TEST_F(MaterialEditorQMLWithOgreTest, GetTexturePreviewPath) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    QString previewPath = editor->getTexturePreviewPath();
    EXPECT_GE(previewPath.length(), 0);
}

TEST_F(MaterialEditorQMLWithOgreTest, MultiplePassOperations) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->techniqueList().isEmpty());
    editor->setSelectedTechniqueIndex(0);

    int originalCount = editor->passList().size();
    editor->createNewPass("Pass1");
    editor->createNewPass("Pass2");
    editor->createNewPass("Pass3");

    EXPECT_EQ(editor->passList().size(), originalCount + 3);
}

TEST_F(MaterialEditorQMLWithOgreTest, SwitchBetweenPasses) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->techniqueList().isEmpty());
    ASSERT_FALSE(editor->passList().isEmpty());

    editor->setSelectedPassIndex(0);
    editor->setLightingEnabled(false);
    EXPECT_FALSE(editor->lightingEnabled());

    editor->createNewPass("SecondPass");
    editor->setSelectedPassIndex(editor->passList().size() - 1);

    EXPECT_TRUE(editor->lightingEnabled());
}

TEST_F(MaterialEditorQMLWithOgreTest, ApplyMaterial_InvalidText) {
    editor->loadMaterial("BaseWhite");
    editor->setMaterialText("invalid material script {{{" );

    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::errorOccurred);
    bool result = editor->applyMaterial();

    EXPECT_FALSE(result);
    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(MaterialEditorQMLWithOgreTest, ApplyMaterial_EmptyText) {
    editor->loadMaterial("BaseWhite");
    editor->setMaterialText("");

    bool result = editor->applyMaterial();
    EXPECT_FALSE(result);
}

TEST_F(MaterialEditorQMLWithOgreTest, OpenMaterialEditorWindow_WithMaterialName) {
    editor->openMaterialEditorWindow("BaseWhite");
    EXPECT_EQ(editor->materialName(), "BaseWhite");
}

TEST_F(MaterialEditorQMLWithOgreTest, OpenMaterialEditorWindow_WithoutMaterialName) {
    editor->openMaterialEditorWindow();
    EXPECT_EQ(editor->materialName(), "new_material");
}

// ===========================================================================
// NEW: Branch coverage — applyMaterial without Ogre (no-Ogre branch)
// ===========================================================================

TEST_F(MaterialEditorQMLTest, ApplyMaterial_NoOgre_ValidScript) {
    // Without Ogre, applyMaterial validates + emits materialApplied
    QString validScript =
        "material TestApplyNoOgre\n"
        "{\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\tpass\n"
        "\t\t{\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    editor->setMaterialText(validScript);

    QSignalSpy appliedSpy(editor.get(), &MaterialEditorQML::materialApplied);
    bool result = editor->applyMaterial();
    EXPECT_TRUE(result);
    EXPECT_GE(appliedSpy.count(), 1);
}

TEST_F(MaterialEditorQMLTest, ApplyMaterial_NoOgre_InvalidScript) {
    // Without Ogre, applyMaterial returns false for invalid scripts
    editor->setMaterialText("invalid garbage {{{");

    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::errorOccurred);
    bool result = editor->applyMaterial();
    EXPECT_FALSE(result);
    EXPECT_GE(errorSpy.count(), 1);
}

// ===========================================================================
// NEW: Branch coverage — loadMaterial without Ogre (isOgreAvailable=false)
// ===========================================================================

TEST_F(MaterialEditorQMLTest, LoadMaterial_NoOgre_SetsTemplate) {
    // Without Ogre, loadMaterial(name) generates a template script
    QSignalSpy nameSpy(editor.get(), &MaterialEditorQML::materialNameChanged);
    editor->loadMaterial("TestNoOgreMat");

    EXPECT_EQ(editor->materialName(), "TestNoOgreMat");
    EXPECT_TRUE(editor->materialText().contains("material TestNoOgreMat"));
    EXPECT_TRUE(editor->materialText().contains("technique"));
    EXPECT_GE(nameSpy.count(), 1);
}

// ===========================================================================
// NEW: Branch coverage — undo stack max limit (>50 items)
// ===========================================================================

TEST_F(MaterialEditorQMLTest, UndoRedo_StackMaxLimit) {
    // Push more than the 50-step limit to test truncation
    for (int i = 0; i < 55; ++i) {
        editor->setMaterialText(QString("text_%1").arg(i));
    }
    EXPECT_TRUE(editor->canUndo());

    // Undo all possible steps
    int undoCount = 0;
    while (editor->canUndo()) {
        editor->undo();
        ++undoCount;
    }
    // Should be capped at 50 max undo steps
    EXPECT_LE(undoCount, 50);
}

TEST_F(MaterialEditorQMLTest, UndoRedo_RedoClearedOnNewEdit) {
    editor->setMaterialText("first");
    editor->setMaterialText("second");
    editor->setMaterialText("third");

    // Undo twice
    editor->undo();
    editor->undo();
    EXPECT_TRUE(editor->canRedo());

    // New edit should clear redo stack
    editor->setMaterialText("new_text");
    EXPECT_FALSE(editor->canRedo());
    EXPECT_TRUE(editor->canUndo());
}

// ===========================================================================
// NEW: Branch coverage — applyMaterial with Ogre (remove-existing branch)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, ApplyMaterial_ReapplyRemovesExisting) {
    editor->loadMaterial("BaseWhite");
    QString originalText = editor->materialText();

    // Apply once
    bool result1 = editor->applyMaterial();
    EXPECT_TRUE(result1);

    // Apply again — exercises the "resourceExists → remove" branch
    bool result2 = editor->applyMaterial();
    EXPECT_TRUE(result2);
}

// ===========================================================================
// NEW: Branch coverage — loadMaterial then createNewMaterial (state reset)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, LoadThenCreateNew_ResetsState) {
    // Load an existing material
    editor->loadMaterial("BaseWhite");
    EXPECT_EQ(editor->materialName(), "BaseWhite");
    EXPECT_GE(editor->selectedTechniqueIndex(), 0);

    // Create a new material — should reset state
    editor->createNewMaterial("FreshMat");
    EXPECT_EQ(editor->materialName(), "FreshMat");
    EXPECT_TRUE(editor->materialText().contains("material FreshMat"));
}

// ===========================================================================
// NEW: Branch coverage — ValidateMaterialScript with deeper nesting
// ===========================================================================

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_MultipleTechniques) {
    QString script =
        "material MultiTech\n"
        "{\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\tpass\n"
        "\t\t{\n"
        "\t\t}\n"
        "\t}\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\tpass\n"
        "\t\t{\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    EXPECT_TRUE(editor->validateMaterialScript(script));
}

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_MissingMaterialKeyword) {
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::errorOccurred);
    // Script without "material" keyword
    QString script = "{\n\ttechnique\n\t{\n\t\tpass\n\t\t{\n\t\t}\n\t}\n}";
    EXPECT_FALSE(editor->validateMaterialScript(script));
    EXPECT_GE(errorSpy.count(), 1);
}

// ===========================================================================
// Signal-only tests (MaterialEditorQMLTest fixture)
// ===========================================================================

TEST_F(MaterialEditorQMLTest, DepthCheckEnabled_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::depthCheckEnabledChanged);
    editor->setDepthCheckEnabled(false);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FALSE(editor->depthCheckEnabled());
}

TEST_F(MaterialEditorQMLTest, SpecularColor_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::specularColorChanged);
    QColor newColor(100, 200, 50);
    editor->setSpecularColor(newColor);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->specularColor(), newColor);
}

TEST_F(MaterialEditorQMLTest, EmissiveColor_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::emissiveColorChanged);
    QColor newColor(255, 128, 64);
    editor->setEmissiveColor(newColor);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->emissiveColor(), newColor);
}

TEST_F(MaterialEditorQMLTest, ShadingMode_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::shadingModeChanged);
    editor->setShadingMode(2); // Phong
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->shadingMode(), 2);
}

TEST_F(MaterialEditorQMLTest, CullHardware_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::cullHardwareChanged);
    editor->setCullHardware(2); // Counter-Clockwise
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->cullHardware(), 2);
}

TEST_F(MaterialEditorQMLTest, CullSoftware_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::cullSoftwareChanged);
    editor->setCullSoftware(1); // Back
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->cullSoftware(), 1);
}

TEST_F(MaterialEditorQMLTest, DepthFunction_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::depthFunctionChanged);
    editor->setDepthFunction(3); // Greater
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->depthFunction(), 3);
}

TEST_F(MaterialEditorQMLTest, AlphaRejectionEnabled_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::alphaRejectionEnabledChanged);
    editor->setAlphaRejectionEnabled(true);
    EXPECT_GE(spy.count(), 1);
    EXPECT_TRUE(editor->alphaRejectionEnabled());
}

TEST_F(MaterialEditorQMLTest, AlphaRejectionFunction_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::alphaRejectionFunctionChanged);
    editor->setAlphaRejectionFunction(3); // Greater
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->alphaRejectionFunction(), 3);
}

TEST_F(MaterialEditorQMLTest, AlphaRejectionValue_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::alphaRejectionValueChanged);
    editor->setAlphaRejectionValue(200);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->alphaRejectionValue(), 200);
}

TEST_F(MaterialEditorQMLTest, AlphaToCoverageEnabled_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::alphaToCoverageEnabledChanged);
    editor->setAlphaToCoverageEnabled(true);
    EXPECT_GE(spy.count(), 1);
    EXPECT_TRUE(editor->alphaToCoverageEnabled());
}

TEST_F(MaterialEditorQMLTest, ColourWriteChannels_SignalOnly) {
    // Red channel
    QSignalSpy redSpy(editor.get(), &MaterialEditorQML::colourWriteRedChanged);
    editor->setColourWriteRed(false);
    EXPECT_GE(redSpy.count(), 1);
    EXPECT_FALSE(editor->colourWriteRed());

    // Green channel
    QSignalSpy greenSpy(editor.get(), &MaterialEditorQML::colourWriteGreenChanged);
    editor->setColourWriteGreen(false);
    EXPECT_GE(greenSpy.count(), 1);
    EXPECT_FALSE(editor->colourWriteGreen());

    // Blue channel
    QSignalSpy blueSpy(editor.get(), &MaterialEditorQML::colourWriteBlueChanged);
    editor->setColourWriteBlue(false);
    EXPECT_GE(blueSpy.count(), 1);
    EXPECT_FALSE(editor->colourWriteBlue());

    // Alpha channel
    QSignalSpy alphaSpy(editor.get(), &MaterialEditorQML::colourWriteAlphaChanged);
    editor->setColourWriteAlpha(false);
    EXPECT_GE(alphaSpy.count(), 1);
    EXPECT_FALSE(editor->colourWriteAlpha());
}

TEST_F(MaterialEditorQMLTest, SceneBlendOperation_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::sceneBlendOperationChanged);
    editor->setSceneBlendOperation(2); // Reverse Subtract
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->sceneBlendOperation(), 2);
}

TEST_F(MaterialEditorQMLTest, LineWidth_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::lineWidthChanged);
    editor->setLineWidth(3.5f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FLOAT_EQ(editor->lineWidth(), 3.5f);
}

TEST_F(MaterialEditorQMLTest, PointSpritesEnabled_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::pointSpritesEnabledChanged);
    editor->setPointSpritesEnabled(true);
    EXPECT_GE(spy.count(), 1);
    EXPECT_TRUE(editor->pointSpritesEnabled());
}

TEST_F(MaterialEditorQMLTest, MaxLights_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::maxLightsChanged);
    editor->setMaxLights(8);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->maxLights(), 8);
}

TEST_F(MaterialEditorQMLTest, StartLight_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::startLightChanged);
    editor->setStartLight(2);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->startLight(), 2);
}

TEST_F(MaterialEditorQMLTest, SourceBlendFactor_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::sourceBlendFactorChanged);
    editor->setSourceBlendFactor(3); // Different from default 6
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->sourceBlendFactor(), 3);
}

TEST_F(MaterialEditorQMLTest, DestBlendFactor_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::destBlendFactorChanged);
    editor->setDestBlendFactor(5); // Different from default 1
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->destBlendFactor(), 5);
}

TEST_F(MaterialEditorQMLTest, DiffuseAlpha_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::diffuseAlphaChanged);
    editor->setDiffuseAlpha(0.7f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FLOAT_EQ(editor->diffuseAlpha(), 0.7f);
}

TEST_F(MaterialEditorQMLTest, SpecularAlpha_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::specularAlphaChanged);
    editor->setSpecularAlpha(0.4f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FLOAT_EQ(editor->specularAlpha(), 0.4f);
}

TEST_F(MaterialEditorQMLTest, DepthBiasConstant_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::depthBiasConstantChanged);
    editor->setDepthBiasConstant(2.5f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FLOAT_EQ(editor->depthBiasConstant(), 2.5f);
}

TEST_F(MaterialEditorQMLTest, DepthBiasSlopeScale_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::depthBiasSlopeScaleChanged);
    editor->setDepthBiasSlopeScale(1.5f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FLOAT_EQ(editor->depthBiasSlopeScale(), 1.5f);
}

TEST_F(MaterialEditorQMLTest, FogMode_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::fogModeChanged);
    editor->setFogMode(2); // Exponential
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->fogMode(), 2);
}

TEST_F(MaterialEditorQMLTest, FogDensity_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::fogDensityChanged);
    editor->setFogDensity(0.8f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FLOAT_EQ(editor->fogDensity(), 0.8f);
}

TEST_F(MaterialEditorQMLTest, FogStart_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::fogStartChanged);
    editor->setFogStart(25.0f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FLOAT_EQ(editor->fogStart(), 25.0f);
}

TEST_F(MaterialEditorQMLTest, FogEnd_SignalOnly) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::fogEndChanged);
    editor->setFogEnd(200.0f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_FLOAT_EQ(editor->fogEnd(), 200.0f);
}

// ===========================================================================
// No-change-no-signal tests (MaterialEditorQMLTest fixture)
// ===========================================================================

TEST_F(MaterialEditorQMLTest, DepthWriteEnabled_NoChangeNoSignal) {
    // Default is true
    QSignalSpy spy(editor.get(), &MaterialEditorQML::depthWriteEnabledChanged);
    editor->setDepthWriteEnabled(true);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(MaterialEditorQMLTest, DepthCheckEnabled_NoChangeNoSignal) {
    // Default is true
    QSignalSpy spy(editor.get(), &MaterialEditorQML::depthCheckEnabledChanged);
    editor->setDepthCheckEnabled(true);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(MaterialEditorQMLTest, Shininess_NoChangeNoSignal) {
    // Default is 0.0f
    QSignalSpy spy(editor.get(), &MaterialEditorQML::shininessChanged);
    editor->setShininess(0.0f);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(MaterialEditorQMLTest, PolygonMode_NoChangeNoSignal) {
    // Default is 2 (Solid)
    QSignalSpy spy(editor.get(), &MaterialEditorQML::polygonModeChanged);
    editor->setPolygonMode(2);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(MaterialEditorQMLTest, FogOverride_NoChangeNoSignal) {
    // Default is false
    QSignalSpy spy(editor.get(), &MaterialEditorQML::fogOverrideChanged);
    editor->setFogOverride(false);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(MaterialEditorQMLTest, PointSize_NoChangeNoSignal) {
    // Default is 1.0f
    QSignalSpy spy(editor.get(), &MaterialEditorQML::pointSizeChanged);
    editor->setPointSize(1.0f);
    EXPECT_EQ(spy.count(), 0);
}

// ===========================================================================
// Vertex color tracking without Ogre (MaterialEditorQMLTest fixture)
// ===========================================================================

TEST_F(MaterialEditorQMLTest, VertexColorTracking_SignalOnly) {
    // Ambient
    QSignalSpy ambientSpy(editor.get(), &MaterialEditorQML::useVertexColorToAmbientChanged);
    editor->setUseVertexColorToAmbient(true);
    EXPECT_GE(ambientSpy.count(), 1);
    EXPECT_TRUE(editor->useVertexColorToAmbient());

    // Diffuse
    QSignalSpy diffuseSpy(editor.get(), &MaterialEditorQML::useVertexColorToDiffuseChanged);
    editor->setUseVertexColorToDiffuse(true);
    EXPECT_GE(diffuseSpy.count(), 1);
    EXPECT_TRUE(editor->useVertexColorToDiffuse());

    // Specular
    QSignalSpy specularSpy(editor.get(), &MaterialEditorQML::useVertexColorToSpecularChanged);
    editor->setUseVertexColorToSpecular(true);
    EXPECT_GE(specularSpy.count(), 1);
    EXPECT_TRUE(editor->useVertexColorToSpecular());

    // Emissive
    QSignalSpy emissiveSpy(editor.get(), &MaterialEditorQML::useVertexColorToEmissiveChanged);
    editor->setUseVertexColorToEmissive(true);
    EXPECT_GE(emissiveSpy.count(), 1);
    EXPECT_TRUE(editor->useVertexColorToEmissive());
}

// ===========================================================================
// With-Ogre material edge cases (MaterialEditorQMLWithOgreTest fixture)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, CreateMaterial_DuplicateName) {
    // Create a material first
    editor->createNewMaterial("DupTestMat");
    EXPECT_EQ(editor->materialName(), "DupTestMat");

    // Create another material with the same name -- should not crash
    editor->createNewMaterial("DupTestMat");
    EXPECT_EQ(editor->materialName(), "DupTestMat");
    EXPECT_TRUE(editor->materialText().contains("material DupTestMat"));
}

TEST_F(MaterialEditorQMLWithOgreTest, CreateMaterial_SpecialCharacters) {
    // Create material with special characters in name
    editor->createNewMaterial("Mat_With-Special.Chars/123");
    EXPECT_EQ(editor->materialName(), "Mat_With-Special.Chars/123");
    EXPECT_TRUE(editor->materialText().contains("material Mat_With-Special.Chars/123"));
}

TEST_F(MaterialEditorQMLWithOgreTest, ResetPropertiesToDefaults) {
    // Load a material and change various properties
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    editor->setLightingEnabled(false);
    editor->setShininess(99.0f);
    editor->setDepthWriteEnabled(false);
    editor->setDepthCheckEnabled(false);
    editor->setPolygonMode(0); // Points

    // Create a fresh new material. Its script template has an empty pass,
    // which Ogre parses with default pass properties (lighting on, depth
    // write on, depth check on).
    editor->createNewMaterial("ResetTest");
    ASSERT_TRUE(editor->applyMaterial());

    // Load the freshly-created Ogre material to read its pass properties
    editor->loadMaterial("ResetTest");
    ASSERT_FALSE(editor->passList().isEmpty());

    // A brand-new Ogre pass has these defaults; the editor must reflect them
    // and not carry stale values from the previously edited BaseWhite material
    EXPECT_TRUE(editor->lightingEnabled());
    EXPECT_TRUE(editor->depthWriteEnabled());
    EXPECT_TRUE(editor->depthCheckEnabled());
}

TEST_F(MaterialEditorQMLWithOgreTest, MultipleTextureUnits) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    // Create 3 texture units
    editor->createNewTextureUnit("TU_First");
    editor->createNewTextureUnit("TU_Second");
    editor->createNewTextureUnit("TU_Third");

    EXPECT_GE(editor->textureUnitList().size(), 3);

    // Switch between texture units
    editor->setSelectedTextureUnitIndex(0);
    EXPECT_EQ(editor->selectedTextureUnitIndex(), 0);

    editor->setSelectedTextureUnitIndex(1);
    EXPECT_EQ(editor->selectedTextureUnitIndex(), 1);

    editor->setSelectedTextureUnitIndex(2);
    EXPECT_EQ(editor->selectedTextureUnitIndex(), 2);
}

TEST_F(MaterialEditorQMLWithOgreTest, TextureUnitProperties_AfterSwitch) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    // Create two texture units
    editor->createNewTextureUnit("TU_A");
    editor->createNewTextureUnit("TU_B");
    int lastIdx = editor->textureUnitList().size() - 1;

    // Set properties on first texture unit (index lastIdx - 1)
    editor->setSelectedTextureUnitIndex(lastIdx - 1);
    editor->setTextureUOffset(0.3f);
    editor->setTextureVOffset(0.7f);
    EXPECT_FLOAT_EQ(editor->textureUOffset(), 0.3f);
    EXPECT_FLOAT_EQ(editor->textureVOffset(), 0.7f);

    // Switch to second texture unit (index lastIdx)
    editor->setSelectedTextureUnitIndex(lastIdx);
    // Properties should reflect the second TU's values (defaults)
    // The key assertion is that switching doesn't carry values from TU_A
    float uOffset = editor->textureUOffset();
    float vOffset = editor->textureVOffset();
    // Second TU was just created with default offsets (0.0)
    EXPECT_FLOAT_EQ(uOffset, 0.0f);
    EXPECT_FLOAT_EQ(vOffset, 0.0f);
}

// ===========================================================================
// LLM callback stubs (MaterialEditorQMLTest fixture)
// ===========================================================================

TEST_F(MaterialEditorQMLTest, LLMProperties_InitialState) {
    // llmGenerationProgress should be 0 initially
    EXPECT_FLOAT_EQ(editor->llmGenerationProgress(), 0.0f);

    // llmModelLoaded and llmCurrentModel depend on LLMManager singleton,
    // which may or may not be available. Just verify they don't crash.
    bool loaded = editor->llmModelLoaded();
    QString model = editor->llmCurrentModel();
    // We don't assert specific values since LLMManager state depends on
    // whether llama.cpp was compiled in, but the calls should not throw.
    Q_UNUSED(loaded);
    Q_UNUSED(model);
}

TEST_F(MaterialEditorQMLTest, GenerateMaterialFromPrompt_EmptyPromptEmitsError) {
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::aiGenerationError);

    editor->generateMaterialFromPrompt("");

    ASSERT_EQ(errorSpy.count(), 1);
    const QList<QVariant> args = errorSpy.takeFirst();
    ASSERT_EQ(args.size(), 1);
    EXPECT_EQ(args.at(0).toString(), "Please enter a prompt");
}

TEST_F(MaterialEditorQMLTest, GenerateMaterialFromPrompt_NoModelLoadedEmitsError) {
#ifdef ENABLE_LOCAL_LLM
    LLMManager::instance()->setAutoLoadModel(false);
    LLMManager::instance()->unloadModel();
    for (int i = 0; i < 100 && editor->llmModelLoaded(); ++i) {
        QThread::msleep(20);
        if (app) app->processEvents();
    }
#endif
    ASSERT_FALSE(editor->llmModelLoaded())
        << "LLM model must be unloaded to test the no-model error path";

    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::aiGenerationError);

    editor->generateMaterialFromPrompt("polished metal with scratches");

    ASSERT_GE(errorSpy.count(), 1);
    const QList<QVariant> args = errorSpy.takeFirst();
    ASSERT_EQ(args.size(), 1);
    EXPECT_TRUE(args.at(0).toString().contains("No AI model loaded"));
}

TEST_F(MaterialEditorQMLTest, GenerateTextureFromPrompt_EmptyPromptEmitsError) {
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::sdGenerationError);

    editor->generateTextureFromPrompt("", 512, 512);

    ASSERT_EQ(errorSpy.count(), 1);
    const QList<QVariant> args = errorSpy.takeFirst();
    ASSERT_EQ(args.size(), 1);
    EXPECT_EQ(args.at(0).toString(), "Please enter a texture prompt");
}

TEST_F(MaterialEditorQMLTest, GenerateTextureFromPrompt_ReportsUnavailableBackendOrModel) {
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::sdGenerationError);

    editor->generateTextureFromPrompt("brushed steel", 512, 512);

    ASSERT_GE(errorSpy.count(), 1);
    const QList<QVariant> args = errorSpy.takeFirst();
    ASSERT_EQ(args.size(), 1);
    const QString message = args.at(0).toString();

    if (editor->stableDiffusionEnabled()) {
        EXPECT_TRUE(message.contains("No SD model loaded") || message.contains("AI Settings"));
    } else {
        EXPECT_TRUE(message.contains("Stable Diffusion support is not enabled"));
    }
}

TEST_F(MaterialEditorQMLTest, StopGenerationMethodsWithoutActiveJobsDoNotCrash) {
    EXPECT_NO_THROW(editor->stopAIGeneration());
    EXPECT_NO_THROW(editor->stopTextureGeneration());
}

// ===========================================================================
// Additional coverage tests (MaterialEditorQMLTest fixture)
// ===========================================================================

TEST_F(MaterialEditorQMLTest, CreateNewMaterial_EmptyName) {
    // Empty name should use default "new_material"
    editor->createNewMaterial("");
    EXPECT_EQ(editor->materialName(), "new_material");
    EXPECT_TRUE(editor->materialText().contains("material new_material"));
}

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_TextureUnitInPass) {
    QString script =
        "material TexturedMat\n"
        "{\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\tpass\n"
        "\t\t{\n"
        "\t\t\ttexture_unit\n"
        "\t\t\t{\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    EXPECT_TRUE(editor->validateMaterialScript(script));
}

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_MultiplePassesInTechnique) {
    QString script =
        "material MultiPassMat\n"
        "{\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\tpass FirstPass\n"
        "\t\t{\n"
        "\t\t}\n"
        "\t\tpass SecondPass\n"
        "\t\t{\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    EXPECT_TRUE(editor->validateMaterialScript(script));
}

// ===========================================================================
// NEW: applyMaterial with modified properties (Ogre fixture)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, ApplyMaterial_ModifiedProperties_ReflectedInPass) {
    // Load BaseWhite, change several properties, apply, and verify Ogre::Pass reflects them
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->techniqueList().isEmpty());
    ASSERT_FALSE(editor->passList().isEmpty());

    // Modify properties
    editor->setLightingEnabled(false);
    editor->setPolygonMode(1); // Wireframe
    editor->setShininess(77.0f);
    editor->setDepthWriteEnabled(false);
    editor->setDepthCheckEnabled(false);
    QColor red(255, 0, 0);
    editor->setAmbientColor(red);
    QColor green(0, 255, 0);
    editor->setDiffuseColor(green);

    // The material text should have been updated to reflect changes
    QString matText = editor->materialText();
    EXPECT_FALSE(matText.isEmpty());

    // Apply the material
    bool result = editor->applyMaterial();
    EXPECT_TRUE(result);

    // Reload the material to pick up the applied changes from Ogre
    editor->loadMaterial(editor->materialName());
    ASSERT_FALSE(editor->passList().isEmpty());

    // Verify properties match what we set
    EXPECT_FALSE(editor->lightingEnabled());
    EXPECT_FALSE(editor->depthWriteEnabled());
    EXPECT_FALSE(editor->depthCheckEnabled());
}

// ===========================================================================
// NEW: undoMaterialChange / redoMaterialChange with Ogre
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, UndoRedo_OgrePropertyChangesRevertState) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    // Record the initial state
    QString initialText = editor->materialText();
    bool initialLighting = editor->lightingEnabled();

    // Change lighting -- this updates material text and pushes to undo stack
    editor->setLightingEnabled(!initialLighting);
    QString afterLightingText = editor->materialText();
    EXPECT_NE(initialText, afterLightingText);
    EXPECT_TRUE(editor->canUndo());

    // Change depth write -- another undo entry (depth_write appears in serialized text)
    editor->setDepthWriteEnabled(!editor->depthWriteEnabled());
    QString afterDepthText = editor->materialText();
    EXPECT_NE(afterLightingText, afterDepthText);

    // Undo depth write change
    editor->undo();
    EXPECT_EQ(editor->materialText(), afterLightingText);

    // Undo lighting change
    editor->undo();
    EXPECT_EQ(editor->materialText(), initialText);

    // Redo lighting change
    EXPECT_TRUE(editor->canRedo());
    editor->redo();
    EXPECT_EQ(editor->materialText(), afterLightingText);

    // Redo depth write change
    editor->redo();
    EXPECT_EQ(editor->materialText(), afterDepthText);
}

// ===========================================================================
// NEW: validateMaterialScript with various edge cases
// ===========================================================================

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_WithComments) {
    // Script with comments should be valid
    QString script =
        "material CommentedMat\n"
        "{\n"
        "\t// This is a comment\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\tpass\n"
        "\t\t{\n"
        "\t\t\t// Another comment\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    EXPECT_TRUE(editor->validateMaterialScript(script));
}

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_TextureUnitOutsidePass) {
    // texture_unit outside a pass should fail
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::errorOccurred);
    QString script =
        "material BadMat\n"
        "{\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\ttexture_unit\n"
        "\t\t{\n"
        "\t\t}\n"
        "\t\tpass\n"
        "\t\t{\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    EXPECT_FALSE(editor->validateMaterialScript(script));
    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_UnterminatedString) {
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::errorOccurred);
    QString script =
        "material BadStringMat\n"
        "{\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\tpass\n"
        "\t\t{\n"
        "\t\t\ttexture \"unterminated\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    EXPECT_FALSE(editor->validateMaterialScript(script));
    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_NestedMaterialDeclaration) {
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::errorOccurred);
    QString script =
        "material OuterMat\n"
        "{\n"
        "\tmaterial InnerMat\n"
        "\t{\n"
        "\t\ttechnique\n"
        "\t\t{\n"
        "\t\t\tpass\n"
        "\t\t\t{\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t}\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\tpass\n"
        "\t\t{\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    EXPECT_FALSE(editor->validateMaterialScript(script));
    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_MaterialNameMissing) {
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::errorOccurred);
    // "material" keyword but no name (just "material" followed by {)
    QString script =
        "material\n"
        "{\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\tpass\n"
        "\t\t{\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    // This should fail because "material " has less than 2 parts
    // Note: "material\n" without trailing space - startsWith("material ") won't match
    // Actually, the validator checks line.startsWith("material ") — the space is important.
    // "material\n" trimmed is just "material" which does NOT start with "material ".
    // So it will not be recognized as a material declaration and fail with "No valid material declaration found".
    EXPECT_FALSE(editor->validateMaterialScript(script));
    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_TypoTextureUnt) {
    QSignalSpy errorSpy(editor.get(), &MaterialEditorQML::errorOccurred);
    QString script =
        "material TypoMat\n"
        "{\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\tpass\n"
        "\t\t{\n"
        "\t\t\ttexture_unt\n"
        "\t\t\t{\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    EXPECT_FALSE(editor->validateMaterialScript(script));
    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_WithPropertyValues) {
    // Valid script with property keywords that exercise the property-value check branch
    QString script =
        "material PropMat\n"
        "{\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\tpass\n"
        "\t\t{\n"
        "\t\t\tambient 0.5 0.5 0.5\n"
        "\t\t\tdiffuse 1.0 1.0 1.0 1.0\n"
        "\t\t\tspecular 0.3 0.3 0.3 32.0\n"
        "\t\t\temissive 0.0 0.0 0.0\n"
        "\t\t\tlighting on\n"
        "\t\t\tdepth_write on\n"
        "\t\t\tdepth_check on\n"
        "\t\t\tscene_blend alpha_blend\n"
        "\t\t\tcull_hardware clockwise\n"
        "\t\t\tcull_software back\n"
        "\t\t\tshininess 32.0\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    EXPECT_TRUE(editor->validateMaterialScript(script));
}

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_BraceOnSameLine) {
    // Material declaration with { on the same line
    QString script =
        "material InlineBraceMat {\n"
        "\ttechnique {\n"
        "\t\tpass {\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    EXPECT_TRUE(editor->validateMaterialScript(script));
}

TEST_F(MaterialEditorQMLTest, ValidateMaterialScript_MultipleTextureUnitsInPass) {
    QString script =
        "material MultiTexMat\n"
        "{\n"
        "\ttechnique\n"
        "\t{\n"
        "\t\tpass\n"
        "\t\t{\n"
        "\t\t\ttexture_unit first\n"
        "\t\t\t{\n"
        "\t\t\t}\n"
        "\t\t\ttexture_unit second\n"
        "\t\t\t{\n"
        "\t\t\t}\n"
        "\t\t\ttexture_unit third\n"
        "\t\t\t{\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    EXPECT_TRUE(editor->validateMaterialScript(script));
}

// ===========================================================================
// NEW: removeTechnique / removePass / removeTextureUnit (Ogre fixture)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, RemoveTextureUnit) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    // Create two texture units
    editor->createNewTextureUnit("RemTU_A");
    editor->createNewTextureUnit("RemTU_B");
    int countBefore = editor->textureUnitList().size();
    ASSERT_GE(countBefore, 2);

    // Remove the texture (which recreates the TU without a texture name)
    editor->setSelectedTextureUnitIndex(countBefore - 1);
    editor->removeTexture();
    // After removeTexture, the texture name should be reset
    EXPECT_EQ(editor->textureName(), "*Select a texture*");
}

// ===========================================================================
// NEW: setTextureName with a valid texture name (Ogre fixture)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, SetTextureName_ValidName) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    editor->createNewTextureUnit("TexNameTU");
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    QSignalSpy spy(editor.get(), &MaterialEditorQML::textureNameChanged);
    editor->setTextureName("some_texture.png");
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->textureName(), "some_texture.png");
}

TEST_F(MaterialEditorQMLTest, SetTextureName_NoOgre) {
    QSignalSpy spy(editor.get(), &MaterialEditorQML::textureNameChanged);
    editor->setTextureName("test_texture.dds");
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(editor->textureName(), "test_texture.dds");
}

// ===========================================================================
// NEW: Multiple technique/pass selection (Ogre fixture)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, MultipleTechniqueSelection) {
    editor->loadMaterial("BaseWhite");
    int origTechCount = editor->techniqueList().size();

    // Create additional techniques
    editor->createNewTechnique("Tech_A");
    editor->createNewTechnique("Tech_B");
    EXPECT_EQ(editor->techniqueList().size(), origTechCount + 2);

    // Select first technique
    editor->setSelectedTechniqueIndex(0);
    EXPECT_EQ(editor->selectedTechniqueIndex(), 0);
    QStringList passListForTech0 = editor->passList();

    // Select second technique (newly created -- may have no passes)
    editor->setSelectedTechniqueIndex(origTechCount);
    EXPECT_EQ(editor->selectedTechniqueIndex(), origTechCount);
    QStringList passListForTechA = editor->passList();

    // Select third technique
    editor->setSelectedTechniqueIndex(origTechCount + 1);
    EXPECT_EQ(editor->selectedTechniqueIndex(), origTechCount + 1);

    // Switch back to first technique, pass list should match original
    editor->setSelectedTechniqueIndex(0);
    EXPECT_EQ(editor->passList(), passListForTech0);
}

// ===========================================================================
// NEW: loadMaterial for GUI_Material (Ogre fixture)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, LoadMaterial_GUI_Material) {
    editor->loadMaterial("GUI_Material");

    EXPECT_EQ(editor->materialName(), "GUI_Material");
    EXPECT_FALSE(editor->techniqueList().isEmpty());
    EXPECT_TRUE(editor->materialText().contains("GUI_Material"));

    // GUI_Material has lighting disabled in its setup (see TestHelpers.h)
    EXPECT_FALSE(editor->lightingEnabled());
}

// ===========================================================================
// NEW: saveMaterial / importMaterial round-trip (Ogre fixture)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, ExportAndImportMaterial_RoundTrip) {
    // Create a custom material
    editor->createNewMaterial("RoundTripTestMat");
    EXPECT_TRUE(editor->applyMaterial());

    // Load the material via Ogre to set it up properly
    editor->loadMaterial("RoundTripTestMat");
    ASSERT_FALSE(editor->techniqueList().isEmpty());

    // Export it
    QString exportPath = QDir(QDir::tempPath()).filePath("round_trip_test.material");
    editor->exportMaterial(exportPath);
    EXPECT_TRUE(QFile::exists(exportPath));

    // Now import it -- importMaterialFile reads .material files
    editor->importMaterialFile(exportPath);

    // Clean up
    QFile::remove(exportPath);
}

// ===========================================================================
// NEW: createNewMaterial with duplicate name (Ogre fixture)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, CreateNewMaterial_DuplicateExisting) {
    // BaseWhite already exists in Ogre
    editor->createNewMaterial("BaseWhite");
    // Should not crash. The editor should still have a valid state
    EXPECT_EQ(editor->materialName(), "BaseWhite");
    EXPECT_TRUE(editor->materialText().contains("material BaseWhite"));
}

// ===========================================================================
// NEW: Verify Ogre Pass reflects color changes after apply
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, ApplyMaterial_ColorsReflectedInOgrePass) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    // Set distinct colors
    QColor red(255, 0, 0);
    QColor green(0, 255, 0);
    QColor blue(0, 0, 255);
    QColor yellow(255, 255, 0);

    editor->setAmbientColor(red);
    editor->setDiffuseColor(green);
    editor->setSpecularColor(blue);
    editor->setEmissiveColor(yellow);
    editor->setShininess(50.0f);

    // Apply the material
    bool result = editor->applyMaterial();
    EXPECT_TRUE(result);

    // Reload to verify Ogre actually parsed the applied script
    editor->loadMaterial(editor->materialName());
    ASSERT_FALSE(editor->passList().isEmpty());

    // The shininess should be close to what we set
    EXPECT_NEAR(editor->shininess(), 50.0f, 1.0f);

    // Verify the colors round-tripped
    // Note: Ogre may adjust color precision, so we use component comparison
    EXPECT_NEAR(editor->ambientColor().redF(), 1.0, 0.1);
    EXPECT_NEAR(editor->diffuseColor().greenF(), 1.0, 0.1);
    EXPECT_NEAR(editor->specularColor().blueF(), 1.0, 0.1);
}

// ===========================================================================
// NEW: Additional undo/redo edge cases
// ===========================================================================

TEST_F(MaterialEditorQMLTest, UndoRedo_UndoOnEmptyStackDoesNothing) {
    // Undo when nothing is on the stack should not crash
    EXPECT_FALSE(editor->canUndo());
    editor->undo();
    // No crash = pass
    EXPECT_FALSE(editor->canUndo());
}

TEST_F(MaterialEditorQMLTest, UndoRedo_RedoOnEmptyStackDoesNothing) {
    EXPECT_FALSE(editor->canRedo());
    editor->redo();
    // No crash = pass
    EXPECT_FALSE(editor->canRedo());
}

// ===========================================================================
// NEW: openMaterialEditorWindow with different states (Ogre fixture)
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, OpenMaterialEditorWindow_WithGUIMaterial) {
    editor->openMaterialEditorWindow("GUI_Material");
    EXPECT_EQ(editor->materialName(), "GUI_Material");
    EXPECT_FALSE(editor->techniqueList().isEmpty());
}

// ===========================================================================
// NEW: Adding a pass to a new technique, then selecting it
// ===========================================================================

TEST_F(MaterialEditorQMLWithOgreTest, UpdateTextureUnitProperties_NoTextureUnit_ResetsDefaults) {
    editor->loadMaterial("BaseWhite");
    ASSERT_FALSE(editor->passList().isEmpty());

    // Create a texture unit and select it to populate properties
    editor->createNewTextureUnit("TestTU");
    ASSERT_FALSE(editor->textureUnitList().isEmpty());
    editor->setSelectedTextureUnitIndex(editor->textureUnitList().size() - 1);

    // Modify some texture properties so we can verify they reset
    editor->setTextureUScale(2.0f);
    editor->setTextureVScale(3.0f);
    editor->setTextureRotation(45.0f);
    editor->setMaxAnisotropy(8);
    editor->setTexCoordSet(2);

    // Now deselect texture unit (index -1) to trigger the !textureUnit branch
    editor->setSelectedTextureUnitIndex(-1);

    // All texture properties should be reset to defaults
    EXPECT_EQ(editor->textureName(), "*Select a texture*");
    EXPECT_DOUBLE_EQ(editor->scrollAnimUSpeed(), 0.0);
    EXPECT_DOUBLE_EQ(editor->scrollAnimVSpeed(), 0.0);
    EXPECT_EQ(editor->texCoordSet(), 0);
    EXPECT_EQ(editor->textureAddressMode(), 0);
    EXPECT_EQ(editor->textureBorderColor(), QColor(0, 0, 0));
    EXPECT_EQ(editor->textureFiltering(), 1);
    EXPECT_EQ(editor->maxAnisotropy(), 1);
    EXPECT_FLOAT_EQ(editor->textureUOffset(), 0.0f);
    EXPECT_FLOAT_EQ(editor->textureVOffset(), 0.0f);
    EXPECT_FLOAT_EQ(editor->textureUScale(), 1.0f);
    EXPECT_FLOAT_EQ(editor->textureVScale(), 1.0f);
    EXPECT_FLOAT_EQ(editor->textureRotation(), 0.0f);
    EXPECT_EQ(editor->environmentMapping(), 0);
    EXPECT_DOUBLE_EQ(editor->rotateAnimSpeed(), 0.0);
}

TEST_F(MaterialEditorQMLWithOgreTest, NewTechniqueAddPassAndSelectIt) {
    editor->loadMaterial("BaseWhite");
    int origTechCount = editor->techniqueList().size();

    // Create a new technique
    editor->createNewTechnique("MyNewTech");
    EXPECT_EQ(editor->techniqueList().size(), origTechCount + 1);

    // Select the new technique
    editor->setSelectedTechniqueIndex(origTechCount);
    int passCountBefore = editor->passList().size();

    // Create a pass in it
    editor->createNewPass("MyNewPass");
    EXPECT_EQ(editor->passList().size(), passCountBefore + 1);

    // Select the new pass and modify its properties
    editor->setSelectedPassIndex(editor->passList().size() - 1);
    editor->setLightingEnabled(false);
    EXPECT_FALSE(editor->lightingEnabled());

    // Switch back to original technique and verify properties are independent
    editor->setSelectedTechniqueIndex(0);
    editor->setSelectedPassIndex(0);
    EXPECT_TRUE(editor->lightingEnabled());
}

// ---------------------------------------------------------------------------
// Slice G: Q_INVOKABLE wrappers for the texture channel packer
// ---------------------------------------------------------------------------

TEST_F(MaterialEditorQMLTest, PackTextureChannels_AllConstantsWritesPng) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outPath = tmp.filePath("packed_const.png");

    QString err = editor->packTextureChannels(
        QString(), QString(), QString(), QString(),
        1.0, 0.5, 0.0, 1.0,
        false, false, false, false,
        true, outPath);
    EXPECT_TRUE(err.isEmpty()) << err.toStdString();
    EXPECT_TRUE(QFile::exists(outPath));
}

TEST_F(MaterialEditorQMLTest, PackTextureChannels_MissingPathReturnsError) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outPath = tmp.filePath("never.png");

    QString err = editor->packTextureChannels(
        "/nonexistent/missing_for_test.png", QString(), QString(), QString(),
        0.0, 0.0, 0.0, 1.0,
        false, false, false, false,
        true, outPath);
    EXPECT_FALSE(err.isEmpty());
    EXPECT_FALSE(QFile::exists(outPath));
}

TEST_F(MaterialEditorQMLTest, PackTextureChannels_EmptyOutputPathReturnsError) {
    QString err = editor->packTextureChannels(
        QString(), QString(), QString(), QString(),
        0.5, 0.0, 0.0, 1.0,
        false, false, false, false,
        true, QString());
    EXPECT_FALSE(err.isEmpty());
}

TEST_F(MaterialEditorQMLTest, PackTextureChannels_InvertFlagFlipsConstant) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outPath = tmp.filePath("inv.png");
    QString err = editor->packTextureChannels(
        QString(), QString(), QString(), QString(),
        1.0, 0.0, 0.0, 1.0,
        /*invertR=*/true, false, false, false,
        true, outPath);
    EXPECT_TRUE(err.isEmpty());
    QImage img(outPath);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(qRed(img.pixel(0, 0)), 0);
}

// ---------------------------------------------------------------------------
// Slice G2: live preview wrapper
// ---------------------------------------------------------------------------

TEST_F(MaterialEditorQMLTest, PreviewPackedTextureChannels_AllConstantsReturnsDataUrl) {
    QString url = editor->previewPackedTextureChannels(
        QString(), QString(), QString(), QString(),
        1.0, 0.5, 0.0, 1.0,
        false, false, false, false,
        true, /*previewSize=*/64);
    ASSERT_TRUE(url.startsWith("data:image/png;base64,")) << url.toStdString();

    // Decode the base64 payload back to a QImage and check the size.
    const QByteArray payload = QByteArray::fromBase64(
        url.mid(QString("data:image/png;base64,").size()).toLatin1());
    QImage img;
    ASSERT_TRUE(img.loadFromData(payload, "PNG"));
    EXPECT_EQ(img.width(), 64);
    EXPECT_EQ(img.height(), 64);
    EXPECT_EQ(qRed(img.pixel(10, 10)), 255);
    EXPECT_NEAR(qGreen(img.pixel(10, 10)), 128, 2);
    EXPECT_EQ(qBlue(img.pixel(10, 10)), 0);
}

TEST_F(MaterialEditorQMLTest, PreviewPackedTextureChannels_MissingFileReturnsEmpty) {
    QString url = editor->previewPackedTextureChannels(
        "/nonexistent/missing_for_preview_test.png",
        QString(), QString(), QString(),
        0.0, 0.0, 0.0, 1.0,
        false, false, false, false,
        true, 64);
    EXPECT_TRUE(url.isEmpty());
}

TEST_F(MaterialEditorQMLTest, PreviewPackedTextureChannels_SizeIsClampedToBounds) {
    // previewSize is clamped to [32, 512]. A request for 8 should bump
    // up to 32 — the smallest sensible thumbnail.
    QString url = editor->previewPackedTextureChannels(
        QString(), QString(), QString(), QString(),
        0.5, 0.5, 0.5, 1.0,
        false, false, false, false,
        true, 8);
    ASSERT_TRUE(url.startsWith("data:image/png;base64,"));
    const QByteArray payload = QByteArray::fromBase64(
        url.mid(QString("data:image/png;base64,").size()).toLatin1());
    QImage img;
    ASSERT_TRUE(img.loadFromData(payload, "PNG"));
    EXPECT_EQ(img.width(), 32);
}

// ---------------------------------------------------------------------------
// Slice H: Q_INVOKABLE wrappers for the normal map generator
// ---------------------------------------------------------------------------

namespace {
QString writeGreyPngForNormal(const QTemporaryDir& dir, const QString& name,
                               int w, int h, int grey)
{
    QImage img(w, h, QImage::Format_RGBA8888);
    img.fill(qRgba(grey, grey, grey, 255));
    const QString path = dir.filePath(name);
    img.save(path, "PNG");
    return path;
}
} // namespace

TEST_F(MaterialEditorQMLTest, GenerateNormalMap_FlatHeightWritesPng) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = writeGreyPngForNormal(tmp, "flat.png", 8, 8, 128);
    const QString out = tmp.filePath("normal.png");

    QString err = editor->generateNormalMap(src, 2.0, false, false, out);
    EXPECT_TRUE(err.isEmpty()) << err.toStdString();
    EXPECT_TRUE(QFile::exists(out));
    QImage img(out);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(qBlue(img.pixel(4, 4)), 255);
}

TEST_F(MaterialEditorQMLTest, GenerateNormalMap_MissingSourceReturnsError) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QString err = editor->generateNormalMap(
        "/nonexistent/missing_for_test.png", 2.0, false, false,
        tmp.filePath("never.png"));
    EXPECT_FALSE(err.isEmpty());
}

TEST_F(MaterialEditorQMLTest, PreviewNormalMap_FlatHeightReturnsDataUrl) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = writeGreyPngForNormal(tmp, "flat.png", 32, 32, 128);

    QString url = editor->previewNormalMap(src, 2.0, false, false, 64);
    ASSERT_TRUE(url.startsWith("data:image/png;base64,")) << url.toStdString();

    const QByteArray payload = QByteArray::fromBase64(
        url.mid(QString("data:image/png;base64,").size()).toLatin1());
    QImage img;
    ASSERT_TRUE(img.loadFromData(payload, "PNG"));
    EXPECT_EQ(img.width(), 64);
    EXPECT_NEAR(qRed(img.pixel(32, 32)), 128, 2);
}

TEST_F(MaterialEditorQMLTest, PreviewNormalMap_MissingSourceReturnsEmpty) {
    QString url = editor->previewNormalMap(
        "/nonexistent/missing.png", 2.0, false, false, 64);
    EXPECT_TRUE(url.isEmpty());
}

TEST_F(MaterialEditorQMLTest, PreviewNormalMap_SizeIsClampedToBounds) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = writeGreyPngForNormal(tmp, "flat.png", 8, 8, 128);
    QString url = editor->previewNormalMap(src, 2.0, false, false, 8);
    ASSERT_TRUE(url.startsWith("data:image/png;base64,"));
    const QByteArray payload = QByteArray::fromBase64(
        url.mid(QString("data:image/png;base64,").size()).toLatin1());
    QImage img;
    ASSERT_TRUE(img.loadFromData(payload, "PNG"));
    EXPECT_EQ(img.width(), 32);  // clamped to lower bound
}

// ===========================================================================
// Slice I — theme colors expose Inspector-parity surfaces.
//
// The Material Editor and the Inspector should agree on which palette
// role drives which kind of surface (Window for panels, Base for input
// fields, Window.darker(110) for header strips). Without this, the
// Material Editor window looked visibly darker on macOS dark mode and
// the inner GroupBoxes did not match the Inspector tools.
// ===========================================================================

TEST_F(MaterialEditorQMLTest, ThemeColors_PanelMatchesWindowRole) {
    // Match how PropertiesPanelController exposes panel surfaces so
    // QML controls can read the same vocabulary regardless of which
    // singleton they bind to.
    const QPalette palette = QApplication::palette();
    EXPECT_EQ(editor->panelColor(), palette.color(QPalette::Window));
    EXPECT_EQ(editor->backgroundColor(), palette.color(QPalette::Window));
}

TEST_F(MaterialEditorQMLTest, ThemeColors_InputColorIsBaseRole) {
    const QPalette palette = QApplication::palette();
    EXPECT_EQ(editor->inputColor(), palette.color(QPalette::Base));
}

TEST_F(MaterialEditorQMLTest, ThemeColors_HeaderColorIsWindowDarker110) {
    // Hairline contrast strip — Inspector convention is Window.darker(110).
    const QPalette palette = QApplication::palette();
    EXPECT_EQ(editor->headerColor(), palette.color(QPalette::Window).darker(110));
}

TEST_F(MaterialEditorQMLTest, ThemeColors_InputAndHeaderAreValid) {
    EXPECT_TRUE(editor->inputColor().isValid());
    EXPECT_TRUE(editor->headerColor().isValid());
}

// Slice I — interactiveMaterialPreview is a thin wrapper over
// MaterialPreviewRenderer::renderInteractivePreview. The renderer
// holds its own SceneManager keyed off Ogre::Root; running multiple
// MaterialEditorQMLWithOgreTest cases against it tickles a fixture
// teardown-order bug where Manager::kill() destroys Ogre::Root while
// the renderer's cached SceneManager pointer becomes stale. The
// wrapper itself is one line; the renderer's clamping / wrapping /
// shape switching / unknown-material handling is exhaustively
// covered in MaterialPreviewRenderer_test.cpp (which kills the
// renderer in TearDown, sidestepping the issue).
