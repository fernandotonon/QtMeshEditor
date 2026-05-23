#ifndef GP0CAPTURESTATS_H
#define GP0CAPTURESTATS_H

#include <QString>

/**
 * How GP0 primitives reached the capture buffer (#657).
 */
enum class Gp0CaptureSource : uint8_t {
    None = 0,
    /** Core/plugin called EmuHooks::submitGp0Words (stub FIFO simulation). */
    DirectHook,
    /** Ordering-table chain walk from system RAM. */
    RamOrderingTable,
    /** Linear opcode scan fallback. */
    RamLinear,
    /** Standalone linked GP0 chain roots in RAM. */
    RamChainRoot,
};

struct Gp0CaptureStats {
    Gp0CaptureSource primarySource = Gp0CaptureSource::None;
    int directHookPrims = 0;
    int ramOtPrims = 0;
    int ramLinearPrims = 0;
    int ramChainRootPrims = 0;
    int totalPrims = 0;
    bool liveFrame = false;

    QString primarySourceLabel() const;
};

#endif // GP0CAPTURESTATS_H
