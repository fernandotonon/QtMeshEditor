#ifndef GP0HOOKDISPATCH_H
#define GP0HOOKDISPATCH_H

#include "CaptureTypes.h"
#include "GpuCommandParser.h"
#include "Gp0CaptureStats.h"

#include <QSet>
#include <QString>

#include <cstddef>
#include <cstdint>

class EmuHooks;

/**
 * GP0 opcode dispatch → EmuHooks (#418). Linked by emulator plugins so GP0 decode
 * stays on the plugin side of the GPL boundary (#657 live + merged RAM paths).
 */
class Gp0HookDispatch
{
public:
    static QString primDedupeKey(const PrimRecord &prim);

    /** Dispatch one decoded GP0 step to @p hooks (primitives, draw mode, VRAM I/O). */
    static void dispatchStep(const GpuCommandParser::Gp0Step &step, EmuHooks *hooks,
                             DrawModeRecord &currentMode, int &primCount);

    /**
     * Submit GP0 packets as the core would (#657 direct-hook path).
     * @return primitives dispatched.
     */
    static int submitGp0Words(const uint32_t *words, size_t wordCount, EmuHooks *hooks);

    /**
     * Merged RAM capture: OT chains, standalone chain roots, then linear scan (#657).
     */
    static Gp0CaptureStats captureFromSystemRam(const uint8_t *ram, size_t byteSize,
                                                EmuHooks *hooks, QSet<QString> *seenPrimKeys);

    /** Legacy RAM-only baseline (OT then linear, no chain-root pass). */
    static Gp0CaptureStats captureFromSystemRamLegacy(const uint8_t *ram, size_t byteSize,
                                                    EmuHooks *hooks);

    static void captureLinearScan(const uint8_t *ram, size_t byteSize, EmuHooks *hooks,
                                  QSet<QString> &seenPrimKeys, DrawModeRecord &currentMode,
                                  int &primCount);

    /**
     * Frame capture with optional GTE instruction + RAM scan (#418, #419, #657).
     * @p scanGteRam When false, skips the 2 MiB matrix heuristic (use on per-frame ticks).
     * @p accumulate When true, RipperHooks keeps prior primitives (live armed capture).
     */
    static Gp0CaptureStats captureFrameFromSystemRam(const uint8_t *ram, size_t byteSize,
                                                     EmuHooks *hooks, bool scanGteRam = true,
                                                     bool accumulate = false);
};

#endif // GP0HOOKDISPATCH_H
