#ifndef AIASSISTMANAGER_H
#define AIASSISTMANAGER_H

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QQmlEngine>
#include <QJSEngine>

#include "PbrMapSynth.h"

/// Result of AIAssistManager::synthesizePbrMaps — the on-disk output paths.
struct PbrMapSynthResult {
    bool ok = false;
    QString error;
    QString normalPath;
    QString roughnessPath;
    QString heightPath;
    bool fromCache = false;

    QVariantMap toVariantMap() const {
        return {
            {"ok", ok}, {"error", error},
            {"normalPath", normalPath}, {"roughnessPath", roughnessPath},
            {"heightPath", heightPath}, {"fromCache", fromCache}
        };
    }
};

/// #404: facade for AI-assisted authoring that runs on ONNX Runtime.
///
/// First (and currently only) capability: PBR map synthesis from a single
/// albedo/diffuse texture (normal + height from a DeepBump-style UNet, roughness
/// from an albedo-luminance heuristic). Mirrors the SDManager / LLMManager
/// QML_SINGLETON pattern. ONNX inference is fast and synchronous, so there is no
/// worker thread — `synthesizePbrMaps()` blocks and returns a result; progress
/// signals are still emitted for the GUI.
///
/// The production model is downloaded on first use via ModelDownloader from a
/// configurable permissive URL; everything fails gracefully (ok=false + error)
/// when the model is missing/offline or the binary was built without
/// ENABLE_ONNX.
class AIAssistManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool available READ isAvailable CONSTANT)
    Q_PROPERTY(bool modelReady READ isModelReady NOTIFY modelReadyChanged)

public:
    static AIAssistManager* instance();
    static AIAssistManager* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);

    /// Per-model ONNX files (each a separate download). PBRify maps (#404, CC0)
    /// + Real-ESRGAN upscalers (#405, BSD-3).
    enum class Map { Normal, Roughness, Height, UpscaleX2, UpscaleX4 };

    /// True only when the binary was compiled with ENABLE_ONNX.
    Q_INVOKABLE bool isAvailable() const;
    /// True when ALL configured per-map model files exist on disk.
    Q_INVOKABLE bool isModelReady() const;
    /// Absolute path a given map's model is expected at (AppData/ai_models/pbr).
    QString modelPath(Map map) const;

    /// Kick off background downloads of any missing per-map models. No-op for
    /// maps already present or with no URL configured. GUI prefetch hook.
    Q_INVOKABLE void ensureModel();

    /// Synthesize PBR maps from an albedo image on disk, writing the requested
    /// maps as PNGs next to the source (<stem>_normal.png / _roughness.png /
    /// _height.png). Synchronous. Returns the output paths + an ok flag.
    PbrMapSynthResult synthesizePbrMaps(const QString& albedoPath,
                                        const PbrMapSynth::Options& opts = {});

    /// QML-friendly wrapper: opts/result as QVariantMap.
    Q_INVOKABLE QVariantMap synthesizePbrMapsQml(const QString& albedoPath,
                                                 const QVariantMap& opts = {});

    // ── #405: Real-ESRGAN texture upscaling ─────────────────────────────────
    /// Upscale the texture at `srcPath` by `scale` (2 or 4) via the BSD-3
    /// Real-ESRGAN ONNX model (downloaded on first use). Writes
    /// `<stem>_upscaled.png` next to the source and returns its path.
    /// Synchronous; emits upscaleStarted/Completed/Error. Cached: an existing
    /// output is reused unless `overwrite`.
    Q_INVOKABLE QString upscaleTexture(const QString& srcPath, int scale = 4,
                                       bool overwrite = false);

    /// Ensure the 2×/4× upscale model is present (download + block) — MUST be
    /// called on a thread with an event loop (the GUI thread). Lets the
    /// GUI fetch the model first, then run the pure-CPU inference on a worker.
    /// Returns the model path, or empty if it couldn't be made available.
    QString ensureUpscaleModel(int scale);

    // ── #764: image-to-3D mesh generation (TripoSR) ─────────────────────────
    /// Generate a 3D mesh from a single image via the TripoSR ONNX models
    /// (downloaded on first use). Builds the mesh, loads it into the scene, and
    /// (when `outputPath` is non-empty) exports it. Synchronous; emits
    /// meshGenStarted / meshGenCompleted / meshGenError. Returns a QVariantMap
    /// {ok, error, vertexCount, triangleCount, meshPath}. `resolution` is the
    /// marching-cubes grid resolution (16..512).
    Q_INVOKABLE QVariantMap generateMeshFromImage(const QString& imagePath,
                                                  int resolution = 256,
                                                  bool vertexColor = true,
                                                  const QString& outputPath = {});

signals:
    void modelReadyChanged();
    void modelDownloadProgress(qint64 received, qint64 total);
    void synthesisStarted();
    void synthesisCompleted(QVariantMap result);
    void synthesisError(const QString& error);
    void upscaleStarted();
    void upscaleCompleted(const QString& outputPath);
    void upscaleError(const QString& error);
    void meshGenStarted();
    void meshGenCompleted(QVariantMap result);
    void meshGenError(const QString& error);

private:
    explicit AIAssistManager(QObject* parent = nullptr);

    QString defaultModelUrl(Map map) const;   // configurable; QSettings override
    static QString mapModelFile(Map map);     // bare .onnx filename per map
    // Download `map`'s model if absent and BLOCK until it lands (local event
    // loop on ModelDownloader). Returns true if the file is present afterward.
    // Used by the synchronous synthesize path so first-run auto-downloads work
    // from CLI/MCP/GUI alike.
    bool ensureModelBlocking(Map map);
    static AIAssistManager* s_instance;
};

#endif // AIASSISTMANAGER_H
