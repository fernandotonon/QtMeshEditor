#ifdef ENABLE_STABLE_DIFFUSION

#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QDir>
#include <QStandardPaths>
#include "SDManager.h"

class SDManagerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        previousOrgName = QCoreApplication::organizationName();
        previousAppName = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName("QtMeshEditor");
        QCoreApplication::setApplicationName("QtMeshEditor");
        manager = SDManager::instance();
        ASSERT_NE(manager, nullptr);
    }

    void TearDown() override
    {
        QCoreApplication::setOrganizationName(previousOrgName);
        QCoreApplication::setApplicationName(previousAppName);
    }

    QString createModelFile(const QString& directory, const QString& fileName)
    {
        QDir().mkpath(directory);
        QFile file(QDir(directory).filePath(fileName));
        const QString path = file.fileName();
        if (!file.open(QIODevice::WriteOnly)) {
            ADD_FAILURE() << "Failed to create temporary model file: " << path.toStdString();
            return QString();
        }
        file.write("stub");
        file.close();
        return path;
    }

    QApplication* app = nullptr;
    SDManager* manager = nullptr;
    QString previousOrgName;
    QString previousAppName;
};

TEST_F(SDManagerTest, Singleton)
{
    SDManager* instance1 = SDManager::instance();
    SDManager* instance2 = SDManager::instance();
    EXPECT_EQ(instance1, instance2);
}

TEST_F(SDManagerTest, InitialState)
{
    EXPECT_FALSE(manager->isModelLoaded());
    EXPECT_FALSE(manager->isGenerating());
    EXPECT_FALSE(manager->isLoading());
    EXPECT_TRUE(manager->currentModelName().isEmpty());
}

TEST_F(SDManagerTest, DefaultSettings)
{
    EXPECT_EQ(manager->imageWidth(), 512);
    EXPECT_EQ(manager->imageHeight(), 512);
    EXPECT_EQ(manager->steps(), 20);
    EXPECT_FLOAT_EQ(manager->cfgScale(), 7.0f);
    EXPECT_TRUE(manager->negativePrompt().isEmpty());
}

TEST_F(SDManagerTest, SetImageWidth)
{
    QSignalSpy spy(manager, &SDManager::settingsChanged);
    manager->setImageWidth(256);
    EXPECT_EQ(manager->imageWidth(), 256);
    EXPECT_GE(spy.count(), 1);
    // Restore
    manager->setImageWidth(512);
}

TEST_F(SDManagerTest, SetImageHeight)
{
    QSignalSpy spy(manager, &SDManager::settingsChanged);
    manager->setImageHeight(256);
    EXPECT_EQ(manager->imageHeight(), 256);
    EXPECT_GE(spy.count(), 1);
    // Restore
    manager->setImageHeight(512);
}

TEST_F(SDManagerTest, SetSteps)
{
    QSignalSpy spy(manager, &SDManager::settingsChanged);
    manager->setSteps(10);
    EXPECT_EQ(manager->steps(), 10);
    EXPECT_GE(spy.count(), 1);
    // Restore
    manager->setSteps(20);
}

TEST_F(SDManagerTest, SetCfgScale)
{
    QSignalSpy spy(manager, &SDManager::settingsChanged);
    manager->setCfgScale(5.0f);
    EXPECT_FLOAT_EQ(manager->cfgScale(), 5.0f);
    EXPECT_GE(spy.count(), 1);
    // Restore
    manager->setCfgScale(7.0f);
}

TEST_F(SDManagerTest, SetNegativePrompt)
{
    QSignalSpy spy(manager, &SDManager::settingsChanged);
    manager->setNegativePrompt("blurry, low quality");
    EXPECT_EQ(manager->negativePrompt(), "blurry, low quality");
    EXPECT_GE(spy.count(), 1);
    // Restore
    manager->setNegativePrompt("");
}

