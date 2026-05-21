#include "PsxGteInstructionCapture.h"

#include "EmuHooks.h"
#include "GteCapture.h"
#include "PsxGp0Opcode.h"
#include "PsxGteEngine.h"
#include "PsxMipsGteRunner.h"

#include <QSet>

#include <cstring>

namespace {

constexpr uint32_t kCop2Opcode = 0x12u;
constexpr int kMaxRtpsSitesPerFrame = 64;
constexpr int kBackwardSteps = 96;
constexpr int kForwardSteps = 8;

bool isGteCommand(uint32_t insn)
{
    const uint32_t cmd = insn & 0x3Fu;
    if (cmd != 0x01 && cmd != 0x30)
        return false;
    if ((insn >> 26) == kCop2Opcode && ((insn >> 21) & 0x1Fu) == 0x01)
        return true;
    // Common macro-assembler encodings (incl. mednafen/psxdev samples).
    return insn == 0x42000001u || insn == 0x42000030u;
}

uint32_t fetchWord(const uint8_t *ram, size_t byteSize, size_t offset)
{
    if (offset + 4 > byteSize)
        return 0;
    uint32_t word = 0;
    std::memcpy(&word, ram + offset, sizeof(word));
    return word;
}

} // namespace

void PsxGteInstructionCapture::captureFromSystemRam(const uint8_t *ram, size_t byteSize,
                                                    EmuHooks *hooks)
{
    if (!ram || byteSize < 16 || !hooks || !hooks->isCaptureEnabled())
        return;

    QSet<uint64_t> seenHashes;
    int sites = 0;

    for (size_t offset = 0; offset + 4 <= byteSize && sites < kMaxRtpsSitesPerFrame; offset += 4) {
        const uint32_t word = fetchWord(ram, byteSize, offset);
        if (!isGteCommand(word))
            continue;
        if (psxLooksLikeGp0Opcode(word))
            continue;

        const uint32_t startPc =
            offset >= static_cast<size_t>(kBackwardSteps * 4) ? static_cast<uint32_t>(offset - kBackwardSteps * 4)
                                                              : 0u;

        PsxGteEngine gte;
        gte.reset();
        const PsxMipsGteRunner::Result run =
            PsxMipsGteRunner::runBlock(ram, byteSize, startPc, kBackwardSteps + kForwardSteps, gte, hooks);

        if (run.rtpsEvents > 0) {
            MatrixRecord matrix = gte.matrixRecord();
            if (!seenHashes.contains(matrix.hash)) {
                seenHashes.insert(matrix.hash);
                hooks->onGteMatrix(matrix);
                ++sites;
            }
        }
    }
}
