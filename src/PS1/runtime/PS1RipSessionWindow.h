#ifndef PS1RIPSESSIONWINDOW_H
#define PS1RIPSESSIONWINDOW_H

#include "Gp0CaptureStats.h"

#include <QImage>
#include <QMainWindow>
#include <QVector>

class EmuViewport;
class QAction;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QShortcut;
class QSpinBox;
class PS1RipGamepadBridge;
class PS1RipManager;
class QMenu;
class VramViewerWidget;
struct Ps1NormalizerSettings;

enum class PsxVramMirrorMode;

/** Temporary host for emulator viewport + transport (#416 / #417). */
class PS1RipSessionWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit PS1RipSessionWindow(QWidget *parent = nullptr);
    ~PS1RipSessionWindow() override;

    static void showSession(QWidget *parent);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void pickBios();
    void pickIso();
    void openRecentIso();
    void onReloadIso();
    void onStart();
    void onStop();
    void onPause();
    void onStep();
    void onReset();
    void onOpenInputSettings();
    void onFrame(const QImage &frame, quint64 frameIndex);
    void onError(const QString &message);
    void onDumpVram();
    void onCaptureFrame();
    /** Pull the duration spinbox value, ask the manager to start a scene
     *  capture, persist the duration to QSettings (#425). */
    void onCaptureScene();
    /** Cancel an in-flight scene capture without disarming the regular
     *  Capture button (#425). */
    void onStopCapture();
    void onSceneCaptureStarted(int totalSeconds);
    void onSceneCaptureProgress(int remainingSeconds, int totalSeconds);
    void onSceneCaptureFinished(bool cancelled, const QString &captureId);
    /** Live capture-buffer stats forwarded from the worker (#425). */
    void onCaptureProgress(qint64 primitives, qint64 triangles, int texturePages,
                           qint64 bytesEstimate);
    void onVramDumped(const QString &captureId, const QString &pngPath, const QVector<uint16_t> &cells,
                      const QImage &nativePreview);
    void onMeshBuilt(const QString &captureId, int capturedParts, int uniqueMeshes, int instanceCount,
                     int vertexCount, int triangleCount, int matrixCount, uint32_t cameraMatrixId,
                     bool hasCameraMatrix, int gteInversePercent, bool slabLike,
                     int primsWithMatrixId, int primsTotal, PsxVramMirrorMode vramMirrorMode,
                     Gp0CaptureStats captureStats);
    void onPausedChanged(bool paused);

private:
    void updateFps(quint64 frameIndex);
    void addRecentIso(const QString &path);
    void applyIsoPath(const QString &path);
    void rebuildRecentIsoMenu();
    /** Builds the right-side "Normalize" dock (capture scale, per-axis flip,
     *  perspective-correct UVs). Each control persists to QSettings under
     *  `ps1Rip/normalize/*` and pushes the new value to PS1RipManager so the
     *  user sees the change live without re-capturing (#424). */
    void createNormalizerDock();
    /** Snapshot the current dock widget values into a settings struct and
     *  forward to the manager + QSettings. */
    void pushNormalizerSettings();
    /** Rebuild the rightmost status footer chunk (#425): when armed, it shows
     *  live capture stats; when a scene capture is in flight it shows the
     *  remaining countdown; when idle it stays empty. Stored values come from
     *  `onCaptureProgress` and `onSceneCaptureProgress`. */
    void refreshCaptureStatusFooter();

    EmuViewport *m_viewport = nullptr;
    VramViewerWidget *m_vramViewer = nullptr;
    QLabel *m_statusLabel = nullptr;
    /** Permanent right-side footer carrying the live capture-buffer stats and
     *  scene-capture countdown (#425). Separate from `m_statusLabel` so the
     *  primary mesh-built / session-state message isn't overwritten by every
     *  4 Hz progress update. */
    QLabel *m_captureFooterLabel = nullptr;
    QMenu *m_recentIsoMenu = nullptr;
    PS1RipGamepadBridge *m_gamepadBridge = nullptr;
    PS1RipManager *m_manager = nullptr;
    QDoubleSpinBox *m_normalizeScaleSpin = nullptr;
    QCheckBox *m_normalizeFlipX = nullptr;
    QCheckBox *m_normalizeFlipY = nullptr;
    QCheckBox *m_normalizeFlipZ = nullptr;
    QCheckBox *m_normalizePerspectiveUV = nullptr;
    /** Scene-capture toolbar widgets (#425). */
    QSpinBox *m_sceneCaptureSecondsSpin = nullptr;
    QAction *m_captureSceneAct = nullptr;
    QAction *m_stopCaptureAct = nullptr;
    QShortcut *m_hotkeyCaptureFrame = nullptr;
    QShortcut *m_hotkeyCaptureScene = nullptr;
    QShortcut *m_hotkeyDumpVram = nullptr;
    qint64 m_lastFrameMs = 0;
    quint64 m_lastFrameIndex = 0;
    double m_smoothedFps = 0.0;
    /** Last live-progress snapshot from the worker (#425). */
    qint64 m_lastCaptureTriangles = 0;
    int m_lastCaptureTexPages = 0;
    qint64 m_lastCaptureBytes = 0;
    /** Scene-capture countdown state mirrored locally for footer rendering. */
    int m_sceneCaptureRemaining = 0;
    int m_sceneCaptureTotal = 0;
};

#endif // PS1RIPSESSIONWINDOW_H