TEST_F(SDManagerTest, ModelsDirectory)
{
    QString dir = manager->modelsDirectory();
    EXPECT_FALSE(dir.isEmpty());
    EXPECT_TRUE(dir.contains("sd_models"));
}

TEST_F(SDManagerTest, ScanForModels)
{
    // Should not crash even with empty directory
    manager->scanForModels();
}

TEST_F(SDManagerTest, ScanForModelsFindsSupportedExtensionsAndMarksRecommendationsDownloaded)
{
    QString originalDir = manager->modelsDirectory();
    QString tempDir = QDir::temp().filePath("qtmesh_sd_scan_models");

    createModelFile(tempDir, "custom_a.safetensors");
    createModelFile(tempDir, "custom_b.ckpt");
    createModelFile(tempDir, "custom_c.gguf");
    createModelFile(tempDir, "v1-5-pruned-emaonly.safetensors");

    manager->setModelsDirectory(tempDir);
    manager->scanForModels();

    const QStringList models = manager->availableModels();
    EXPECT_TRUE(models.contains("custom_a"));
    EXPECT_TRUE(models.contains("custom_b"));
    EXPECT_TRUE(models.contains("custom_c"));
    EXPECT_TRUE(models.contains("v1-5-pruned-emaonly"));

    const QVariantList recommended = manager->getRecommendedModelsInfo();
    bool foundDownloadedRecommendation = false;
    for (const QVariant& entry : recommended) {
        const QVariantMap info = entry.toMap();
        if (info.value("fileName").toString() == "v1-5-pruned-emaonly.safetensors") {
            foundDownloadedRecommendation = true;
            EXPECT_TRUE(info.value("isDownloaded").toBool());
        }
    }
    EXPECT_TRUE(foundDownloadedRecommendation);

    manager->setModelsDirectory(originalDir);
    QDir(tempDir).removeRecursively();
}

TEST_F(SDManagerTest, LoadModelNotFound)
{
    QSignalSpy errorSpy(manager, &SDManager::modelLoadError);
    manager->loadModel("nonexistent_model");
    ASSERT_EQ(errorSpy.count(), 1);
    EXPECT_TRUE(errorSpy.first().first().toString().contains("not found"));
}

TEST_F(SDManagerTest, GenerateWithoutModel)
{
    QSignalSpy errorSpy(manager, &SDManager::generationError);
    manager->generateTexture("test prompt");
    ASSERT_EQ(errorSpy.count(), 1);
    EXPECT_TRUE(errorSpy.first().first().toString().contains("No SD model loaded"));
}

TEST_F(SDManagerTest, RecommendedModels)
{
    QVariantList models = manager->getRecommendedModelsInfo();
    EXPECT_GT(models.size(), 0);

    QVariantMap first = models.first().toMap();
    EXPECT_FALSE(first["name"].toString().isEmpty());
    EXPECT_FALSE(first["fileName"].toString().isEmpty());
    EXPECT_GT(first["size"].toLongLong(), 0);
}

TEST_F(SDManagerTest, ModelFileExistsNonexistent)
{
    EXPECT_FALSE(manager->modelFileExists("nonexistent_model"));
}

TEST_F(SDManagerTest, GetModelFilePathNonexistent)
{
    EXPECT_TRUE(manager->getModelFilePath("nonexistent_model").isEmpty());
}

TEST_F(SDManagerTest, GetModelFilePathResolvesDirectFilenameAndBaseNameWithDots)
{
    QString originalDir = manager->modelsDirectory();
    QString tempDir = QDir::temp().filePath("qtmesh_sd_path_models");

    const QString directPath = createModelFile(tempDir, "direct-model.gguf");
    const QString dottedPath = createModelFile(tempDir, "model.1.0.safetensors");

    manager->setModelsDirectory(tempDir);

    EXPECT_EQ(manager->getModelFilePath("direct-model.gguf"), directPath);
    EXPECT_EQ(manager->getModelFilePath("model.1.0"), dottedPath);
    EXPECT_TRUE(manager->modelFileExists("model.1.0"));

    manager->setModelsDirectory(originalDir);
    QDir(tempDir).removeRecursively();
}

