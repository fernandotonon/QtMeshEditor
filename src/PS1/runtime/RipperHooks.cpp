#include "RipperHooks.h"

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
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)pixels;
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
