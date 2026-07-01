#ifndef MESH_GEN_CONTROLLER_H
#define MESH_GEN_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QVariantMap>

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

public:
    static MeshGenController* instance();
    static MeshGenController* create(QQmlEngine*, QJSEngine*);

    bool available() const;         // ENABLE_ONNX build
    bool busy() const { return m_busy; }

    // Open a native file dialog to pick an image and start generation. Convenience
    // for the panel's "Generate from Image…" button. Returns immediately.
    Q_INVOKABLE void pickImageAndGenerate(int resolution = 256,
                                          bool removeBackground = true);

    // Start generation from an explicit image path on the worker thread. No-op if
    // already busy. Emits progress → (completed | error). Returns immediately.
    Q_INVOKABLE void generate(const QString& imagePath, int resolution = 256,
                              bool removeBackground = true);

    // Request cancellation of the in-flight run (flips the atomic the predictor's
    // progress callback checks). The run ends with meshGenError("cancelled").
    Q_INVOKABLE void cancel();

signals:
    void busyChanged();
    // stage: "prep" | "background" | "encode" | "decode" | "surface" | "build"
    void progress(const QString& stage, int done, int total);
    void statusMessage(const QString& message);
    void completed(QVariantMap result);   // {vertexCount, triangleCount}
    void error(const QString& message);

private:
    explicit MeshGenController(QObject* parent = nullptr);

    // Runs on the MAIN thread (queued from the worker) to build + attach the mesh.
    Q_INVOKABLE void buildOnMainThread();

    void setBusy(bool b);

    bool m_busy = false;
    std::atomic<bool> m_cancel{false};
    static MeshGenController* s_instance;

    // Worker→main handoff: the predicted arrays live in a pimpl-ish holder so this
    // header stays free of MeshGenPredictor.
    struct Pending;
    Pending* m_pending = nullptr;
};

#endif // MESH_GEN_CONTROLLER_H
