#include "Gp0HookDispatch.h"

#include <QtGlobal>

#include "EmuHooks.h"
#include "PsxCaptureFilters.h"
#include "PsxGp0ChainRootScanner.h"
#include "PsxGp0Opcode.h"
#include "PsxGteInstructionCapture.h"
#include "PsxGteRamScanner.h"
#include "PsxHmdRamScanner.h"
#include "PsxOrderingTableScanner.h"
#include "PsxTmdRamScanner.h"

#include <QSet>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cstring>

namespace {

constexpr int kMaxPrimsPerFrame = 2048;

void ensureCaptureProjectionMatrix(EmuHooks *hooks)
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

void applyDrawMode(PrimRecord &prim, const DrawModeRecord &mode)
{
    if (prim.drawModeBits == 0 && mode.drawModeBits != 0)
        prim.drawModeBits = mode.drawModeBits;

    switch (prim.kind) {
    case PrimKind::TexturedTri:
    case PrimKind::TexturedQuad:
    case PrimKind::Sprite:
        break;
    default:
        prim.tpage = mode.tpage;
        prim.clut = mode.clut;
        break;
    }
}

Gp0CaptureSource pickPrimarySource(const Gp0CaptureStats &stats)
{
    int best = 0;
    Gp0CaptureSource source = Gp0CaptureSource::None;
    if (stats.directHookPrims > best) {
        best = stats.directHookPrims;
        source = Gp0CaptureSource::DirectHook;
    }
    if (stats.ramOtPrims > best) {
        best = stats.ramOtPrims;
        source = Gp0CaptureSource::RamOrderingTable;
    }
    if (stats.ramChainRootPrims > best) {
        best = stats.ramChainRootPrims;
        source = Gp0CaptureSource::RamChainRoot;
    }
    if (stats.ramLinearPrims > best) {
        source = Gp0CaptureSource::RamLinear;
    }
    return source;
}

/** Model-space meshes always beat screen-space prims for quality, so if any TMD or HMD
 *  surfaced this frame the primary label flips to ram_model_mesh regardless of the GP0
 *  prim count winner from `pickPrimarySource`. (#674) */
Gp0CaptureSource promoteModelMeshSource(const Gp0CaptureStats &stats, Gp0CaptureSource fallback)
{
    if (stats.ramTmdMeshes > 0 || stats.ramHmdMeshes > 0)
        return Gp0CaptureSource::RamModelMesh;
    return fallback;
}

QString primDedupeKeyImpl(const PrimRecord &prim)
{
    QString key = QStringLiteral("%1|%2").arg(static_cast<int>(prim.kind)).arg(prim.vertexCount);
    for (int v = 0; v < 4; ++v)
        key += QStringLiteral("|%1,%2").arg(prim.verts[v].x).arg(prim.verts[v].y);
    key += QStringLiteral("|%1,%2|%3|%4")
               .arg(prim.tpage)
               .arg(prim.clut)
               .arg(prim.semiTrans)
               .arg(prim.matrixId);
    return key;
}

} // namespace

QString Gp0HookDispatch::primDedupeKey(const PrimRecord &prim)
{
    return primDedupeKeyImpl(prim);
}

uint32_t matrixIdForGpuSubmit(EmuHooks *hooks, uint32_t currentMatrixId)
{
    if (currentMatrixId != UINT32_MAX)
        return currentMatrixId;
    const uint32_t submit = hooks->submitMatrixId();
    if (submit != UINT32_MAX)
        return submit;
    return hooks->latestMatrixId();
}

