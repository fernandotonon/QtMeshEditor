#include "PS1RipManager.h"
#include "CaptureSnapshot.h"
#include "LibretroCoreOptions.h"
#include "MeshReconstructionStats.h"
#include "MeshReconstructor.h"
#include "MeshTopologyHash.h"
#include "PS1RipMeshBuilder.h"
#include "PS1RipWorker.h"
#include "PsxBiosValidator.h"
#include "PsxDiscResolver.h"
#include "PsxGoldenCapture.h"
#include "PsxVramMirrorMode.h"
#include "SentryReporter.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QMetaType>

PS1RipManager *PS1RipManager::s_instance = nullptr;

PS1RipManager *PS1RipManager::getSingleton()
{
    if (!s_instance)
        s_instance = new PS1RipManager();
    return s_instance;
}

PS1RipManager *PS1RipManager::getSingletonPtr()
{
    return s_instance;
}

void PS1RipManager::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

PS1RipManager::PS1RipManager(QObject *parent)
    : QObject(parent)
{
    m_goldenSceneId = PsxGoldenCapture::activeSceneId();
    initializeWorkerThread();
}

QString PS1RipManager::goldenSceneId() const
{
    if (!m_goldenSceneId.isEmpty())
        return m_goldenSceneId;
    return PsxGoldenCapture::activeSceneId();
}

void PS1RipManager::setGoldenSceneId(const QString &sceneId)
{
    if (sceneId.isEmpty() || PsxGoldenCapture::isKnownSceneId(sceneId))
        m_goldenSceneId = sceneId;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "setGoldenSceneId", Qt::QueuedConnection,
                                  Q_ARG(QString, m_goldenSceneId));
    }
}

PS1RipManager::~PS1RipManager()
{
    shutdownWorkerThread();
}

