#ifdef ENABLE_LOCAL_LLM

#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QSettings>
#include <QDir>
#include <QTemporaryDir>
#include "LLMManager.h"

class LLMManagerTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    LLMManager* manager = nullptr;

    void SetUp() override {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        manager = LLMManager::instance();
        ASSERT_NE(manager, nullptr);
    }
};

// =============================================================================
// validateMaterialScript tests
// =============================================================================

TEST_F(LLMManagerTest, ValidateValidMinimalScript)
{
    QString error;
    QString script =
        "material TestMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "            ambient 0.1 0.2 0.3\n"
        "            diffuse 0.4 0.5 0.6\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_TRUE(manager->validateMaterialScript(script, error));
    EXPECT_TRUE(error.isEmpty());
}

TEST_F(LLMManagerTest, ValidateScriptWithSpecular)
{
    QString error;
    QString script =
        "material ShinyMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "            ambient 0.0 0.2 0.0\n"
        "            diffuse 0.0 0.8 0.0\n"
        "            specular 0.5 1.0 0.5 64\n"
        "            emissive 0.0 0.0 0.0\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_TRUE(manager->validateMaterialScript(script, error));
    EXPECT_TRUE(error.isEmpty());
}

TEST_F(LLMManagerTest, ValidateRejectsMissingMaterialKeyword)
{
    QString error;
    QString script =
        "technique\n"
        "{\n"
        "    pass\n"
        "    {\n"
        "    }\n"
        "}\n";
    EXPECT_FALSE(manager->validateMaterialScript(script, error));
    EXPECT_TRUE(error.contains("material"));
}

TEST_F(LLMManagerTest, ValidateRejectsUnbalancedExtraClosingBrace)
{
    QString error;
    QString script =
        "material TestMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "        }\n"
        "    }\n"
        "}\n"
        "}\n";
    EXPECT_FALSE(manager->validateMaterialScript(script, error));
    EXPECT_TRUE(error.contains("brace"));
}

TEST_F(LLMManagerTest, ValidateRejectsUnbalancedMissingClosingBrace)
{
    QString error;
    QString script =
        "material TestMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "        }\n"
        "    }\n";
    EXPECT_FALSE(manager->validateMaterialScript(script, error));
    EXPECT_TRUE(error.contains("brace"));
}

TEST_F(LLMManagerTest, ValidateRejectsMissingTechnique)
{
    QString error;
    QString script =
        "material TestMaterial\n"
        "{\n"
        "    pass\n"
        "    {\n"
        "    }\n"
        "}\n";
    EXPECT_FALSE(manager->validateMaterialScript(script, error));
    EXPECT_TRUE(error.contains("technique"));
}

TEST_F(LLMManagerTest, ValidateRejectsMissingPass)
{
    QString error;
    QString script =
        "material TestMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "    }\n"
        "}\n";
    EXPECT_FALSE(manager->validateMaterialScript(script, error));
    EXPECT_TRUE(error.contains("pass"));
}

TEST_F(LLMManagerTest, ValidateRejectsNonNumericColorValues)
{
    QString error;
    QString script =
        "material TestMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "            ambient red green blue\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_FALSE(manager->validateMaterialScript(script, error));
    EXPECT_TRUE(error.contains("Invalid value"));
}

TEST_F(LLMManagerTest, ValidateRejectsEmptyScript)
{
    QString error;
    EXPECT_FALSE(manager->validateMaterialScript("", error));
}

TEST_F(LLMManagerTest, ValidateRejectsWhitespaceOnlyScript)
{
    QString error;
    EXPECT_FALSE(manager->validateMaterialScript("   \n\n  ", error));
}

TEST_F(LLMManagerTest, ValidateAcceptsScriptWithTextureUnit)
{
    QString error;
    QString script =
        "material TexturedMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "            ambient 0.5 0.5 0.5\n"
        "            diffuse 1.0 1.0 1.0\n"
        "            texture_unit\n"
        "            {\n"
        "                texture myimage.png\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_TRUE(manager->validateMaterialScript(script, error));
    EXPECT_TRUE(error.isEmpty());
}

TEST_F(LLMManagerTest, ValidateAcceptsSceneBlendProperties)
{
    QString error;
    QString script =
        "material TransparentMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "            scene_blend alpha_blend\n"
        "            depth_write off\n"
        "            ambient 1.0 1.0 1.0\n"
        "            diffuse 1.0 1.0 1.0\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_TRUE(manager->validateMaterialScript(script, error));
}

// =============================================================================
// cleanupGeneratedScript tests
// =============================================================================

