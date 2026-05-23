#ifndef PS1RIPMANAGER_H
#define PS1RIPMANAGER_H

#include <QImage>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVector>

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
    bool captureScene(int seconds);
    bool dumpVRAM();

    bool dedupeStrict() const { return m_dedupeStrict; }
    void setDedupeStrict(bool strict) { m_dedupeStrict = strict; }

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
                   bool hasCameraMatrix, int gteInversePercent, bool slabLike);
    void sceneCaptured(const QString &captureId);
    void vramDumped(const QString &captureId, const QString &pngPath, const QVector<uint16_t> &cells,
                    const QImage &nativePreview);
    void error(const QString &message);
    void pausedChanged(bool paused);

private:
    explicit PS1RipManager(QObject *parent = nullptr);
    ~PS1RipManager() override;

    void initializeWorkerThread();
    void shutdownWorkerThread();
    void reportError(const QString &message);
    void syncWorkerSession();
    void syncWorkerCaptureArmed();

    static PS1RipManager *s_instance;

    QString m_biosPath;
    QString m_isoPath;
    QString m_activeCoreId;
    bool m_sessionActive = false;
    bool m_startPending = false;
    bool m_paused = false;
    bool m_captureArmed = false;
    bool m_dedupeStrict = false;

    QThread *m_workerThread = nullptr;
    PS1RipWorker *m_worker = nullptr;
};

#endif // PS1RIPMANAGER_H