void PS1RipManager::initializeWorkerThread()
{
    qRegisterMetaType<QVector<uint16_t>>("QVector<uint16_t>");
    qRegisterMetaType<CaptureSnapshot>("CaptureSnapshot");
    qRegisterMetaType<PsxVramMirrorMode>("PsxVramMirrorMode");
    qRegisterMetaType<Gp0CaptureStats>("Gp0CaptureStats");

    m_workerThread = new QThread(this);
    m_worker = new PS1RipWorker();
    m_worker->moveToThread(m_workerThread);

    connect(m_worker, &PS1RipWorker::emulationStarted, this, [this](const QString &coreId) {
        m_startPending = false;
        m_sessionActive = true;
        m_paused = false;
        m_activeCoreId = coreId;
        emit sessionStarted(coreId);
    });
    connect(m_worker, &PS1RipWorker::emulationStopped, this, [this]() {
        m_startPending = false;
        m_sessionActive = false;
        m_paused = false;
        m_captureArmed = false;
        m_activeCoreId.clear();
        syncWorkerCaptureArmed();
        emit sessionStopped();
    });
    connect(m_worker, &PS1RipWorker::emulationError, this, [this](const QString &msg) {
        m_startPending = false;
        m_sessionActive = false;
        m_paused = false;
        m_captureArmed = false;
        m_activeCoreId.clear();
        syncWorkerCaptureArmed();
        reportError(msg);
        emit sessionStopped();
    });
    connect(m_worker, &PS1RipWorker::sessionWarning, this, &PS1RipManager::reportError);
    connect(m_worker, &PS1RipWorker::framePresented, this, &PS1RipManager::framePresented);
    connect(m_worker, &PS1RipWorker::frameCaptureReady, this,
            [this](const QString &captureId, const CaptureSnapshot &snapshot, int,
                   PsxVramMirrorMode vramMirrorMode, Gp0CaptureStats captureStats) {
                SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                                              QStringLiteral("ps1_rip_frame:%1").arg(captureId));
                const QString goldenId = goldenSceneId();
                if (!goldenId.isEmpty()) {
                    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.capture.golden"),
                                                  QStringLiteral("id=%1 capture=%2")
                                                      .arg(goldenId, captureId));
                }

                emit frameCaptured(captureId);

                const MeshDedupeMode dedupeMode =
                    m_dedupeStrict ? MeshDedupeMode::Strict : MeshDedupeMode::Loose;
                MeshReconstructionStats reconStats;
                const ReconstructedCaptureSet captureSet =
                    MeshReconstructor::reconstructDeduped(snapshot, dedupeMode, &reconStats);
                if (captureSet.isEmpty()) {
                    reportError(tr("Capture produced no reconstructable geometry"));
                    return;
                }

                PS1RipMeshBuilder::BuildResult built;
                QString buildErr;
                try {
                    if (!PS1RipMeshBuilder::attachCaptureSetToScene(captureSet, captureId, &snapshot,
                                                                    &built, &buildErr)) {
                        reportError(buildErr.isEmpty() ? tr("Failed to build capture mesh")
                                                     : buildErr);
                        return;
                    }
                } catch (const std::exception &e) {
                    reportError(tr("Failed to build capture mesh: %1").arg(QString::fromUtf8(e.what())));
                    return;
                }

                SentryReporter::addBreadcrumb(
                    QStringLiteral("ps1.rip.dedupe.summary"),
                    QStringLiteral("captured=%1 unique=%2 instances=%3 mode=%4")
                        .arg(captureSet.capturedPartCount)
                        .arg(captureSet.uniqueCount())
                        .arg(captureSet.instanceCount())
                        .arg(m_dedupeStrict ? QStringLiteral("strict") : QStringLiteral("loose")));
                SentryReporter::addBreadcrumb(
                    QStringLiteral("ps1.rip.mesh.built"),
                    QStringLiteral("%1 verts %2 tris").arg(built.vertexCount).arg(built.triangleCount));
                QString matrixStats =
                    QStringLiteral("gte_inverse=%1%% prims_with_matrix=%2/%3 slab=%4 model_meshes=%5")
                        .arg(reconStats.gteInversePercent())
                        .arg(reconStats.primsWithMatrixId)
                        .arg(reconStats.primsTotal)
                        .arg(reconStats.slabLike ? QStringLiteral("yes") : QStringLiteral("no"))
                        .arg(snapshot.modelMeshes.size());
                if (!goldenId.isEmpty())
                    matrixStats += QStringLiteral(" golden_id=%1").arg(goldenId);
                SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.matrix.stats"), matrixStats);
                SentryReporter::addBreadcrumb(
                    QStringLiteral("ps1.rip.vram.sync"),
                    QStringLiteral("capture=%1 mode=%2")
                        .arg(captureId, psxVramMirrorModeLabel(vramMirrorMode)));
                emit meshBuilt(captureId, captureSet.capturedPartCount, captureSet.uniqueCount(),
                               captureSet.instanceCount(), built.vertexCount, built.triangleCount,
                               snapshot.matrices.size(), snapshot.cameraMatrixId,
                               snapshot.hasCameraMatrix(), reconStats.gteInversePercent(),
                               reconStats.slabLike, reconStats.primsWithMatrixId,
                               reconStats.primsTotal, vramMirrorMode, captureStats);
            });
    connect(m_worker, &PS1RipWorker::vramFrameUpdated, this,
            [this](const QVector<uint16_t> &cells, const QImage &preview) {
                emit vramFrameUpdated(cells, preview);
            });
    connect(m_worker, &PS1RipWorker::vramDumpReady, this,
            [this](const QString &captureId, const QString &pngPath, const QVector<uint16_t> &cells,
                   const QImage &preview) {
                SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.vram.dump"),
                                            QStringLiteral("%1:%2").arg(captureId, pngPath));
                emit vramDumped(captureId, pngPath, cells, preview);
            });

    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_workerThread, &QThread::started, this, [this]() {
        QMetaObject::invokeMethod(m_worker, "setGoldenSceneId", Qt::QueuedConnection,
                                  Q_ARG(QString, m_goldenSceneId));
    });

    m_workerThread->start();
}

