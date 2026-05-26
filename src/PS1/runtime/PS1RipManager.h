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
    bool captureScene(int seconds);
    bool dumpVRAM();

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
    Ps1NormalizerSettings m_normalize;
    QString m_goldenSceneId;

    QThread *m_workerThread = nullptr;
    PS1RipWorker *m_worker = nullptr;
};

#endif // PS1RIPMANAGER_H
