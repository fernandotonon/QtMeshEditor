#include <gtest/gtest.h>

#include "OnnxRuntimeSettings.h"

#include <QCoreApplication>
#include <QSettings>

TEST(OnnxRuntimeSettings, PreferGpuPersists)
{
    QSettings settings;
    settings.remove("ai/onnxPreferGpu");

    OnnxRuntimeSettings* ort = OnnxRuntimeSettings::instance();
    ort->loadSettings();

    const bool initial = ort->preferGpu();
    ort->setPreferGpu(!initial);
    EXPECT_EQ(ort->preferGpu(), !initial);

    OnnxRuntimeSettings* reloaded = OnnxRuntimeSettings::instance();
    reloaded->loadSettings();
    EXPECT_EQ(reloaded->preferGpu(), !initial);

    ort->setPreferGpu(initial);
}

TEST(OnnxRuntimeSettings, GpuProviderNoteNonEmpty)
{
    OnnxRuntimeSettings::instance()->refreshGpuProviderStatus();
    const QString note = OnnxRuntimeSettings::instance()->gpuProviderNote();
    EXPECT_FALSE(note.isEmpty());
}
