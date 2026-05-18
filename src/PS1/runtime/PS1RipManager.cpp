#include "PS1RipManager.h"
#include "PS1RipWorker.h"
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
    initializeWorkerThread();
}

PS1RipManager::~PS1RipManager()
{
    shutdownWorkerThread();
}

void PS1RipManager::initializeWorkerThread()
{
    qRegisterMetaType<QVector<uint16_t>>("QVector<uint16_t>");

    m_workerThread = new QThread(this);
    m_worker = new PS1RipWorker();
    m_worker->moveToThread(m_workerThread);

    connect(m_worker, &PS1RipWorker::emulationStarted, this, [this]() {
        m_startPending = false;
        m_sessionActive = true;
        m_paused = false;
        emit sessionStarted();
    });
    connect(m_worker, &PS1RipWorker::emulationStopped, this, [this]() {
        m_startPending = false;
        m_sessionActive = false;
        m_paused = false;
        m_captureArmed = false;
        syncWorkerCaptureArmed();
        emit sessionStopped();
    });
    connect(m_worker, &PS1RipWorker::emulationError, this, [this](const QString &msg) {
        m_startPending = false;
        reportError(msg);
    });
    connect(m_worker, &PS1RipWorker::framePresented, this, &PS1RipManager::framePresented);
    connect(m_worker, &PS1RipWorker::frameCaptureReady, this, [this](const QString &captureId, int) {
        SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                                      QStringLiteral("ps1_rip_frame:%1").arg(captureId));
        emit frameCaptured(captureId);
    });
    connect(m_worker, &PS1RipWorker::vramDumpReady, this,
            [this](const QString &captureId, const QString &pngPath, const QVector<uint16_t> &cells,
                   const QImage &preview) {
                SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.vram.dump"),
                                            QStringLiteral("%1:%2").arg(captureId, pngPath));
                emit vramDumped(captureId, pngPath, cells, preview);
            });

    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

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

void PS1RipManager::syncWorkerCaptureArmed()
{
    if (!m_worker)
        return;
    PS1RipWorker *worker = m_worker;
    const bool armed = m_captureArmed;
    QMetaObject::invokeMethod(worker, [worker, armed]() { worker->setCaptureArmed(armed); },
                              Qt::QueuedConnection);
}

bool PS1RipManager::loadBios(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        reportError(tr("BIOS file not found: %1").arg(path));
        return false;
    }

    if (m_sessionActive)
        stop();

    m_biosPath = info.absoluteFilePath();
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.bios.load"), m_biosPath);
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

    m_isoPath = info.absoluteFilePath();
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.iso.load"), m_isoPath);
    syncWorkerSession();
    return true;
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
                                      QStringLiteral("ps1_rip_arm_capture"));
    } else {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("ps1_rip_disarm_capture"));
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
    QMetaObject::invokeMethod(worker, [worker]() { worker->finalizeFrameCapture(); },
                              Qt::QueuedConnection);
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
