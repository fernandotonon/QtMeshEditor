#ifndef RIPPERHOOKS_H
#define RIPPERHOOKS_H

#include "CaptureBuffer.h"
#include "EmuHooks.h"

#include <atomic>

class VramSnapshot;

/**
 * Concrete EmuHooks that records into CaptureBuffer when capture is armed (#418).
 */
class RipperHooks final : public EmuHooks
{
public:
    void setArmedFlag(std::atomic<bool> *armed) { m_armed = armed; }
    void setBuffer(CaptureBuffer *buffer) { m_buffer = buffer; }
    void setVram(VramSnapshot *vram) { m_vram = vram; }

    bool isCaptureEnabled() const override;

    void onFrameBegin() override;
    void onFrameEnd() override;

    uint32_t onGteMatrix(const MatrixRecord &matrix) override;
    void onGpuPrim(const PrimRecord &prim) override;
    void onVramWrite(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     const uint16_t *pixels) override;
    void onVramRead(uint16_t x, uint16_t y, uint16_t w, uint16_t h) override;
    void onDrawMode(const DrawModeRecord &mode) override;

private:
    std::atomic<bool> *m_armed = nullptr;
    CaptureBuffer *m_buffer = nullptr;
    VramSnapshot *m_vram = nullptr;
};

#endif // RIPPERHOOKS_H
