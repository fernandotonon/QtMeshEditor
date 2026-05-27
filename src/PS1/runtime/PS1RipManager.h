#ifndef PS1RIPMANAGER_H
#define PS1RIPMANAGER_H

#include "Gp0CaptureStats.h"
#include "Ps1CoordinateNormalizer.h"

#include <QImage>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVector>

class PS1RipWorker;
class QTimer;

enum class PsxVramMirrorMode;

/**
 * Main-thread coordinator for PS1 runtime geometry extraction (epic #412).
 */
class PS1RipManager : public QObject
{
    Q_OBJECT

public:
    static PS1RipManager *getSingleton();
    static PS1RipManager *getSingletonPtr();
    static void kill();

    bool hasIso() const { return !m_isoPath.isEmpty(); }
    bool hasBios() const { return !m_biosPath.isEmpty(); }
    bool isSessionActive() const { return m_sessionActive; }
    bool isStartPending() const { return m_startPending; }
    bool isPaused() const { return m_paused; }
    bool isCaptureArmed() const { return m_captureArmed; }
    QString isoPath() const { return m_isoPath; }
    QString biosPath() const { return m_biosPath; }
    QString activeCoreId() const { return m_activeCoreId; }

    bool loadBios(const QString &path);
    bool loadIso(const QString &path);
    bool reloadIso();
    bool start();
    bool stop();
    bool pause();
    bool step();
    bool armCapture(bool armed = true);
    bool captureFrame();
    /** Multi-frame capture (#425). Auto-arms if not already armed, accumulates
     *  every frame's primitive stream for `seconds` real-time seconds, then
     *  finalises into a single deduped capture. Emits
     *  `sceneCaptureStarted/Progress/Finished` so the UI can show a countdown
     *  + live counters. Cancellable via `stopSceneCapture()` (or by stopping
     *  the session / explicitly disarming). */
    bool captureScene(int seconds);
    /** Cancel a running scene capture without disarming. No-op if no scene
     *  capture is in flight. Emits `sceneCaptureFinished(true, "")`. */
    bool stopSceneCapture();
    bool dumpVRAM();

    /** True from `captureScene()` until either cancellation or the worker
     *  delivers the finalised snapshot. Note this is **wider** than
     *  `m_sceneCaptureRemaining > 0` — once the countdown hits zero we're
     *  still in a scene capture while the worker's queued
     *  `finalizeFrameCapture` completes and `frameCaptureReady` round-trips
     *  back to the GUI thread. Codex P1 / CodeRabbit Major on #677: gating
     *  only on `m_sceneCaptureRemaining` here let a Stop Capture click in
     *  the finalize window disarm the worker before it processed the queued
     *  finalize, so the worker bailed out with "Capture is not armed" and
     *  no `sceneCaptureFinished` was ever emitted. */
    bool isSceneCaptureActive() const
    {
        return m_sceneCaptureRemaining > 0 || m_sceneCaptureAwaitingResult;
    }
    int sceneCaptureSecondsRemaining() const { return m_sceneCaptureRemaining; }
    int sceneCaptureSecondsTotal() const { return m_sceneCaptureTotal; }

    bool dedupeStrict() const { return m_dedupeStrict; }
    void setDedupeStrict(bool strict) { m_dedupeStrict = strict; }

    /** Coordinate normalization knobs (#424). userScale + per-axis flips are
     *  applied as SceneNode scale at attach time and re-applied to existing
     *  capture nodes on every setter call, so the user sees changes live
     *  without re-capturing. Perspective-correct UVs is consumed by the next
     *  mesh build (it bakes into the mesh data — can't be a SceneNode toggle). */
    const Ps1NormalizerSettings &normalizerSettings() const { return m_normalize; }
    void setNormalizerSettings(const Ps1NormalizerSettings &settings);

    QString goldenSceneId() const;
    void setGoldenSceneId(const QString &sceneId);

