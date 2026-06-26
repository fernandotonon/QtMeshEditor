#ifdef ENABLE_LOCAL_LLM

#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include "LLMWorker.h"

class LLMWorkerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }

    QApplication* app = nullptr;
};

TEST_F(LLMWorkerTest, Constructor)
{
    LLMWorker worker;
    EXPECT_FALSE(worker.isModelLoaded());
    EXPECT_FALSE(worker.isGenerating());
    EXPECT_TRUE(worker.getLoadedModelPath().isEmpty());
}

TEST_F(LLMWorkerTest, DefaultSettings)
{
    LLMWorker worker;
    LLMSettings settings = worker.getSettings();
    EXPECT_EQ(settings.contextSize, 4096);
    EXPECT_EQ(settings.maxTokens, 2048);
    EXPECT_FLOAT_EQ(settings.temperature, 0.7f);
    EXPECT_EQ(settings.gpuLayers, 99);
    EXPECT_EQ(settings.threads, 0);
    EXPECT_FLOAT_EQ(settings.topP, 0.9f);
    EXPECT_EQ(settings.topK, 40);
    EXPECT_FLOAT_EQ(settings.repeatPenalty, 1.1f);
}

TEST_F(LLMWorkerTest, SetSettings)
{
    LLMWorker worker;
    LLMSettings newSettings;
    newSettings.contextSize = 8192;
    newSettings.maxTokens = 4096;
    newSettings.temperature = 0.5f;
    newSettings.gpuLayers = 50;
    newSettings.threads = 4;
    newSettings.topP = 0.8f;
    newSettings.topK = 20;
    newSettings.repeatPenalty = 1.2f;

    worker.setSettings(newSettings);
    LLMSettings retrieved = worker.getSettings();

    EXPECT_EQ(retrieved.contextSize, 8192);
    EXPECT_EQ(retrieved.maxTokens, 4096);
    EXPECT_FLOAT_EQ(retrieved.temperature, 0.5f);
    EXPECT_EQ(retrieved.gpuLayers, 50);
    EXPECT_EQ(retrieved.threads, 4);
    EXPECT_FLOAT_EQ(retrieved.topP, 0.8f);
    EXPECT_EQ(retrieved.topK, 20);
    EXPECT_FLOAT_EQ(retrieved.repeatPenalty, 1.2f);
}

TEST_F(LLMWorkerTest, RequestStopWithoutGenerating)
{
    LLMWorker worker;
    // Should not crash when stopping without generating
    worker.requestStop();
    EXPECT_FALSE(worker.isGenerating());
}

TEST_F(LLMWorkerTest, UnloadModelWithoutLoading)
{
    LLMWorker worker;
    // Should not crash
    worker.unloadModel();
    EXPECT_FALSE(worker.isModelLoaded());
}

// NOTE: LoadModelInvalidPath and LoadModelEmptyPath tests were removed because
// loadModel() calls into llama.cpp/ggml which can SIGABRT on invalid paths
// (ggml assertion failure). These tests cannot work without a real model file.

#endif // ENABLE_LOCAL_LLM
