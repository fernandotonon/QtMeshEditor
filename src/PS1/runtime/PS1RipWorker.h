#ifndef PS1RIPWORKER_H
#define PS1RIPWORKER_H

#include <QImage>
#include <QObject>
#include <QString>
#include <QtGlobal>

#include <atomic>
#include <memory>

class CaptureBuffer;
class EmuCore;
class QTimer;
class RipperHooks;

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
    void startEmulation();
    void cancelPendingStart();
    void stopEmulation();
    void pauseEmulation();
    void stepFrame();
    void setCaptureArmed(bool armed);
    void finalizeFrameCapture();

signals:
    void emulationStarted();
    void emulationStopped();
    void framePresented(const QImage &frame, quint64 frameIndex);
    void emulationError(const QString &message);
    void frameCaptureReady(const QString &captureId, int primCount);

private slots:
    void runFrameTick();

private:
    bool ensureCore(QString *errorOut);
    static QImage framebufferToImage(const class EmuFramebuffer &fb);

    QString m_biosPath;
    QString m_isoPath;
    std::unique_ptr<EmuCore> m_core;
    QTimer *m_frameTimer = nullptr;
    bool m_running = false;
    bool m_paused = false;
    std::atomic<bool> m_startSuperseded{false};
    std::atomic<bool> m_captureArmed{false};

    std::unique_ptr<CaptureBuffer> m_captureBuffer;
    std::unique_ptr<RipperHooks> m_ripperHooks;
};

#endif // PS1RIPWORKER_H
