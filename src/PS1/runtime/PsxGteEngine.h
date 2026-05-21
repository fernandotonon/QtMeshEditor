#ifndef PSXGTEENGINE_H
#define PSXGTEENGINE_H

#include "CaptureTypes.h"

#include <cstdint>

/** Software model of the PS1 GTE register file (duckstation-compatible layout). */
class PsxGteEngine
{
public:
    static constexpr int kNumRegs = 64;

    void reset();

    uint32_t readReg(int index) const;
    void writeReg(int index, uint32_t value);

    /** Build capture matrix from RT/TR/OFX/OFY/H control registers. */
    MatrixRecord matrixRecord() const;

    /** Execute one COP2 GTE command word (0x4200xxxx). Returns true on RTPS/RTPT. */
    bool executeGteCommand(uint32_t insn);

    /** Load one 32-bit word into a GTE register (LWC2). */
    void loadWordToReg(int cop2Reg, uint32_t value);

private:
    struct Regs {
        int32_t r32[kNumRegs]{};
    };

    Regs m_regs{};

    void pushScreen(int sx, int sy, int sz);
    bool rtpsOnVector(int vx, int vy, int vz, bool last);
};

#endif // PSXGTEENGINE_H
