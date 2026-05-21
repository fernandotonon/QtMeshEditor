#ifndef PSXGTEINSTRUCTIONCAPTURE_H
#define PSXGTEINSTRUCTIONCAPTURE_H

#include <cstddef>
#include <cstdint>

class EmuHooks;

/** Scan PS1 RAM for COP2/GTE instructions and capture matrices at RTPS/RTPT. */
class PsxGteInstructionCapture
{
public:
    static void captureFromSystemRam(const uint8_t *ram, size_t byteSize, EmuHooks *hooks);
};

#endif // PSXGTEINSTRUCTIONCAPTURE_H
