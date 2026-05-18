#include "PS1RipWorker.h"
#include "CaptureBuffer.h"
#include "CaptureSnapshot.h"
#include "CaptureSnapshot.h"
#include "EmuCore.h"
#include "EmuCoreLoader.h"
#include "EmuFramebuffer.h"
#include "GpuCommandParser.h"
#include "RipperHooks.h"
#include "SentryReporter.h"
#include "VramSnapshot.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTimer>

#include <cstring>

PS1RipWorker::PS1RipWorker(QObject *parent)
    : QObject(parent)
    , m_captureBuffer(std::make_unique<CaptureBuffer>())
    , m_ripperHooks(std::make_unique<RipperHooks>())
    , m_vram(std::make_unique<VramSnapshot>())
{
    m_ripperHooks->setArmedFlag(&m_captureArmed);
    m_ripperHooks->setBuffer(m_captureBuffer.get());
    m_ripperHooks->setVram(m_vram.get());

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

void PS1RipWorker::cancelPendingStart()
{
    requestCancelStart();
    if (m_running)
        stopEmulation();
}

void PS1RipWorker::startEmulation()
{
    if (m_startSuperseded.load(std::memory_order_acquire)) {
        m_startSuperseded.store(false, std::memory_order_release);
        return;
    }

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

    m_core->setHooks(m_ripperHooks.get());
    m_core->reset();

    if (m_startSuperseded.load(std::memory_order_acquire)) {
        m_startSuperseded.store(false, std::memory_order_release);
        return;
    }

    m_running = true;
    m_paused = false;
    m_frameTimer->start();
    emit emulationStarted();
}

void PS1RipWorker::stopEmulation()
{
    clearStartCancel();

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

void PS1RipWorker::setCaptureArmed(bool armed)
{
    m_captureArmed.store(armed, std::memory_order_release);
}

void PS1RipWorker::finalizeFrameCapture()
{
    if (!m_captureArmed.load(std::memory_order_acquire)) {
        emit emulationError(tr("Capture is not armed"));
        return;
    }

    const QVector<PrimRecord> &prims = m_captureBuffer->prims();
    if (prims.isEmpty()) {
        emit emulationError(tr("No primitives captured in the current frame"));
        return;
    }

    const QString captureId = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString dir = QDir::temp().filePath(QStringLiteral("qtmesh_ps1_capture"));
    if (!QDir().mkpath(dir)) {
        emit emulationError(tr("Failed to create capture directory: %1").arg(dir));
        return;
    }

    const QString csvPath = dir + QLatin1Char('/') + captureId + QStringLiteral(".csv");
    QFile file(csvPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit emulationError(tr("Failed to write capture file: %1").arg(csvPath));
        return;
    }

    const QByteArray csv = GpuCommandParser::primsToCsv(prims).toUtf8();
    if (file.write(csv) != csv.size()) {
        emit emulationError(tr("Failed to write capture data: %1").arg(csvPath));
        return;
    }
    file.close();

    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.capture"),
                                QStringLiteral("%1 prims:%2").arg(captureId).arg(prims.size()));
    emit frameCaptureReady(captureId, CaptureSnapshot::fromBuffer(*m_captureBuffer), prims.size());
}

void PS1RipWorker::dumpVram()
{
    if (!m_running) {
        emit emulationError(tr("No active emulation session"));
        return;
    }

    const QString captureId = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString baseDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/ps1_rip/captures");
    if (!QDir().mkpath(baseDir)) {
        emit emulationError(tr("Failed to create capture directory: %1").arg(baseDir));
        return;
    }

    const QString pngPath = baseDir + QLatin1Char('/') + captureId + QStringLiteral("_vram.png");
    if (!m_vram->savePng(pngPath)) {
        emit emulationError(tr("Failed to write VRAM PNG: %1").arg(pngPath));
        return;
    }

    const QVector<uint16_t> cells = m_vram->mutablePixels();
    const QImage preview = m_vram->toImage(VramSnapshot::ViewMode::Native16);
    emit vramDumpReady(captureId, pngPath, cells, preview);
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