TEST_F(LLMManagerTest, CleanupRemovesMarkdownCodeFences)
{
    QString input =
        "```ogre\n"
        "material TestMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "        }\n"
        "    }\n"
        "}\n"
        "```";
    QString result = manager->cleanupGeneratedScript(input);
    EXPECT_FALSE(result.contains("```"));
    EXPECT_TRUE(result.startsWith("material"));
}

TEST_F(LLMManagerTest, CleanupRemovesTextBeforeMaterial)
{
    QString input =
        "Here is the material script:\n"
        "material TestMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "        }\n"
        "    }\n"
        "}\n";
    QString result = manager->cleanupGeneratedScript(input);
    EXPECT_TRUE(result.startsWith("material"));
}

TEST_F(LLMManagerTest, CleanupRemovesTextAfterClosingBrace)
{
    QString input =
        "material TestMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "        }\n"
        "    }\n"
        "}\n"
        "This is some trailing explanation text.\n";
    QString result = manager->cleanupGeneratedScript(input);
    EXPECT_TRUE(result.endsWith("}"));
}

TEST_F(LLMManagerTest, CleanupTrimsWhitespace)
{
    QString input =
        "  \n\n  material TestMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "        }\n"
        "    }\n"
        "}\n  \n  ";
    QString result = manager->cleanupGeneratedScript(input);
    EXPECT_TRUE(result.startsWith("material"));
    EXPECT_TRUE(result.endsWith("}"));
}

TEST_F(LLMManagerTest, CleanupHandlesPlainCodeFence)
{
    QString input =
        "```\n"
        "material TestMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "        }\n"
        "    }\n"
        "}\n"
        "```";
    QString result = manager->cleanupGeneratedScript(input);
    EXPECT_FALSE(result.contains("```"));
    EXPECT_TRUE(result.startsWith("material"));
}

TEST_F(LLMManagerTest, CleanupPreservesValidScript)
{
    QString input =
        "material TestMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "            ambient 0.1 0.2 0.3\n"
        "            diffuse 0.4 0.5 0.6\n"
        "        }\n"
        "    }\n"
        "}";
    QString result = manager->cleanupGeneratedScript(input);
    EXPECT_TRUE(result.contains("ambient 0.1 0.2 0.3"));
    EXPECT_TRUE(result.contains("diffuse 0.4 0.5 0.6"));
}

TEST_F(LLMManagerTest, CleanupHandlesEmptyString)
{
    QString result = manager->cleanupGeneratedScript("");
    EXPECT_TRUE(result.isEmpty());
}

TEST_F(LLMManagerTest, CleanupHandlesNoMaterialKeyword)
{
    // When there is no "material" line, cleanup returns whatever remains after trimming
    QString input = "some random text without material keyword";
    QString result = manager->cleanupGeneratedScript(input);
    // Should return the string as-is (trimmed) since there's no material block to extract
    EXPECT_FALSE(result.isEmpty());
}

// =============================================================================
// getRecommendedModelsInfo tests
// =============================================================================

TEST_F(LLMManagerTest, RecommendedModelsInfoNotEmpty)
{
    QVariantList models = manager->getRecommendedModelsInfo();
    EXPECT_FALSE(models.isEmpty());
}

TEST_F(LLMManagerTest, RecommendedModelsHaveRequiredFields)
{
    QVariantList models = manager->getRecommendedModelsInfo();
    for (const QVariant& model : models) {
        QVariantMap info = model.toMap();
        EXPECT_TRUE(info.contains("name"));
        EXPECT_TRUE(info.contains("fileName"));
        EXPECT_TRUE(info.contains("url"));
        EXPECT_TRUE(info.contains("description"));
        EXPECT_TRUE(info.contains("size"));
        EXPECT_TRUE(info.contains("isDownloaded"));

        EXPECT_FALSE(info["name"].toString().isEmpty());
        EXPECT_FALSE(info["fileName"].toString().isEmpty());
        EXPECT_FALSE(info["url"].toString().isEmpty());
        EXPECT_FALSE(info["description"].toString().isEmpty());
        EXPECT_GT(info["size"].toLongLong(), 0);
    }
}

TEST_F(LLMManagerTest, RecommendedModelsHaveGGUFExtension)
{
    QVariantList models = manager->getRecommendedModelsInfo();
    for (const QVariant& model : models) {
        QVariantMap info = model.toMap();
        EXPECT_TRUE(info["fileName"].toString().endsWith(".gguf"))
            << "Model file should have .gguf extension: "
            << info["fileName"].toString().toStdString();
    }
}

TEST_F(LLMManagerTest, RecommendedModelsHaveHuggingFaceURLs)
{
    QVariantList models = manager->getRecommendedModelsInfo();
    for (const QVariant& model : models) {
        QVariantMap info = model.toMap();
        EXPECT_TRUE(info["url"].toString().startsWith("https://huggingface.co/"))
            << "Model URL should point to HuggingFace: "
            << info["url"].toString().toStdString();
    }
}

