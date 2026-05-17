#ifndef PS1RIPMANAGER_H
#define PS1RIPMANAGER_H

#include <QImage>
#include <QObject>
#include <QString>
#include <QThread>

class PS1RipWorker;

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

    bool loadBios(const QString &path);
    bool loadIso(const QString &path);
    bool start();
    bool stop();
    bool pause();
    bool step();
    bool armCapture(bool armed = true);
    bool captureFrame();
    bool captureScene(int seconds);
    bool dumpVRAM();

    PS1RipWorker *worker() const { return m_worker; }

signals:
    void sessionStarted();
    void sessionStopped();
    void framePresented(const QImage &frame, quint64 frameIndex);
    void frameCaptured(const QString &captureId);
    void sceneCaptured(const QString &captureId);
    void vramDumped(const QString &captureId);
    void error(const QString &message);
    void pausedChanged(bool paused);

private:
    explicit PS1RipManager(QObject *parent = nullptr);
    ~PS1RipManager() override;

    void initializeWorkerThread();
    void shutdownWorkerThread();
    void reportError(const QString &message);
    void syncWorkerSession();

    static PS1RipManager *s_instance;

    QString m_biosPath;
    QString m_isoPath;
    bool m_sessionActive = false;
    bool m_startPending = false;
    bool m_paused = false;
    bool m_captureArmed = false;

    QThread *m_workerThread = nullptr;
    PS1RipWorker *m_worker = nullptr;
};

#endif // PS1RIPMANAGER_H
