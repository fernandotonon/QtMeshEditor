#ifndef SDWORKER_H
#define SDWORKER_H

#include <QObject>
#include <QThread>
#include <QString>
#include <QMutex>
#include <QImage>
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

    // Mesh-aware texture generation (issue #403). When
    // `controlNetPath` is non-empty AND a control image is supplied
    // to `generateTextureControlled`, sd.cpp conditions generation
    // on that image (a rendered depth map) so the result follows
    // the mesh's silhouette + form. Empty path → plain txt2img
    // (the existing behavior).
    QString controlNetPath;       // path to the ControlNet model file
    float controlStrength = 0.9f; // 0..1, how strongly the depth map steers generation
};

class SDWorker : public QObject
{
    Q_OBJECT

public:
    explicit SDWorker(QObject *parent = nullptr);
    ~SDWorker();

    /// FLUX.2-klein component set (prompt-to-3D image generation): sd.cpp
    /// loads it from THREE files — the diffusion GGUF, the flux2 VAE, and
    /// the Qwen3 text encoder — instead of one checkpoint. `detectFlux2Set`
    /// scans a directory for them by name (diffusion: "flux"* .gguf/.safetensors
    /// that isn't the VAE; VAE: name containing "vae" or starting "ae.";
    /// text encoder: name containing "qwen" or "mistral"). Pure — compiled
    /// (and unit-tested) in every build.
    struct Flux2Set {
        QString diffusion;
        QString vae;
        QString llm;
        bool valid() const {
            return !diffusion.isEmpty() && !vae.isEmpty() && !llm.isEmpty();
        }
    };
    static Flux2Set detectFlux2Set(const QString &dir);

    /// Accepts either a single checkpoint FILE (the historical behavior) or
    /// a DIRECTORY holding a FLUX.2 component set (detectFlux2Set).
    bool loadModel(const QString &modelPath);
    bool isFlux2Loaded() const { return m_isFlux2; }
    void unloadModel();
    bool isModelLoaded() const;
    QString getLoadedModelPath() const { return m_modelPath; }

    void setSettings(const SDSettings &settings);
    SDSettings getSettings() const { return m_settings; }

    void requestStop();
    bool isGenerating() const { return m_isGenerating.load(); }

public slots:
    void generateTexture(const QString &prompt, const QString &outputPath);

    // Issue #403: depth-conditioned generation. `controlImage` is a
    // rendered depth map (RGB8, any size — resized to the generation
    // resolution). When empty, behaves exactly like generateTexture.
    // Uses `m_settings.controlNetPath` / `controlStrength`.
    void generateTextureControlled(const QString &prompt,
                                   const QImage &controlImage,
                                   const QString &outputPath);

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
    Flux2Set m_flux2;          ///< resolved component set when m_isFlux2
    bool m_isFlux2 = false;    ///< model loaded as a FLUX.2 component set
    SDSettings m_settings;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_isGenerating{false};
    std::atomic<bool> m_isModelLoaded{false};
    mutable QMutex m_mutex;

    void unloadModelInternal();

#ifdef ENABLE_STABLE_DIFFUSION
    sd_ctx_t *m_ctx = nullptr;

    void recreateContext();
    static void progressCallback(int step, int steps, float time, void *data);
#endif
};

#endif // SDWORKER_H