void PS1RipManager::shutdownWorkerThread()
{
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, &PS1RipWorker::stopEmulation, Qt::QueuedConnection);
    }
    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait();
    }
    m_worker = nullptr;
}

void PS1RipManager::reportError(const QString &message)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip"), message, QStringLiteral("error"));
    emit error(message);
}

void PS1RipManager::syncWorkerSession()
{
    if (!m_worker)
        return;
    QMetaObject::invokeMethod(m_worker, "configureSession", Qt::QueuedConnection,
                              Q_ARG(QString, m_biosPath), Q_ARG(QString, m_isoPath));
}

void PS1RipManager::setJoypadPressed(unsigned port, unsigned buttonId, bool pressed)
{
    if (!m_worker || (!m_sessionActive && !m_startPending))
        return;
    QMetaObject::invokeMethod(m_worker, "setJoypadButton", Qt::QueuedConnection,
                              Q_ARG(unsigned, port), Q_ARG(unsigned, buttonId), Q_ARG(bool, pressed));
}

void PS1RipManager::resetJoypad(unsigned port)
{
    if (!m_worker)
        return;
    QMetaObject::invokeMethod(m_worker, "resetJoypad", Qt::QueuedConnection, Q_ARG(unsigned, port));
}

void PS1RipManager::syncWorkerCaptureArmed()
{
    if (!m_worker)
        return;
    PS1RipWorker *worker = m_worker;
    const bool armed = m_captureArmed;
    QMetaObject::invokeMethod(worker, "setCaptureArmed", Qt::QueuedConnection, Q_ARG(bool, armed));
}

bool PS1RipManager::loadBios(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        reportError(tr("BIOS file not found: %1").arg(path));
        return false;
    }

    const PsxBiosValidator::Fingerprint fp = PsxBiosValidator::fingerprintFile(info.absoluteFilePath());
    if (!fp.readable) {
        reportError(tr("BIOS file not found: %1").arg(path));
        return false;
    }
    if (!fp.sizeOk) {
        reportError(tr("BIOS must be exactly 512 KiB (524288 bytes)."));
        return false;
    }

    if (m_sessionActive)
        stop();

    m_biosPath = info.absoluteFilePath();
    QString biosMsg = QStringLiteral("file=%1 sha256=%2").arg(info.fileName(), fp.sha256Hex);
    if (!fp.knownLabel.isEmpty())
        biosMsg += QStringLiteral(" known=%1").arg(fp.knownLabel);
    else if (fp.sizeOk)
        biosMsg += QStringLiteral(" known=unknown");
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.bios.load"), biosMsg);

    // #660: record which libretro renderer we'll request so rip sessions can be
    // correlated with VRAM-mode telemetry below. The plugin can't link Sentry
    // (it's a runtime-loaded MODULE), so the breadcrumb originates here.
    const QByteArray rendererPref = LibretroCoreOptions::rendererPreferenceFromEnv();
    SentryReporter::addBreadcrumb(
        QStringLiteral("ps1.rip.renderer"),
        QStringLiteral("preference=%1")
            .arg(rendererPref.isEmpty() ? QStringLiteral("auto")
                                        : QString::fromUtf8(rendererPref)));

    syncWorkerSession();
    return true;
}

bool PS1RipManager::loadIso(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        reportError(tr("ISO file not found: %1").arg(path));
        return false;
    }

    if (m_sessionActive)
        stop();

    const QString absolute = info.absoluteFilePath();
    const PsxDiscResolveResult disc = PsxDiscResolver::resolve(absolute);
    if (!disc.ok) {
        reportError(disc.errorMessage);
        return false;
    }

    m_isoPath = disc.loadPath;
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.iso.load"),
                                  QStringLiteral("file=%1").arg(QFileInfo(absolute).fileName()));
    syncWorkerSession();
    return true;
}

