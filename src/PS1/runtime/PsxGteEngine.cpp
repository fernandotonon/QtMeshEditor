#include "PsxGteEngine.h"

#include "GteCapture.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr int32_t kIr123Min = -(1 << 15);
constexpr int32_t kIr123Max = (1 << 15) - 1;

int32_t signExtend16(uint32_t value)
{
    return static_cast<int32_t>(static_cast<int16_t>(value & 0xFFFFu));
}

int32_t clampIr123(int32_t value, bool lm)
{
    const int32_t minVal = lm ? 0 : kIr123Min;
    return std::clamp(value, minVal, kIr123Max);
}

uint32_t unrDivide(uint32_t lhs, uint32_t rhs)
{
    if (rhs == 0)
        return 0;
    if (rhs * 2u <= lhs)
        return 0x1FFFFu;

    uint32_t shift = 0;
    for (uint32_t bit = 0x80000000u; bit != 0 && (rhs & bit) == 0; bit >>= 1)
        ++shift;

    lhs <<= shift;
    rhs <<= shift;

    const uint32_t divisor = rhs | 0x8000u;
    const int32_t x = static_cast<int32_t>(0x101u + ((divisor & 0x7FFFu) + 0x40u) >> 7);
    const int32_t d = ((static_cast<int32_t>(divisor) * -x) + 0x80) >> 8;
    const uint32_t recip =
        static_cast<uint32_t>(((x * (0x20000 + d)) + 0x80) >> 8);

    const uint64_t result = (static_cast<uint64_t>(lhs) * static_cast<uint64_t>(recip) + 0x8000u) >> 16;
    return std::min<uint32_t>(0x1FFFFu, static_cast<uint32_t>(result));
}

} // namespace

void PsxGteEngine::reset()
{
    std::memset(&m_regs, 0, sizeof(m_regs));
}

uint32_t PsxGteEngine::readReg(int index) const
{
    if (index < 0 || index >= kNumRegs)
        return 0;
    return static_cast<uint32_t>(m_regs.r32[index]);
}

void PsxGteEngine::writeReg(int index, uint32_t value)
{
    if (index < 0 || index >= kNumRegs)
        return;

    switch (index) {
    case 1:
    case 3:
    case 5:
    case 8:
    case 9:
    case 10:
    case 11:
    case 36:
    case 44:
    case 52:
    case 58:
    case 59:
    case 61:
    case 62:
        m_regs.r32[index] = signExtend16(value);
        break;
    case 7:
    case 16:
    case 17:
    case 18:
    case 19:
        m_regs.r32[index] = static_cast<int32_t>(value & 0xFFFFu);
        break;
    case 15:
        m_regs.r32[12] = m_regs.r32[13];
        m_regs.r32[13] = m_regs.r32[14];
        m_regs.r32[14] = static_cast<int32_t>(value);
        break;
    case 29:
    case 31:
        break;
    default:
        m_regs.r32[index] = static_cast<int32_t>(value);
        break;
    }
}

MatrixRecord PsxGteEngine::matrixRecord() const
{
    MatrixRecord matrix{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            matrix.rt.m[r][c] = signExtend16(static_cast<uint32_t>(m_regs.r32[32 + r * 3 + c]));
    }
    matrix.tr[0] = m_regs.r32[37];
    matrix.tr[1] = m_regs.r32[38];
    matrix.tr[2] = m_regs.r32[39];
    matrix.ofx = m_regs.r32[56];
    matrix.ofy = m_regs.r32[57];
    matrix.h = static_cast<int32_t>(m_regs.r32[58] & 0xFFFFu);
    matrix.hash = GteCapture::hashMatrix(matrix);
    return matrix;
}

void PsxGteEngine::loadWordToReg(int cop2Reg, uint32_t value)
{
    writeReg(cop2Reg, value);
}

