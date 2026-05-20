#ifndef PSXORDERINGTABLESCANNER_H
#define PSXORDERINGTABLESCANNER_H

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
    /** Returns number of primitives dispatched from OT chains. */
    static int captureFromOrderingTables(const uint8_t *ram, size_t byteSize, EmuHooks *hooks);
};

#endif // PSXORDERINGTABLESCANNER_H
