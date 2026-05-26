#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/Gp0HookDispatch.h"
#include "PS1/runtime/PsxGteRamScanner.h"
#include "PS1/runtime/RipperHooks.h"

#include <QSet>

#include <atomic>
#include <cmath>
#include <cstring>
#include <random>

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

// #675 — 45° Y rotation in 12.4 fixed point. Real PS1 SDK matrices look like this:
// orthonormal rows, |row|=4096, det=+4096^3.
MatrixRecord make45DegYRotation(int h = 300)
{
    constexpr double kFixed = 4096.0;
    const int32_t cos45 = static_cast<int32_t>(std::lround(std::cos(M_PI / 4.0) * kFixed));
    const int32_t sin45 = static_cast<int32_t>(std::lround(std::sin(M_PI / 4.0) * kFixed));

    MatrixRecord matrix{};
    matrix.rt.m[0][0] = cos45;
    matrix.rt.m[0][1] = 0;
    matrix.rt.m[0][2] = sin45;
    matrix.rt.m[1][0] = 0;
    matrix.rt.m[1][1] = static_cast<int32_t>(kFixed);
    matrix.rt.m[1][2] = 0;
    matrix.rt.m[2][0] = -sin45;
    matrix.rt.m[2][1] = 0;
    matrix.rt.m[2][2] = cos45;
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
    QSet<QString> seen;
    Gp0HookDispatch::captureFromSystemRam(ram + kGpuPacket, sizeof(ram) - kGpuPacket, &hooks, &seen);
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
    QSet<QString> seen2;
    Gp0HookDispatch::captureFromSystemRam(ram + kGpuOffset, sizeof(ram) - kGpuOffset, &hooks, &seen2);
    hooks.onFrameEnd();

    ASSERT_EQ(buffer.matrices().size(), 1u);
    ASSERT_GE(buffer.prims().size(), 1);
    EXPECT_EQ(buffer.prims()[0].matrixId, 0u);
}

// #675 regression: pre-#675 looksLikeMatrixRecord only checked per-entry magnitude,
// so a scaled rotation (each entry doubled) would still be accepted as a valid
// matrix.  This is the false-positive class that produced ~192 "matrices" in
// real single-box scenes.  With the orthonormal check it must be rejected.
TEST(PsxGteRamScannerTest, RejectsNonOrthonormalScaledRotation)
{
    alignas(4) uint8_t ram[2 * 1024];
    std::memset(ram, 0, sizeof(ram));

    MatrixRecord scaled = makeMatrix(0);
    // Each row magnitude is now (2*4096)^2 = 4*4096^2 — well outside the 5% gate.
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            scaled.rt.m[r][c] *= 2;
    writeMatrix(ram, 0x000, scaled);

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    hooks.onFrameBegin();
    PsxGteRamScanner::captureFromSystemRam(ram, sizeof(ram), &hooks);
    hooks.onFrameEnd();

    EXPECT_EQ(buffer.matrices().size(), 0u);
}

// #675 regression: a real-game 45° Y rotation must still be accepted by the
// tightened scanner.  Pre-tightening this always passed; post-tightening we
// must not over-reject and lock out the actual use case.
TEST(PsxGteRamScannerTest, Accepts45DegYRotation)
{
    alignas(4) uint8_t ram[2 * 1024];
    std::memset(ram, 0, sizeof(ram));
    writeMatrix(ram, 0x000, make45DegYRotation());

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    hooks.onFrameBegin();
    PsxGteRamScanner::captureFromSystemRam(ram, sizeof(ram), &hooks);
    hooks.onFrameEnd();

    EXPECT_EQ(buffer.matrices().size(), 1u);
}

// #675 regression: 16 KB of pseudo-random bytes must produce zero accepted
// matrices.  Pre-tightening this would accept dozens (false-positive class
// that produced the "192 matrices in a single-box scene" stat).  Post-tightening
// the orthonormal invariant rejects all of them — random bytes cannot happen
// to form a unit-length, mutually-orthogonal 3×3 with det == +4096^3.
TEST(PsxGteRamScannerTest, RejectsPseudoRandomGarbage)
{
    constexpr size_t kRamSize = 16 * 1024;
    std::vector<uint8_t> ram(kRamSize, 0);
    std::mt19937 rng(0x675);
    std::uniform_int_distribution<int> byteDist(0, 255);
    for (size_t i = 0; i < kRamSize; ++i)
        ram[i] = static_cast<uint8_t>(byteDist(rng));

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    hooks.onFrameBegin();
    PsxGteRamScanner::captureFromSystemRam(ram.data(), ram.size(), &hooks);
    hooks.onFrameEnd();

    EXPECT_EQ(buffer.matrices().size(), 0u);
}

#endif // ENABLE_PS1_RIP
