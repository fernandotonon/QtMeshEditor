#include <gtest/gtest.h>

#include "OnnxRuntimeSettings.h"

#include <QCoreApplication>
#include <QSettings>

namespace {
struct ScopedEnvUnset {
    const char* key;
    QByteArray saved;
    explicit ScopedEnvUnset(const char* k) : key(k)
    {
        saved = qgetenv(key);
        qunsetenv(key);
    }
    ~ScopedEnvUnset()
    {
        if (saved.isNull())
            qunsetenv(key);
        else
            qputenv(key, saved);
    }
};
}  // namespace

TEST(OnnxRuntimeSettings, PreferGpuPersists)
{
    ScopedEnvUnset guard("QTMESH_ONNX_PREFER_GPU");
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
