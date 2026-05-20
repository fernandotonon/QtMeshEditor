#ifndef GP0HOOKDISPATCH_H
#define GP0HOOKDISPATCH_H

#include "GpuCommandParser.h"

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
     * Linear scan of emulated main RAM for GP0 packets while capture is armed.
     * Used by the libretro plugin when true in-core GPU hooks are unavailable.
     */
    static void captureFromSystemRam(const uint8_t *ram, size_t byteSize, EmuHooks *hooks);

    /** Begin/end frame + stable projection matrix for libretro heuristic capture. */
    static void captureFrameFromSystemRam(const uint8_t *ram, size_t byteSize, EmuHooks *hooks);
};

#endif // GP0HOOKDISPATCH_H
