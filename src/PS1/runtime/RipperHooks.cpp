#include "RipperHooks.h"

#include "GpuCommandParser.h"
#include "GteCapture.h"
#include "Gp0HookDispatch.h"
#include "PsxCaptureFilters.h"
#include "SentryReporter.h"
#include "VramSnapshot.h"

#include <QString>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

/** Per-frame packet cap for the in-core stream (#815). The RAM-scan cap of
 *  2048 exists to bound heuristic false positives; a true packet stream on
 *  retail games can legitimately exceed it. Overflow warns instead of
 *  silently truncating. */
constexpr int kMaxInCoreDrawsPerFrame = 16384;

/** Backstop epsilon when cross-checking a resolved GTE record against the
 *  vertex shadow that referenced it: a tag that survived a pure-move chain
 *  reproduces the record's precise coords bit-exactly, so anything beyond
 *  float noise means the value was mutated after transform (or the ring slot
 *  was reused) and the vertex must degrade to DepthOnly. */
constexpr float kRecordShadowEpsilon = 0.51f;

} // namespace

void RipperHooks::resetLiveCaptureState()
{
    m_liveDedupe.clear();
    m_directHookPrimPass = 0;
    m_pendingTrackedDraws.clear();
    m_gteRingToBuffer.clear();
    m_inCoreStreamSeen = false;
    m_inCorePrimsPass = 0;
    m_gteRecordsPass = 0;
    m_inCoreOverflowDropped = 0;
    m_inCoreCurrentMode = DrawModeRecord{};
    m_inCoreCurrentMatrixId = UINT32_MAX;
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
    stats.inCoreHookPrims = m_inCorePrimsPass;
    stats.gteRecords = m_gteRecordsPass;
    stats.inCoreStream = m_inCoreStreamSeen;
    stats.totalPrims = capturePrimCount();
    if (stats.directHookPrims > 0
        && stats.directHookPrims
               >= stats.ramOtPrims + stats.ramLinearPrims + stats.ramChainRootPrims)
        stats.primarySource = Gp0CaptureSource::DirectHook;
    // #815: the in-core packet stream outranks every screen-space heuristic
    // source — it is the true submission order with PGXP provenance. Gate on
    // the sticky stream flag, not this pass's count: once live dedupe has
    // seen the scene, later passes legitimately add 0 new prims.
    if (stats.inCoreStream && stats.totalPrims > 0)
        stats.primarySource = Gp0CaptureSource::InCoreHook;
    // #674: model-space meshes always beat any screen-space source for quality, so flip
    // the label here AFTER the DirectHook override above. Only actually emitted meshes
    // count — bare HMD candidate counts must not flip the label (#674 review).
    // (Kept above InCoreHook too: a recognised TMD is exact model data, while
    // in-core tracked coverage can be partial — #816 reports per-tier shares.)
    if (stats.ramTmdMeshes > 0 || stats.ramHmdMeshes > 0)
        stats.primarySource = Gp0CaptureSource::RamModelMesh;
    m_inCorePrimsPass = 0;
    m_gteRecordsPass = 0;
    m_lastStats = stats;
    m_lastStatsFresh = true;
    m_ramCaptureActive = false;
    if (!isCaptureEnabled())
        return;

    SentryReporter::addBreadcrumb(
        QStringLiteral("ps1.rip.capture.gp0_hook"),
        QStringLiteral(
            "source:%1 total:%2 hook:%3 ot:%4 linear:%5 chain:%6 tmd:%7 hmd:%8 hmd_cand:%9 live:%10"
            " incore:%11 gte:%12")
            .arg(stats.primarySourceLabel())
            .arg(stats.totalPrims)
            .arg(stats.directHookPrims)
            .arg(stats.ramOtPrims)
            .arg(stats.ramLinearPrims)
            .arg(stats.ramChainRootPrims)
            .arg(stats.ramTmdMeshes)
            .arg(stats.ramHmdMeshes)
            .arg(stats.ramHmdCandidates)
            .arg(stats.liveFrame ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(stats.inCoreHookPrims)
            .arg(stats.gteRecords));

    if (m_inCoreOverflowDropped > 0) {
        SentryReporter::addBreadcrumb(
            QStringLiteral("ps1.rip.capture.overflow"),
            QStringLiteral("in-core draws dropped by per-frame cap: %1")
                .arg(m_inCoreOverflowDropped));
        m_inCoreOverflowDropped = 0;
    }
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

bool RipperHooks::onModelMesh(const CapturedModelMesh &mesh)
{
    if (!isCaptureEnabled() || !m_buffer)
        return false;
    return m_buffer->addModelMesh(mesh);
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

void RipperHooks::onGteRecords(const qtmesh_rip_gte_record *recs, uint32_t count)
{
    if (!isCaptureEnabled() || !m_buffer || !recs)
        return;

    m_gteRecordsPass += static_cast<int>(count);

    for (uint32_t i = 0; i < count; ++i) {
        const GteRecordEntry &rec = recs[i];
        const uint32_t bufferIdx = m_buffer->addGteRecord(rec);
        if (bufferIdx != UINT32_MAX)
            m_gteRingToBuffer.insert(rec.seq % QTMESH_RIP_GTE_RING_ENTRIES, bufferIdx);

        // Feed each unique (rt, tr, ofx/ofy/h) into the existing matrix table
        // so per-draw matrix tagging and reconstruction benefit immediately
        // (#814). addMatrix dedupes by hash, so this is cheap for the common
        // case of thousands of records sharing a handful of matrices.
        MatrixRecord matrix{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                matrix.rt.m[r][c] = rec.rt[r * 3 + c];
        matrix.tr[0] = rec.tr[0];
        matrix.tr[1] = rec.tr[1];
        matrix.tr[2] = rec.tr[2];
        matrix.ofx = rec.ofx;
        matrix.ofy = rec.ofy;
        matrix.h = rec.h;
        onGteMatrix(matrix);
    }
}

void RipperHooks::onGpuDrawTracked(const uint32_t *words, uint32_t wordCount,
                                   const qtmesh_rip_vertex_shadow *shadows, uint32_t shadowCount)
{
    if (!isCaptureEnabled() || !m_buffer || !words || wordCount == 0)
        return;

    m_inCoreStreamSeen = true;

    if (m_pendingTrackedDraws.size() >= kMaxInCoreDrawsPerFrame) {
        ++m_inCoreOverflowDropped;
        return;
    }

    PendingTrackedDraw draw;
    draw.words.resize(static_cast<int>(wordCount));
    std::memcpy(draw.words.data(), words, wordCount * sizeof(uint32_t));
    if (shadows && shadowCount > 0) {
        draw.shadows.resize(static_cast<int>(shadowCount));
        std::memcpy(draw.shadows.data(), shadows, shadowCount * sizeof(qtmesh_rip_vertex_shadow));
    }
    m_pendingTrackedDraws.append(std::move(draw));
}

void RipperHooks::onCoreFrameEnd(uint32_t frame)
{
    if (!isCaptureEnabled() || !m_buffer) {
        m_pendingTrackedDraws.clear();
        return;
    }
    resolveTrackedDraws(frame);
}

bool RipperHooks::inCoreStreamActiveThisFrame() const
{
    return m_inCoreStreamSeen;
}

void RipperHooks::resolveTrackedDraws(uint32_t frame)
{
    const QVector<GteRecordEntry> &records = m_buffer->gteRecords();

    for (const PendingTrackedDraw &draw : m_pendingTrackedDraws) {
        const GpuCommandParser::Gp0Step step = GpuCommandParser::stepGp0(
            draw.words.constData(), static_cast<size_t>(draw.words.size()));
        if (step.wordsConsumed == 0 || !step.error.isEmpty())
            continue;

        // Draw-env words arrive interleaved in submission order — route them
        // through the existing handlers so per-draw TPAGE/semi-trans state is
        // exact (#815).
        if (step.hasDrawMode) {
            m_inCoreCurrentMode = step.drawMode;
            onDrawMode(m_inCoreCurrentMode);
            m_inCoreCurrentMatrixId = submitMatrixId();
        }
        if (step.hasDrawingOffset)
            onDrawingOffset(step.drawingOfx, step.drawingOfy);

        if (!step.hasPrim)
            continue;

        PrimRecord prim = step.prim;
        if (!PsxCaptureFilters::isOnScreenPrim(prim))
            continue;

        // Overlay per-vertex PGXP shadows (packet vertex order == parser
        // vertex order). Sprites deliver one shadow for the anchor vertex.
        const int overlayCount =
            std::min<int>(prim.vertexCount, draw.shadows.size());
        for (int v = 0; v < overlayCount; ++v) {
            const qtmesh_rip_vertex_shadow &sh = draw.shadows[v];
            PsxVertex &vert = prim.verts[v];
            if (!(sh.flags & QTMESH_RIP_SHADOW_XY_VALID))
                continue;
            vert.preciseX = sh.sx;
            vert.preciseY = sh.sy;
            if (sh.flags & QTMESH_RIP_SHADOW_W_VALID) {
                vert.viewW = sh.w;
                vert.provenance = static_cast<uint8_t>(PsxVertexProvenance::DepthOnly);
            }
            if (!(sh.flags & QTMESH_RIP_SHADOW_TAG_VALID))
                continue;
            const auto it = m_gteRingToBuffer.constFind(sh.gte_record);
            if (it == m_gteRingToBuffer.constEnd())
                continue; // record never delivered (ring wrap / pre-arm tag)
            const uint32_t bufferIdx = it.value();
            if (bufferIdx >= static_cast<uint32_t>(records.size()))
                continue;
            const GteRecordEntry &rec = records[static_cast<int>(bufferIdx)];
            // Backstop: the ring slot may have been reused since the shadow
            // was tagged, or the value was mutated after transform. A true
            // pure-move chain reproduces the record's precise coords exactly.
            if (std::fabs(rec.sx - sh.sx) > kRecordShadowEpsilon
                || std::fabs(rec.sy - sh.sy) > kRecordShadowEpsilon)
                continue; // stays DepthOnly (or None when w was invalid)
            vert.gteRecordIndex = bufferIdx;
            vert.provenance = static_cast<uint8_t>(PsxVertexProvenance::GteTracked);
        }

        if (prim.drawModeBits == 0 && m_inCoreCurrentMode.drawModeBits != 0)
            prim.drawModeBits = m_inCoreCurrentMode.drawModeBits;
        switch (prim.kind) {
        case PrimKind::TexturedTri:
        case PrimKind::TexturedQuad:
        case PrimKind::Sprite:
            break;
        default:
            prim.tpage = m_inCoreCurrentMode.tpage;
            prim.clut = m_inCoreCurrentMode.clut;
            break;
        }

        uint32_t matrixId = m_inCoreCurrentMatrixId;
        if (matrixId == UINT32_MAX)
            matrixId = submitMatrixId();
        if (matrixId == UINT32_MAX)
            matrixId = latestMatrixId();
        if (matrixId != UINT32_MAX)
            prim.matrixId = matrixId;

        prim.frame = frame;

        // In-core capture accumulates across frames by construction (the
        // stream fires every armed retro_run), so dedupe against the live
        // session set unconditionally.
        const QString key = primDedupeKey(prim);
        if (m_liveDedupe.contains(key))
            continue;
        m_liveDedupe.insert(key);

        m_buffer->addPrim(prim);
        ++m_inCorePrimsPass;
    }

    m_pendingTrackedDraws.clear();
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