void Gp0HookDispatch::dispatchStep(const GpuCommandParser::Gp0Step &step, EmuHooks *hooks,
                                   DrawModeRecord &currentMode, uint32_t &currentMatrixId,
                                   int &primCount)
{
    if (!hooks || !hooks->isCaptureEnabled())
        return;

    if (step.hasDrawMode) {
        currentMode = step.drawMode;
        hooks->onDrawMode(currentMode);
        currentMatrixId = hooks->submitMatrixId();
    }

    if (step.hasDrawingOffset)
        hooks->onDrawingOffset(step.drawingOfx, step.drawingOfy);

    if (step.hasVramWrite && step.vramPixels)
        hooks->onVramWrite(step.vramX, step.vramY, step.vramW, step.vramH, step.vramPixels);

    if (step.hasVramRead)
        hooks->onVramRead(step.vramReadX, step.vramReadY, step.vramReadW, step.vramReadH);

    if (!step.hasPrim || primCount >= kMaxPrimsPerFrame)
        return;

    PrimRecord prim = step.prim;
    if (!PsxCaptureFilters::isOnScreenPrim(prim))
        return;

    applyDrawMode(prim, currentMode);
    const uint32_t matrixId = matrixIdForGpuSubmit(hooks, currentMatrixId);
    if (matrixId != UINT32_MAX)
        prim.matrixId = matrixId;

    hooks->onGpuPrim(prim);
    ++primCount;
}

int Gp0HookDispatch::submitGp0Words(const uint32_t *words, size_t wordCount, EmuHooks *hooks,
                                    int maxPrims)
{
    if (!words || wordCount == 0 || !hooks || !hooks->isCaptureEnabled())
        return 0;

    // -1 (default) means "use the per-frame cap". Any positive value is
    // clamped to the per-frame cap so callers can chain submit passes and
    // share a single per-frame budget (#662).
    const int limit = (maxPrims < 0)
                          ? kMaxPrimsPerFrame
                          : std::min(maxPrims, kMaxPrimsPerFrame);
    if (limit <= 0)
        return 0;

    DrawModeRecord currentMode{};
    uint32_t currentMatrixId = UINT32_MAX;
    int primCount = 0;
    size_t offset = 0;
    while (offset < wordCount) {
        if (primCount >= limit)
            break;

        const size_t remaining = wordCount - offset;
        const GpuCommandParser::Gp0Step step = GpuCommandParser::stepGp0(words + offset, remaining);
        if (step.wordsConsumed == 0)
            break;
        if (!step.error.isEmpty()) {
            offset += 1;
            continue;
        }

        dispatchStep(step, hooks, currentMode, currentMatrixId, primCount);
        offset += step.wordsConsumed;
    }
    return primCount;
}

namespace {

// Probe a single chain root: returns the linked packet count + the byte range
// that covers every packet (so we can slice it as one submitGp0Words call).
// Mirrors PsxGp0ChainRootScanner::estimateChainPackets but also returns the
// last-packet word count so we can size the slice exactly.
struct ChainProbe {
    int packets = 0;
    uint32_t startByte = 0;
    size_t totalWords = 0;
};

ChainProbe probeChainRoot(const uint8_t *ram, size_t byteSize, uint32_t startAddr)
{
    ChainProbe out;
    out.startByte = startAddr;

    uint32_t addr = startAddr;
    for (int guard = 0; guard < 64; ++guard) {
        if (addr + 4 > byteSize || (addr % 4) != 0)
            break;

        uint32_t word = 0;
        std::memcpy(&word, ram + addr, sizeof(word));
        if (!psxLooksLikeGp0Opcode(word))
            break;

        const size_t remainingWords = (byteSize - addr) / 4;
        const auto *words = reinterpret_cast<const uint32_t *>(ram + addr);
        const GpuCommandParser::Gp0Step step =
            GpuCommandParser::stepGp0(words, remainingWords);
        if (step.wordsConsumed == 0 || !step.error.isEmpty())
            break;

        out.packets += 1;
        out.totalWords += step.wordsConsumed;

        if ((word & 1u) == 0)
            break;

        const uint32_t nextAddr = psxGp0TagNextByteAddr(word);
        if (nextAddr == addr || nextAddr + 4 > byteSize || (nextAddr % 4) != 0)
            break;
        // Live FIFO bridge: we only submit packets that are contiguous in RAM,
        // because submitGp0Words takes a flat word slice. Chains that hop to a
        // non-contiguous address fall back to the merged RAM scanner.
        if (nextAddr != addr + static_cast<uint32_t>(step.wordsConsumed) * 4u)
            break;
        addr = nextAddr;
    }
    return out;
}

bool chainOverlaps(uint32_t aStart, size_t aWords, uint32_t bStart, size_t bWords)
{
    const uint32_t aEnd = aStart + static_cast<uint32_t>(aWords) * 4u;
    const uint32_t bEnd = bStart + static_cast<uint32_t>(bWords) * 4u;
    return !(aEnd <= bStart || bEnd <= aStart);
}

} // namespace

