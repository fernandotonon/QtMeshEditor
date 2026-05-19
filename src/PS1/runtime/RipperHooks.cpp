#include "RipperHooks.h"
#include "PsxGpuRamScanner.h"
#include "PsxGteRamScanner.h"
#include "VramSnapshot.h"

namespace {

void ensureCaptureProjectionMatrix(RipperHooks *hooks)
{
    if (!hooks || hooks->latestMatrixId() != UINT32_MAX)
        return;

    MatrixRecord matrix{};
    matrix.rt.m[0][0] = 1 << 12;
    matrix.rt.m[1][1] = 1 << 12;
    matrix.rt.m[2][2] = 1 << 12;
    matrix.h = 256;
    hooks->onGteMatrix(matrix);
}

} // namespace

bool RipperHooks::isCaptureEnabled() const
{
    return m_armed && m_armed->load(std::memory_order_acquire);
}

void RipperHooks::onFrameBegin()
{
    if (!isCaptureEnabled() || !m_buffer)
        return;
    m_buffer->beginFrame();
}

void RipperHooks::onFrameEnd()
{
    if (!isCaptureEnabled() || !m_buffer)
        return;
    m_buffer->endFrame();
}

uint32_t RipperHooks::onGteMatrix(const MatrixRecord &matrix)
{
    if (!isCaptureEnabled() || !m_buffer)
        return 0;
    return m_buffer->addMatrix(matrix);
}

void RipperHooks::onGpuPrim(const PrimRecord &prim)
{
    if (!isCaptureEnabled() || !m_buffer)
        return;
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
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

void RipperHooks::onDrawMode(const DrawModeRecord &mode)
{
    if (!isCaptureEnabled() || !m_buffer)
        return;
    m_buffer->addDrawMode(mode);
}

uint32_t RipperHooks::latestMatrixId() const
{
    if (!m_buffer || m_buffer->matrices().isEmpty())
        return UINT32_MAX;
    return static_cast<uint32_t>(m_buffer->matrices().size() - 1);
}

void RipperHooks::ingestSystemRamForGpuCapture(const uint8_t *ram, size_t byteSize)
{
    if (!isCaptureEnabled() || !ram || byteSize < 16)
        return;

    onFrameBegin();
    // Heuristic GTE matrix scan from RAM produces many false positives; use a stable
    // projection matrix for libretro captures until real GTE hooks land (#419).
    ensureCaptureProjectionMatrix(this);
    PsxGpuRamScanner::captureFromSystemRam(ram, byteSize, this);
    onFrameEnd();
}