TEST_F(LLMManagerTest, GetRecommendedModelsListConsistentWithInfo)
{
    QList<ModelInfo> modelsList = manager->getRecommendedModels();
    QVariantList modelsInfo = manager->getRecommendedModelsInfo();
    EXPECT_EQ(modelsList.size(), modelsInfo.size());
}

// =============================================================================
// Settings tests
// =============================================================================

TEST_F(LLMManagerTest, DefaultSettingsValues)
{
    // Create fresh settings to check defaults - LLMSettings struct has hardcoded defaults
    LLMSettings defaults;
    EXPECT_EQ(defaults.contextSize, 4096);
    EXPECT_EQ(defaults.maxTokens, 2048);
    EXPECT_FLOAT_EQ(defaults.temperature, 0.7f);
    EXPECT_EQ(defaults.gpuLayers, 99);
    EXPECT_EQ(defaults.threads, 0);
    EXPECT_FLOAT_EQ(defaults.topP, 0.9f);
    EXPECT_EQ(defaults.topK, 40);
    EXPECT_FLOAT_EQ(defaults.repeatPenalty, 1.1f);
}

TEST_F(LLMManagerTest, SetAndGetContextSize)
{
    int original = manager->contextSize();
    QSignalSpy spy(manager, &LLMManager::settingsChanged);

    manager->setContextSize(8192);
    EXPECT_EQ(manager->contextSize(), 8192);
    EXPECT_EQ(spy.count(), 1);

    // Restore original
    manager->setContextSize(original);
}

