#include "PS1RipWorker.h"
#include "CaptureBuffer.h"
#include "CaptureSnapshot.h"
#include "CaptureTypes.h"
#include "EmuCore.h"
#include "EmuCoreLoader.h"
#include "EmuFramebuffer.h"
#include "GpuCommandParser.h"
#include "PsxGoldenCapture.h"
#include "PsxVramMirrorMode.h"
#include "RipperHooks.h"
#include "SentryReporter.h"
#include "VramSnapshot.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRect>
#include <QSet>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QTimer>

#include <cstring>

namespace {

PsxVramMirrorMode effectiveVramMirrorMode(const EmuCore *core, const VramSnapshot *vram)
{
    if (!core)
        return PsxVramMirrorMode::Unknown;

    PsxVramMirrorMode mode = core->lastVramMirrorMode();
    if (mode != PsxVramMirrorMode::FramebufferFallback || !vram)
        return mode;

    const EmuFramebuffer &fb = core->framebuffer();
    if (!fb.isValid())
        return mode;

    if (vram->hasNonZeroOutsideRect(QRect(0, 0, fb.width, fb.height), 8))
        return PsxVramMirrorMode::Gp0Hybrid;
    return mode;
}

} // namespace

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
    m_frameTimer->setSingleShot(true);
    m_frameTimer->setTimerType(Qt::PreciseTimer);
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

void PS1RipWorker::setGoldenSceneId(const QString &sceneId)
{
    m_goldenSceneId = sceneId;
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
        const QString detail = m_core->lastError();
        emit emulationError(detail.isEmpty()
                                ? tr("Failed to load ISO: %1").arg(m_isoPath)
                                : detail);
        return;
    }

    m_core->setHooks(m_ripperHooks.get());
    m_core->reset();

    QString bootErr;
    if (!m_core->boot(&bootErr)) {
        m_core.reset();
        emit emulationError(bootErr.isEmpty() ? tr("Failed to boot emulator core") : bootErr);
        return;
    }

    if (m_startSuperseded.load(std::memory_order_acquire)) {
        m_startSuperseded.store(false, std::memory_order_release);
        return;
    }

    m_running = true;
    m_paused = false;
    scheduleNextFrame(0);
    emit emulationStarted(m_core ? m_core->coreId() : QString());
}

void PS1RipWorker::stopEmulation()
{
    clearStartCancel();

    if (!m_running && !m_frameTimer->isActive())
        return;

    m_frameTimer->stop();
    m_running = false;
    m_paused = false;
    m_captureArmed.store(false, std::memory_order_release);
    m_captureBuffer->clear();
    m_core.reset();
    emit emulationStopped();
}

void PS1RipWorker::pauseEmulation()
{
    if (!m_running)
        return;
    m_paused = !m_paused;
    if (m_paused) {
        m_frameTimer->stop();
    } else {
        scheduleNextFrame(0);
    }
}

void PS1RipWorker::stepFrame()
{
    if (!m_running || !m_core)
        return;
    m_core->runFrame();
    emit framePresented(framebufferToImage(m_core->framebuffer()), m_core->framebuffer().frameIndex);
}

void PS1RipWorker::scheduleNextFrame(int delayMs)
{
    if (!m_running || m_paused || !m_core || !m_frameTimer)
        return;
    m_frameTimer->start(qMax(0, delayMs));
}

