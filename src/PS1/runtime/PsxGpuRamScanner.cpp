#include "PsxGpuRamScanner.h"

#include "Gp0HookDispatch.h"
#include "EmuHooks.h"

void PsxGpuRamScanner::captureFromSystemRam(const uint8_t *ram, size_t byteSize, EmuHooks *hooks)
{
    Gp0HookDispatch::captureFromSystemRam(ram, byteSize, hooks);
}
