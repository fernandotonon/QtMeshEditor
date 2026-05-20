#include "RipperHooks.h"
#include "Gp0HookDispatch.h"
#include "VramSnapshot.h"

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
    if (!m_vram || w == 0 || h == 0)
        return;
    // GP0 0xC0 read-back hook (#418): record that VRAM was sampled (no CPU FIFO replay).
    (void)x;
    (void)y;
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
    Gp0HookDispatch::captureFrameFromSystemRam(ram, byteSize, this);
}