void PS1RipWorker::runFrameTick()
{
    if (!m_running || m_paused || !m_core)
        return;

    QElapsedTimer frameClock;
    frameClock.start();

    m_core->runFrame();
    const EmuFramebuffer &fb = m_core->framebuffer();
    emit framePresented(framebufferToImage(fb), fb.frameIndex);

    if ((fb.frameIndex % 30) == 0 && m_vram && m_vram->hasVisibleContent(32)) {
        const QVector<uint16_t> cells = m_vram->mutablePixels();
        emit vramFrameUpdated(cells, m_vram->toImage(VramSnapshot::ViewMode::Native16));
    }

    // Throttled live capture-buffer stats for the status footer (#425). Every
    // 15 frames at the ~60 Hz target ≈ 4 Hz updates — fast enough that the
    // user sees prims tick up during a scene capture, slow enough that the
    // GUI thread isn't woken on every emulated frame. Uses default
    // (seq_cst) ordering — these reads are far from a hot path so the
    // tighter ordering pays for itself in simpler reasoning (SonarCloud
    // S8417 also prefers it over an explicit acquire here).
    if (m_captureArmed.load() && (fb.frameIndex % 15) == 0 && m_captureBuffer) {
        const QVector<PrimRecord> &prims = m_captureBuffer->prims();
        qint64 tris = 0;
        QSet<uint16_t> pages;
        pages.reserve(16);
        for (const PrimRecord &p : prims) {
            switch (p.kind) {
            case PrimKind::MonoTri:
            case PrimKind::ShadedTri:
            case PrimKind::TexturedTri:
                tris += 1;
                break;
            case PrimKind::MonoQuad:
            case PrimKind::ShadedQuad:
            case PrimKind::TexturedQuad:
            case PrimKind::Sprite:
                tris += 2;
                break;
            }
            if (p.kind == PrimKind::TexturedTri || p.kind == PrimKind::TexturedQuad
                || p.kind == PrimKind::Sprite)
                pages.insert(p.tpage);
        }
        const qint64 bytes =
            qint64(prims.size()) * qint64(sizeof(PrimRecord))
            + qint64(m_captureBuffer->matrices().size()) * qint64(sizeof(MatrixRecord))
            + qint64(m_captureBuffer->drawModes().size()) * qint64(sizeof(DrawModeRecord));
        emit captureProgress(qint64(prims.size()), tris,
                             static_cast<int>(pages.size()), bytes);
    }

    if (!m_running || m_paused || !m_core)
        return;

    constexpr int kTargetFrameMs = 16;
    const int delayMs = qMax(0, kTargetFrameMs - static_cast<int>(frameClock.elapsed()));
    scheduleNextFrame(delayMs);
}

void PS1RipWorker::setCaptureArmed(bool armed)
{
    m_captureArmed.store(armed, std::memory_order_release);
    if (armed) {
        m_ripperHooks->resetLiveCaptureState();
        if (!m_running)
            m_captureBuffer->clear();
    } else {
        m_captureBuffer->clear();
        m_ripperHooks->resetLiveCaptureState();
    }
}

void PS1RipWorker::setJoypadButton(unsigned port, unsigned buttonId, bool pressed)
{
    if (m_core)
        m_core->setJoypadButton(port, buttonId, pressed);
}

void PS1RipWorker::resetJoypad(unsigned port)
{
    if (m_core)
        m_core->resetJoypad(port);
}

