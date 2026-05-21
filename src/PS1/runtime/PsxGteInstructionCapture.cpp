#include "PsxGteInstructionCapture.h"

#include "EmuHooks.h"
#include "GteCapture.h"
#include "PsxCop2Opcode.h"
#include "PsxGp0Opcode.h"
#include "PsxGteEngine.h"
#include "PsxMipsGteRunner.h"

#include <QSet>

#include <cstring>

namespace {

constexpr int kMaxRtpsSitesPerFrame = 64;
constexpr int kBackwardSteps = 96;
constexpr int kForwardSteps = 8;

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
        if (!psxIsGteCommand(word))
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
