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
    // FLUX.2-klein is guidance-distilled: it IGNORES cfg (pinned to 1.0) and
    // needs far fewer steps than an SD checkpoint, so `steps` above (an
    // SD-oriented knob that auto-detect writes 12/30 into) must not reach it.
    // 4 is the distillation's documented MINIMUM, not a ceiling — anatomy
    // (limb/finger counts) is what resolves with the extra steps, so the
    // default is 8. Clamped to [4,20] in SDWorker: below 4 the model has not
    // converged, and beyond ~20 a distilled model stops improving.
    int flux2Steps = 8;
    // FLUX.3 Image Klein (distilled) is documented by stable-diffusion.cpp at
    // 16 steps / cfg 1.0 / euler — twice the FLUX.2 default. Clamped to
    // [8,30] in SDWorker.
    int flux3Steps = 16;
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

    /// True when a resolved diffusion file belongs to the FLUX.3 family
    /// ("flux3-image-klein-Q4_0.gguf", "flux-3-image-klein.safetensors"). The
    /// FLUX.3 set loads through the same three params as FLUX.2 (diffusion +
    /// VAE + LLM), so the only per-family difference is the step default.
    /// Pure — unit-tested in every build.
    static bool isFlux3Diffusion(const QString &diffusionPath);

    /// FLUX.3 is generated OUT OF PROCESS (SdCliHelper / `qtmesh_sdcli`, built
    /// from a newer stable-diffusion.cpp with its own ggml): the in-process
    /// sd.cpp shares llama.cpp's ggml and aborts on newer sd.cpp builds. So
    /// FLUX.3 is only offered when that helper is installed next to the editor.
    static bool flux3Available();

    /// Accepts either a single checkpoint FILE (the historical behavior) or
    /// a DIRECTORY holding a FLUX.2 component set (detectFlux2Set).
    bool loadModel(const QString &modelPath);
    bool isFlux2Loaded() const { return m_isFlux2; }
    void unloadModel();
    bool isModelLoaded() const;
    QString getLoadedModelPath() const { return m_modelPath; }

    void setSettings(const SDSettings &settings);
    SDSettings getSettings() const { return m_settings; }

    /// FLUX.2 image EDITING (kontext-style): the next generation call uses
    /// `image` as a reference image, so the prompt describes a CHANGE to it
    /// ("give him golden armor") instead of a scene from scratch. Consumed by
    /// (and cleared after) the next generateTexture*/generateImage run; only
    /// honoured when a FLUX.2 set is loaded (SD-class checkpoints have no
    /// reference conditioning here).
    void setRefImage(const QImage &image);

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
    bool m_isFlux2 = false;    ///< model loaded as a FLUX.2/3 component set
    bool m_isFlux3 = false;    ///< ...and that set is the FLUX.3 family
    bool m_helperMode = false; ///< FLUX.3: no in-process context, generate via SdCliHelper
    QImage m_refImage;         ///< one-shot reference image (setRefImage)
    SDSettings m_settings;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_isGenerating{false};
    std::atomic<bool> m_isModelLoaded{false};
    mutable QMutex m_mutex;

    void unloadModelInternal();
    void generateWithHelper(const QString &prompt, const QImage &controlImage,
                            const QString &outputPath);

#ifdef ENABLE_STABLE_DIFFUSION
    sd_ctx_t *m_ctx = nullptr;

    void recreateContext();
    static void progressCallback(int step, int steps, float time, void *data);
#endif
};

#endif // SDWORKER_H
