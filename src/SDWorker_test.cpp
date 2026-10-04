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

// ─── Prompt-to-3D: FLUX.2 component-set detection ───────────────────────────
// Pure file-set detection, compiled (and tested) in EVERY build — no
// ENABLE_STABLE_DIFFUSION guard (the AI Model Settings catalog and
// SDManager's model list rely on it even before generation is possible).
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#ifndef ENABLE_STABLE_DIFFUSION
#include <gtest/gtest.h>
#include "SDWorker.h"
#endif

namespace {
void touch(const QString& path)
{
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("x");
}
} // namespace

TEST(SDWorkerFlux2Test, DetectsCompleteSet)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    touch(dir.filePath("flux-2-klein-4b-Q4_0.gguf"));
    touch(dir.filePath("flux2-vae.safetensors"));
    touch(dir.filePath("Qwen3-4B-Q4_K_M.gguf"));

    const auto set = SDWorker::detectFlux2Set(dir.path());
    EXPECT_TRUE(set.valid());
    EXPECT_TRUE(set.diffusion.endsWith("flux-2-klein-4b-Q4_0.gguf"));
    EXPECT_TRUE(set.vae.endsWith("flux2-vae.safetensors"));
    EXPECT_TRUE(set.llm.endsWith("Qwen3-4B-Q4_K_M.gguf"));
}

TEST(SDWorkerFlux2Test, IncompleteSetIsInvalid)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    touch(dir.filePath("flux-2-klein-4b-Q4_0.gguf"));
    touch(dir.filePath("flux2-vae.safetensors"));
    // No text encoder → invalid.
    EXPECT_FALSE(SDWorker::detectFlux2Set(dir.path()).valid());

    // Missing / empty dirs are safe.
    EXPECT_FALSE(SDWorker::detectFlux2Set(QString()).valid());
    EXPECT_FALSE(SDWorker::detectFlux2Set("/no/such/dir").valid());
}

TEST(SDWorkerFlux2Test, VaeNameIsNotMistakenForDiffusion)
{
    // "flux2-vae.safetensors" contains "flux" — the VAE check must win, or
    // the VAE would be loaded as the diffusion model.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    touch(dir.filePath("flux2-vae.safetensors"));
    touch(dir.filePath("flux-2-klein-base-4b.safetensors"));
    touch(dir.filePath("mistral-small.gguf"));   // alt text encoder naming

    const auto set = SDWorker::detectFlux2Set(dir.path());
    EXPECT_TRUE(set.valid());
    EXPECT_TRUE(set.diffusion.endsWith("flux-2-klein-base-4b.safetensors"));
    EXPECT_TRUE(set.vae.endsWith("flux2-vae.safetensors"));
    EXPECT_TRUE(set.llm.endsWith("mistral-small.gguf"));
}