    void setJoypadPressed(unsigned port, unsigned buttonId, bool pressed);
    void resetJoypad(unsigned port = 0);

    PS1RipWorker *worker() const { return m_worker; }

signals:
    void sessionStarted(const QString &coreId);
    void vramFrameUpdated(const QVector<uint16_t> &cells, const QImage &nativePreview);
    void sessionStopped();
    void framePresented(const QImage &frame, quint64 frameIndex);
    void frameCaptured(const QString &captureId);
    void meshBuilt(const QString &captureId, int capturedParts, int uniqueMeshes, int instanceCount,
                   int vertexCount, int triangleCount, int matrixCount, uint32_t cameraMatrixId,
                   bool hasCameraMatrix, int gteInversePercent, bool slabLike,
                   int primsWithMatrixId, int primsTotal, PsxVramMirrorMode vramMirrorMode,
                   Gp0CaptureStats captureStats);
    void sceneCaptured(const QString &captureId);
    /** Fires when `captureScene()` accepts a duration and starts the countdown
     *  (#425). UI uses this to flip into "scene capture in flight" mode. */
    void sceneCaptureStarted(int totalSeconds);
    /** 1 Hz countdown tick from a running scene capture (#425). */
    void sceneCaptureProgress(int remainingSeconds, int totalSeconds);
    /** Fires after a scene capture finishes — either via the timer completing
     *  (`cancelled=false`, `captureId` from the finalised buffer) or via
     *  `stopSceneCapture()` / `stop()` / `armCapture(false)` (#425). */
    void sceneCaptureFinished(bool cancelled, const QString &captureId);
    /** Live capture-buffer stats while armed, rate-limited from the worker so
     *  the UI's status footer (#425) doesn't churn every emulated frame. */
    void captureProgress(qint64 primitives, qint64 triangles, int texturePages,
                         qint64 bytesEstimate);
    void vramDumped(const QString &captureId, const QString &pngPath, const QVector<uint16_t> &cells,
                    const QImage &nativePreview);
    void error(const QString &message);
    void pausedChanged(bool paused);

private slots:
    void onSceneCaptureTick();

private:
    explicit PS1RipManager(QObject *parent = nullptr);
    ~PS1RipManager() override;

    void initializeWorkerThread();
    void shutdownWorkerThread();
    void reportError(const QString &message);
    void syncWorkerSession();
    void syncWorkerCaptureArmed();
    /** Forwards the worker thread's capture-buffer counters to the GUI thread. */
    void forwardCaptureProgress(qint64 prims, qint64 triangles, int texturePages,
                                qint64 bytesEstimate);
    /** Resets scene-capture state and emits sceneCaptureFinished. Idempotent. */
    void finalizeSceneCapture(bool cancelled, const QString &captureId);

    static PS1RipManager *s_instance;

    QString m_biosPath;
    QString m_isoPath;
    QString m_activeCoreId;
    bool m_sessionActive = false;
    bool m_startPending = false;
    bool m_paused = false;
    bool m_captureArmed = false;
    bool m_dedupeStrict = false;
    Ps1NormalizerSettings m_normalize;
    QString m_goldenSceneId;
    // Scene-capture countdown state (#425). `m_sceneCaptureRemaining > 0`
    // means a scene capture is in flight; the timer (1 Hz) decrements until 0,
    // then triggers a worker finalize via the same queued-invoke path as the
    // single-shot `captureFrame()`.
    QTimer *m_sceneCaptureTimer = nullptr;
    int m_sceneCaptureRemaining = 0;
    int m_sceneCaptureTotal = 0;
    /** When true, the next `frameCaptureReady` from the worker should be
     *  attributed to the scene-capture path (Sentry category + completion
     *  signal). Cleared in the handler. */
    bool m_sceneCaptureAwaitingResult = false;

    QThread *m_workerThread = nullptr;
    PS1RipWorker *m_worker = nullptr;
};

#endif // PS1RIPMANAGER_H
