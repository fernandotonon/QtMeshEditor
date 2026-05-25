#ifndef GP0CAPTURESTATS_H
#define GP0CAPTURESTATS_H

#include <QMetaType>
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
    /** Model-space TMD/HMD blob found by a format-aware RAM scanner (#674). */
    RamModelMesh,
};

struct Gp0CaptureStats {
    Gp0CaptureSource primarySource = Gp0CaptureSource::None;
    int directHookPrims = 0;
    int ramOtPrims = 0;
    int ramLinearPrims = 0;
    int ramChainRootPrims = 0;
    /** Unique TMD meshes accepted from `PsxTmdRamScanner` this frame (#674). */
    int ramTmdMeshes = 0;
    /** HMD candidates found (or meshes emitted, once v2 lands) by `PsxHmdRamScanner`. */
    int ramHmdMeshes = 0;
    int totalPrims = 0;
    bool liveFrame = false;

    QString primarySourceLabel() const;
};

Q_DECLARE_METATYPE(Gp0CaptureStats)

#endif // GP0CAPTURESTATS_H