void PS1RipWorker::finalizeFrameCapture()
{
    if (!m_running || !m_core) {
        emit sessionWarning(tr("No active emulation session — press Start and wait for gameplay"));
        return;
    }

    if (!m_captureArmed.load(std::memory_order_acquire)) {
        emit sessionWarning(tr("Capture is not armed"));
        return;
    }

    m_core->syncCaptureMirrors();
    if (m_captureBuffer->prims().isEmpty())
        m_core->runFrame();
    // Clear the freshness flag before ingest so we can tell whether the core
    // path actually ran a GP0 capture pass (libretro path) vs left
    // lastCaptureStats() stale (stub core / cores without GP0 hooks). Without
    // this, the breadcrumb + status bar can surface stats from a prior pass
    // and misattribute the source (#662 review).
    if (m_ripperHooks)
        m_ripperHooks->markCaptureStatsConsumed();
    m_core->ingestCaptureFrame();

    const QVector<PrimRecord> &prims = m_captureBuffer->prims();
    if (prims.isEmpty()) {
        emit sessionWarning(
            tr("No primitives captured — let the game render for a few seconds, keep Arm Capture on, "
               "then try again"));
        return;
    }

    const QString captureId = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString dir = QDir::temp().filePath(QStringLiteral("qtmesh_ps1_capture"));
    if (!QDir().mkpath(dir)) {
        emit sessionWarning(tr("Failed to create capture directory: %1").arg(dir));
        return;
    }

    const QString csvPath = dir + QLatin1Char('/') + captureId + QStringLiteral(".csv");
    QFile file(csvPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit sessionWarning(tr("Failed to write capture file: %1").arg(csvPath));
        return;
    }

    const QByteArray csv = GpuCommandParser::primsToCsv(prims).toUtf8();
    if (file.write(csv) != csv.size()) {
        emit sessionWarning(tr("Failed to write capture data: %1").arg(csvPath));
        return;
    }
    file.close();

    QString captureMsg = QStringLiteral("%1 prims:%2").arg(captureId).arg(prims.size());
    QString goldenId = m_goldenSceneId;
    if (goldenId.isEmpty())
        goldenId = PsxGoldenCapture::activeSceneId();
    if (!goldenId.isEmpty())
        captureMsg += QStringLiteral(" golden_id=%1").arg(goldenId);
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.capture"), captureMsg);
    const PsxVramMirrorMode vramMode = effectiveVramMirrorMode(m_core.get(), m_vram.get());
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.vram.sync"),
                                  QStringLiteral("mode=%1").arg(psxVramMirrorModeLabel(vramMode)));
    QVector<uint16_t> vramCells;
    if (m_vram && !m_vram->isEmpty())
        vramCells = m_vram->mutablePixels();

    Gp0CaptureStats stats;
    if (m_ripperHooks && m_ripperHooks->lastCaptureStatsFresh()) {
        stats = m_ripperHooks->lastCaptureStats();
    } else {
        // Stub-core path didn't run a GP0 capture pass — synthesize a minimal
        // stats record from the buffer so the breadcrumb reflects this
        // capture instead of a stale one (#662 review).
        stats.totalPrims = prims.size();
    }
    SentryReporter::addBreadcrumb(
        QStringLiteral("ps1.rip.capture.summary"),
        QStringLiteral("capture=%1 source=%2 total=%3 hook=%4 ot=%5 chain=%6 linear=%7 tmd=%8 "
                       "hmd=%9 hmd_cand=%10 legacy=%11")
            .arg(captureId)
            .arg(stats.primarySourceLabel())
            .arg(stats.totalPrims)
            .arg(stats.directHookPrims)
            .arg(stats.ramOtPrims)
            .arg(stats.ramChainRootPrims)
            .arg(stats.ramLinearPrims)
            .arg(stats.ramTmdMeshes)
            .arg(stats.ramHmdMeshes)
            .arg(stats.ramHmdCandidates)
            .arg(qEnvironmentVariableIsSet("QTMESH_PS1_GP0_RAM_LEGACY")
                         && qEnvironmentVariableIntValue("QTMESH_PS1_GP0_RAM_LEGACY") != 0
                     ? QStringLiteral("yes")
                     : QStringLiteral("no")));

    // Only log the modelmesh breadcrumb when actual model-mesh emissions happened.
    // ramHmdCandidates by itself is just a diagnostics count of plausible HMD magic
    // bytes — it does NOT indicate a successful capture (#674 review).
    if (stats.ramTmdMeshes > 0 || stats.ramHmdMeshes > 0) {
        SentryReporter::addBreadcrumb(
            QStringLiteral("ps1.rip.capture.modelmesh"),
            QStringLiteral("capture=%1 tmd=%2 hmd=%3 hmd_cand=%4 buffer_modelmeshes=%5")
                .arg(captureId)
                .arg(stats.ramTmdMeshes)
                .arg(stats.ramHmdMeshes)
                .arg(stats.ramHmdCandidates)
                .arg(m_captureBuffer ? m_captureBuffer->modelMeshes().size() : 0));
    }

    emit frameCaptureReady(captureId, CaptureSnapshot::fromBuffer(*m_captureBuffer, vramCells),
                           prims.size(), vramMode, stats);
}

void PS1RipWorker::dumpVram()
{
    if (!m_running || !m_core) {
        emit sessionWarning(tr("No active emulation session — press Start before dumping VRAM"));
        return;
    }

    m_core->runFrame();
    m_core->syncCaptureMirrors();

    if (!m_vram || !m_vram->hasVisibleContent(8)) {
        emit sessionWarning(
            tr("VRAM mirror is still empty — confirm the game is running (video in the viewport), "
               "then try again."));
        return;
    }

    const PsxVramMirrorMode vramMode = effectiveVramMirrorMode(m_core.get(), m_vram.get());
    if (vramMode == PsxVramMirrorMode::FramebufferFallback) {
        emit sessionWarning(
            tr("VRAM is framebuffer-only — textures may be missing. Use mednafen_psx_libretro "
               "(software renderer) in PS1Cores/; avoid beetle_psx_hw."));
    } else if (vramMode == PsxVramMirrorMode::Gp0Hybrid) {
        emit sessionWarning(
            tr("VRAM is partial (framebuffer + GP0 patches). Full texture pages require software "
               "renderer VRAM access."));
    }

    const QString captureId = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString baseDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/ps1_rip/captures");
    if (!QDir().mkpath(baseDir)) {
        emit sessionWarning(tr("Failed to create capture directory: %1").arg(baseDir));
        return;
    }

    const QString pngPath = baseDir + QLatin1Char('/') + captureId + QStringLiteral("_vram.png");
    if (!m_vram->savePng(pngPath)) {
        emit sessionWarning(tr("Failed to write VRAM PNG: %1").arg(pngPath));
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
