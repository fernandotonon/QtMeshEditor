#include "PsxOrderingTableScanner.h"

#include "EmuHooks.h"
#include "PsxGp0ChainWalker.h"
#include "PsxGp0Opcode.h"

#include <QSet>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cstring>

namespace {

constexpr int kMaxPrimsPerFrame = 2048;
constexpr int kMaxOtCandidates = 12;
constexpr int kMaxChainsPerTable = 384;

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

struct OtCandidate {
    uint32_t baseByte = 0;
    int entryCount = 0;
    int score = 0;
};

int scoreOrderingTable(const uint8_t *ram, size_t byteSize, uint32_t baseByte, int entryCount)
{
    if (baseByte + static_cast<size_t>(entryCount) * 4 > byteSize)
        return 0;

    int nonZero = 0;
    int validChains = 0;
    for (int i = 0; i < entryCount; ++i) {
        uint32_t offset = 0;
        std::memcpy(&offset, ram + baseByte + static_cast<size_t>(i) * 4, sizeof(offset));
        if (offset == 0)
            continue;
        ++nonZero;
        if (offset < 4 || offset >= static_cast<uint32_t>(entryCount) * 4 || (offset % 4) != 0)
            continue;
        const uint32_t chainAddr = baseByte + offset;
        if (chainAddr + 4 > byteSize)
            continue;
        uint32_t header = 0;
        std::memcpy(&header, ram + chainAddr, sizeof(header));
        if (looksLikeGp0Opcode(header))
            ++validChains;
    }

    if (nonZero < 1 || validChains < 1)
        return 0;
    return validChains * 4 + nonZero;
}

QVector<OtCandidate> findOrderingTableCandidates(const uint8_t *ram, size_t byteSize)
{
    static const int kOtEntryCounts[] = {256, 512, 128, 1024, 64, 2048};
    QVector<OtCandidate> candidates;

    const size_t scanEnd = byteSize > 4096 ? byteSize - 4096 : 0;
    for (size_t base = 0; base < scanEnd; base += 256) {
        for (int entryCount : kOtEntryCounts) {
            const int score = scoreOrderingTable(ram, byteSize, static_cast<uint32_t>(base),
                                                 entryCount);
            if (score < 6)
                continue;

            OtCandidate candidate{};
            candidate.baseByte = static_cast<uint32_t>(base);
            candidate.entryCount = entryCount;
            candidate.score = score;
            candidates.append(candidate);
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const OtCandidate &a, const OtCandidate &b) { return a.score > b.score; });

    QVector<OtCandidate> unique;
    for (const OtCandidate &candidate : candidates) {
        bool overlaps = false;
        for (const OtCandidate &picked : unique) {
            const uint32_t delta = candidate.baseByte > picked.baseByte
                                       ? candidate.baseByte - picked.baseByte
                                       : picked.baseByte - candidate.baseByte;
            if (delta < static_cast<uint32_t>(picked.entryCount) * 4) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps)
            unique.append(candidate);
        if (unique.size() >= kMaxOtCandidates)
            break;
    }

    return unique;
}

} // namespace

int PsxOrderingTableScanner::captureFromOrderingTables(const uint8_t *ram, size_t byteSize,
                                                       EmuHooks *hooks)
{
    if (!ram || byteSize < 256 || !hooks || !hooks->isCaptureEnabled())
        return 0;

    const QVector<OtCandidate> tables = findOrderingTableCandidates(ram, byteSize);
    if (tables.isEmpty())
        return 0;

    QSet<QString> seenPrimKeys;
    DrawModeRecord currentMode{};
    int primCount = 0;
    int chainsWalked = 0;

    for (const OtCandidate &table : tables) {
        for (int i = 0; i < table.entryCount; ++i) {
            if (primCount >= kMaxPrimsPerFrame || chainsWalked >= kMaxChainsPerTable)
                break;

            uint32_t offset = 0;
            std::memcpy(&offset, ram + table.baseByte + static_cast<size_t>(i) * 4, sizeof(offset));
            if (offset < 4 || offset >= static_cast<uint32_t>(table.entryCount) * 4
                || (offset % 4) != 0)
                continue;

            const uint32_t chainAddr = table.baseByte + offset;
            const PsxGp0ChainWalker::WalkResult walked =
                PsxGp0ChainWalker::walkChain(ram, byteSize, chainAddr, hooks, currentMode,
                                             primCount, seenPrimKeys);
            if (walked.packetsParsed > 0)
                ++chainsWalked;
        }
    }

    return primCount;
}