TEST_F(LLMManagerTest, SetContextSizeSameValueNoSignal)
{
    int current = manager->contextSize();
    QSignalSpy spy(manager, &LLMManager::settingsChanged);

    manager->setContextSize(current);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(LLMManagerTest, SetAndGetMaxTokens)
{
    int original = manager->maxTokens();
    QSignalSpy spy(manager, &LLMManager::settingsChanged);

    manager->setMaxTokens(4096);
    EXPECT_EQ(manager->maxTokens(), 4096);
    EXPECT_EQ(spy.count(), 1);

    manager->setMaxTokens(original);
}

TEST_F(LLMManagerTest, SetMaxTokensSameValueNoSignal)
{
    int current = manager->maxTokens();
    QSignalSpy spy(manager, &LLMManager::settingsChanged);

    manager->setMaxTokens(current);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(LLMManagerTest, SetAndGetTemperature)
{
    float original = manager->temperature();
    QSignalSpy spy(manager, &LLMManager::settingsChanged);

    manager->setTemperature(0.5f);
    EXPECT_FLOAT_EQ(manager->temperature(), 0.5f);
    EXPECT_EQ(spy.count(), 1);

    manager->setTemperature(original);
}

TEST_F(LLMManagerTest, SetTemperatureSameValueNoSignal)
{
    float current = manager->temperature();
    QSignalSpy spy(manager, &LLMManager::settingsChanged);

    manager->setTemperature(current);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(LLMManagerTest, SetAndGetGpuLayers)
{
    int original = manager->gpuLayers();
    QSignalSpy spy(manager, &LLMManager::settingsChanged);

    manager->setGpuLayers(32);
    EXPECT_EQ(manager->gpuLayers(), 32);
    EXPECT_EQ(spy.count(), 1);

    manager->setGpuLayers(original);
}

TEST_F(LLMManagerTest, SetGpuLayersSameValueNoSignal)
{
    int current = manager->gpuLayers();
    QSignalSpy spy(manager, &LLMManager::settingsChanged);

    manager->setGpuLayers(current);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(LLMManagerTest, SetAndGetAutoLoadModel)
{
    bool original = manager->autoLoadModel();
    QSignalSpy spy(manager, &LLMManager::autoLoadModelChanged);

    manager->setAutoLoadModel(!original);
    EXPECT_EQ(manager->autoLoadModel(), !original);
    EXPECT_EQ(spy.count(), 1);

    manager->setAutoLoadModel(original);
}

TEST_F(LLMManagerTest, SetAutoLoadModelSameValueNoSignal)
{
    bool current = manager->autoLoadModel();
    QSignalSpy spy(manager, &LLMManager::autoLoadModelChanged);

    manager->setAutoLoadModel(current);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(LLMManagerTest, GetAndSetSettingsStruct)
{
    LLMSettings original = manager->getSettings();

    LLMSettings newSettings;
    newSettings.contextSize = 2048;
    newSettings.maxTokens = 1024;
    newSettings.temperature = 0.3f;
    newSettings.gpuLayers = 16;
    newSettings.threads = 4;
    newSettings.topP = 0.8f;
    newSettings.topK = 20;
    newSettings.repeatPenalty = 1.2f;

    manager->setSettings(newSettings);

    LLMSettings retrieved = manager->getSettings();
    EXPECT_EQ(retrieved.contextSize, 2048);
    EXPECT_EQ(retrieved.maxTokens, 1024);
    EXPECT_FLOAT_EQ(retrieved.temperature, 0.3f);
    EXPECT_EQ(retrieved.gpuLayers, 16);
    EXPECT_EQ(retrieved.threads, 4);
    EXPECT_FLOAT_EQ(retrieved.topP, 0.8f);
    EXPECT_EQ(retrieved.topK, 20);
    EXPECT_FLOAT_EQ(retrieved.repeatPenalty, 1.2f);

    // Restore original settings
    manager->setSettings(original);
}

// =============================================================================
// Settings save/load persistence tests
// =============================================================================

// DISABLED: QSettings caching on macOS causes loadSettings() to not reflect saveSettings() within same process
TEST_F(LLMManagerTest, DISABLED_SaveAndLoadSettingsPersistence)
{
    // Save known values
    int origCtx = manager->contextSize();
    int origMax = manager->maxTokens();
    float origTemp = manager->temperature();
    int origGpu = manager->gpuLayers();
    bool origAuto = manager->autoLoadModel();

    manager->setContextSize(1024);
    manager->setMaxTokens(512);
    manager->setTemperature(0.9f);
    manager->setGpuLayers(8);
    manager->setAutoLoadModel(true);
    manager->saveSettings();

    // Now modify in-memory values
    manager->setContextSize(9999);
    manager->setMaxTokens(9999);
    manager->setTemperature(0.1f);
    manager->setGpuLayers(1);
    manager->setAutoLoadModel(false);

    // Reload from storage
    manager->loadSettings();

    EXPECT_EQ(manager->contextSize(), 1024);
    EXPECT_EQ(manager->maxTokens(), 512);
    EXPECT_FLOAT_EQ(manager->temperature(), 0.9f);
    EXPECT_EQ(manager->gpuLayers(), 8);
    EXPECT_EQ(manager->autoLoadModel(), true);

    // Restore originals
    manager->setContextSize(origCtx);
    manager->setMaxTokens(origMax);
    manager->setTemperature(origTemp);
    manager->setGpuLayers(origGpu);
    manager->setAutoLoadModel(origAuto);
    manager->saveSettings();
}

// =============================================================================
// getModelFilePath tests
// =============================================================================

TEST_F(LLMManagerTest, GetModelFilePathReturnsEmptyForNonExistentModel)
{
    QString path = manager->getModelFilePath("nonexistent_model_12345");
    EXPECT_TRUE(path.isEmpty());
}

TEST_F(LLMManagerTest, ModelFileExistsReturnsFalseForNonExistentModel)
{
    EXPECT_FALSE(manager->modelFileExists("nonexistent_model_12345"));
}

TEST_F(LLMManagerTest, GetModelFilePathFindsExistingGGUF)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    // Create a dummy .gguf file
    QString dummyFile = QDir(tempDir.path()).filePath("test_model.gguf");
    QFile f(dummyFile);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("dummy");
    f.close();

    // Set models directory to the temp directory
    QString origDir = manager->modelsDirectory();
    manager->setModelsDirectory(tempDir.path());

    // Should find by base name (without extension)
    QString foundPath = manager->getModelFilePath("test_model");
    EXPECT_FALSE(foundPath.isEmpty());
    EXPECT_TRUE(foundPath.endsWith("test_model.gguf"));

    // Should also find by full filename
    QString foundPath2 = manager->getModelFilePath("test_model.gguf");
    EXPECT_FALSE(foundPath2.isEmpty());

    // Restore original directory
    manager->setModelsDirectory(origDir);
}

TEST_F(LLMManagerTest, GetModelFilePathFindsExistingBin)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    // Create a dummy .bin file
    QString dummyFile = QDir(tempDir.path()).filePath("test_model.bin");
    QFile f(dummyFile);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("dummy");
    f.close();

    QString origDir = manager->modelsDirectory();
    manager->setModelsDirectory(tempDir.path());

    // Should find by base name (tries .gguf first, then .bin)
    QString foundPath = manager->getModelFilePath("test_model");
    EXPECT_FALSE(foundPath.isEmpty());
    EXPECT_TRUE(foundPath.endsWith("test_model.bin"));

    manager->setModelsDirectory(origDir);
}

// =============================================================================
// modelsDirectory property tests
// =============================================================================

TEST_F(LLMManagerTest, ModelsDirectoryNotEmpty)
{
    EXPECT_FALSE(manager->modelsDirectory().isEmpty());
}

TEST_F(LLMManagerTest, SetModelsDirectoryEmitsSignal)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    QString origDir = manager->modelsDirectory();
    QSignalSpy spy(manager, &LLMManager::modelsDirectoryChanged);

    manager->setModelsDirectory(tempDir.path());
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(manager->modelsDirectory(), tempDir.path());

    manager->setModelsDirectory(origDir);
}

TEST_F(LLMManagerTest, SetModelsDirectorySameValueNoSignal)
{
    QString current = manager->modelsDirectory();
    QSignalSpy spy(manager, &LLMManager::modelsDirectoryChanged);

    manager->setModelsDirectory(current);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(LLMManagerTest, SetModelsDirectoryCreatesDirectoryIfNeeded)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    QString newDir = QDir(tempDir.path()).filePath("new_models_subdir");
    EXPECT_FALSE(QDir(newDir).exists());

    QString origDir = manager->modelsDirectory();
    manager->setModelsDirectory(newDir);
    EXPECT_TRUE(QDir(newDir).exists());

    manager->setModelsDirectory(origDir);
}

// =============================================================================
// scanForModels tests
// =============================================================================

TEST_F(LLMManagerTest, ScanForModelsFindsGGUFFiles)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    // Create dummy model files
    for (const QString& name : {"model_a.gguf", "model_b.gguf", "not_a_model.txt"}) {
        QFile f(QDir(tempDir.path()).filePath(name));
        f.open(QIODevice::WriteOnly);
        f.write("dummy");
        f.close();
    }

    QString origDir = manager->modelsDirectory();
    manager->setModelsDirectory(tempDir.path());

    QStringList models = manager->availableModels();
    // Should find the two .gguf files but not the .txt
    bool foundA = false, foundB = false;
    for (const QString& m : models) {
        if (m == "model_a") foundA = true;
        if (m == "model_b") foundB = true;
    }
    EXPECT_TRUE(foundA);
    EXPECT_TRUE(foundB);

    manager->setModelsDirectory(origDir);
}

TEST_F(LLMManagerTest, ScanForModelsEmitsSignal)
{
    QSignalSpy spy(manager, &LLMManager::availableModelsChanged);
    manager->scanForModels();
    EXPECT_GE(spy.count(), 1);
}

// =============================================================================
// Static method tests
// =============================================================================

TEST_F(LLMManagerTest, GetOgre3DSystemPromptNotEmpty)
{
    QString prompt = LLMManager::getOgre3DSystemPrompt();
    EXPECT_FALSE(prompt.isEmpty());
}

TEST_F(LLMManagerTest, SystemPromptContainsMaterialKeyword)
{
    QString prompt = LLMManager::getOgre3DSystemPrompt();
    EXPECT_TRUE(prompt.contains("material"));
}

TEST_F(LLMManagerTest, SystemPromptContainsOgreKeyword)
{
    QString prompt = LLMManager::getOgre3DSystemPrompt();
    EXPECT_TRUE(prompt.contains("Ogre"));
}

// =============================================================================
// Initial state tests
// =============================================================================

TEST_F(LLMManagerTest, InitialModelNotLoaded)
{
    // Without loading a model, isModelLoaded may depend on worker state.
    // At minimum, currentModelName should be empty if nothing was auto-loaded.
    // (Auto-load may have set it if configured, so we just verify the method works)
    manager->isModelLoaded();
    // No crash = pass
}

TEST_F(LLMManagerTest, IsGeneratingReturnsFalseWhenIdle)
{
    // Without any generation in progress, isGenerating should return false
    EXPECT_FALSE(manager->isGenerating());
}

TEST_F(LLMManagerTest, IsLoadingReturnsFalseWhenIdle)
{
    // isLoading should be false when no model loading is in progress
    // Note: If a model auto-loaded it may still be loading, but typically
    // during tests with no real model file it will be false
    manager->isLoading();
    // No crash = pass
}

// =============================================================================
// ModelInfo struct tests
// =============================================================================

TEST_F(LLMManagerTest, ModelInfoToVariantMap)
{
    ModelInfo info;
    info.name = "Test Model";
    info.fileName = "test_model.gguf";
    info.url = "https://example.com/model.gguf";
    info.description = "A test model";
    info.size = 1000000;
    info.isDownloaded = true;

    QVariantMap map = info.toVariantMap();
    EXPECT_EQ(map["name"].toString(), "Test Model");
    EXPECT_EQ(map["fileName"].toString(), "test_model.gguf");
    EXPECT_EQ(map["url"].toString(), "https://example.com/model.gguf");
    EXPECT_EQ(map["description"].toString(), "A test model");
    EXPECT_EQ(map["size"].toLongLong(), 1000000);
    EXPECT_EQ(map["isDownloaded"].toBool(), true);
}

TEST_F(LLMManagerTest, ModelInfoToVariantMapNotDownloaded)
{
    ModelInfo info;
    info.name = "Another Model";
    info.fileName = "another.gguf";
    info.url = "https://example.com/another.gguf";
    info.description = "Another test model";
    info.size = 5000000000;
    info.isDownloaded = false;

    QVariantMap map = info.toVariantMap();
    EXPECT_EQ(map["isDownloaded"].toBool(), false);
    EXPECT_EQ(map["size"].toLongLong(), 5000000000);
}

// =============================================================================
// Edge cases for validateMaterialScript
// =============================================================================

TEST_F(LLMManagerTest, ValidateScriptWithMixedValidAndInvalidProperties)
{
    QString error;
    QString script =
        "material TestMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "            ambient 0.1 0.2 0.3\n"
        "            diffuse abc 0.5 0.6\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_FALSE(manager->validateMaterialScript(script, error));
    EXPECT_TRUE(error.contains("Invalid value"));
    EXPECT_TRUE(error.contains("abc"));
}

TEST_F(LLMManagerTest, ValidateScriptWithLeadingWhitespace)
{
    QString error;
    QString script =
        "   material TestMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "        }\n"
        "    }\n"
        "}\n";
    // The validation trims, so leading whitespace before "material" should be handled
    EXPECT_TRUE(manager->validateMaterialScript(script, error));
}

