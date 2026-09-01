#ifndef MESH_GEN_CONTROLLER_H
#define MESH_GEN_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QVariantMap>

#include "MeshGenPredictor.h"   // MeshGenPredictor::Quality

#include <atomic>

// QML controller for image-to-3D (epic #764): drives MeshGenPredictor on a WORKER
// thread so the UI stays responsive, reports staged progress, and supports cancel.
// Lives in the Object-mode "Mode Tools" panel (qml/PropertiesPanel.qml).
//
// Threading contract (important — Ogre is main-thread-only):
//   * The heavy work — U²-Net background removal + the TripoSR encoder/decoder
//     ONNX inference + marching cubes — is pure data (QImage in, float arrays out)
//     and runs on a worker thread.
//   * Mesh CONSTRUCTION (MeshGenBuilder → Ogre::Mesh/Entity/SceneNode) then runs
//     back on the MAIN thread (marshalled via a queued signal), because it touches
//     the Ogre scene.
// Progress is emitted from the predictor's per-chunk ProgressFn, marshalled to the
// GUI thread; cancel flips a shared atomic the ProgressFn checks.
class MeshGenController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // The currently-selected source image (empty until the user picks one). The
    // panel's "Generate" button binds its enabled state to this being non-empty.
    Q_PROPERTY(QString selectedImagePath READ selectedImagePath NOTIFY selectedImageChanged)
    // A small preview thumbnail of the selected image as a data:image/png;base64
    // URL the QML Image element can show directly (same idiom as the texture
    // packer previews). Empty when no image is selected.
    Q_PROPERTY(QString previewSource READ previewSource NOTIFY selectedImageChanged)
    // Auto-generated caption of the selected image (SmolVLM), computed on a
    // WORKER thread the moment an image is picked so it's ready by the time the
    // (slow) mesh generation finishes — the AI texture pass reuses it instead
    // of blocking the UI to caption. The panel shows it under the thumbnail.
    // Empty until captioning completes (or if the captioner is unavailable).
    Q_PROPERTY(QString caption READ caption NOTIFY captionChanged)
    Q_PROPERTY(bool captioning READ captioning NOTIFY captionChanged)

public:
    static MeshGenController* instance();
    static MeshGenController* create(QQmlEngine*, QJSEngine*);
    static void kill();

    bool available() const;         // ENABLE_ONNX build
    bool busy() const { return m_busy; }
    QString selectedImagePath() const { return m_selectedImage; }
    QString previewSource() const { return m_previewSource; }
    QString caption() const { return m_caption; }
    bool captioning() const { return m_captioning; }

    // Open a native file dialog to pick a source image; stores it as the selected
    // image and builds the preview thumbnail (does NOT start generation). Returns
    // immediately.
    Q_INVOKABLE void selectImage();

    // Generate from the currently-selected image (selectImage() first). No-op if
    // nothing selected or already busy. Emits progress → (completed | error).
    // `quality` is the encoder tier: 0=fp32 (best), 1=int8 (smallest).
    //
    // `options` are the user-selectable pipeline stages (all optional; defaults
    // in braces): {"smooth": true, "refine": true, "bake_texture": true,
    // "upscale_texture": false, "generate_pbr": true, "texture_size": 1024}.
    // The QML panel exposes them as checkboxes next to "Remove background".
    Q_INVOKABLE void generateSelected(int resolution = 256,
                                      bool removeBackground = true,
                                      int quality = 0,
                                      const QVariantMap& options = {});

    // Open a native file dialog to pick an image and start generation immediately.
    // Convenience one-shot (kept for callers/tests). Returns immediately.
    Q_INVOKABLE void pickImageAndGenerate(int resolution = 256,
                                          bool removeBackground = true,
                                          int quality = 0,
                                          const QVariantMap& options = {});

    // Start generation from an explicit image path on the worker thread. No-op if
    // already busy. Emits progress → (completed | error). Returns immediately.
    Q_INVOKABLE void generate(const QString& imagePath, int resolution = 256,
                              bool removeBackground = true, int quality = 0,
                              const QVariantMap& options = {});

    // Request cancellation of the in-flight run (flips the atomic the predictor's
    // progress callback checks). The run ends with meshGenError("cancelled").
    Q_INVOKABLE void cancel();

    // ── Pre-download support (AI Settings modal) ────────────────────────────
    // Whether the decoder + the given tier's encoder are already on disk.
    // TRELLIS.2 sidecar runtime probe (the backend combo preselects TRELLIS.2
    // and enables its option rows only when this is true) + the install hint
    // shown when it isn't. Invokables (not properties): the runtime can be
    // installed while the app runs, so QML re-queries on section open.
    Q_INVOKABLE bool trellis2Available() const;
    Q_INVOKABLE QString trellis2RuntimeHint() const;

    Q_INVOKABLE bool modelsPresent(int quality = 0) const;
    // Download the decoder + the given tier's encoder (blocks on the caller's
    // event loop, driven by ModelDownloader → its progress bar updates in the
    // dialog). No-op if already present. Emits modelDownloadFinished(ok) and sets
    // busy while running. For pre-fetching from the AI Settings modal.
    Q_INVOKABLE void downloadModels(int quality = 0);

signals:
    void busyChanged();
    void selectedImageChanged();
    // stage: "prep" | "background" | "encode" | "decode" | "surface" | "build"
    void progress(const QString& stage, int done, int total);
    void statusMessage(const QString& message);
    void completed(QVariantMap result);   // {vertexCount, triangleCount}
    void error(const QString& message);
    void modelDownloadFinished(bool ok);  // pre-download from AI Settings
    void captionChanged();                 // caption / captioning state updated

private:
    explicit MeshGenController(QObject* parent = nullptr);

    // Kick off SmolVLM captioning of `path` on a detached worker thread; the
    // result is marshalled back to the main thread (setCaptionResult).
    void startCaptioning(const QString& path);
    Q_INVOKABLE void setCaptionResult(const QString& caption, const QString& forPath);

    // Runs on the MAIN thread (queued from the worker) to build + attach the mesh.
    Q_INVOKABLE void buildOnMainThread();

    void setBusy(bool b);

    bool m_busy = false;
    std::atomic<bool> m_cancel{false};
    QString m_selectedImage;    // currently-selected source image path
    QString m_previewSource;    // data:image/png;base64 thumbnail of it
    QString m_caption;          // auto-caption of the selected image
    bool    m_captioning = false;
    // Guards against a stale caption landing after the image changed again:
    // startCaptioning stamps the path, setCaptionResult drops mismatches.
    QString m_captionForPath;
    MeshGenPredictor::Quality m_quality = MeshGenPredictor::Quality::Fp32;
    // Parsed pipeline options for the in-flight run (see generateSelected doc).
    bool m_generatePbr = true;          // consumed by buildOnMainThread
    bool m_upscaleTexture = false;      // consumed by the worker (post-predict)
    QString m_upscaleModelPath;         // ensured on the main thread pre-worker
    static MeshGenController* s_instance;

    // Map a QML int (0/1/2) to the encoder tier.
    static MeshGenPredictor::Quality qualityFromInt(int q);

    // Worker→main handoff: the predicted arrays live in a pimpl-ish holder so this
    // header stays free of MeshGenPredictor.
    struct Pending;
    Pending* m_pending = nullptr;
};

#endif // MESH_GEN_CONTROLLER_H
