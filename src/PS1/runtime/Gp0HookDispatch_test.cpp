#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/Gp0CaptureStats.h"
#include "PS1/runtime/Gp0HookDispatch.h"
#include "PS1/runtime/RipperHooks.h"

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

TEST(Gp0HookDispatchTest, DisarmedCaptureDoesNotRecordPrimitives)
{
    const uint32_t words[] = {
        colorCmd(0x20, 30, 20, 10),
        pos(8, 16),
        pos(40, 16),
        pos(24, 32),
    };

    alignas(4) uint8_t ram[sizeof(words)];
    std::memcpy(ram, words, sizeof(words));

    std::atomic<bool> armed{false};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    hooks.ingestSystemRamForGpuCapture(ram, sizeof(ram));
    EXPECT_TRUE(buffer.prims().isEmpty());

    armed.store(true, std::memory_order_release);
    hooks.ingestSystemRamForGpuCapture(ram, sizeof(ram), true, false);
    EXPECT_GE(buffer.prims().size(), 1);
    EXPECT_EQ(hooks.lastCaptureStats().primarySource, Gp0CaptureSource::RamLinear);
}

#endif // ENABLE_PS1_RIP
