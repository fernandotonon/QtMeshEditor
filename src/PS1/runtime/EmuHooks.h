#ifndef EMUHOOKS_H
#define EMUHOOKS_H

#include "CaptureTypes.h"
#include "Gp0CaptureStats.h"

#include <QSet>
#include <QString>

#include <cstddef>
#include <cstdint>

/**
 * GPU/GTE interception callbacks (Phase 2 — #418, #419; GP0 hook path #657).
 * Implemented by RipperHooks in the app; invoked from the emulator plugin worker thread.
 */
class EmuHooks
{
public:
    virtual ~EmuHooks() = default;

    virtual bool isCaptureEnabled() const { return false; }
    virtual uint32_t latestMatrixId() const { return UINT32_MAX; }

    /** Matrix bound at the last drawing-environment command (#658). */
    virtual uint32_t submitMatrixId() const { return UINT32_MAX; }

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

    /** GP0 0xE4 drawing offset — updates the active submit matrix OFX/OFY (#658). */
    virtual void onDrawingOffset(int32_t ofx, int32_t ofy)
    {
        (void)ofx;
        (void)ofy;
    }

    /** Core GP0 FIFO path (#657): sequential GP0 packets as submitted by the plugin. */
    virtual int submitGp0Words(const uint32_t *words, size_t wordCount)
    {
        (void)words;
        (void)wordCount;
        return 0;
    }

    /** Libretro path: scan main RAM for GP0 packets when capture is armed (#418, #657). */
    virtual void ingestSystemRamForGpuCapture(const uint8_t *ram, size_t byteSize, bool scanGteRam = true,
                                            bool accumulate = false)
    {
        (void)ram;
        (void)byteSize;
        (void)scanGteRam;
        (void)accumulate;
    }

    /**
     * Live FIFO bridge (#662): scan main RAM for contiguous GP0 DMA chains
     * and submit them through @ref submitGp0Words so prims are tagged as
     * `Gp0CaptureSource::DirectHook`. Returns prims dispatched (informational).
     */
    virtual int submitFifoChainsFromRam(const uint8_t *ram, size_t byteSize)
    {
        (void)ram;
        (void)byteSize;
        return 0;
    }

    /** Live armed capture: shared dedupe keys across frames (nullptr when not accumulating). */
    virtual QSet<QString> *livePrimDedupeKeys() { return nullptr; }

    virtual void beginGpuCapturePass(bool accumulate) { (void)accumulate; }
    virtual void endGpuCapturePass(Gp0CaptureStats &stats) { (void)stats; }

    virtual int capturePrimCount() const { return 0; }
    virtual int lastDirectHookPrimCount() const { return 0; }
};

#endif // EMUHOOKS_H