int Gp0HookDispatch::submitChainsFromRam(const uint8_t *ram, size_t byteSize, EmuHooks *hooks,
                                         QSet<QString> *seenPrimKeys)
{
    if (!ram || byteSize < 64 || !hooks || !hooks->isCaptureEnabled())
        return 0;

    constexpr int kMaxRootsToWalk = 48;
    constexpr int kMinChainPackets = 2;
    constexpr size_t kScanStrideBytes = 16;

    // PS1 main RAM is 2 MiB. We only walk the lower 512 KiB by default to match
    // PsxGp0ChainRootScanner's budget — large enough for typical game GPU
    // double-buffer + display list regions, small enough to keep per-frame cost
    // sub-millisecond on commodity hardware.
    const size_t scanLimit = byteSize > (512u * 1024u) ? (512u * 1024u) : byteSize;

    QVector<ChainProbe> candidates;
    candidates.reserve(64);
    for (size_t off = 0; off + 4 <= scanLimit; off += kScanStrideBytes) {
        uint32_t word = 0;
        std::memcpy(&word, ram + off, sizeof(word));
        if ((word & 1u) == 0u)
            continue;
        if (!psxLooksLikeGp0Opcode(word))
            continue;

        const ChainProbe probe = probeChainRoot(ram, byteSize, static_cast<uint32_t>(off));
        if (probe.packets < kMinChainPackets)
            continue;

        candidates.append(probe);
    }

    if (candidates.isEmpty())
        return 0;

    std::sort(candidates.begin(), candidates.end(),
              [](const ChainProbe &a, const ChainProbe &b) {
                  return a.packets > b.packets;
              });

    // Dedupe key set is accepted for API parity with captureFromSystemRam, but
    // we rely on RipperHooks::m_liveDedupe for cross-pass dedupe between this
    // chain bridge and the subsequent merged-RAM scan in the same accumulate
    // pass — both go through onGpuPrim which consults that set.
    Q_UNUSED(seenPrimKeys);

    int totalDispatched = 0;
    QVector<ChainProbe> walked;
    walked.reserve(kMaxRootsToWalk);

    for (const ChainProbe &probe : candidates) {
        if (walked.size() >= kMaxRootsToWalk)
            break;
        // Per-frame primitive budget shared across every chain dispatched in
        // this pass (#662 review). Without this, each submitGp0Words call had
        // its own 2048-prim cap, so scenes with many candidate roots could
        // ingest several × the intended budget in a single frame.
        if (totalDispatched >= kMaxPrimsPerFrame)
            break;

        bool overlaps = false;
        for (const ChainProbe &prior : walked) {
            if (chainOverlaps(probe.startByte, probe.totalWords, prior.startByte, prior.totalWords)) {
                overlaps = true;
                break;
            }
        }
        if (overlaps)
            continue;

        const auto *chainWords =
            reinterpret_cast<const uint32_t *>(ram + probe.startByte);
        const int remainingBudget = kMaxPrimsPerFrame - totalDispatched;
        // submitGp0Words is the hook code path (#657): RipperHooks tags prims
        // dispatched outside the m_ramCaptureActive window as DirectHook, so
        // these prims surface in Gp0CaptureStats as gp0_hook (#662).
        const int dispatched =
            Gp0HookDispatch::submitGp0Words(chainWords, probe.totalWords, hooks, remainingBudget);

        totalDispatched += dispatched;
        walked.append(probe);
    }

    return totalDispatched;
}

