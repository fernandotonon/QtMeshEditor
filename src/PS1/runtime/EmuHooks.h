#ifndef EMUHOOKS_H
#define EMUHOOKS_H

#include "CaptureTypes.h"

#include <cstdint>

/**
 * GPU/GTE interception callbacks (Phase 2 — #418, #419).
 * Implemented by RipperHooks in the app; invoked from the emulator plugin worker thread.
 */
class EmuHooks
{
public:
    virtual ~EmuHooks() = default;

    virtual bool isCaptureEnabled() const { return false; }

    virtual void onFrameBegin() {}
    virtual void onFrameEnd() {}

    virtual uint32_t onGteMatrix(const MatrixRecord &matrix) { (void)matrix; return 0; }
    virtual void onGpuPrim(const PrimRecord &prim) { (void)prim; }
    virtual void onVramWrite(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels)
    {
        (void)x;
        (void)y;
        (void)w;
        (void)h;
        (void)pixels;
    }
    virtual void onVramRead(uint16_t x, uint16_t y, uint16_t w, uint16_t h) { (void)x; (void)y; (void)w; (void)h; }
    virtual void onDrawMode(const DrawModeRecord &mode) { (void)mode; }
};

#endif // EMUHOOKS_H
