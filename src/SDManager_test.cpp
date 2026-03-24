#ifdef ENABLE_STABLE_DIFFUSION

#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
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
        manager = SDManager::instance();
        ASSERT_NE(manager, nullptr);
    }

    QApplication* app = nullptr;
    SDManager* manager = nullptr;
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