void Gp0HookDispatch::captureLinearScan(const uint8_t *ram, size_t byteSize, EmuHooks *hooks,
                                        QSet<QString> &seen, DrawModeRecord &currentMode,
                                        uint32_t &currentMatrixId, int &primCount)
{
    if (!ram || byteSize < 16 || !hooks)
        return;

    const size_t wordCount = byteSize / 4;

    for (size_t offset = 0; offset < wordCount;) {
        if (primCount >= kMaxPrimsPerFrame)
            break;

        uint32_t word = 0;
        std::memcpy(&word, ram + offset * 4, sizeof(word));
        if (!psxLooksLikeGp0Opcode(word)) {
            ++offset;
            continue;
        }

        const size_t remaining = wordCount - offset;
        const auto *words = reinterpret_cast<const uint32_t *>(ram + offset * 4);
        const GpuCommandParser::Gp0Step step = GpuCommandParser::stepGp0(words, remaining);
        if (step.wordsConsumed == 0) {
            ++offset;
            continue;
        }

        if (!step.error.isEmpty()) {
            offset += 1;
            continue;
        }

        if (step.hasPrim) {
            PrimRecord prim = step.prim;
            if (!PsxCaptureFilters::isOnScreenPrim(prim)) {
                offset += step.wordsConsumed;
                continue;
            }

            applyDrawMode(prim, currentMode);
            const uint32_t matrixId = matrixIdForGpuSubmit(hooks, currentMatrixId);
            if (matrixId != UINT32_MAX)
                prim.matrixId = matrixId;

            const QString key = primDedupeKeyImpl(prim);
            if (seen.contains(key)) {
                offset += step.wordsConsumed;
                continue;
            }
            seen.insert(key);
        }

        dispatchStep(step, hooks, currentMode, currentMatrixId, primCount);
        offset += step.wordsConsumed;
    }
}

Gp0CaptureStats Gp0HookDispatch::captureFromSystemRam(const uint8_t *ram, size_t byteSize,
                                                      EmuHooks *hooks, QSet<QString> *seenPrimKeys)
{
    Gp0CaptureStats stats;
    if (!ram || byteSize < 16 || !hooks || !hooks->isCaptureEnabled())
        return stats;

    QSet<QString> localSeen;
    QSet<QString> &seen = seenPrimKeys ? *seenPrimKeys : localSeen;
    DrawModeRecord currentMode{};
    uint32_t currentMatrixId = UINT32_MAX;
    int primCount = hooks->capturePrimCount();

    const int otBefore = primCount;
    PsxOrderingTableScanner::captureFromOrderingTables(ram, byteSize, hooks, &seen, primCount);
    stats.ramOtPrims = primCount - otBefore;

    stats.ramChainRootPrims =
        PsxGp0ChainRootScanner::captureFromChainRoots(ram, byteSize, hooks, &seen, primCount);

    const int linearBefore = primCount;
    captureLinearScan(ram, byteSize, hooks, seen, currentMode, currentMatrixId, primCount);
    stats.ramLinearPrims = primCount - linearBefore;

    stats.totalPrims = primCount;
    stats.primarySource = promoteModelMeshSource(stats, pickPrimarySource(stats));
    return stats;
}

Gp0CaptureStats Gp0HookDispatch::captureFromSystemRamLegacy(const uint8_t *ram, size_t byteSize,
                                                            EmuHooks *hooks)
{
    Gp0CaptureStats stats;
    if (!ram || byteSize < 16 || !hooks || !hooks->isCaptureEnabled())
        return stats;

    QSet<QString> seen;
    DrawModeRecord currentMode{};
    uint32_t currentMatrixId = UINT32_MAX;
    int primCount = 0;

    const int otBefore = primCount;
    PsxOrderingTableScanner::captureFromOrderingTables(ram, byteSize, hooks, &seen, primCount);
    stats.ramOtPrims = primCount - otBefore;
    if (stats.ramOtPrims > 0) {
        stats.totalPrims = primCount;
        stats.primarySource = Gp0CaptureSource::RamOrderingTable;
        return stats;
    }

    const int linearBefore = primCount;
    captureLinearScan(ram, byteSize, hooks, seen, currentMode, currentMatrixId, primCount);
    stats.ramLinearPrims = primCount - linearBefore;
    stats.totalPrims = primCount;
    stats.primarySource =
        stats.ramLinearPrims > 0 ? Gp0CaptureSource::RamLinear : Gp0CaptureSource::None;
    return stats;
}

