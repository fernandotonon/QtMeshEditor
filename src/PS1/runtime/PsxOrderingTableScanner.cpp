#include "PsxOrderingTableScanner.h"

#include "EmuHooks.h"
#include "PsxGp0ChainWalker.h"
#include "PsxGp0Opcode.h"

#include <QSet>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace {

constexpr int kMaxPrimsPerFrame = 2048;
constexpr int kMaxOtCandidates = 12;
constexpr int kMaxChainsPerTable = 384;

struct OtCandidate {
    uint32_t baseByte = 0;
    int entryCount = 0;
    int score = 0;
};

/** DrawOTag entries are 24-bit RAM pointers; tests may use offsets from the OT base. */
uint32_t resolveOtEntryChainAddr(const uint8_t *ram, size_t byteSize, uint32_t otBase,
                                 uint32_t entry, int entryCount)
{
    if (entry == 0)
        return UINT32_MAX;

    const uint32_t absAddr = psxGp0TagNextByteAddr(entry);
    if (absAddr >= 4 && absAddr + 4 <= byteSize && (absAddr % 4) == 0) {
        uint32_t header = 0;
        std::memcpy(&header, ram + absAddr, sizeof(header));
        if (psxLooksLikeGp0Opcode(header))
            return absAddr;
    }

    const uint32_t rel = entry & 0x3FFFFCu;
    if (rel >= 4 && rel < static_cast<uint32_t>(entryCount) * 4 && (rel % 4) == 0) {
        const uint32_t relAddr = otBase + rel;
        if (relAddr + 4 <= byteSize) {
            uint32_t header = 0;
            std::memcpy(&header, ram + relAddr, sizeof(header));
            if (psxLooksLikeGp0Opcode(header))
                return relAddr;
        }
    }

    return UINT32_MAX;
}

int scoreOrderingTable(const uint8_t *ram, size_t byteSize, uint32_t baseByte, int entryCount)
{
    if (baseByte + static_cast<size_t>(entryCount) * 4 > byteSize)
        return 0;

    int nonZero = 0;
    int validChains = 0;
    for (int i = 0; i < entryCount; ++i) {
        uint32_t entry = 0;
        std::memcpy(&entry, ram + baseByte + static_cast<size_t>(i) * 4, sizeof(entry));
        if (entry == 0)
            continue;
        ++nonZero;
        if (resolveOtEntryChainAddr(ram, byteSize, baseByte, entry, entryCount) != UINT32_MAX)
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
                                                       EmuHooks *hooks, QSet<QString> *seenPrimKeys,
                                                       int &primCount)
{
    if (!ram || byteSize < 256 || !hooks || !hooks->isCaptureEnabled() || !seenPrimKeys)
        return 0;

    const QVector<OtCandidate> tables = findOrderingTableCandidates(ram, byteSize);
    if (tables.isEmpty())
        return 0;

    const int before = primCount;
    DrawModeRecord currentMode{};
    uint32_t currentMatrixId = UINT32_MAX;
    int chainsWalked = 0;

    for (const OtCandidate &table : tables) {
        for (int i = 0; i < table.entryCount; ++i) {
            if (primCount >= kMaxPrimsPerFrame || chainsWalked >= kMaxChainsPerTable)
                break;

            uint32_t entry = 0;
            std::memcpy(&entry, ram + table.baseByte + static_cast<size_t>(i) * 4, sizeof(entry));
            const uint32_t chainAddr =
                resolveOtEntryChainAddr(ram, byteSize, table.baseByte, entry, table.entryCount);
            if (chainAddr == UINT32_MAX)
                continue;
            const PsxGp0ChainWalker::WalkResult walked =
                PsxGp0ChainWalker::walkChain(ram, byteSize, chainAddr, hooks, currentMode,
                                             currentMatrixId, primCount, *seenPrimKeys);
            if (walked.packetsParsed > 0)
                ++chainsWalked;
        }
    }

    return primCount - before;
}
