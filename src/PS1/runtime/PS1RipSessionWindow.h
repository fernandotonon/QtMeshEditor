#ifndef PS1RIPSESSIONWINDOW_H
#define PS1RIPSESSIONWINDOW_H

#include "Gp0CaptureStats.h"

#include <QImage>
#include <QMainWindow>
#include <QVector>

class EmuViewport;
class QAction;
class QCheckBox;
class QDockWidget;
class QDoubleSpinBox;
class QLabel;
class QShortcut;
class QSpinBox;
class PS1ExtractedAssetBrowser;
class PS1GeometryInspectorPanel;
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

    /** Entry point for `MainWindow::dropEvent` so PS1 Asset Browser →
     *  editor viewport drag-and-drop produces a permanent SceneNode
     *  even when the session window doesn't currently own the focus
     *  (#426). Routes to the shared `promoteUniqueMesh` impl through
     *  any live `PS1RipSessionWindow` so the promotion counter, the
     *  capture-id lookup, and the Sentry breadcrumb all stay in one
     *  place. No-op when no session window exists or the captured
     *  asset store has been cleared. */
    static bool promoteUniqueMeshById(int meshIndex, const QString &assetId);

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
     *  the `ps1Rip/normalize/` prefix and pushes the new value to
     *  PS1RipManager so the user sees the change live without re-capturing
     *  (#424). */
    void createNormalizerDock();
    /** Snapshot the current dock widget values into a settings struct and
     *  forward to the manager + QSettings. */
    void pushNormalizerSettings();
    /** Rebuild the rightmost status footer chunk (#425): when armed, it shows
     *  live capture stats; when a scene capture is in flight it shows the
     *  remaining countdown; when idle it stays empty. Stored values come from
     *  `onCaptureProgress` and `onSceneCaptureProgress`. */
    void refreshCaptureStatusFooter();

    /** Build the Geometry Inspector dock + Extracted Asset Browser dock
     *  off the captured-asset store and wire their intent signals back to
     *  the session window (#426). Docks are added to the bottom and right
     *  dock areas respectively so they sit alongside the emulator
     *  viewport without obscuring it. */
    void createInspectorDocks();
    /** Translates an inspector row click into editor SelectionSet calls
     *  so the matching SubEntity is outlined in the viewport (#426
     *  acceptance: "Highlighting a draw-call row visibly outlines the
     *  corresponding submesh"). */
    void highlightInspectorRow(int rowIndex);
    /** Toggle visibility of the SceneNode backing the row's instance. */
    void setInspectorRowVisible(int rowIndex, bool visible);
    /** Clone the row's unique mesh into a fresh "PS1Imported_*" SceneNode
     *  that survives capture clearing. Backed by the same code path as
     *  the asset-browser drag-and-drop (#426 acceptance: "Drag-and-drop
     *  creates a permanent entity"). */
    void promoteInspectorRow(int rowIndex);
    /** Mark a row as discarded and hide its scene node (no destruction —
     *  the row stays addressable for undo via "Restore"). */
    void discardInspectorRow(int rowIndex, bool discarded);
    /** Shared promotion impl used by the inspector right-click action
     *  and the asset-browser double-click / drag-drop. */
    bool promoteUniqueMesh(int meshIndex, const QString &assetId);

    /** Capture toolbar widgets bundled into one struct so the class stays
     *  under SonarCloud's S1820 field-count threshold (#425 — without the
     *  bundling the #425 additions would have pushed the class from 14 to
     *  27 raw fields). Each member is owned by Qt's parent-child
     *  hierarchy; the struct itself holds non-owning pointers. */
    struct CaptureUi {
        QLabel *footerLabel = nullptr;
        QSpinBox *sceneSecondsSpin = nullptr;
        QAction *captureSceneAct = nullptr;
        QAction *stopCaptureAct = nullptr;
        /** Arm Capture toggle promoted to a member so Stop Capture and
         *  sessionStopped can keep its checked state in sync with the
         *  backend (Codex P2 / CodeRabbit Major on #677). Without this,
         *  clicking Stop Capture while Arm Capture was checked left the
         *  toolbar visibly armed even though the manager had already
         *  disarmed, so the next Capture Frame click was rejected with
         *  "Capture is not armed". */
        QAction *armCaptureAct = nullptr;
    };

    /** Window-scoped capture hotkeys (#425). Bundled to keep the class
     *  under S1820. */
    struct CaptureHotkeys {
        QShortcut *captureFrame = nullptr;
        QShortcut *captureScene = nullptr;
        QShortcut *dumpVram = nullptr;
    };

    /** Last live-progress snapshot from the worker plus scene-capture
     *  countdown mirror, bundled for footer rendering (#425). */
    struct CaptureLiveStats {
        qint64 triangles = 0;
        int texPages = 0;
        qint64 bytes = 0;
        int sceneRemaining = 0;
        int sceneTotal = 0;
    };

    EmuViewport *m_viewport = nullptr;
    VramViewerWidget *m_vramViewer = nullptr;
    QLabel *m_statusLabel = nullptr;
    QMenu *m_recentIsoMenu = nullptr;
    PS1RipGamepadBridge *m_gamepadBridge = nullptr;
    PS1RipManager *m_manager = nullptr;
    QDoubleSpinBox *m_normalizeScaleSpin = nullptr;
    QCheckBox *m_normalizeFlipX = nullptr;
    QCheckBox *m_normalizeFlipY = nullptr;
    QCheckBox *m_normalizeFlipZ = nullptr;
    QCheckBox *m_normalizePerspectiveUV = nullptr;
    qint64 m_lastFrameMs = 0;
    quint64 m_lastFrameIndex = 0;
    double m_smoothedFps = 0.0;
    CaptureUi m_captureUi;
    CaptureHotkeys m_captureHotkeys;
    CaptureLiveStats m_captureStats;

    /** Geometry inspector + asset browser docks (#426). Bundled into a
     *  struct so the new feature contributes a single field to the
     *  outer class (SonarCloud S1820). */
    struct InspectorDocks {
        QDockWidget *inspectorDock = nullptr;
        QDockWidget *browserDock = nullptr;
        PS1GeometryInspectorPanel *inspectorPanel = nullptr;
        PS1ExtractedAssetBrowser *browser = nullptr;
        /** Counter feeding the unique permanent-entity node name suffix
         *  so promote-twice produces two distinct nodes. Reset on
         *  session start; persists across captures so successive
         *  promotes don't reuse a stale id even if a previous promote
         *  was undone. */
        int promotionCounter = 0;
    } m_inspector;

    /** Tracks the most-recently constructed session window so the static
     *  drop-event entry point can route into the right instance without
     *  walking the QWidget tree. Cleared on destruction. */
    static PS1RipSessionWindow *s_lastInstance;
};

#endif // PS1RIPSESSIONWINDOW_H
