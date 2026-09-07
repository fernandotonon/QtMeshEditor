#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "SDWorker.h"

// SDWorker::detectFlux2Set is pure file-set detection (prompt-to-3D image
// generation, FLUX.2-klein-4B): it must find the diffusion GGUF + VAE + text
// encoder trio in a directory by name, in every build (no ENABLE_STABLE_
// DIFFUSION guard — the AI Model Settings catalog and SDManager's model list
// rely on it even before generation is possible).

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
