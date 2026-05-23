#include "Gp0HookDispatch.h"

#include <QtGlobal>

#include "EmuHooks.h"
#include "PsxCaptureFilters.h"
#include "PsxGp0ChainRootScanner.h"
#include "PsxGp0Opcode.h"
#include "PsxGteInstructionCapture.h"
#include "PsxGteRamScanner.h"
#include "PsxOrderingTableScanner.h"

#include <QSet>
#include <QString>

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

int Gp0HookDispatch::submitGp0Words(const uint32_t *words, size_t wordCount, EmuHooks *hooks)
{
    if (!words || wordCount == 0 || !hooks || !hooks->isCaptureEnabled())
        return 0;

    DrawModeRecord currentMode{};
    uint32_t currentMatrixId = UINT32_MAX;
    int primCount = 0;
    size_t offset = 0;
    while (offset < wordCount) {
        if (primCount >= kMaxPrimsPerFrame)
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
    stats.primarySource = pickPrimarySource(stats);
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

    QSet<QString> *seen = hooks->livePrimDedupeKeys();
    if (qEnvironmentVariableIsSet("QTMESH_PS1_GP0_RAM_LEGACY")
        && qEnvironmentVariableIntValue("QTMESH_PS1_GP0_RAM_LEGACY") != 0) {
        stats = captureFromSystemRamLegacy(ram, scanSize, hooks);
    } else {
        stats = captureFromSystemRam(ram, scanSize, hooks, seen);
    }
    stats.liveFrame = liveFrame;

    hooks->onFrameEnd();
    hooks->endGpuCapturePass(stats);
    return stats;
}