namespace {

constexpr size_t kPs1MainRamBytes = 2u * 1024u * 1024u;

size_t clampPs1RamSize(size_t byteSize)
{
    return byteSize > kPs1MainRamBytes ? kPs1MainRamBytes : byteSize;
}

} // namespace

Gp0CaptureStats Gp0HookDispatch::captureFrameFromSystemRam(const uint8_t *ram, size_t byteSize,
                                                           EmuHooks *hooks, bool scanGteRam,
                                                           bool accumulate)
{
    Gp0CaptureStats stats;
    if (!hooks || !hooks->isCaptureEnabled() || !ram || byteSize < 16)
        return stats;

    const size_t scanSize = clampPs1RamSize(byteSize);
    const bool liveFrame = accumulate;

    hooks->beginGpuCapturePass(accumulate);
    hooks->onFrameBegin();
    if (scanGteRam) {
        PsxGteInstructionCapture::captureFromSystemRam(ram, scanSize, hooks);
        PsxGteRamScanner::captureFromSystemRam(ram, scanSize, hooks);
    }
    ensureCaptureProjectionMatrix(hooks);

    // #674 model-space RAM scanners: look for Sony SDK TMD (0x00000041) blobs in main RAM
    // and emit them as fully-formed model-space meshes via EmuHooks::onModelMesh. Unlike
    // the screen-space GP0 path below, these bypass MeshReconstructor::screenToModel
    // entirely — they're the only reliable way to recover model-space geometry from
    // closed-source retail games until #676 (forked mednafen with in-core GTE hook) lands.
    // Disable per-format with QTMESH_PS1_TMD_SCANNER=0; HMD is opt-in via QTMESH_PS1_HMD_SCANNER=1.
    const bool tmdScannerDisabled = qEnvironmentVariableIsSet("QTMESH_PS1_TMD_SCANNER")
                                    && qEnvironmentVariableIntValue("QTMESH_PS1_TMD_SCANNER") == 0;
    int tmdMeshes = 0;
    int hmdMeshes = 0;
    if (!tmdScannerDisabled)
        tmdMeshes = PsxTmdRamScanner::captureFromSystemRam(ram, scanSize, hooks);
    hmdMeshes = PsxHmdRamScanner::captureFromSystemRam(ram, scanSize, hooks);

    // #662 live FIFO bridge: route contiguous RAM-resident DMA chains through
    // submitGp0Words so each prim is attributed to Gp0CaptureSource::DirectHook
    // before the merged RAM scan runs. The bridge runs in-pass; RipperHooks
    // toggles m_ramCaptureActive so the DirectHook counter only counts the
    // bridge's prims, not the merged scan's. Disable with
    // QTMESH_PS1_GP0_FIFO_BRIDGE=0 for the legacy RAM-only baseline.
    const bool fifoBridgeDisabled = qEnvironmentVariableIsSet("QTMESH_PS1_GP0_FIFO_BRIDGE")
                                    && qEnvironmentVariableIntValue("QTMESH_PS1_GP0_FIFO_BRIDGE") == 0;
    if (!fifoBridgeDisabled)
        hooks->submitFifoChainsFromRam(ram, scanSize);

    QSet<QString> *seen = hooks->livePrimDedupeKeys();
    if (qEnvironmentVariableIsSet("QTMESH_PS1_GP0_RAM_LEGACY")
        && qEnvironmentVariableIntValue("QTMESH_PS1_GP0_RAM_LEGACY") != 0) {
        stats = captureFromSystemRamLegacy(ram, scanSize, hooks);
    } else {
        stats = captureFromSystemRam(ram, scanSize, hooks, seen);
    }
    stats.ramTmdMeshes = tmdMeshes;
    stats.ramHmdMeshes = hmdMeshes;
    stats.liveFrame = liveFrame;

    hooks->onFrameEnd();
    hooks->endGpuCapturePass(stats);
    return stats;
}
