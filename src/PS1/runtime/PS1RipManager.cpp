#include "PS1RipManager.h"
#include "PS1RipWorker.h"
#include "SentryReporter.h"

#include <QFileInfo>
#include <QMetaObject>

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
    m_workerThread = new QThread(this);
    m_worker = new PS1RipWorker();
    m_worker->moveToThread(m_workerThread);

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
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip"),
                                QStringLiteral("loadIso: %1").arg(m_isoPath));
    return true;
}

bool PS1RipManager::start()
{
    if (m_isoPath.isEmpty()) {
        reportError(tr("No ISO loaded"));
        return false;
    }
    if (m_sessionActive)
        return true;

    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip"), QStringLiteral("start (stub)"));
    reportError(tr("PS1 emulator core not integrated yet"));
    return false;
}

bool PS1RipManager::stop()
{
    if (!m_sessionActive)
        return false;

    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, &PS1RipWorker::stopEmulation, Qt::QueuedConnection);
    }

    m_sessionActive = false;
    m_captureArmed = false;
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip"), QStringLiteral("stop"));
    emit sessionStopped();
    return true;
}

bool PS1RipManager::pause()
{
    if (!m_sessionActive) {
        reportError(tr("No active PS1 session"));
        return false;
    }
    reportError(tr("PS1 emulator core not integrated yet"));
    return false;
}

bool PS1RipManager::step()
{
    if (!m_sessionActive) {
        reportError(tr("No active PS1 session"));
        return false;
    }
    reportError(tr("PS1 emulator core not integrated yet"));
    return false;
}

bool PS1RipManager::armCapture(bool armed)
{
    m_captureArmed = armed;
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip"),
                                  armed ? QStringLiteral("armCapture") : QStringLiteral("disarmCapture"));
    return true;
}

bool PS1RipManager::captureFrame()
{
    if (!m_captureArmed) {
        reportError(tr("Capture is not armed"));
        return false;
    }
    reportError(tr("Capture pipeline not implemented yet"));
    return false;
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
    reportError(tr("VRAM dump not implemented yet"));
    return false;
}
