#include "RipperHooks.h"

#include "GteCapture.h"
#include "Gp0HookDispatch.h"
#include "SentryReporter.h"
#include "VramSnapshot.h"

#include <QString>

void RipperHooks::resetLiveCaptureState()
{
    m_liveDedupe.clear();
    m_directHookPrimPass = 0;
}

bool RipperHooks::isCaptureEnabled() const
{
    return m_armed && m_armed->load(std::memory_order_acquire);
}

void RipperHooks::beginGpuCapturePass(bool accumulate)
{
    m_accumulatePass = accumulate && isCaptureEnabled();
    m_clearPrimsOnFrameBegin = !m_accumulatePass;
    m_ramCaptureActive = true;
    m_directHookPrimPass = 0;
}

void RipperHooks::endGpuCapturePass(Gp0CaptureStats &stats)
{
    stats.directHookPrims = m_directHookPrimPass;
    stats.totalPrims = capturePrimCount();
    if (stats.directHookPrims > 0
        && stats.directHookPrims
               >= stats.ramOtPrims + stats.ramLinearPrims + stats.ramChainRootPrims)
        stats.primarySource = Gp0CaptureSource::DirectHook;
    m_lastStats = stats;
    m_lastStatsFresh = true;
    m_ramCaptureActive = false;
    if (!isCaptureEnabled())
        return;

    SentryReporter::addBreadcrumb(
        QStringLiteral("ps1.rip.capture.gp0_hook"),
        QStringLiteral("source:%1 total:%2 hook:%3 ot:%4 linear:%5 chain:%6 live:%7")
            .arg(stats.primarySourceLabel())
            .arg(stats.totalPrims)
            .arg(stats.directHookPrims)
            .arg(stats.ramOtPrims)
            .arg(stats.ramLinearPrims)
            .arg(stats.ramChainRootPrims)
            .arg(stats.liveFrame ? QStringLiteral("yes") : QStringLiteral("no")));
}

int RipperHooks::capturePrimCount() const
{
    return m_buffer ? m_buffer->prims().size() : 0;
}

int RipperHooks::lastDirectHookPrimCount() const
{
    return m_directHookPrimPass;
}

QSet<QString> *RipperHooks::livePrimDedupeKeys()
{
    return m_accumulatePass ? &m_liveDedupe : nullptr;
}

void RipperHooks::onFrameBegin()
{
    if (!isCaptureEnabled() || !m_buffer)
        return;
    m_latestMatrixId = UINT32_MAX;
    m_submitMatrixId = UINT32_MAX;
    if (m_clearPrimsOnFrameBegin)
        m_buffer->beginFrame();
}

void RipperHooks::onFrameEnd()
{
    if (!isCaptureEnabled() || !m_buffer)
        return;
    if (!m_accumulatePass)
        m_buffer->endFrame();
}

uint32_t RipperHooks::onGteMatrix(const MatrixRecord &matrix)
{
    if (!isCaptureEnabled() || !m_buffer)
        return UINT32_MAX;
    m_latestMatrixId = m_buffer->addMatrix(matrix);
    return m_latestMatrixId;
}

QString RipperHooks::primDedupeKey(const PrimRecord &prim) const
{
    return Gp0HookDispatch::primDedupeKey(prim);
}

void RipperHooks::onGpuPrim(const PrimRecord &prim)
{
    if (!isCaptureEnabled() || !m_buffer)
        return;

    if (!m_ramCaptureActive)
        ++m_directHookPrimPass;

    if (m_accumulatePass) {
        const QString key = primDedupeKey(prim);
        if (m_liveDedupe.contains(key))
            return;
        m_liveDedupe.insert(key);
    }

    m_buffer->addPrim(prim);
}

void RipperHooks::onVramWrite(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels)
{
    if (!m_vram || !pixels)
        return;
    m_vram->writeRect(x, y, w, h, pixels);
}

void RipperHooks::onVramRead(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (!m_vram || w == 0 || h == 0)
        return;
    (void)x;
    (void)y;
}

void RipperHooks::onDrawMode(const DrawModeRecord &mode)
{
    if (!isCaptureEnabled() || !m_buffer)
        return;
    m_buffer->addDrawMode(mode);
    if (m_latestMatrixId != UINT32_MAX)
        m_submitMatrixId = m_latestMatrixId;
}

void RipperHooks::onDrawingOffset(int32_t ofx, int32_t ofy)
{
    if (!isCaptureEnabled() || !m_buffer || m_submitMatrixId == UINT32_MAX)
        return;
    if (m_submitMatrixId >= static_cast<uint32_t>(m_buffer->matrices().size()))
        return;

    MatrixRecord matrix = m_buffer->matrices()[static_cast<int>(m_submitMatrixId)];
    matrix.ofx = ofx;
    matrix.ofy = ofy;
    matrix.hash = GteCapture::hashMatrix(matrix);
    m_submitMatrixId = m_buffer->addMatrix(matrix);
}

uint32_t RipperHooks::latestMatrixId() const
{
    return m_latestMatrixId;
}

uint32_t RipperHooks::submitMatrixId() const
{
    return m_submitMatrixId;
}

void RipperHooks::ingestSystemRamForGpuCapture(const uint8_t *ram, size_t byteSize, bool scanGteRam,
                                               bool accumulate)
{
    Gp0HookDispatch::captureFrameFromSystemRam(ram, byteSize, this, scanGteRam, accumulate);
}

int RipperHooks::submitGp0Words(const uint32_t *words, size_t wordCount)
{
    if (!isCaptureEnabled())
        return 0;
    return Gp0HookDispatch::submitGp0Words(words, wordCount, this);
}

int RipperHooks::submitFifoChainsFromRam(const uint8_t *ram, size_t byteSize)
{
    if (!isCaptureEnabled())
        return 0;
    // Temporarily clear the RAM-capture flag so the bridge's prims arrive via
    // submitGp0Words → onGpuPrim with m_ramCaptureActive=false and bump
    // m_directHookPrimPass (Gp0CaptureSource::DirectHook for #662 attribution).
    // We may be called from inside a wrapping captureFromSystemRam pass, so
    // restore the prior value to keep the merged RAM scan working unchanged.
    const bool wasRamActive = m_ramCaptureActive;
    m_ramCaptureActive = false;
    const int dispatched =
        Gp0HookDispatch::submitChainsFromRam(ram, byteSize, this, livePrimDedupeKeys());
    m_ramCaptureActive = wasRamActive;
    return dispatched;
}
