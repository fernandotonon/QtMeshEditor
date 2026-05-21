#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/PsxGteEngine.h"
#include "PS1/runtime/PsxGteInstructionCapture.h"
#include "PS1/runtime/PsxMipsGteRunner.h"
#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/RipperHooks.h"

#include <atomic>
#include <cstddef>
#include <cstring>

namespace {

constexpr uint32_t kInsnRtops = 0x42000001u;

uint32_t insnLw(int rt, int rs, int offset)
{
    return (0x23u << 26) | (static_cast<uint32_t>(rs) << 21) | (static_cast<uint32_t>(rt) << 16)
           | static_cast<uint32_t>(offset & 0xFFFF);
}

uint32_t insnCtc2(int gpr, int ctrlIndex)
{
    return (0x12u << 26) | (6u << 21) | (static_cast<uint32_t>(gpr) << 16)
           | (static_cast<uint32_t>(ctrlIndex) << 11);
}

uint32_t insnLui(int rt, uint16_t imm)
{
    return (0x0Fu << 26) | (static_cast<uint32_t>(rt) << 16) | imm;
}

uint32_t insnOri(int rt, int rs, uint16_t imm)
{
    return (0x0Du << 26) | (static_cast<uint32_t>(rs) << 21) | (static_cast<uint32_t>(rt) << 16) | imm;
}

void writeMatrixAt(uint8_t *ram, size_t offset, int trX)
{
    MatrixRecord matrix{};
    matrix.rt.m[0][0] = 1 << 12;
    matrix.rt.m[1][1] = 1 << 12;
    matrix.rt.m[2][2] = 1 << 12;
    matrix.tr[0] = trX;
    matrix.h = 256;
    matrix.ofx = 160 << 16;
    matrix.ofy = 120 << 16;
    std::memcpy(ram + offset, &matrix.rt, sizeof(matrix.rt));
    std::memcpy(ram + offset + sizeof(matrix.rt), matrix.tr, sizeof(matrix.tr));
    std::memcpy(ram + offset + sizeof(matrix.rt) + sizeof(matrix.tr), &matrix.ofx, sizeof(matrix.ofx));
    std::memcpy(ram + offset + sizeof(matrix.rt) + sizeof(matrix.tr) + sizeof(matrix.ofx), &matrix.ofy,
                sizeof(matrix.ofy));
    std::memcpy(ram + offset + sizeof(matrix.rt) + sizeof(matrix.tr) + sizeof(matrix.ofx) + sizeof(matrix.ofy),
                &matrix.h, sizeof(matrix.h));
}

void embedRtopsProgram(uint8_t *ram, size_t matAddr, size_t codeAddr)
{
    writeMatrixAt(ram, matAddr, 0);

    uint32_t *code = reinterpret_cast<uint32_t *>(ram + codeAddr);
    int i = 0;
    const uint16_t hi = static_cast<uint16_t>((matAddr >> 16) & 0xFFFF);
    const uint16_t lo = static_cast<uint16_t>(matAddr & 0xFFFF);
    code[i++] = insnLui(8, hi);
    code[i++] = insnOri(8, 8, lo);
    auto loadCtrl = [&](int ctrlIndex, size_t byteOff) {
        code[i++] = insnLw(9, 8, static_cast<int>(byteOff));
        code[i++] = insnCtc2(9, ctrlIndex);
    };
    for (int ctrl = 0; ctrl <= 4; ++ctrl)
        loadCtrl(ctrl, offsetof(MatrixRecord, rt.m[0][0]) + static_cast<size_t>(ctrl) * sizeof(int32_t));
    loadCtrl(5, offsetof(MatrixRecord, tr[0]));
    loadCtrl(6, offsetof(MatrixRecord, tr[1]));
    loadCtrl(7, offsetof(MatrixRecord, tr[2]));
    loadCtrl(24, offsetof(MatrixRecord, ofx));
    loadCtrl(25, offsetof(MatrixRecord, ofy));
    loadCtrl(26, offsetof(MatrixRecord, h));
    code[i++] = kInsnRtops;
}

} // namespace

TEST(PsxGteCop2Test, EngineRtopsProducesScreenCoords)
{
    PsxGteEngine gte;
    gte.reset();
    gte.writeReg(32, 1 << 12);
    gte.writeReg(34, 1 << 12);
    gte.writeReg(36, 1 << 12);
    gte.writeReg(56, 160 << 16);
    gte.writeReg(57, 120 << 16);
    gte.writeReg(58, 256);
    gte.writeReg(0, 1000 | (2000 << 16));
    gte.writeReg(1, 3000);

    ASSERT_TRUE(gte.executeGteCommand(kInsnRtops));
    const int sx = static_cast<int16_t>(gte.readReg(14) & 0xFFFFu);
    const int sy = static_cast<int16_t>(gte.readReg(14) >> 16);
    EXPECT_GT(sx, 0);
    EXPECT_GT(sy, 0);
}

TEST(PsxGteCop2Test, MipsRunnerExecutesLwc2RtopsBlock)
{
    alignas(4) uint8_t ram[32 * 1024];
    std::memset(ram, 0, sizeof(ram));
    constexpr size_t kCodeAddr = 0x100;
    embedRtopsProgram(ram, 0x1000, kCodeAddr);

    PsxGteEngine gte;
    gte.reset();
    const PsxMipsGteRunner::Result run =
        PsxMipsGteRunner::runBlock(ram, sizeof(ram), kCodeAddr, 64, gte, nullptr);
    EXPECT_GE(run.stepsExecuted, 8);
    EXPECT_GE(run.rtpsEvents, 1);
}

TEST(PsxGteCop2Test, InstructionCaptureFindsCop2RtopsMatrix)
{
    alignas(4) uint8_t ram[32 * 1024];
    std::memset(ram, 0, sizeof(ram));
    embedRtopsProgram(ram, 0x1000, 0x100);

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    hooks.onFrameBegin();
    PsxGteInstructionCapture::captureFromSystemRam(ram, sizeof(ram), &hooks);
    hooks.onFrameEnd();

    EXPECT_GE(buffer.matrices().size(), 1);
    // Camera matrix is inferred from prim usage in endFrame(); COP2-only capture has no prims.
}

#endif // ENABLE_PS1_RIP
