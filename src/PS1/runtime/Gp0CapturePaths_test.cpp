#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/Gp0HookDispatch.h"
#include "PS1/runtime/RipperHooks.h"

#include <QSet>
#include <atomic>
#include <cstring>

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

#endif // ENABLE_PS1_RIP
