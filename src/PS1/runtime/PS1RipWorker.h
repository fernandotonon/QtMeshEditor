#ifndef PS1RIPWORKER_H
#define PS1RIPWORKER_H

#include "CaptureSnapshot.h"
#include "Gp0CaptureStats.h"

#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <atomic>
#include <memory>

class CaptureBuffer;
class VramSnapshot;
class EmuCore;
class QTimer;
class RipperHooks;

enum class PsxVramMirrorMode;

/**
 * Runs EmuCore on a dedicated QThread owned by PS1RipManager (#415).
 */
class PS1RipWorker : public QObject
{
    Q_OBJECT

public:
    explicit PS1RipWorker(QObject *parent = nullptr);
    ~PS1RipWorker() override;

    void clearStartCancel() { m_startSuperseded.store(false, std::memory_order_release); }
    void requestCancelStart() { m_startSuperseded.store(true, std::memory_order_release); }

    const CaptureBuffer *captureBuffer() const { return m_captureBuffer.get(); }

public slots:
    void configureSession(const QString &biosPath, const QString &isoPath);
    void setGoldenSceneId(const QString &sceneId);
    void startEmulation();
    void cancelPendingStart();
    void stopEmulation();
    void pauseEmulation();
    void stepFrame();
    void setCaptureArmed(bool armed);
    void setJoypadButton(unsigned port, unsigned buttonId, bool pressed);
    void resetJoypad(unsigned port = 0);
    void finalizeFrameCapture();
    void dumpVram();

signals:
    void emulationStarted(const QString &coreId);
    /** In-core rip hook availability for the booted core (#813): true when
     *  the qtmesh fork registered its capture interface, false on stock
     *  cores (or QTMESH_PS1_RIP_INCORE=0). Emitted right after
     *  emulationStarted. */
    void inCoreHooksState(bool active);
    void emulationStopped();
    void framePresented(const QImage &frame, quint64 frameIndex);
    void emulationError(const QString &message);
    /** Non-fatal operational warning (does not stop the session). */
    void sessionWarning(const QString &message);
    void frameCaptureReady(const QString &captureId, const CaptureSnapshot &snapshot, int primCount,
                           PsxVramMirrorMode vramMirrorMode, Gp0CaptureStats captureStats);
    void vramDumpReady(const QString &captureId, const QString &pngPath, const QVector<uint16_t> &cells,
                       const QImage &nativePreview);
    void vramFrameUpdated(const QVector<uint16_t> &cells, const QImage &nativePreview);
    /** Throttled live snapshot of the capture buffer while armed (#425). Used
     *  by the session UI to populate the status footer; emitted from
     *  `runFrameTick` every ~250 ms so a 5-second scene capture only stirs
     *  the GUI thread ~20 times. */
    void captureProgress(qint64 primitives, qint64 triangles, int texturePages,
                         qint64 bytesEstimate);

private slots:
    void runFrameTick();

private:
    bool ensureCore(QString *errorOut);
    void scheduleNextFrame(int delayMs);
    static QImage framebufferToImage(const class EmuFramebuffer &fb);

    QString m_biosPath;
    QString m_isoPath;
    QString m_goldenSceneId;
    std::unique_ptr<EmuCore> m_core;
    QTimer *m_frameTimer = nullptr;
    bool m_running = false;
    bool m_paused = false;
    std::atomic<bool> m_startSuperseded{false};
    std::atomic<bool> m_captureArmed{false};

    std::unique_ptr<CaptureBuffer> m_captureBuffer;
    std::unique_ptr<RipperHooks> m_ripperHooks;
    std::unique_ptr<VramSnapshot> m_vram;
};

#endif // PS1RIPWORKER_H