TEST_F(SDManagerTest, GetModelFilePathResolvesRecommendedModelFilename)
{
    QString originalDir = manager->modelsDirectory();
    QString tempDir = QDir::temp().filePath("qtmesh_sd_recommended_models");

    const QString recommendedPath = createModelFile(tempDir, "sd_xl_turbo_1.0_fp16.safetensors");
    manager->setModelsDirectory(tempDir);

    EXPECT_EQ(manager->getModelFilePath("SDXL Turbo (FP16)"), recommendedPath);
    EXPECT_TRUE(manager->modelFileExists("SDXL Turbo (FP16)"));

    manager->setModelsDirectory(originalDir);
    QDir(tempDir).removeRecursively();
}

TEST_F(SDManagerTest, GenerationProgress)
{
    EXPECT_EQ(manager->generationStep(), 0);
    EXPECT_EQ(manager->generationTotalSteps(), 0);
}

// ---- enhanceTexturePrompt ----

TEST_F(SDManagerTest, EnhanceTexturePrompt)
{
    QString enhanced = manager->enhanceTexturePrompt("wood");
    EXPECT_FALSE(enhanced.isEmpty());
    // Should contain the original prompt
    EXPECT_TRUE(enhanced.contains("wood"));
    // Should add texture-related terms
    EXPECT_TRUE(enhanced.contains("texture") || enhanced.contains("seamless") || enhanced.contains("tileable"));
}

TEST_F(SDManagerTest, EnhanceTexturePromptEmpty)
{
    QString enhanced = manager->enhanceTexturePrompt("");
    // Even empty prompt should get enhancement
    EXPECT_FALSE(enhanced.isEmpty());
}

TEST_F(SDManagerTest, EnhanceTexturePromptAlreadyDetailed)
{
    QString prompt = "seamless wood texture high resolution tileable";
    QString enhanced = manager->enhanceTexturePrompt(prompt);
    EXPECT_FALSE(enhanced.isEmpty());
}

// ---- getTextureNegativePrompt ----

TEST_F(SDManagerTest, GetTextureNegativePrompt)
{
    QString negative = manager->getTextureNegativePrompt();
    EXPECT_FALSE(negative.isEmpty());
}

// ---- getAvailableModelsInfo ----

TEST_F(SDManagerTest, GetAvailableModelsInfo)
{
    QVariantList models = manager->getAvailableModelsInfo();
    // Might be empty if no models installed, but should not crash
    EXPECT_GE(models.size(), 0);
}

TEST_F(SDManagerTest, GetAvailableModelsInfoIncludesFileMetadata)
{
    QString originalDir = manager->modelsDirectory();
    QString tempDir = QDir::temp().filePath("qtmesh_sd_info_models");

    createModelFile(tempDir, "metadata-model.ckpt");
    manager->setModelsDirectory(tempDir);
    manager->scanForModels();

    const QVariantList models = manager->getAvailableModelsInfo();
    ASSERT_EQ(models.size(), 1);

    const QVariantMap info = models.first().toMap();
    EXPECT_EQ(info.value("name").toString(), QString("metadata-model"));
    EXPECT_EQ(info.value("fileName").toString(), QString("metadata-model.ckpt"));
    EXPECT_GT(info.value("size").toLongLong(), 0);
    EXPECT_TRUE(info.value("isDownloaded").toBool());

    manager->setModelsDirectory(originalDir);
    QDir(tempDir).removeRecursively();
}

// ---- setAutoLoadModel ----

TEST_F(SDManagerTest, SetAutoLoadModel)
{
    bool original = manager->autoLoadModel();
    manager->setAutoLoadModel(!original);
    EXPECT_EQ(manager->autoLoadModel(), !original);
    // Restore
    manager->setAutoLoadModel(original);
}

// ---- getSettings / setSettings ----

