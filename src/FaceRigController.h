#ifndef FACE_RIG_CONTROLLER_H
#define FACE_RIG_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QVariantMap>

#include <atomic>
#include <memory>

// QML-facing singleton for the face auto-rig (#889, Slice F #895). Wraps
// FaceRig::attachFaceRig* plus selection state so the Inspector's "Add ARKit
// Blendshapes" button can enable itself only on a mesh selection and run the
// (heavy) fit on a worker thread while the UI stays responsive.
//
// The pipeline is split across threads: geometry extraction + the pose attach
// touch Ogre and run on the MAIN thread; the heavy Ogre-free buildFaceRig()
// (NRICP + deformation transfer over 52 shapes) runs on a WORKER. The attach
// goes through AddMorphTargetCommand so it is undoable, and it lands the
// shapes in the same Ogre::Pose + VAT_POSE form the Vertex Morph section reads,
// so the #869 face-capture panel drives them immediately.
class FaceRigController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool hasMeshSelection READ hasMeshSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // True while the bundled ARKit template is downloading on first use.
    Q_PROPERTY(bool downloading READ downloading NOTIFY statusChanged)
    // Short human-readable status for the button label / tooltip.
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    // Worker progress for a progress bar: done / total steps (0 total = idle).
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(int progressTotal READ progressTotal NOTIFY progressChanged)

public:
    static FaceRigController* instance();
    static FaceRigController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool hasMeshSelection() const;
    bool busy() const { return m_busy; }
    bool downloading() const { return m_downloading; }
    QString status() const { return m_status; }
    int progress() const { return m_progress; }
    int progressTotal() const { return m_progressTotal; }

    /// Run the face auto-rig on the first selected entity and attach the ARKit
    /// blendshapes. Prepares (extracts geometry + loads the bundled template)
    /// on the main thread, runs the fit on a WORKER, commits the undoable
    /// attach back on the main thread. Returns false when it could not start
    /// (invalid selection / already busy — `error` is emitted); the real
    /// outcome arrives via completed(report) or error(msg).
    Q_INVOKABLE bool addArkitBlendshapesAsync(int maxShapes = 0,
                                              double maxResidualPct = 8.0);

    /// Request cancellation of an in-flight fit (no-op otherwise). The worker
    /// stops at the next progress step; `error("cancelled")` follows.
    Q_INVOKABLE void cancel();

signals:
    void selectionChanged();
    void busyChanged();
    void statusChanged();
    void progressChanged();
    void completed(const QVariantMap& report);
    void error(const QString& message);

private:
    FaceRigController();
    ~FaceRigController() override = default;

    void setStatus(const QString& s);

    static FaceRigController* m_pSingleton;
    bool m_busy = false;
    bool m_downloading = false;
    QString m_status;
    int m_progress = 0;
    int m_progressTotal = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
};

#endif // FACE_RIG_CONTROLLER_H