void PsxGteEngine::pushScreen(int sx, int sy, int sz)
{
    sx = std::clamp(sx, -1024, 1023);
    sy = std::clamp(sy, -1024, 1023);
    sz = std::clamp(sz, 0, 0xFFFF);

    m_regs.r32[12] = m_regs.r32[13];
    m_regs.r32[13] = m_regs.r32[14];
    m_regs.r32[14] = (sx & 0xFFFF) | (sy << 16);

    m_regs.r32[16] = m_regs.r32[17];
    m_regs.r32[17] = m_regs.r32[18];
    m_regs.r32[18] = m_regs.r32[19];
    m_regs.r32[19] = sz;
}

bool PsxGteEngine::rtpsOnVector(int vx, int vy, int vz, bool last)
{
    const bool sf = (m_regs.r32[63] & 0x100000) != 0; // unused; shift from insn instead
    (void)sf;
    const uint8_t shift = 12;
    const bool lm = false;

    auto dotRow = [&](int row) -> int64_t {
        const int64_t tr = static_cast<int64_t>(m_regs.r32[37 + row]) << 12;
        const int64_t r0 = static_cast<int64_t>(signExtend16(static_cast<uint32_t>(m_regs.r32[32 + row * 3 + 0])))
                           * vx;
        const int64_t r1 = static_cast<int64_t>(signExtend16(static_cast<uint32_t>(m_regs.r32[32 + row * 3 + 1])))
                           * vy;
        const int64_t r2 = static_cast<int64_t>(signExtend16(static_cast<uint32_t>(m_regs.r32[32 + row * 3 + 2])))
                           * vz;
        return tr + r0 + r1 + r2;
    };

    const int64_t mac1 = dotRow(0) >> shift;
    const int64_t mac2 = dotRow(1) >> shift;
    const int64_t mac3 = dotRow(2) >> shift;

    m_regs.r32[25] = static_cast<int32_t>(mac1);
    m_regs.r32[26] = static_cast<int32_t>(mac2);
    m_regs.r32[27] = static_cast<int32_t>(mac3);
    m_regs.r32[9] = clampIr123(static_cast<int32_t>(mac1), lm);
    m_regs.r32[10] = clampIr123(static_cast<int32_t>(mac2), lm);
    m_regs.r32[11] = clampIr123(static_cast<int32_t>(mac3 >> 12), false);

    const int32_t sz3 = static_cast<int32_t>(mac3 >> 12);
    pushScreen(0, 0, sz3);

    const uint32_t h = static_cast<uint32_t>(m_regs.r32[58] & 0xFFFFu);
    const uint32_t div = unrDivide(h, static_cast<uint32_t>(sz3));
    const int64_t sx = (static_cast<int64_t>(div) * m_regs.r32[9] + m_regs.r32[56]) >> 16;
    const int64_t sy = (static_cast<int64_t>(div) * m_regs.r32[10] + m_regs.r32[57]) >> 16;
    pushScreen(static_cast<int>(sx), static_cast<int>(sy), sz3);

    if (last) {
        const int64_t depth = static_cast<int64_t>(div) * m_regs.r32[59] + m_regs.r32[60];
        m_regs.r32[24] = static_cast<int32_t>(depth);
        m_regs.r32[8] = static_cast<int32_t>(depth >> 12);
    }
    return true;
}

bool PsxGteEngine::executeGteCommand(uint32_t insn)
{
    const uint32_t cmd = insn & 0x3Fu;
    const bool sf = (insn & 0x100000) != 0;
    (void)sf;

    switch (cmd) {
    case 0x01: { // RTPS
        const int vx = signExtend16(static_cast<uint32_t>(m_regs.r32[0]));
        const int vy = signExtend16(static_cast<uint32_t>(m_regs.r32[0] >> 16));
        const int vz = signExtend16(static_cast<uint32_t>(m_regs.r32[1]));
        rtpsOnVector(vx, vy, vz, true);
        return true;
    }
    case 0x30: { // RTPT
        for (int i = 0; i < 3; ++i) {
            const int base = i * 2;
            const int vx = signExtend16(static_cast<uint32_t>(m_regs.r32[base]));
            const int vy = signExtend16(static_cast<uint32_t>(m_regs.r32[base] >> 16));
            const int vz = signExtend16(static_cast<uint32_t>(m_regs.r32[base + 1]));
            rtpsOnVector(vx, vy, vz, i == 2);
        }
        return true;
    }
    default:
        return false;
    }
}
