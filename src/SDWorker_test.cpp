#ifdef ENABLE_STABLE_DIFFUSION

#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include "SDWorker.h"

class SDWorkerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }

    QApplication* app = nullptr;
};

TEST_F(SDWorkerTest, DefaultSettings)
{
    SDWorker worker;
    SDSettings settings = worker.getSettings();
    EXPECT_EQ(settings.width, 512);
    EXPECT_EQ(settings.height, 512);
    EXPECT_EQ(settings.steps, 30);
    EXPECT_FLOAT_EQ(settings.cfgScale, 7.0f);
    EXPECT_EQ(settings.seed, -1);
    EXPECT_FALSE(settings.negativePrompt.isEmpty()); // Has default negative prompt
    EXPECT_EQ(settings.sampleMethod, 0);
    EXPECT_EQ(settings.threads, 0);
    EXPECT_EQ(settings.gpuLayers, 99);
}

TEST_F(SDWorkerTest, SetSettings)
{
    SDWorker worker;
    SDSettings newSettings;
    newSettings.width = 256;
    newSettings.height = 256;
    newSettings.steps = 10;
    newSettings.cfgScale = 5.0f;
    newSettings.seed = 42;
    newSettings.negativePrompt = "blurry";
    newSettings.sampleMethod = 1;
    newSettings.threads = 4;
    newSettings.gpuLayers = 50;

    worker.setSettings(newSettings);
    SDSettings retrieved = worker.getSettings();

    EXPECT_EQ(retrieved.width, 256);
    EXPECT_EQ(retrieved.height, 256);
    EXPECT_EQ(retrieved.steps, 10);
    EXPECT_FLOAT_EQ(retrieved.cfgScale, 5.0f);
    EXPECT_EQ(retrieved.seed, 42);
    EXPECT_EQ(retrieved.negativePrompt, "blurry");
    EXPECT_EQ(retrieved.sampleMethod, 1);
    EXPECT_EQ(retrieved.threads, 4);
    EXPECT_EQ(retrieved.gpuLayers, 50);
}

TEST_F(SDWorkerTest, RequestStopWithoutGenerating)
{
    SDWorker worker;
    worker.requestStop();
    EXPECT_FALSE(worker.isGenerating());
}

TEST_F(SDWorkerTest, UnloadModelWithoutLoading)
{
    SDWorker worker;
    worker.unloadModel();
    EXPECT_FALSE(worker.isModelLoaded());
}

TEST_F(SDWorkerTest, LoadModelInvalidPath)
{
    SDWorker worker;

    QSignalSpy errorSpy(&worker, &SDWorker::modelLoadError);
    bool result = worker.loadModel("/nonexistent/path/model.safetensors");
    EXPECT_FALSE(result);
    EXPECT_FALSE(worker.isModelLoaded());
}

TEST_F(SDWorkerTest, LoadModelEmptyPath)
{
    SDWorker worker;

    bool result = worker.loadModel("");
    EXPECT_FALSE(result);
    EXPECT_FALSE(worker.isModelLoaded());
}

TEST_F(SDWorkerTest, GenerateWithoutModel)
{
    SDWorker worker;
    QSignalSpy errorSpy(&worker, &SDWorker::generationError);
    worker.generateTexture("test prompt", "/tmp/test.png");
    QCoreApplication::processEvents();
    ASSERT_EQ(errorSpy.count(), 1);
    EXPECT_TRUE(errorSpy.first().first().toString().contains("No SD model loaded"));
}

#endif // ENABLE_STABLE_DIFFUSION
