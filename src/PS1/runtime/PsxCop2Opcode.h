#ifndef PSXCOP2OPCODE_H
#define PSXCOP2OPCODE_H

#include <cstdint>

/** COP2 primary opcode (MIPS bits 31–26). */
inline constexpr uint32_t kPsxCop2PrimaryOpcode = 0x12u;

/** Common macro-assembler RTPS / RTPT encodings (not standard COP2 primary opcode). */
inline bool psxIsGteMacroCommand(uint32_t insn)
{
    return insn == 0x42000001u || insn == 0x42000030u;
}

inline bool psxIsCop2Primary(uint32_t insn)
{
    return (insn >> 26) == kPsxCop2PrimaryOpcode;
}

/** True for RTPS/RTPT — standard COP2 (rs=1) or mednafen-style macro words. */
inline bool psxIsGteCommand(uint32_t insn)
{
    if (psxIsGteMacroCommand(insn))
        return true;
    const uint32_t cmd = insn & 0x3Fu;
    if (cmd != 0x01u && cmd != 0x30u)
        return false;
    return psxIsCop2Primary(insn) && ((insn >> 21) & 0x1Fu) == 0x01u;
}

#endif // PSXCOP2OPCODE_H
