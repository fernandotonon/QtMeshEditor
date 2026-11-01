#pragma once

// QML_SINGLETON behind the Animation panel's text-to-motion block: NVIDIA Kimodo (the default) or the smaller,
// experimental QtMesh T2M model (`backend`, persisted as ai/textToMotionBackend). Generation (native runtime or the Python sidecar) runs on a WORKER thread (minutes of
// diffusion must not freeze the editor); the result is applied on the main
// thread (Ogre and the undo stack live there) as one clip per variation, one
// undo step for the batch. A saved motion (motion_NN.npz) can be re-applied
// to any rig with no Python at all.

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>

class QThread;

namespace Ogre { class Entity; }

namespace AIMotion {

class KimodoController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString stage READ stage NOTIFY progressChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)   ///< 0..1, or -1 = indeterminate
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool lastOk READ lastOk NOTIFY statusChanged)
    Q_PROPERTY(bool runtimeAvailable READ runtimeAvailable NOTIFY runtimeChanged)
    Q_PROPERTY(QString runtimeDescription READ runtimeDescription NOTIFY runtimeChanged)
    Q_PROPERTY(QStringList lastClips READ lastClips NOTIFY statusChanged)
    Q_PROPERTY(QString lastMotionDir READ lastMotionDir NOTIFY statusChanged)
    /// The native (Python-free) runtime is built in but its models are not downloaded yet.
    Q_PROPERTY(bool nativeDownloadable READ nativeDownloadable NOTIFY runtimeChanged)
    Q_PROPERTY(QString nativeDownloadSize READ nativeDownloadSize NOTIFY runtimeChanged)
    /// "kimodo" (default) | "qtmesh_t2m".
    Q_PROPERTY(QString backend READ backend WRITE setBackend NOTIFY backendChanged)
    Q_PROPERTY(bool t2mSupported READ t2mSupported CONSTANT)
    Q_PROPERTY(bool t2mReady READ t2mReady NOTIFY runtimeChanged)
    Q_PROPERTY(QString t2mDownloadSize READ t2mDownloadSize NOTIFY runtimeChanged)

public:
    static KimodoController* instance();
    static KimodoController* qmlInstance(QQmlEngine*, QJSEngine*);
    static void kill();

    bool busy() const { return m_busy; }
    QString stage() const { return m_stage; }
    double progress() const { return m_progress; }
    QString status() const { return m_status; }
    bool lastOk() const { return m_lastOk; }
    bool runtimeAvailable() const { return m_runtimeAvailable; }
    QString runtimeDescription() const { return m_runtimeDescription; }
    QStringList lastClips() const { return m_lastClips; }
    QString lastMotionDir() const { return m_lastMotionDir; }
    bool nativeDownloadable() const;
    QString nativeDownloadSize() const;
    QString backend() const { return m_backend; }
    void setBackend(const QString& b);
    bool t2mSupported() const;
    bool t2mReady() const;
    QString t2mDownloadSize() const;
    static QStringList backends() { return {QStringLiteral("kimodo"), QStringLiteral("qtmesh_t2m")}; }

    /// Generate on the selected rig. `prompt` is one prompt, or one segment per
    /// line ("walk forward @3" — a trailing @SECONDS sets that line's duration,
    /// otherwise `duration` is used). `seed` < 0 = random. Returns false when
    /// it could not start (the reason is in `status`).
    Q_INVOKABLE bool generate(const QString& prompt, double duration, int variations, int seed,
                              bool rootMotion, bool footPin);
    /// Download the QtMesh T2M model (~740 MB, explicit request; main thread, like downloadNativeModels).
    Q_INVOKABLE void downloadT2MModels();
    Q_INVOKABLE void cancel();
    /// Pick saved motion_NN.npz file(s) and apply them to the selected rig with the panel's settings.
    Q_INVOKABLE bool applyMotionFile(bool rootMotion = true, bool footPin = true);
    /// Non-dialog variant (tests / scripting).
    bool applyMotionFiles(const QStringList& files, bool rootMotion = true, bool footPin = true);
    Q_INVOKABLE void openMotionFolder();
    Q_INVOKABLE void refreshRuntime();
    /// Download the native Kimodo models (~9 GB, an explicit user request). Runs on the main thread through
    /// ModelFetch's own event loop (the downloader lives there), so the window keeps painting; Cancel stops it.
    Q_INVOKABLE void downloadNativeModels();

    /// Split the panel's prompt into segments (pure; exposed for tests).
    /// Returns false with `error` set on an empty prompt or a bad duration.
    static bool parsePromptSegments(const QString& prompt, double defaultDuration,
                                    QStringList* texts, QList<double>* durations, QString* error);

signals:
    void busyChanged();
    void progressChanged();
    void statusChanged();
    void runtimeChanged();
    void backendChanged();
    /// Clips were added to `entity` (the panel refreshes its list and selects the first).
    void applied(const QString& entity, const QStringList& clips);

private:
    friend struct std::default_delete<KimodoController>;   // the singleton holder releases it in kill()
    explicit KimodoController(QObject* parent = nullptr);
    ~KimodoController() override;

    Ogre::Entity* targetEntity() const;
    Ogre::Entity* entityByName(const std::string& name) const;
    void setStatus(const QString& s, bool ok);
    void setBusy(bool b);
    bool applyFiles(Ogre::Entity* entity, const QStringList& files, bool rootMotion, bool footPin);
    bool generateT2M(Ogre::Entity* entity, const QString& prompt, double duration, int variations, int seed, bool rootMotion,
                     bool footPin);

    bool m_busy = false;
    QString m_stage;
    double m_progress = -1.0;
    QString m_status;
    bool m_lastOk = true;
    bool m_runtimeAvailable = false;
    QString m_runtimeDescription;
    QStringList m_lastClips;
    QString m_lastMotionDir;
    QString m_backend;
    QThread* m_worker = nullptr;
    std::atomic<bool> m_cancel{false};
};

} // namespace AIMotion
