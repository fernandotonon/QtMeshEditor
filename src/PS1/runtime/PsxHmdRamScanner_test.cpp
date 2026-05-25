#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/Gp0CaptureStats.h"
#include "PS1/runtime/Gp0HookDispatch.h"
#include "PS1/runtime/PsxHmdRamScanner.h"
#include "PS1/runtime/RipperHooks.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace {

constexpr uint32_t kHmdMagic = 0x00000050u;

void writeU32le(uint8_t *dst, uint32_t value)
{
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

} // namespace

TEST(PsxHmdRamScannerTest, CountsPlausibleHmdCandidates)
{
    std::vector<uint8_t> ram(64u * 1024u, 0u);
    // Plausible-looking HMD header at 0x2000.
    writeU32le(ram.data() + 0x2000, kHmdMagic);
    writeU32le(ram.data() + 0x2000 + 4, 0x1000u);  // mapLen — within bounds
    writeU32le(ram.data() + 0x2000 + 8, 4u);       // numPrimHeaders — modest
    writeU32le(ram.data() + 0x2000 + 12, 0u);

    EXPECT_EQ(PsxHmdRamScanner::countHmdCandidates(ram.data(), ram.size()), 1);

    // A nonsense "HMD" with insane map length should NOT count.
    writeU32le(ram.data() + 0x4000, kHmdMagic);
    writeU32le(ram.data() + 0x4000 + 4, 0xFFFFFFFFu); // mapLen way out of bounds
    writeU32le(ram.data() + 0x4000 + 8, 1u);

    EXPECT_EQ(PsxHmdRamScanner::countHmdCandidates(ram.data(), ram.size()), 1)
        << "Out-of-range mapLen must be filtered out";
}

TEST(PsxHmdRamScannerTest, EmitsNothingByDefault)
{
    // v1 is opt-in via QTMESH_PS1_HMD_SCANNER. With the env var unset, the scanner returns
    // 0 even when valid-looking candidates exist — no model meshes should be emitted.
    qunsetenv("QTMESH_PS1_HMD_SCANNER");

    std::vector<uint8_t> ram(64u * 1024u, 0u);
    writeU32le(ram.data() + 0x2000, kHmdMagic);
    writeU32le(ram.data() + 0x2000 + 4, 0x1000u);
    writeU32le(ram.data() + 0x2000 + 8, 4u);

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    EXPECT_EQ(PsxHmdRamScanner::captureFromSystemRam(ram.data(), ram.size(), &hooks), 0);
    EXPECT_EQ(buffer.modelMeshes().size(), 0);
}

// #674 review: a bare HMD-candidate hit (no actual onModelMesh emission) must NOT
// flip primarySource to RamModelMesh. The frame-capture pipeline writes the count
// into Gp0CaptureStats::ramHmdCandidates instead.
TEST(PsxHmdRamScannerTest, HmdCandidatesDoNotPromoteModelMeshSource)
{
    qputenv("QTMESH_PS1_HMD_SCANNER", QByteArrayLiteral("1"));

    std::vector<uint8_t> ram(64u * 1024u, 0u);
    writeU32le(ram.data() + 0x2000, kHmdMagic);
    writeU32le(ram.data() + 0x2000 + 4, 0x1000u);
    writeU32le(ram.data() + 0x2000 + 8, 4u);

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    const Gp0CaptureStats stats =
        Gp0HookDispatch::captureFrameFromSystemRam(ram.data(), ram.size(), &hooks,
                                                   /*scanGteRam=*/false,
                                                   /*accumulate=*/false);
    qunsetenv("QTMESH_PS1_HMD_SCANNER");

    EXPECT_GE(stats.ramHmdCandidates, 1);
    EXPECT_EQ(stats.ramHmdMeshes, 0);
    // Without emitted TMD or HMD meshes, primarySource must NOT be RamModelMesh.
    EXPECT_NE(stats.primarySource, Gp0CaptureSource::RamModelMesh);
}

#endif // ENABLE_PS1_RIP
