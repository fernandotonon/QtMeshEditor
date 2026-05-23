#include "PsxGp0ChainRootScanner.h"

#include <QtGlobal>

#include "EmuHooks.h"
#include "Gp0HookDispatch.h"
#include "PsxGp0ChainWalker.h"
#include "PsxGp0Opcode.h"

#include <QSet>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cstring>

namespace {

constexpr int kMaxPrimsPerFrame = 2048;
constexpr int kMaxRootsToWalk = 48;
constexpr int kMinChainPackets = 2;
constexpr size_t kScanStrideBytes = 16;

struct ChainRootCandidate {
    uint32_t byteAddr = 0;
    int packetEstimate = 0;
};

int estimateChainPackets(const uint8_t *ram, size_t byteSize, uint32_t startAddr)
{
    uint32_t addr = startAddr;
    int packets = 0;
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

        ++packets;
        if ((word & 1u) == 0)
            break;

        const uint32_t nextAddr = psxGp0TagNextByteAddr(word);
        if (nextAddr == addr || nextAddr + 4 > byteSize || (nextAddr % 4) != 0)
            break;
        addr = nextAddr;
    }
    return packets;
}

bool regionsOverlap(uint32_t aStart, int aPackets, uint32_t bStart, int bPackets)
{
    const uint32_t aEnd = aStart + static_cast<uint32_t>(aPackets) * 64u;
    const uint32_t bEnd = bStart + static_cast<uint32_t>(bPackets) * 64u;
    return !(aEnd <= bStart || bEnd <= aStart);
}

} // namespace

int PsxGp0ChainRootScanner::captureFromChainRoots(const uint8_t *ram, size_t byteSize,
                                                  EmuHooks *hooks, QSet<QString> *seenPrimKeys,
                                                  int &primCount)
{
    if (!ram || byteSize < 64 || !hooks || !hooks->isCaptureEnabled() || !seenPrimKeys)
        return 0;

    if (qEnvironmentVariableIsSet("QTMESH_PS1_GP0_CHAIN_ROOT_SCAN")
        && qEnvironmentVariableIntValue("QTMESH_PS1_GP0_CHAIN_ROOT_SCAN") == 0)
        return 0;

    QVector<ChainRootCandidate> candidates;
    candidates.reserve(256);

    const size_t scanLimit = byteSize > (512u * 1024u) ? (512u * 1024u) : byteSize;
    for (size_t off = 0; off + 4 <= scanLimit; off += kScanStrideBytes) {
        uint32_t word = 0;
        std::memcpy(&word, ram + off, sizeof(word));
        if ((word & 1u) == 0u)
            continue;
        if (!psxLooksLikeGp0Opcode(word))
            continue;

        const int packets = estimateChainPackets(ram, byteSize, static_cast<uint32_t>(off));
        if (packets < kMinChainPackets)
            continue;

        candidates.append({static_cast<uint32_t>(off), packets});
    }

    if (candidates.isEmpty())
        return 0;

    std::sort(candidates.begin(), candidates.end(),
              [](const ChainRootCandidate &a, const ChainRootCandidate &b) {
                  return a.packetEstimate > b.packetEstimate;
              });

    DrawModeRecord currentMode{};
    int before = primCount;
    QVector<ChainRootCandidate> walked;

    for (const ChainRootCandidate &root : candidates) {
        if (primCount >= kMaxPrimsPerFrame || walked.size() >= kMaxRootsToWalk)
            break;

        bool overlaps = false;
        for (const ChainRootCandidate &prior : walked) {
            if (regionsOverlap(root.byteAddr, root.packetEstimate, prior.byteAddr,
                               prior.packetEstimate)) {
                overlaps = true;
                break;
            }
        }
        if (overlaps)
            continue;

        const PsxGp0ChainWalker::WalkResult result =
            PsxGp0ChainWalker::walkChain(ram, byteSize, root.byteAddr, hooks, currentMode,
                                         primCount, *seenPrimKeys);
        if (result.packetsParsed > 0)
            walked.append(root);
    }

    return primCount - before;
}
