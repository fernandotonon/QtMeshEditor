#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/Gp0HookDispatch.h"
#include "PS1/runtime/PsxOrderingTableScanner.h"
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

void writeU32(uint8_t *ram, size_t byteOffset, uint32_t value)
{
    std::memcpy(ram + byteOffset, &value, sizeof(value));
}

} // namespace

TEST(PsxOrderingTableScannerTest, CapturesPrimitivesFromSyntheticOrderingTable)
{
    alignas(4) uint8_t ram[16 * 1024];
    std::memset(ram, 0, sizeof(ram));

    constexpr uint32_t kOtBase = 0x1000;
    constexpr uint32_t kTriChain = 0x2000;
    constexpr uint32_t kQuadChain = 0x2100;

    writeU32(ram, kOtBase + 0, kTriChain - kOtBase);
    writeU32(ram, kOtBase + 4, 0);
    writeU32(ram, kOtBase + 8, kQuadChain - kOtBase);
    writeU32(ram, kOtBase + 12, 0);

    const uint32_t triPacket[] = {
        colorCmd(0x20, 30, 20, 10),
        pos(8, 16),
        pos(40, 16),
        pos(24, 32),
    };
    std::memcpy(ram + kTriChain, triPacket, sizeof(triPacket));

    const uint32_t quadPacket[] = {
        colorCmd(0x28, 10, 20, 30),
        pos(0, 0),
        pos(32, 0),
        pos(32, 32),
        pos(0, 32),
    };
    std::memcpy(ram + kQuadChain, quadPacket, sizeof(quadPacket));

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    const int prims = PsxOrderingTableScanner::captureFromOrderingTables(ram, sizeof(ram), &hooks);
    EXPECT_GE(prims, 2);
    EXPECT_GE(buffer.prims().size(), 2);

    bool hasTri = false;
    bool hasQuad = false;
    for (const PrimRecord &prim : buffer.prims()) {
        if (prim.kind == PrimKind::MonoTri)
            hasTri = true;
        if (prim.kind == PrimKind::MonoQuad)
            hasQuad = true;
    }
    EXPECT_TRUE(hasTri);
    EXPECT_TRUE(hasQuad);
}

TEST(PsxOrderingTableScannerTest, LinkedChainCapture)
{
    alignas(4) uint8_t ram[16 * 1024];
    std::memset(ram, 0, sizeof(ram));

    constexpr uint32_t kOtBase = 0x400;
    constexpr uint32_t kChain = 0x800;

    const uint32_t linkOffsetWords = 4; // tri packet is 4 words; second packet follows immediately
    const uint32_t linkedHeader =
        (static_cast<uint32_t>(0x20) << 24) | (linkOffsetWords << 2) | 1u;
    const uint32_t firstPacket[] = {
        linkedHeader,
        pos(0, 0),
        pos(10, 0),
        pos(5, 10),
    };
    constexpr uint32_t kSecond = kChain + static_cast<uint32_t>(sizeof(firstPacket));

    writeU32(ram, kOtBase, kChain - kOtBase);
    std::memcpy(ram + kChain, firstPacket, sizeof(firstPacket));

    const uint32_t secondPacket[] = {
        colorCmd(0x28, 4, 5, 6),
        pos(20, 20),
        pos(40, 20),
        pos(40, 40),
        pos(20, 40),
    };
    std::memcpy(ram + kSecond, secondPacket, sizeof(secondPacket));

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    const int prims = PsxOrderingTableScanner::captureFromOrderingTables(ram, sizeof(ram), &hooks);
    EXPECT_GE(prims, 2);
}

#endif // ENABLE_PS1_RIP
