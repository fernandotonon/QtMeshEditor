#include "PsxGpuRamScanner.h"

#include "GpuCommandParser.h"
#include "RipperHooks.h"

#include <QSet>
#include <QString>

#include <algorithm>
#include <cstring>

namespace {

bool looksLikeGp0Opcode(uint32_t word)
{
    const uint8_t cmd = static_cast<uint8_t>((word >> 24) & 0xFF);
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
    QSet<QString> seen;

    hooks->onFrameBegin();

    for (size_t offset = 0; offset + 4 < wordCount; offset += 1) {
        uint32_t word = 0;
        std::memcpy(&word, ram + offset * 4, sizeof(word));
        if (!looksLikeGp0Opcode(word))
            continue;

        const size_t maxWords = std::min(wordCount - offset, static_cast<size_t>(256));
        uint32_t window[256];
        for (size_t i = 0; i < maxWords; ++i)
            std::memcpy(&window[i], ram + (offset + i) * 4, sizeof(uint32_t));
        const auto result = GpuCommandParser::parseGp0(window, maxWords);
        if (!result.error.isEmpty() || result.prims.isEmpty())
            continue;

        for (const PrimRecord &prim : result.prims) {
            QString key = QStringLiteral("%1|%2").arg(static_cast<int>(prim.kind)).arg(prim.vertexCount);
            for (int v = 0; v < 4; ++v)
                key += QStringLiteral("|%1,%2").arg(prim.verts[v].x).arg(prim.verts[v].y);
            key += QStringLiteral("|%1,%2|%3").arg(prim.tpage).arg(prim.clut).arg(prim.semiTrans);
            if (seen.contains(key))
                continue;
            seen.insert(key);
            hooks->onGpuPrim(prim);
        }
        for (const DrawModeRecord &mode : result.drawModes)
            hooks->onDrawMode(mode);
    }

    hooks->onFrameEnd();
}
