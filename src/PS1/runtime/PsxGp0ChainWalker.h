#ifndef PSXGP0CHAINWALKER_H
#define PSXGP0CHAINWALKER_H

#include "GpuCommandParser.h"

#include <QSet>
#include <QString>
#include <cstddef>
#include <cstdint>

class EmuHooks;

/**
 * Walks PSX GPU packet linked lists (nocash psx-spx: bit0 of header links to next packet).
 */
class PsxGp0ChainWalker
{
public:
    struct WalkResult {
        int packetsParsed = 0;
        int primsDispatched = 0;
    };

    /** Follow one OT / DMA chain starting at @p chainBaseByte (byte address in main RAM). */
    static WalkResult walkChain(const uint8_t *ram, size_t byteSize, uint32_t chainBaseByte,
                                EmuHooks *hooks, DrawModeRecord &currentMode, uint32_t &currentMatrixId,
                                int &primCount, QSet<QString> &seenPrimKeys);
};

#endif // PSXGP0CHAINWALKER_H
