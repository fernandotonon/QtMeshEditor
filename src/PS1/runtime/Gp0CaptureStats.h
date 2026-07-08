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
    /** True in-core GP0 command stream from the rip-instrumented beetle fork
     *  (#815). Ranked above DirectHook — packet-for-packet submission order
     *  with per-vertex PGXP provenance. */
    InCoreHook,
};

struct Gp0CaptureStats {
    Gp0CaptureSource primarySource = Gp0CaptureSource::None;
    int directHookPrims = 0;
    int ramOtPrims = 0;
    int ramLinearPrims = 0;
    int ramChainRootPrims = 0;
    /** Unique TMD meshes accepted (via `EmuHooks::onModelMesh`) from
     *  `PsxTmdRamScanner` this frame (#674). */
    int ramTmdMeshes = 0;
    /** HMD meshes accepted (via `EmuHooks::onModelMesh`) from `PsxHmdRamScanner`
     *  this frame. Zero until the v2 walker actually emits meshes (#674). */
    int ramHmdMeshes = 0;
    /** Plausible HMD magic-bytes candidates found in RAM this frame.
     *  v1 is a diagnostics count only — it does NOT prove model-mesh capture
     *  succeeded, so it must not flip `primarySource` to `RamModelMesh`. */
    int ramHmdCandidates = 0;
    int totalPrims = 0;
    bool liveFrame = false;
    /** Prims ingested from the in-core GP0 draw hook this frame (#815). */
    int inCoreHookPrims = 0;
    /** GTE transform records delivered by the in-core hook this frame (#814). */
    int gteRecords = 0;
    /** True when the in-core stream was active and the RAM GP0 passes were
     *  skipped for this frame (#815). */
    bool inCoreStream = false;

    QString primarySourceLabel() const;
};

Q_DECLARE_METATYPE(Gp0CaptureStats)

#endif // GP0CAPTURESTATS_H
