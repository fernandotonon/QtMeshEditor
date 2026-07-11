#ifndef RIPPERHOOKS_H
#define RIPPERHOOKS_H

#include "CaptureBuffer.h"
#include "EmuHooks.h"
#include "Gp0CaptureStats.h"

#include <QHash>
#include <QVector>

#include <atomic>
#include <cstdint>

class VramSnapshot;

/**
 * Concrete EmuHooks that records into CaptureBuffer when capture is armed (#418, #657).
 */
class RipperHooks final : public EmuHooks
{
public:
    void setArmedFlag(std::atomic<bool> *armed) { m_armed = armed; }
    void setBuffer(CaptureBuffer *buffer) { m_buffer = buffer; }
    void setVram(VramSnapshot *vram) { m_vram = vram; }

    bool isCaptureEnabled() const override;
    uint32_t latestMatrixId() const override;
    uint32_t submitMatrixId() const override;

    void ingestSystemRamForGpuCapture(const uint8_t *ram, size_t byteSize, bool scanGteRam = true,
                                      bool accumulate = false) override;
    int submitGp0Words(const uint32_t *words, size_t wordCount) override;
    int submitFifoChainsFromRam(const uint8_t *ram, size_t byteSize) override;

    QSet<QString> *livePrimDedupeKeys() override;
    void beginGpuCapturePass(bool accumulate) override;
    void endGpuCapturePass(Gp0CaptureStats &stats) override;
    int capturePrimCount() const override;
    int lastDirectHookPrimCount() const override;

    void resetLiveCaptureState();

    const Gp0CaptureStats &lastCaptureStats() const { return m_lastStats; }

    /**
     * Returns true iff @ref endGpuCapturePass was called since the last
     * @ref markCaptureStatsConsumed (or since construction). Used by the
     * session worker to detect stub-core paths that never run the GP0 capture
     * pass, so the breadcrumb / status bar don't surface stale stats from a
     * prior pass (#662 review).
     */
    bool lastCaptureStatsFresh() const { return m_lastStatsFresh; }
    void markCaptureStatsConsumed() { m_lastStatsFresh = false; }

    void onFrameBegin() override;
    void onFrameEnd() override;

    uint32_t onGteMatrix(const MatrixRecord &matrix) override;
    void onGpuPrim(const PrimRecord &prim) override;
    bool onModelMesh(const CapturedModelMesh &mesh) override;

    // In-core rip stream (#814/#815). All three fire on the worker thread
    // inside retro_run; draws are buffered until the frame's GTE record flush
    // arrives so gte_record ring indices can be resolved.
    void onGteRecords(const qtmesh_rip_gte_record *recs, uint32_t count) override;
    void onGpuDrawTracked(const uint32_t *words, uint32_t wordCount,
                          const qtmesh_rip_vertex_shadow *shadows, uint32_t shadowCount) override;
    void onCoreFrameEnd(uint32_t frame) override;
    bool inCoreStreamActiveThisFrame() const override;

    /** In-core prims dropped by the per-frame cap since the last stats pass. */
    int inCoreOverflowDropped() const { return m_inCoreOverflowDropped; }
    void onVramWrite(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     const uint16_t *pixels) override;
    void onVramRead(uint16_t x, uint16_t y, uint16_t w, uint16_t h) override;
    void onDrawMode(const DrawModeRecord &mode) override;
    void onDrawingOffset(int32_t ofx, int32_t ofy) override;

private:
    QString primDedupeKey(const PrimRecord &prim) const;
    void resolveTrackedDraws(uint32_t frame);

    struct PendingTrackedDraw {
        QVector<uint32_t> words;
        QVector<qtmesh_rip_vertex_shadow> shadows;
    };

    std::atomic<bool> *m_armed = nullptr;
    CaptureBuffer *m_buffer = nullptr;
    VramSnapshot *m_vram = nullptr;
    uint32_t m_latestMatrixId = UINT32_MAX;
    uint32_t m_submitMatrixId = UINT32_MAX;
    bool m_accumulatePass = false;
    bool m_clearPrimsOnFrameBegin = true;
    bool m_ramCaptureActive = false;
    QSet<QString> m_liveDedupe;
    int m_directHookPrimPass = 0;
    Gp0CaptureStats m_lastStats;
    bool m_lastStatsFresh = false;

    // In-core stream state (#814/#815).
    QVector<PendingTrackedDraw> m_pendingTrackedDraws;
    /** GTE ring slot → index into CaptureBuffer::gteRecords for the latest
     *  record seen at that slot. Persists across frames (a display list may
     *  reuse shadows tagged in an earlier frame); cleared with live state. */
    QHash<uint32_t, uint32_t> m_gteRingToBuffer;
    /** Sticky while armed: the fork's GP0 stream has delivered draws, so the
     *  heuristic RAM GP0/GTE passes stay suppressed (#815). */
    bool m_inCoreStreamSeen = false;
    int m_inCorePrimsPass = 0;
    int m_gteRecordsPass = 0;
    int m_inCoreOverflowDropped = 0;
    DrawModeRecord m_inCoreCurrentMode{};
    uint32_t m_inCoreCurrentMatrixId = UINT32_MAX;
};

#endif // RIPPERHOOKS_H
