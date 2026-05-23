#ifndef PSXORDERINGTABLESCANNER_H
#define PSXORDERINGTABLESCANNER_H

#include <QSet>

#include <cstddef>
#include <cstdint>

class EmuHooks;

/**
 * Locates PSX GPU ordering tables in main RAM and walks linked GP0 chains (#418).
 * Replaces blind linear opcode scanning when the game uses DrawOTag-style submission.
 */
class PsxOrderingTableScanner
{
public:
    /** Appends primitives from OT chains; returns count added this pass. */
    static int captureFromOrderingTables(const uint8_t *ram, size_t byteSize, EmuHooks *hooks,
                                       QSet<QString> *seenPrimKeys, int &primCount);
};

#endif // PSXORDERINGTABLESCANNER_H
