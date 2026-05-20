#include "PsxGp0ChainWalker.h"

#include "EmuHooks.h"
#include "Gp0HookDispatch.h"
#include "PsxCaptureFilters.h"
#include "PsxGp0Opcode.h"

#include <cstring>

namespace {

constexpr int kMaxChainPackets = 512;

bool looksLikeGp0Opcode(uint32_t word)
{
    const uint8_t cmd = psxGp0OpcodeByte(word);
    if (cmd >= 0x20 && cmd <= 0x3F)
        return true;
    if (cmd >= 0x60 && cmd <= 0x7F)
        return true;
    if (cmd >= 0xE1 && cmd <= 0xE6)
        return true;
    if (cmd == 0xA0 || cmd == 0xC0)
        return true;
    return false;
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

PsxGp0ChainWalker::WalkResult PsxGp0ChainWalker::walkChain(const uint8_t *ram, size_t byteSize,
                                                           uint32_t chainBaseByte, EmuHooks *hooks,
                                                           DrawModeRecord &currentMode,
                                                           int &primCount, QSet<QString> &seenPrimKeys)
{
    WalkResult result;
    if (!ram || !hooks || !hooks->isCaptureEnabled() || byteSize < 8)
        return result;

    uint32_t addr = chainBaseByte;
    for (int guard = 0; guard < kMaxChainPackets; ++guard) {
        if (addr + 4 > byteSize || (addr % 4) != 0)
            break;

        const uint32_t *words = reinterpret_cast<const uint32_t *>(ram + addr);
        const size_t remainingWords = (byteSize - addr) / 4;
        if (remainingWords == 0)
            break;

        if (!looksLikeGp0Opcode(words[0]))
            break;

        const GpuCommandParser::Gp0Step step = GpuCommandParser::stepGp0(words, remainingWords);
        if (step.wordsConsumed == 0 || !step.error.isEmpty())
            break;

        ++result.packetsParsed;

        if (step.hasPrim) {
            PrimRecord prim = step.prim;
            if (PsxCaptureFilters::isOnScreenPrim(prim)) {
                const uint32_t matrixId = hooks->latestMatrixId();
                if (matrixId != UINT32_MAX)
                    prim.matrixId = matrixId;
                const QString key = primDedupeKey(prim);
                if (!seenPrimKeys.contains(key)) {
                    seenPrimKeys.insert(key);
                    Gp0HookDispatch::dispatchStep(step, hooks, currentMode, primCount);
                    ++result.primsDispatched;
                }
            }
        } else {
            Gp0HookDispatch::dispatchStep(step, hooks, currentMode, primCount);
        }

        const uint32_t header = words[0];
        if ((header & 1u) == 0)
            break;

        const uint32_t nextAddr = psxGp0TagNextByteAddr(header);
        if (nextAddr == addr || nextAddr + 4 > byteSize || (nextAddr % 4) != 0)
            break;
        addr = nextAddr;
    }

    return result;
}
