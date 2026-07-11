#ifndef EMUHOOKS_H
#define EMUHOOKS_H

#include "CapturedModelMesh.h"
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

    /**
     * Format-aware model-space scanner emission (#674). Called by `PsxTmdRamScanner` and
     * friends when they recognize a Sony SDK mesh structure (TMD/HMD/...) in RAM. The
     * mesh arrives already in editor world units so no GTE inverse is needed; the receiver
     * is expected to dedupe by `CapturedModelMesh::contentHash` and append to the capture
     * buffer. Returns `true` if accepted (newly added), `false` if dedupe rejected it.
     */
    virtual bool onModelMesh(const CapturedModelMesh &mesh)
    {
        (void)mesh;
        return false;
    }

    /**
     * In-core GTE record flush (#814): every RTPS/RTPT transform the
     * rip-instrumented core observed this frame, delivered once per frame
     * immediately before @ref onCoreFrameEnd. Fires on the worker thread
     * inside retro_run.
     */
    virtual void onGteRecords(const qtmesh_rip_gte_record *recs, uint32_t count)
    {
        (void)recs;
        (void)count;
    }

    /**
     * In-core GP0 draw hook (#815): one executed GP0 command (complete
     * packet) with per-vertex PGXP shadows carrying precise coords, view
     * depth and GTE record provenance. Fires during retro_run, before the
     * frame's GTE record flush — implementations must buffer draws until
     * @ref onCoreFrameEnd resolves them against the record table.
     */
    virtual void onGpuDrawTracked(const uint32_t *words, uint32_t wordCount,
                                  const qtmesh_rip_vertex_shadow *shadows, uint32_t shadowCount)
    {
        (void)words;
        (void)wordCount;
        (void)shadows;
        (void)shadowCount;
    }

    /** In-core frame boundary (#813): fired once per retro_run while armed. */
    virtual void onCoreFrameEnd(uint32_t frame) { (void)frame; }

    /**
     * True when the in-core GP0 stream delivered draws for the frame being
     * captured — Gp0HookDispatch uses this to suppress the heuristic RAM
     * GP0/GTE passes, which could only add duplicates and false positives
     * next to a true packet stream (#815).
     */
    virtual bool inCoreStreamActiveThisFrame() const { return false; }

    /** Live armed capture: shared dedupe keys across frames (nullptr when not accumulating). */
    virtual QSet<QString> *livePrimDedupeKeys() { return nullptr; }

    virtual void beginGpuCapturePass(bool accumulate) { (void)accumulate; }
    virtual void endGpuCapturePass(Gp0CaptureStats &stats) { (void)stats; }

    virtual int capturePrimCount() const { return 0; }
    virtual int lastDirectHookPrimCount() const { return 0; }
};

#endif // EMUHOOKS_H
