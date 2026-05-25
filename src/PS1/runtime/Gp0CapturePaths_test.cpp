#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/Gp0HookDispatch.h"
#include "PS1/runtime/RipperHooks.h"

#include <QSet>
#include <atomic>
#include <cstring>
#include <iterator>

namespace {

uint32_t colorCmd(uint8_t opcode, uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint32_t>(opcode) | (static_cast<uint32_t>(r) << 8)
           | (static_cast<uint32_t>(g) << 16) | (static_cast<uint32_t>(b) << 24);
}

uint32_t pos(int x, int y)
{
    return static_cast<uint32_t>((y & 0xFFFF) << 16) | static_cast<uint32_t>(x & 0xFFFF);
}

} // namespace

TEST(Gp0CapturePathsTest, DirectHookSubmitGp0MatchesLinearBaseline)
{
    const uint32_t words[] = {
        colorCmd(0x20, 30, 20, 10),
        pos(8, 16),
        pos(40, 16),
        pos(24, 32),
    };

    alignas(4) uint8_t ram[sizeof(words)];
    std::memcpy(ram, words, sizeof(words));

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    const Gp0CaptureStats legacy =
        Gp0HookDispatch::captureFromSystemRamLegacy(ram, sizeof(ram), &hooks);
    ASSERT_GE(legacy.totalPrims, 1);

    buffer.clear();
    const int hookPrims = hooks.submitGp0Words(words, 4);
    EXPECT_GE(hookPrims, 1);
    EXPECT_EQ(buffer.prims().size(), legacy.totalPrims);
}

TEST(Gp0CapturePathsTest, MergedRamCaptureIncludesLinearWhenOtEmpty)
{
    const uint32_t words[] = {
        colorCmd(0x20, 5, 6, 7),
        pos(12, 20),
        pos(44, 20),
        pos(28, 36),
    };

    alignas(4) uint8_t ram[sizeof(words)];
    std::memcpy(ram, words, sizeof(words));

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    QSet<QString> seen;
    const Gp0CaptureStats merged =
        Gp0HookDispatch::captureFromSystemRam(ram, sizeof(ram), &hooks, &seen);
    EXPECT_GE(merged.totalPrims, 1);
    EXPECT_GE(merged.ramLinearPrims, 1);

    buffer.clear();
    seen.clear();
    const Gp0CaptureStats legacy =
        Gp0HookDispatch::captureFromSystemRamLegacy(ram, sizeof(ram), &hooks);
    EXPECT_GE(merged.totalPrims, legacy.totalPrims);
}

// #662 live FIFO bridge: RAM-resident DMA chains dispatched through
// submitGp0Words must surface as Gp0CaptureSource::DirectHook so the session
// UI shows them under the gp0_hook column rather than RAM_OT.
//
// A real PS1 GP0 DMA chain links N-1 packets via header words with the chain
// bit (bit 0) set and the GP0 opcode in bits 24-31 (libgpu `getaddr` layout);
// the terminal packet uses the normal opcode-in-low-byte layout with bit 0=0.
// The bridge walks contiguous chains so the terminal must immediately follow
// the last linked packet in RAM.
TEST(Gp0CapturePathsTest, FifoBridgeAttributesChainPrimsAsDirectHook)
{
    constexpr size_t kRamWords = 16;
    alignas(4) uint8_t ram[kRamWords * 4] = {};
    auto putWord = [&](size_t i, uint32_t w) {
        std::memcpy(ram + i * 4, &w, sizeof(w));
    };

    // Packet A: chain header pointing at byte 16, opcode 0x20 in high byte,
    // chain bit set. Followed by 3 pos words (4-word mono-tri payload).
    const uint32_t chainHeaderA = (0x20u << 24) | static_cast<uint32_t>(16u) | 1u;
    putWord(0, chainHeaderA);
    putWord(1, pos(8, 16));
    putWord(2, pos(40, 16));
    putWord(3, pos(24, 32));

    // Packet B: terminal mono-tri (opcode in LOW byte, no chain bit).
    putWord(4, colorCmd(0x20, 30, 20, 10));
    putWord(5, pos(60, 16));
    putWord(6, pos(80, 16));
    putWord(7, pos(70, 32));

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    const int dispatched = hooks.submitFifoChainsFromRam(ram, sizeof(ram));
    EXPECT_GE(dispatched, 2);
    EXPECT_GE(buffer.prims().size(), 2);
    EXPECT_GE(hooks.lastDirectHookPrimCount(), 2);
}

// #662 attribution: a full frame capture pass (FIFO bridge + merged RAM scan)
// must report DirectHook prims separately from RamOT/RamLinear in the stats.
TEST(Gp0CapturePathsTest, CaptureFrameReportsDirectHookFromBridge)
{
    constexpr size_t kRamBytes = 64 * 4;
    alignas(4) uint8_t ram[kRamBytes] = {};
    auto putWord = [&](size_t i, uint32_t w) {
        std::memcpy(ram + i * 4, &w, sizeof(w));
    };

    // Two-packet chain at offset 0 — bridge picks them up.
    putWord(0, (0x20u << 24) | static_cast<uint32_t>(16u) | 1u);
    putWord(1, pos(10, 10));
    putWord(2, pos(40, 10));
    putWord(3, pos(25, 30));
    putWord(4, colorCmd(0x20, 1, 2, 3));
    putWord(5, pos(60, 10));
    putWord(6, pos(80, 10));
    putWord(7, pos(70, 30));

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    const Gp0CaptureStats stats =
        Gp0HookDispatch::captureFrameFromSystemRam(ram, sizeof(ram), &hooks, /*scanGteRam=*/false,
                                                   /*accumulate=*/false);
    EXPECT_GE(stats.totalPrims, 2);
    EXPECT_GE(stats.directHookPrims, 2);
    EXPECT_EQ(stats.primarySource, Gp0CaptureSource::DirectHook);
}

