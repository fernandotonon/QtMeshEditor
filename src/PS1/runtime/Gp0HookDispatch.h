#ifndef GP0HOOKDISPATCH_H
#define GP0HOOKDISPATCH_H

#include "GpuCommandParser.h"

#include <QSet>

#include <cstddef>
#include <cstdint>

class EmuHooks;

/**
 * GP0 opcode dispatch → EmuHooks (#418). Linked by emulator plugins so GP0 decode
 * stays on the plugin side of the GPL boundary.
 */
class Gp0HookDispatch
{
public:
    /** Dispatch one decoded GP0 step to @p hooks (primitives, draw mode, VRAM I/O). */
    static void dispatchStep(const GpuCommandParser::Gp0Step &step, EmuHooks *hooks,
                             DrawModeRecord &currentMode, int &primCount);

    /**
     * OT-first capture from main RAM, then linear GP0 opcode scan as fallback (#418).
     */
    static void captureFromSystemRam(const uint8_t *ram, size_t byteSize, EmuHooks *hooks);

    /** Fallback linear scan when no ordering table is found. */
    static void captureLinearScan(const uint8_t *ram, size_t byteSize, EmuHooks *hooks,
                                  QSet<QString> &seenPrimKeys, DrawModeRecord &currentMode,
                                  int &primCount);

    /**
     * Frame capture with optional GTE RAM scan (#418, #419).
     * @p scanGteRam When false, skips the 2 MiB matrix heuristic (use on per-frame ticks).
     */
    static void captureFrameFromSystemRam(const uint8_t *ram, size_t byteSize, EmuHooks *hooks,
                                          bool scanGteRam = true);
};

#endif // GP0HOOKDISPATCH_H
