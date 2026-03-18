#ifndef SDWORKER_H
#define SDWORKER_H

#include <QObject>
#include <QThread>
#include <QString>
#include <QMutex>
#include <atomic>

#ifdef ENABLE_STABLE_DIFFUSION
#include "stable-diffusion.h"
#endif

struct SDSettings {
    int width = 512;
    int height = 512;
    int steps = 30;
    float cfgScale = 7.0f;
    int64_t seed = -1; // -1 = random
    QString negativePrompt = "blurry, low quality, distorted, simple, cartoon";
    int sampleMethod = 0; // 0 = Euler A
    int threads = 0; // 0 = auto
    int gpuLayers = 99;
};

class SDWorker : public QObject
{
    Q_OBJECT

public:
    explicit SDWorker(QObject *parent = nullptr);
    ~SDWorker();

    bool loadModel(const QString &modelPath);
    void unloadModel();
    bool isModelLoaded() const;
    QString getLoadedModelPath() const { return m_modelPath; }

    void setSettings(const SDSettings &settings);
    SDSettings getSettings() const { return m_settings; }

    void requestStop();
    bool isGenerating() const { return m_isGenerating.load(); }

public slots:
    void generateTexture(const QString &prompt, const QString &outputPath);
    void generateFromImage(const QString &prompt, const QString &inputImagePath, const QString &outputPath, float strength);

signals:
    void modelLoaded(const QString &modelPath);
    void modelLoadError(const QString &error);
    void modelUnloaded();

    void generationStarted();
    void generationProgress(int step, int totalSteps);
    void generationCompleted(const QString &outputPath);
    void generationError(const QString &error);
    void generationStopped();

private:
    QString m_modelPath;
    SDSettings m_settings;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_isGenerating{false};
    std::atomic<bool> m_isModelLoaded{false};
    mutable QMutex m_mutex;

    void unloadModelInternal();

#ifdef ENABLE_STABLE_DIFFUSION
    sd_ctx_t *m_ctx = nullptr;

    static void progressCallback(int step, int steps, float time, void *data);
#endif
};

#endif // SDWORKER_H
