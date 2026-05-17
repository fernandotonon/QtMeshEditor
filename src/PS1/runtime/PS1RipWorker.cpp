#include "PS1RipWorker.h"
#include "EmuCore.h"
#include "EmuCoreLoader.h"
#include "EmuFramebuffer.h"

#include <QTimer>

#include <cstring>

PS1RipWorker::PS1RipWorker(QObject *parent)
    : QObject(parent)
{
    m_frameTimer = new QTimer(this);
    m_frameTimer->setTimerType(Qt::PreciseTimer);
    m_frameTimer->setInterval(16);
    connect(m_frameTimer, &QTimer::timeout, this, &PS1RipWorker::runFrameTick);
}

PS1RipWorker::~PS1RipWorker()
{
    stopEmulation();
    m_core.reset();
}

void PS1RipWorker::configureSession(const QString &biosPath, const QString &isoPath)
{
    if (m_running)
        stopEmulation();
    m_biosPath = biosPath;
    m_isoPath = isoPath;
}

bool PS1RipWorker::ensureCore(QString *errorOut)
{
    if (m_core)
        return true;
    m_core = EmuCoreLoader::loadCore(errorOut);
    return static_cast<bool>(m_core);
}

void PS1RipWorker::startEmulation()
{
    if (m_running)
        return;

    if (m_biosPath.isEmpty() || m_isoPath.isEmpty()) {
        emit emulationError(tr("BIOS and ISO paths are required"));
        return;
    }

    QString err;
    if (!ensureCore(&err)) {
        emit emulationError(err);
        return;
    }

    if (!m_core->loadBios(m_biosPath)) {
        emit emulationError(tr("Failed to load BIOS: %1").arg(m_biosPath));
        return;
    }
    if (!m_core->loadIso(m_isoPath)) {
        emit emulationError(tr("Failed to load ISO: %1").arg(m_isoPath));
        return;
    }

    m_core->reset();
    m_running = true;
    m_paused = false;
    m_frameTimer->start();
    emit emulationStarted();
}

void PS1RipWorker::stopEmulation()
{
    if (!m_running && !m_frameTimer->isActive())
        return;

    m_frameTimer->stop();
    m_running = false;
    m_paused = false;
    emit emulationStopped();
}

void PS1RipWorker::pauseEmulation()
{
    if (!m_running)
        return;
    m_paused = !m_paused;
    if (m_paused)
        m_frameTimer->stop();
    else
        m_frameTimer->start();
}

void PS1RipWorker::stepFrame()
{
    if (!m_running || !m_core)
        return;
    m_core->runFrame();
    emit framePresented(framebufferToImage(m_core->framebuffer()), m_core->framebuffer().frameIndex);
}

void PS1RipWorker::runFrameTick()
{
    if (!m_running || m_paused || !m_core)
        return;

    m_core->runFrame();
    const EmuFramebuffer &fb = m_core->framebuffer();
    emit framePresented(framebufferToImage(fb), fb.frameIndex);
}

QImage PS1RipWorker::framebufferToImage(const EmuFramebuffer &fb)
{
    if (!fb.isValid())
        return {};

    QImage img(fb.width, fb.height, QImage::Format_RGB888);
    if (fb.rgb24.size() != img.sizeInBytes())
        return {};

    memcpy(img.bits(), fb.rgb24.constData(), static_cast<size_t>(fb.rgb24.size()));
    return img;
}