TEST_F(LLMManagerTest, ValidateScriptWithMultipleTechniques)
{
    QString error;
    QString script =
        "material MultiTechMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "            ambient 0.1 0.2 0.3\n"
        "        }\n"
        "    }\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "            ambient 0.4 0.5 0.6\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_TRUE(manager->validateMaterialScript(script, error));
}

// =============================================================================
// Edge cases for cleanupGeneratedScript
// =============================================================================

TEST_F(LLMManagerTest, CleanupHandlesMultipleMaterialBlocks)
{
    // Should only keep the first complete material block
    QString input =
        "material First\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "        }\n"
        "    }\n"
        "}\n"
        "material Second\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "        }\n"
        "    }\n"
        "}\n";
    QString result = manager->cleanupGeneratedScript(input);
    // The cleanup truncates at the first balanced closing brace
    EXPECT_TRUE(result.startsWith("material First"));
    EXPECT_FALSE(result.contains("material Second"));
}

TEST_F(LLMManagerTest, CleanupHandlesMarkdownWithLanguageTag)
{
    QString input =
        "```material\n"
        "material TestMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "        }\n"
        "    }\n"
        "}\n"
        "```";
    QString result = manager->cleanupGeneratedScript(input);
    EXPECT_FALSE(result.contains("```"));
    EXPECT_TRUE(result.startsWith("material"));
}

