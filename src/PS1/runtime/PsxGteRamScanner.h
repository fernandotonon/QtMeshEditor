#ifndef PSXGTERAMSCANNER_H
#define PSXGTERAMSCANNER_H

#include <cstddef>
#include <cstdint>

class EmuHooks;

/** Heuristic scan of main RAM for GTE projection matrices (#419). */
class PsxGteRamScanner
{
public:
    static void captureFromSystemRam(const uint8_t *ram, size_t byteSize, EmuHooks *hooks);
};

#endif // PSXGTERAMSCANNER_H
