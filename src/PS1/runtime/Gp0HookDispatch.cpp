#include "Gp0HookDispatch.h"

#include "EmuHooks.h"
#include "PsxCaptureFilters.h"
#include "PsxGp0Opcode.h"
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

QString primDedupeKey(const PrimRecord &prim)
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

void Gp0HookDispatch::dispatchStep(const GpuCommandParser::Gp0Step &step, EmuHooks *hooks,
                                   DrawModeRecord &currentMode, int &primCount)
{
    if (!hooks || !hooks->isCaptureEnabled())
        return;

    if (step.hasDrawMode) {
        currentMode = step.drawMode;
        hooks->onDrawMode(currentMode);
    }

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
    const uint32_t matrixId = hooks->latestMatrixId();
    if (matrixId != UINT32_MAX)
        prim.matrixId = matrixId;

    hooks->onGpuPrim(prim);
    ++primCount;
}

void Gp0HookDispatch::captureLinearScan(const uint8_t *ram, size_t byteSize, EmuHooks *hooks,
                                      QSet<QString> &seen, DrawModeRecord &currentMode, int &primCount)
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
            const uint32_t matrixId = hooks->latestMatrixId();
            if (matrixId != UINT32_MAX)
                prim.matrixId = matrixId;

            const QString key = primDedupeKey(prim);
            if (seen.contains(key)) {
                offset += step.wordsConsumed;
                continue;
            }
            seen.insert(key);
        }

        dispatchStep(step, hooks, currentMode, primCount);
        offset += step.wordsConsumed;
    }
}

void Gp0HookDispatch::captureFromSystemRam(const uint8_t *ram, size_t byteSize, EmuHooks *hooks)
{
    if (!ram || byteSize < 16 || !hooks || !hooks->isCaptureEnabled())
        return;

    QSet<QString> seen;
    DrawModeRecord currentMode{};
    int primCount = 0;

    const int otPrims = PsxOrderingTableScanner::captureFromOrderingTables(ram, byteSize, hooks);
    if (otPrims > 0)
        return;

    captureLinearScan(ram, byteSize, hooks, seen, currentMode, primCount);
}

void Gp0HookDispatch::captureFrameFromSystemRam(const uint8_t *ram, size_t byteSize,
                                                EmuHooks *hooks)
{
    if (!hooks || !hooks->isCaptureEnabled() || !ram || byteSize < 16)
        return;

    hooks->onFrameBegin();
    ensureCaptureProjectionMatrix(hooks);
    captureFromSystemRam(ram, byteSize, hooks);
    hooks->onFrameEnd();
}