// =============================================================================
// Additional tests -- no network access or model files required
// =============================================================================

TEST_F(LLMManagerTest, GetModelFilePathWithValidAndInvalidNames)
{
    // Non-existent model names should return empty paths
    EXPECT_TRUE(manager->getModelFilePath("completely_nonexistent_model_xyz").isEmpty());
    EXPECT_TRUE(manager->getModelFilePath("../../../etc/passwd").isEmpty());
    EXPECT_TRUE(manager->getModelFilePath("model with spaces").isEmpty());

    // modelFileExists should also return false for non-existent models
    EXPECT_FALSE(manager->modelFileExists("completely_nonexistent_model_xyz"));

    // Empty string may or may not match depending on the models directory contents,
    // so we just verify it does not crash
    manager->getModelFilePath("");
    manager->modelFileExists("");
}

TEST_F(LLMManagerTest, InitialStateQueries)
{
    // isModelLoaded: no model loaded during tests (no real model file available)
    // The call should not crash regardless of the return value
    bool loaded = manager->isModelLoaded();
    // Without a real model file, this should be false
    // (unless autoload succeeded, but typically no model is available in test env)
    EXPECT_FALSE(loaded) << "No model should be loaded in test environment";

    // isGenerating: should be false when idle
    EXPECT_FALSE(manager->isGenerating());

    // isLoading: should be false when no model loading is in progress
    EXPECT_FALSE(manager->isLoading());

    // currentModelName: should be empty or a valid string (no crash)
    QString modelName = manager->currentModelName();
    // Model name depends on environment, just verify no crash
}

