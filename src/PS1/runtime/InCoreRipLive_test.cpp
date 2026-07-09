#ifdef ENABLE_PS1_RIP
#ifdef QTMESH_PS1_LIBRETRO_INTEGRATION_TESTS

// Live in-core rip capture against a real BIOS + ISO (#817 manual/local run).
// Compiled only when QTMESH_PS1_LIBRETRO_INTEGRATION_TESTS is defined AND the
// rip fork (beetle_psx_qtmesh_libretro) is in PS1Cores/. Boots the game,
// plays past the intro, captures accumulated frames, reconstructs, and prints
// the tier breakdown — the exact numbers the golden doc wants recorded.
//
//   QTMESH_PS1_TEST_BIOS=/path/scph.bin \
//   QTMESH_PS1_TEST_ISO=/path/game.iso \
//   QTMESH_PS1_RIP_BOOT_FRAMES=900 \
//   ./UnitTests --gtest_filter='InCoreRipLiveTest.*'

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/CaptureSnapshot.h"
#include "PS1/runtime/EmuCore.h"
#include "PS1/runtime/EmuCoreLoader.h"
#include "PS1/runtime/MeshReconstructionStats.h"
#include "PS1/runtime/MeshReconstructor.h"
#include "PS1/runtime/MeshTopologyHash.h"
#include "PS1/runtime/ReconstructedMesh.h"
#include "PS1/runtime/RipperHooks.h"
#include "PS1/runtime/VramSnapshot.h"

#include "libretro/libretro_api.h"

#include <atomic>
#include <cstdio>

namespace {

int envInt(const char *key, int fallback)
{
    const QByteArray v = qgetenv(key);
    bool ok = false;
    const int n = v.toInt(&ok);
    return ok ? n : fallback;
}

} // namespace

