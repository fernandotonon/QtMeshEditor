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
    Q_PROPERTY(QString modelPath READ modelPath NOTIFY modelReadyChanged)

public:
    static AIAssistManager* instance();
    static AIAssistManager* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);

    /// True only when the binary was compiled with ENABLE_ONNX.
    Q_INVOKABLE bool isAvailable() const;
    /// True when the PBR model file exists on disk.
    Q_INVOKABLE bool isModelReady() const;
    /// Absolute path the PBR model is expected at (under AppData/ai_models/pbr).
    QString modelPath() const;

    /// Kick off a background download of the PBR model if it is missing. No-op
    /// if already present or no URL is configured. GUI prefetch hook.
    Q_INVOKABLE void ensureModel();

    /// Synthesize PBR maps from an albedo image on disk, writing the requested
    /// maps as PNGs next to the source (<stem>_normal.png / _roughness.png /
    /// _height.png). Synchronous. Returns the output paths + an ok flag.
    PbrMapSynthResult synthesizePbrMaps(const QString& albedoPath,
                                        const PbrMapSynth::Options& opts = {});

    /// QML-friendly wrapper: opts/result as QVariantMap.
    Q_INVOKABLE QVariantMap synthesizePbrMapsQml(const QString& albedoPath,
                                                 const QVariantMap& opts = {});

signals:
    void modelReadyChanged();
    void modelDownloadProgress(qint64 received, qint64 total);
    void synthesisStarted();
    void synthesisCompleted(QVariantMap result);
    void synthesisError(const QString& error);

private:
    explicit AIAssistManager(QObject* parent = nullptr);

    QString defaultModelUrl() const;   // configurable; QSettings override
    static AIAssistManager* s_instance;
};

#endif // AIASSISTMANAGER_H
