#ifndef RIPPERHOOKS_H
#define RIPPERHOOKS_H

#include "CaptureBuffer.h"
#include "EmuHooks.h"
#include "Gp0CaptureStats.h"

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
    void onVramWrite(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     const uint16_t *pixels) override;
    void onVramRead(uint16_t x, uint16_t y, uint16_t w, uint16_t h) override;
    void onDrawMode(const DrawModeRecord &mode) override;
    void onDrawingOffset(int32_t ofx, int32_t ofy) override;

private:
    QString primDedupeKey(const PrimRecord &prim) const;

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
};

#endif // RIPPERHOOKS_H
