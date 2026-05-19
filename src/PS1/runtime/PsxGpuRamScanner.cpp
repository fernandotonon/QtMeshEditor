#include "PsxGpuRamScanner.h"

#include "GpuCommandParser.h"
#include "RipperHooks.h"

#include <QSet>

#include <algorithm>

namespace {

bool looksLikeGp0Opcode(uint32_t word)
{
    const uint8_t cmd = static_cast<uint8_t>(word & 0xFF);
    if (cmd >= 0x20 && cmd <= 0x2F)
        return true;
    if (cmd >= 0x30 && cmd <= 0x3F)
        return true;
    if (cmd >= 0x60 && cmd <= 0x7F)
        return true;
    if (cmd == 0xE1)
        return true;
    return false;
}

} // namespace

void PsxGpuRamScanner::captureFromSystemRam(const uint8_t *ram, size_t byteSize, RipperHooks *hooks)
{
    if (!ram || byteSize < 16 || !hooks || !hooks->isCaptureEnabled())
        return;

    const size_t wordCount = byteSize / 4;
    const auto *words = reinterpret_cast<const uint32_t *>(ram);
    QSet<uint64_t> seen;

    hooks->onFrameBegin();

    for (size_t offset = 0; offset + 4 < wordCount; offset += 1) {
        if (!looksLikeGp0Opcode(words[offset]))
            continue;

        const size_t maxWords = std::min(wordCount - offset, static_cast<size_t>(256));
        const auto result = GpuCommandParser::parseGp0(words + offset, maxWords);
        if (!result.error.isEmpty() || result.prims.isEmpty())
            continue;

        for (const PrimRecord &prim : result.prims) {
            uint64_t key = 0;
            key ^= static_cast<uint64_t>(prim.kind) << 56;
            key ^= static_cast<uint64_t>(prim.verts[0].x) << 32;
            key ^= static_cast<uint64_t>(prim.verts[0].y);
            key ^= static_cast<uint64_t>(prim.verts[1].x) << 16;
            key ^= static_cast<uint64_t>(prim.verts[1].y);
            if (seen.contains(key))
                continue;
            seen.insert(key);
            hooks->onGpuPrim(prim);
        }
        for (const DrawModeRecord &mode : result.drawModes)
            hooks->onDrawMode(mode);

        offset += maxWords / 4;
    }

    hooks->onFrameEnd();
}