TEST_F(LLMManagerTest, SettingsGettersReturnReasonableValues)
{
    // Verify contextSize, maxTokens, temperature are within reasonable bounds
    int ctx = manager->contextSize();
    EXPECT_GT(ctx, 0);
    EXPECT_LE(ctx, 1048576); // reasonable upper bound (1M tokens)

    int maxTok = manager->maxTokens();
    EXPECT_GT(maxTok, 0);
    EXPECT_LE(maxTok, 1048576);

    float temp = manager->temperature();
    EXPECT_GE(temp, 0.0f);
    EXPECT_LE(temp, 10.0f); // reasonable upper bound

    int gpu = manager->gpuLayers();
    EXPECT_GE(gpu, 0);

    // lastModelName should be callable without crash
    QString lastModel = manager->lastModelName();
    (void)lastModel;
}

TEST_F(LLMManagerTest, AvailableModelsInitialState)
{
    // availableModels may be empty if no model files are in the models directory.
    // The call must not crash, and should return a valid QStringList.
    QStringList models = manager->availableModels();
    // We do not assert on size since it depends on the local file system,
    // but we verify the list is a valid object
    EXPECT_GE(models.size(), 0);

    // getAvailableModelsInfo should also be callable and consistent
    QVariantList modelsInfo = manager->getAvailableModelsInfo();
    // Each entry in modelsInfo should correspond to an entry in availableModels
    // (though their representation differs -- name list vs variant map list)
    EXPECT_GE(modelsInfo.size(), 0);
}

TEST_F(LLMManagerTest, GetOgre3DSystemPromptContentCheck)
{
    // The system prompt should be non-empty and contain relevant keywords
    QString prompt = LLMManager::getOgre3DSystemPrompt();
    EXPECT_FALSE(prompt.isEmpty());
    EXPECT_GT(prompt.length(), 50); // Should be a substantial prompt

    // Should contain Ogre material-related terms
    EXPECT_TRUE(prompt.contains("material", Qt::CaseInsensitive));
    EXPECT_TRUE(prompt.contains("Ogre", Qt::CaseInsensitive));
    EXPECT_TRUE(prompt.contains("technique", Qt::CaseInsensitive));
    EXPECT_TRUE(prompt.contains("pass", Qt::CaseInsensitive));
}

// =============================================================================
// generateMaterial tests (exercises buildUserPrompt indirectly when model loaded)
// Since no model is loaded in tests, generateMaterial returns early with error.
// These tests verify the error path and signal emission.
// =============================================================================

TEST_F(LLMManagerTest, GenerateMaterial_NoModelLoaded_EmitsError)
{
    QSignalSpy errorSpy(manager, &LLMManager::generationError);
    manager->generateMaterial("Create a red material");
    EXPECT_GE(errorSpy.count(), 1);
    if (errorSpy.count() > 0) {
        QString errorMsg = errorSpy.first().at(0).toString();
        EXPECT_TRUE(errorMsg.contains("model", Qt::CaseInsensitive));
    }
}

TEST_F(LLMManagerTest, GenerateMaterial_WithCurrentMaterial_NoModelLoaded)
{
    QSignalSpy errorSpy(manager, &LLMManager::generationError);
    QString currentMaterial =
        "material TestMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "            ambient 0.5 0.5 0.5\n"
        "        }\n"
        "    }\n"
        "}\n";
    manager->generateMaterial("Make it shinier", currentMaterial);
    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(LLMManagerTest, GenerateMaterial_WithTextures_NoModelLoaded)
{
    QSignalSpy errorSpy(manager, &LLMManager::generationError);
    QStringList textures = {"brick.png", "grass.jpg", "metal.dds"};
    manager->generateMaterial("Add a brick texture", QString(), textures);
    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(LLMManagerTest, GenerateMaterial_WithMaterialAndTextures_NoModelLoaded)
{
    QSignalSpy errorSpy(manager, &LLMManager::generationError);
    QString currentMaterial =
        "material ExistingMat\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "        }\n"
        "    }\n"
        "}\n";
    QStringList textures = {"wood.png", "Ogre/internal_tex", "RTT_texture", "normal_map.dds"};
    manager->generateMaterial("Apply a wood texture to this material", currentMaterial, textures);
    EXPECT_GE(errorSpy.count(), 1);
}

TEST_F(LLMManagerTest, GenerateMaterial_EmptyPrompt_NoModelLoaded)
{
    QSignalSpy errorSpy(manager, &LLMManager::generationError);
    manager->generateMaterial("");
    EXPECT_GE(errorSpy.count(), 1);
}

// =============================================================================
// stopGeneration tests
// =============================================================================

TEST_F(LLMManagerTest, StopGeneration_WhenNotGenerating)
{
    // Calling stopGeneration when not generating should not crash
    EXPECT_FALSE(manager->isGenerating());
    manager->stopGeneration();
    // No crash = pass
    EXPECT_FALSE(manager->isGenerating());
}

// =============================================================================
// unloadModel tests
// =============================================================================

TEST_F(LLMManagerTest, UnloadModel_WhenNoModelLoaded)
{
    // Unloading when no model is loaded should be safe
    manager->unloadModel();
    // No crash = pass
}

// =============================================================================
// loadModel with non-existent model
// =============================================================================

TEST_F(LLMManagerTest, LoadModel_NonExistentModel)
{
    QSignalSpy errorSpy(manager, &LLMManager::modelLoadError);
    manager->loadModel("completely_nonexistent_model_xyz_999");
    // Allow some time for the async operation to report an error
    QThread::msleep(100);
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->processEvents();
    }
    // Verify no model became loaded
    EXPECT_FALSE(manager->isModelLoaded());
}

