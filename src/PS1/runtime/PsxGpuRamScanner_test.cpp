#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/GpuCommandParser.h"
#include "PS1/runtime/PsxGpuRamScanner.h"
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

TEST(PsxGpuRamScannerTest, FindsMonochromeTriangleInRam)
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

    hooks.ingestSystemRamForGpuCapture(ram, sizeof(ram));

    EXPECT_GE(buffer.prims().size(), 1);
    EXPECT_EQ(buffer.prims().first().kind, PrimKind::MonoTri);
}

TEST(GpuCommandParserTest, StepGp0ConsumesSinglePacket)
{
    const uint32_t words[] = {
        colorCmd(0x20, 1, 2, 3),
        pos(0, 0),
        pos(1, 0),
        pos(0, 1),
    };

    const GpuCommandParser::Gp0Step step = GpuCommandParser::stepGp0(words, 4);
    EXPECT_EQ(step.wordsConsumed, 4u);
    ASSERT_TRUE(step.hasPrim);
    EXPECT_EQ(step.prim.kind, PrimKind::MonoTri);
}

#endif // ENABLE_PS1_RIP