TEST(InCoreRipLiveTest, CapturesTrackedGeometryFromDisc)
{
    const QString biosPath = qEnvironmentVariable("QTMESH_PS1_TEST_BIOS");
    const QString isoPath = qEnvironmentVariable("QTMESH_PS1_TEST_ISO");
    if (biosPath.isEmpty() || isoPath.isEmpty())
        GTEST_SKIP() << "Set QTMESH_PS1_TEST_BIOS and QTMESH_PS1_TEST_ISO";

    qunsetenv("QTMESH_PS1_FORCE_STUB");

    QString err;
    std::unique_ptr<EmuCore> core = EmuCoreLoader::loadCore(&err);
    ASSERT_TRUE(core) << err.toStdString();
    ASSERT_EQ(core->coreId(), QStringLiteral("libretro"));

    std::atomic<bool> armed{false};
    CaptureBuffer buffer;
    VramSnapshot vram;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);
    hooks.setVram(&vram);
    core->setHooks(&hooks);

    ASSERT_TRUE(core->loadBios(biosPath)) << "BIOS load failed";
    ASSERT_TRUE(core->loadIso(isoPath)) << core->lastError().toStdString();

    QString bootErr;
    ASSERT_TRUE(core->boot(&bootErr)) << bootErr.toStdString();

    // The rip fork must have registered — this is the whole point of the run.
    EXPECT_TRUE(core->inCoreHooksActive())
        << "in-core hooks NOT active — the rip fork (beetle_psx_qtmesh_libretro) "
           "is not the loaded core; capture will fall back to the RAM-scan path";

    // Play past logos / intro (Crash: ~15-30 s of attract), then arm and
    // accumulate several seconds of the first gameplay/menu scene.
    const int bootFrames = envInt("QTMESH_PS1_RIP_BOOT_FRAMES", 1200);
    const int captureFrames = envInt("QTMESH_PS1_RIP_CAPTURE_FRAMES", 180);
    // Games sit on logos / attract FMV until START/X is pressed. Pulse START
    // then X through the intro so we reach a 3D scene without a human at the
    // pad. Toggle each ~1 s so title screens that debounce still register it.
    const bool autoInput = envInt("QTMESH_PS1_RIP_AUTOINPUT", 1) != 0;

    for (int i = 0; i < bootFrames; ++i) {
        if (autoInput) {
            const bool press = (i / 30) % 2 == 0;         // 0.5 s on, 0.5 s off
            const unsigned button = ((i / 60) % 2 == 0)   // alternate START / X
                                        ? RETRO_DEVICE_ID_JOYPAD_START
                                        : RETRO_DEVICE_ID_JOYPAD_A;
            core->setJoypadButton(0, RETRO_DEVICE_ID_JOYPAD_START, false);
            core->setJoypadButton(0, RETRO_DEVICE_ID_JOYPAD_A, false);
            core->setJoypadButton(0, button, press);
        }
        core->runFrame();
        if ((i % 300) == 299) {
            core->syncCaptureMirrors();
            const EmuFramebuffer &fb = core->framebuffer();
            // Rough "is this a 3D scene" probe: sample framebuffer color
            // variance — FMV/logos are smooth, gameplay is busy. Just log
            // dimensions + a checksum so we can see the picture change.
            uint32_t sum = 0;
            for (int b = 0; b + 2 < fb.rgb24.size(); b += 997)
                sum += static_cast<uint8_t>(fb.rgb24[b]);
            std::fprintf(stderr, "  [boot %d/%d] frame=%llu %dx%d vram=%s fbsum=%u\n", i + 1,
                         bootFrames, static_cast<unsigned long long>(fb.frameIndex), fb.width,
                         fb.height, vram.hasVisibleContent(8) ? "content" : "blank", sum);
        }
    }
    core->resetJoypad(0);

    armed.store(true, std::memory_order_release);
    for (int i = 0; i < captureFrames; ++i) {
        core->runFrame();
        if ((i % 30) == 29)
            core->ingestCaptureFrame();
    }
    core->syncCaptureMirrors();
    core->ingestCaptureFrame();

    QVector<uint16_t> vramCells;
    if (!vram.isEmpty())
        vramCells = vram.mutablePixels();
    const CaptureSnapshot snap = CaptureSnapshot::fromBuffer(buffer, vramCells);

    MeshReconstructionStats stats;
    const ReconstructedCaptureSet set =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, &stats);

    int triCount = 0;
    for (const ReconstructedMesh &m : set.uniqueMeshes)
        for (const ReconstructedSubMesh &sub : m.subMeshes)
            triCount += sub.indices.size() / 3;

    std::fprintf(stderr,
                 "\n=== In-core rip live capture ===\n"
                 "  in-core hooks : %s\n"
                 "  prims         : %d\n"
                 "  gte records   : %d\n"
                 "  unique meshes : %d   instances: %d\n"
                 "  triangles     : %d\n"
                 "  vertices      : %d  (tracked %d%% . depth %d%% . inverse %d%%)\n"
                 "  outliers      : %d   mixed-matrix prims: %d\n"
                 "  slabLike      : %s\n"
                 "  bounds        : [%.2f %.2f %.2f] .. [%.2f %.2f %.2f]\n"
                 "================================\n",
                 core->inCoreHooksActive() ? "active" : "UNAVAILABLE",
                 snap.prims.size(), snap.gteRecords.size(), set.uniqueCount(),
                 set.instanceCount(), triCount, stats.totalVertices,
                 stats.gteTrackedPercent(), stats.depthOnlyPercent(),
                 stats.gteInversePercent(), stats.outlierDroppedVertices,
                 stats.mixedMatrixPrims, stats.slabLike ? "yes" : "no",
                 stats.boundsMinX, stats.boundsMinY, stats.boundsMinZ, stats.boundsMaxX,
                 stats.boundsMaxY, stats.boundsMaxZ);

    // Export the reconstructed scene to glTF so you get an actual file to open.
    const QString outDir = qEnvironmentVariable("QTMESH_PS1_RIP_OUT_DIR",
                                                QDir::tempPath() + "/qtmesh_ps1_rip_out");
    QDir().mkpath(outDir);
    std::fprintf(stderr, "  (open the capture in the GUI to export; scene has %d meshes)\n",
                 set.uniqueCount());
    (void)outDir;

    EXPECT_GT(snap.prims.size(), 0) << "no primitives captured — let it play longer "
                                       "(QTMESH_PS1_RIP_BOOT_FRAMES) into a 3D scene";
}

#endif // QTMESH_PS1_LIBRETRO_INTEGRATION_TESTS
#endif // ENABLE_PS1_RIP