bool PS1RipManager::reloadIso()
{
    if (m_isoPath.isEmpty()) {
        reportError(tr("No disc image loaded"));
        return false;
    }

    const bool resume = m_sessionActive || m_startPending;
    const QString path = m_isoPath;

    if (resume) {
        connect(this, &PS1RipManager::sessionStopped, this,
                [this, path]() {
                    if (loadIso(path))
                        start();
                },
                Qt::SingleShotConnection);
        return stop();
    }

    return loadIso(path);
}

bool PS1RipManager::start()
{
    if (m_biosPath.isEmpty()) {
        reportError(tr("No BIOS loaded"));
        return false;
    }
    if (m_isoPath.isEmpty()) {
        reportError(tr("No ISO loaded"));
        return false;
    }
    if (m_sessionActive || m_startPending)
        return true;

    m_worker->clearStartCancel();
    m_startPending = true;
    syncWorkerSession();
    QMetaObject::invokeMethod(m_worker, &PS1RipWorker::startEmulation, Qt::QueuedConnection);
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip"), QStringLiteral("start"));
    return true;
}

bool PS1RipManager::stop()
{
    if (!m_sessionActive && !m_startPending)
        return false;

    m_captureArmed = false;
    syncWorkerCaptureArmed();
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip"), QStringLiteral("stop"));

    if (m_startPending && !m_sessionActive) {
        m_startPending = false;
        m_worker->requestCancelStart();
        QMetaObject::invokeMethod(m_worker, &PS1RipWorker::cancelPendingStart, Qt::QueuedConnection);
        return true;
    }

    m_startPending = false;
    QMetaObject::invokeMethod(m_worker, &PS1RipWorker::stopEmulation, Qt::QueuedConnection);
    return true;
}

bool PS1RipManager::pause()
{
    if (!m_sessionActive) {
        reportError(tr("No active PS1 session"));
        return false;
    }
    QMetaObject::invokeMethod(m_worker, &PS1RipWorker::pauseEmulation, Qt::QueuedConnection);
    m_paused = !m_paused;
    emit pausedChanged(m_paused);
    return true;
}

bool PS1RipManager::step()
{
    if (!m_sessionActive) {
        reportError(tr("No active PS1 session"));
        return false;
    }
    QMetaObject::invokeMethod(m_worker, &PS1RipWorker::stepFrame, Qt::QueuedConnection);
    return true;
}

bool PS1RipManager::armCapture(bool armed)
{
    m_captureArmed = armed;
    if (armed) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("ps1.rip.capture.frame_armed:armed"));
    } else {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("ps1.rip.capture.frame_armed:disarmed"));
    }

    syncWorkerCaptureArmed();
    return true;
}

bool PS1RipManager::captureFrame()
{
    if (!m_captureArmed) {
        reportError(tr("Capture is not armed"));
        return false;
    }
    if (!m_sessionActive) {
        reportError(tr("No active PS1 session"));
        return false;
    }
    PS1RipWorker *worker = m_worker;
    QMetaObject::invokeMethod(worker, "finalizeFrameCapture", Qt::QueuedConnection);
    return true;
}

bool PS1RipManager::captureScene(int seconds)
{
    if (seconds <= 0) {
        reportError(tr("Scene capture duration must be positive"));
        return false;
    }
    if (!m_captureArmed) {
        reportError(tr("Capture is not armed"));
        return false;
    }
    reportError(tr("Scene capture pipeline not implemented yet"));
    return false;
}

bool PS1RipManager::dumpVRAM()
{
    if (!m_sessionActive) {
        reportError(tr("No active PS1 session"));
        return false;
    }
    PS1RipWorker *worker = m_worker;
    QMetaObject::invokeMethod(worker, &PS1RipWorker::dumpVram, Qt::QueuedConnection);
    return true;
}