TEST_F(SDManagerTest, GetSettingsRoundtrip)
{
    SDSettings original = manager->getSettings();

    SDSettings modified = original;
    modified.width = 256;
    modified.height = 256;
    modified.steps = 10;
    modified.cfgScale = 3.0f;
    modified.negativePrompt = "test negative";

    manager->setSettings(modified);
    SDSettings retrieved = manager->getSettings();
    EXPECT_EQ(retrieved.width, 256);
    EXPECT_EQ(retrieved.height, 256);
    EXPECT_EQ(retrieved.steps, 10);
    EXPECT_FLOAT_EQ(retrieved.cfgScale, 3.0f);
    EXPECT_EQ(retrieved.negativePrompt, "test negative");

    // Restore
    manager->setSettings(original);
}

TEST_F(SDManagerTest, SaveSettingsPersistsCoreFields)
{
    const QString originalDir = manager->modelsDirectory();
    const bool originalAutoLoad = manager->autoLoadModel();
    const SDSettings originalSettings = manager->getSettings();

    const QString tempDir = QDir::temp().filePath("qtmesh_sd_settings_models");
    manager->setModelsDirectory(tempDir);
    manager->setImageWidth(768);
    manager->setImageHeight(320);
    manager->setSteps(14);
    manager->setCfgScale(4.5f);
    manager->setNegativePrompt("persist me");
    manager->setAutoLoadModel(false);
    manager->saveSettings();

    QSettings settings;
    settings.beginGroup("StableDiffusion");
    EXPECT_EQ(settings.value("modelsDirectory").toString(), tempDir);
    EXPECT_EQ(settings.value("width").toInt(), 768);
    EXPECT_EQ(settings.value("height").toInt(), 320);
    EXPECT_EQ(settings.value("steps").toInt(), 14);
    EXPECT_FLOAT_EQ(settings.value("cfgScale").toFloat(), 4.5f);
    EXPECT_EQ(settings.value("negativePrompt").toString(), QString("persist me"));
    EXPECT_FALSE(settings.value("autoLoadModel").toBool());
    settings.endGroup();

    manager->setModelsDirectory(originalDir);
    manager->setSettings(originalSettings);
    manager->setAutoLoadModel(originalAutoLoad);
    manager->saveSettings();
    QDir(tempDir).removeRecursively();
}

// ---- setModelsDirectory ----

TEST_F(SDManagerTest, SetModelsDirectorySignal)
{
    QString original = manager->modelsDirectory();
    QSignalSpy spy(manager, &SDManager::modelsDirectoryChanged);

    QString tempDir = QDir::temp().filePath("qtmesh_sd_test_models");
    manager->setModelsDirectory(tempDir);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(manager->modelsDirectory(), tempDir);

    // Restore
    manager->setModelsDirectory(original);

    // Cleanup
    QDir(tempDir).removeRecursively();
}

// ---- Same value doesn't emit ----

TEST_F(SDManagerTest, SetImageWidthSameValueNoSignal)
{
    int current = manager->imageWidth();
    QSignalSpy spy(manager, &SDManager::settingsChanged);
    manager->setImageWidth(current);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(SDManagerTest, SetStepsSameValueNoSignal)
{
    int current = manager->steps();
    QSignalSpy spy(manager, &SDManager::settingsChanged);
    manager->setSteps(current);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(SDManagerTest, SetCfgScaleSameValueNoSignal)
{
    float current = manager->cfgScale();
    QSignalSpy spy(manager, &SDManager::settingsChanged);
    manager->setCfgScale(current);
    EXPECT_EQ(spy.count(), 0);
}

// ---- qmlInstance ----

TEST_F(SDManagerTest, QmlInstanceReturnsSameAsSingleton)
{
    SDManager* qmlInst = SDManager::qmlInstance(nullptr, nullptr);
    EXPECT_EQ(qmlInst, manager);
}

#endif // ENABLE_STABLE_DIFFUSION