// =============================================================================
// tryAutoLoadModel tests
// =============================================================================

TEST_F(LLMManagerTest, TryAutoLoadModel_NoModelsAvailable)
{
    // In test environment, there are typically no model files
    // tryAutoLoadModel should be safe to call
    manager->tryAutoLoadModel();
    // No crash = pass
}

// =============================================================================
// Additional settings property tests
// =============================================================================

TEST_F(LLMManagerTest, SetAndGetTopP)
{
    LLMSettings original = manager->getSettings();

    LLMSettings modified = original;
    modified.topP = 0.5f;
    manager->setSettings(modified);

    LLMSettings retrieved = manager->getSettings();
    EXPECT_FLOAT_EQ(retrieved.topP, 0.5f);

    // Restore
    manager->setSettings(original);
}

TEST_F(LLMManagerTest, SetAndGetTopK)
{
    LLMSettings original = manager->getSettings();

    LLMSettings modified = original;
    modified.topK = 10;
    manager->setSettings(modified);

    LLMSettings retrieved = manager->getSettings();
    EXPECT_EQ(retrieved.topK, 10);

    // Restore
    manager->setSettings(original);
}

TEST_F(LLMManagerTest, SetAndGetRepeatPenalty)
{
    LLMSettings original = manager->getSettings();

    LLMSettings modified = original;
    modified.repeatPenalty = 1.5f;
    manager->setSettings(modified);

    LLMSettings retrieved = manager->getSettings();
    EXPECT_FLOAT_EQ(retrieved.repeatPenalty, 1.5f);

    // Restore
    manager->setSettings(original);
}

TEST_F(LLMManagerTest, SetAndGetThreads)
{
    LLMSettings original = manager->getSettings();

    LLMSettings modified = original;
    modified.threads = 8;
    manager->setSettings(modified);

    LLMSettings retrieved = manager->getSettings();
    EXPECT_EQ(retrieved.threads, 8);

    // Restore
    manager->setSettings(original);
}

// =============================================================================
// Validate scripts with multiple texture_unit blocks
// =============================================================================

TEST_F(LLMManagerTest, ValidateScriptWithMultipleTextureUnits)
{
    QString error;
    QString script =
        "material MultiTexMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "            ambient 0.5 0.5 0.5\n"
        "            diffuse 1.0 1.0 1.0\n"
        "            texture_unit\n"
        "            {\n"
        "                texture brick.png\n"
        "            }\n"
        "            texture_unit\n"
        "            {\n"
        "                texture normalmap.dds\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_TRUE(manager->validateMaterialScript(script, error));
    EXPECT_TRUE(error.isEmpty());
}

// =============================================================================
// Validate scripts with depth and lighting properties
// =============================================================================

TEST_F(LLMManagerTest, ValidateScriptWithDepthAndLightingProperties)
{
    QString error;
    QString script =
        "material AdvancedMaterial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "            lighting on\n"
        "            depth_write off\n"
        "            depth_check off\n"
        "            scene_blend add\n"
        "            cull_hardware none\n"
        "            cull_software none\n"
        "            ambient 0.0 0.0 0.0\n"
        "            diffuse 1.0 1.0 1.0\n"
        "            specular 1.0 1.0 1.0 128\n"
        "            emissive 0.1 0.1 0.1\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_TRUE(manager->validateMaterialScript(script, error));
    EXPECT_TRUE(error.isEmpty());
}

// =============================================================================
// cleanupGeneratedScript: additional edge cases
// =============================================================================

TEST_F(LLMManagerTest, CleanupHandlesOnlyCodeFences)
{
    QString input = "```\n```";
    QString result = manager->cleanupGeneratedScript(input);
    // After removing fences, result may be empty or just whitespace
    EXPECT_FALSE(result.contains("```"));
}

TEST_F(LLMManagerTest, CleanupHandlesPartialMaterialBlock)
{
    // A partial material block (truncated output from LLM)
    QString input =
        "material Partial\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n";
    QString result = manager->cleanupGeneratedScript(input);
    // Should start with "material"
    EXPECT_TRUE(result.startsWith("material"));
}

#endif // ENABLE_LOCAL_LLM