// #662 stub parity: the stub plugin's seven-flavor capture must route through
// submitGp0Words so prims are attributed to Gp0CaptureSource::DirectHook and
// the stub validates the same code path that retail captures use.
TEST(Gp0CapturePathsTest, SubmitGp0WordsBumpsDirectHookCount)
{
    // Two minimal mono-tri packets dispatched as a contiguous FIFO stream.
    const uint32_t words[] = {
        colorCmd(0x20, 1, 2, 3),
        pos(8, 16),
        pos(40, 16),
        pos(24, 32),
        colorCmd(0x20, 4, 5, 6),
        pos(60, 16),
        pos(80, 16),
        pos(70, 32),
    };

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    const int dispatched = hooks.submitGp0Words(words, std::size(words));
    EXPECT_GE(dispatched, 2);
    EXPECT_GE(hooks.lastDirectHookPrimCount(), 2);
}

// QTMESH_PS1_GP0_FIFO_BRIDGE=0 disables the bridge (A/B comparison knob).
TEST(Gp0CapturePathsTest, FifoBridgeEnvDisableHonoured)
{
    constexpr size_t kRamBytes = 64 * 4;
    alignas(4) uint8_t ram[kRamBytes] = {};
    auto putWord = [&](size_t i, uint32_t w) {
        std::memcpy(ram + i * 4, &w, sizeof(w));
    };
    putWord(0, (0x20u << 24) | static_cast<uint32_t>(16u) | 1u);
    putWord(1, pos(10, 10));
    putWord(2, pos(40, 10));
    putWord(3, pos(25, 30));
    putWord(4, colorCmd(0x20, 1, 2, 3));
    putWord(5, pos(60, 10));
    putWord(6, pos(80, 10));
    putWord(7, pos(70, 30));

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    qputenv("QTMESH_PS1_GP0_FIFO_BRIDGE", QByteArrayLiteral("0"));
    const Gp0CaptureStats stats =
        Gp0HookDispatch::captureFrameFromSystemRam(ram, sizeof(ram), &hooks, /*scanGteRam=*/false,
                                                   /*accumulate=*/false);
    qunsetenv("QTMESH_PS1_GP0_FIFO_BRIDGE");

    EXPECT_EQ(stats.directHookPrims, 0);
    // The merged RAM scan still picks the chain up via the chain-root scanner
    // so total prims should remain >= 2 — only the source attribution differs.
    EXPECT_GE(stats.totalPrims, 2);
    EXPECT_NE(stats.primarySource, Gp0CaptureSource::DirectHook);
}

// submitGp0Words honours a caller-supplied maxPrims so chained submits share
// a single per-frame budget (#662 review).
TEST(Gp0CapturePathsTest, SubmitGp0WordsHonoursMaxPrimsCap)
{
    // 4 triangles total — should be trivially dispatchable, but we cap to 2.
    const uint32_t words[] = {
        colorCmd(0x20, 1, 2, 3),  pos(8, 16),  pos(40, 16), pos(24, 32),
        colorCmd(0x20, 4, 5, 6),  pos(60, 16), pos(80, 16), pos(70, 32),
        colorCmd(0x20, 7, 8, 9),  pos(80, 16), pos(100, 16), pos(90, 32),
        colorCmd(0x20, 10, 11, 12), pos(110, 16), pos(130, 16), pos(120, 32),
    };

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    const int dispatched =
        Gp0HookDispatch::submitGp0Words(words, std::size(words), &hooks, /*maxPrims=*/2);
    EXPECT_EQ(dispatched, 2);
    EXPECT_EQ(buffer.prims().size(), 2);
}

// lastCaptureStatsFresh() reports false until endGpuCapturePass runs, so
// PS1RipWorker can detect stub-core paths that never ran a GP0 pass and
// avoid surfacing stale stats (#662 review).
TEST(Gp0CapturePathsTest, CaptureStatsFreshnessTracking)
{
    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    EXPECT_FALSE(hooks.lastCaptureStatsFresh());

    constexpr size_t kRamBytes = 64 * 4;
    alignas(4) uint8_t ram[kRamBytes] = {};
    auto putWord = [&](size_t i, uint32_t w) { std::memcpy(ram + i * 4, &w, sizeof(w)); };
    putWord(0, colorCmd(0x20, 1, 2, 3));
    putWord(1, pos(8, 16));
    putWord(2, pos(40, 16));
    putWord(3, pos(24, 32));

    Gp0HookDispatch::captureFrameFromSystemRam(ram, sizeof(ram), &hooks, /*scanGteRam=*/false,
                                               /*accumulate=*/false);
    EXPECT_TRUE(hooks.lastCaptureStatsFresh());

    hooks.markCaptureStatsConsumed();
    EXPECT_FALSE(hooks.lastCaptureStatsFresh());
}

#endif // ENABLE_PS1_RIP
