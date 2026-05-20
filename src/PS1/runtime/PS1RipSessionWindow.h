#ifndef PS1RIPSESSIONWINDOW_H
#define PS1RIPSESSIONWINDOW_H

#include <QImage>
#include <QMainWindow>
#include <QVector>

class EmuViewport;
class QLabel;
class PS1RipGamepadBridge;
class PS1RipManager;
class QMenu;
class VramViewerWidget;

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
    void onVramDumped(const QString &captureId, const QString &pngPath, const QVector<uint16_t> &cells,
                      const QImage &nativePreview);
    void onMeshBuilt(const QString &captureId, int capturedParts, int uniqueMeshes, int instanceCount,
                     int vertexCount, int triangleCount);
    void onPausedChanged(bool paused);

private:
    void updateFps(quint64 frameIndex);
    void addRecentIso(const QString &path);
    void applyIsoPath(const QString &path);
    void rebuildRecentIsoMenu();

    EmuViewport *m_viewport = nullptr;
    VramViewerWidget *m_vramViewer = nullptr;
    QLabel *m_statusLabel = nullptr;
    QMenu *m_recentIsoMenu = nullptr;
    PS1RipGamepadBridge *m_gamepadBridge = nullptr;
    PS1RipManager *m_manager = nullptr;
    qint64 m_lastFrameMs = 0;
    quint64 m_lastFrameIndex = 0;
    double m_smoothedFps = 0.0;
};

#endif // PS1RIPSESSIONWINDOW_H
