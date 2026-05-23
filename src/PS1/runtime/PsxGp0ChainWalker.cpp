#include "PsxGp0ChainWalker.h"

#include "EmuHooks.h"
#include "Gp0HookDispatch.h"
#include "PsxCaptureFilters.h"
#include "PsxGp0Opcode.h"

#include <cstring>

namespace {

constexpr int kMaxChainPackets = 512;

QString primDedupeKey(const PrimRecord &prim)
{
    return Gp0HookDispatch::primDedupeKey(prim);
}

} // namespace

PsxGp0ChainWalker::WalkResult PsxGp0ChainWalker::walkChain(const uint8_t *ram, size_t byteSize,
                                                           uint32_t chainBaseByte, EmuHooks *hooks,
                                                           DrawModeRecord &currentMode,
                                                           uint32_t &currentMatrixId, int &primCount,
                                                           QSet<QString> &seenPrimKeys)
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

        if (!psxLooksLikeGp0Opcode(words[0]))
            break;

        const GpuCommandParser::Gp0Step step = GpuCommandParser::stepGp0(words, remainingWords);
        if (step.wordsConsumed == 0 || !step.error.isEmpty())
            break;

        ++result.packetsParsed;

        if (step.hasPrim) {
            PrimRecord prim = step.prim;
            if (PsxCaptureFilters::isOnScreenPrim(prim)) {
                const QString key = primDedupeKey(prim);
                if (!seenPrimKeys.contains(key)) {
                    seenPrimKeys.insert(key);
                    Gp0HookDispatch::dispatchStep(step, hooks, currentMode, currentMatrixId, primCount);
                    ++result.primsDispatched;
                }
            }
        } else {
            Gp0HookDispatch::dispatchStep(step, hooks, currentMode, currentMatrixId, primCount);
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
