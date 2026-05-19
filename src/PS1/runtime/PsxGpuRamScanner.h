#ifndef PSXGPURAMSCANNER_H
#define PSXGPURAMSCANNER_H

#include <cstddef>
#include <cstdint>

class RipperHooks;

/**
 * Best-effort scan of PSX main RAM for GP0 polygon packets (#418).
 * Used when running a libretro core that does not expose GPU FIFO hooks.
 */
class PsxGpuRamScanner
{
public:
    static void captureFromSystemRam(const uint8_t *ram, size_t byteSize, RipperHooks *hooks);
};

#endif // PSXGPURAMSCANNER_H
