#include "PsxGpuRamScanner.h"

#include "Gp0HookDispatch.h"

#include <QSet>
#include "EmuHooks.h"

void PsxGpuRamScanner::captureFromSystemRam(const uint8_t *ram, size_t byteSize, EmuHooks *hooks)
{
    QSet<QString> seen;
    Gp0HookDispatch::captureFromSystemRam(ram, byteSize, hooks, &seen);
}
