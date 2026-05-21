#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/Gp0HookDispatch.h"
#include "PS1/runtime/PsxGteRamScanner.h"
#include "PS1/runtime/RipperHooks.h"

#include <atomic>
#include <cstring>

namespace {

constexpr size_t kMatrixBytes = sizeof(MatrixRecord) - sizeof(uint64_t);

void writeMatrix(uint8_t *ram, size_t byteOffset, const MatrixRecord &matrix)
{
    std::memcpy(ram + byteOffset, &matrix.rt, sizeof(matrix.rt));
    std::memcpy(ram + byteOffset + sizeof(matrix.rt), matrix.tr, sizeof(matrix.tr));
    std::memcpy(ram + byteOffset + sizeof(matrix.rt) + sizeof(matrix.tr), &matrix.ofx,
                sizeof(matrix.ofx));
    std::memcpy(ram + byteOffset + sizeof(matrix.rt) + sizeof(matrix.tr) + sizeof(matrix.ofx),
                &matrix.ofy, sizeof(matrix.ofy));
    std::memcpy(ram + byteOffset + sizeof(matrix.rt) + sizeof(matrix.tr) + sizeof(matrix.ofx)
                    + sizeof(matrix.ofy),
                &matrix.h, sizeof(matrix.h));
}

MatrixRecord makeMatrix(int trX, int h = 256)
{
    MatrixRecord matrix{};
    matrix.rt.m[0][0] = 1 << 12;
    matrix.rt.m[1][1] = 1 << 12;
    matrix.rt.m[2][2] = 1 << 12;
    matrix.tr[0] = trX;
    matrix.h = h;
    matrix.ofx = 160 << 16;
    matrix.ofy = 120 << 16;
    return matrix;
}

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

TEST(PsxGteRamScannerTest, SyntheticGteRamLayoutDedupesMatricesAndTagsPrims)
{
    alignas(4) uint8_t ram[8 * 1024];
    std::memset(ram, 0, sizeof(ram));

    constexpr size_t kMatrixA = 0x000;
    constexpr size_t kMatrixB = 0x080;
    constexpr size_t kMatrixDup = 0x100;
    constexpr size_t kGpuPacket = 0x200;

    writeMatrix(ram, kMatrixA, makeMatrix(0));
    writeMatrix(ram, kMatrixB, makeMatrix(512));
    writeMatrix(ram, kMatrixDup, makeMatrix(0));

    const uint32_t triPacket[] = {
        colorCmd(0x20, 10, 20, 30),
        pos(16, 16),
        pos(48, 16),
        pos(32, 32),
    };
    std::memcpy(ram + kGpuPacket, triPacket, sizeof(triPacket));

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    hooks.onFrameBegin();
    PsxGteRamScanner::captureFromSystemRam(ram, kGpuPacket, &hooks);
    Gp0HookDispatch::captureFromSystemRam(ram + kGpuPacket, sizeof(ram) - kGpuPacket, &hooks);
    hooks.onFrameEnd();

    ASSERT_EQ(buffer.matrices().size(), 2);
    EXPECT_TRUE(buffer.hasCameraMatrix());
    // Last matrix registered before the GPU packet tags captured prims (matrix B at 0x080).
    EXPECT_EQ(buffer.cameraMatrixId(), 1u);

    ASSERT_GE(buffer.prims().size(), 1);
    for (const PrimRecord &prim : buffer.prims())
        EXPECT_LT(prim.matrixId, 2u);
}

TEST(PsxGteRamScannerTest, GteScanRunsBeforeGpuFallbackMatrix)
{
    alignas(4) uint8_t ram[4 * 1024];
    std::memset(ram, 0, sizeof(ram));

    constexpr size_t kMatrixOffset = 0x000;
    constexpr size_t kGpuOffset = 0x100;
    writeMatrix(ram, kMatrixOffset, makeMatrix(1024));

    const uint32_t triPacket[] = {
        colorCmd(0x20, 1, 2, 3),
        pos(8, 8),
        pos(24, 8),
        pos(16, 20),
    };
    std::memcpy(ram + kGpuOffset, triPacket, sizeof(triPacket));

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    hooks.onFrameBegin();
    PsxGteRamScanner::captureFromSystemRam(ram, kGpuOffset, &hooks);
    Gp0HookDispatch::captureFromSystemRam(ram + kGpuOffset, sizeof(ram) - kGpuOffset, &hooks);
    hooks.onFrameEnd();

    ASSERT_EQ(buffer.matrices().size(), 1u);
    ASSERT_GE(buffer.prims().size(), 1);
    EXPECT_EQ(buffer.prims()[0].matrixId, 0u);
}

#endif // ENABLE_PS1_RIP
