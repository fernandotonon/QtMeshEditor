#include "PsxMipsGteRunner.h"

#include "EmuHooks.h"
#include "PsxGp0Opcode.h"

#include <cstring>

namespace {

constexpr uint32_t kCop2Opcode = 0x12u; // bits 31-26

bool isCop2(uint32_t insn)
{
    return (insn >> 26) == kCop2Opcode;
}

uint32_t fetchInsn(const uint8_t *ram, size_t ramBytes, uint32_t pc)
{
    if (pc + 4 > ramBytes)
        return 0;
    uint32_t word = 0;
    std::memcpy(&word, ram + pc, sizeof(word));
    return word;
}

uint32_t signExtendImm16(uint32_t imm)
{
    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(imm & 0xFFFFu)));
}

} // namespace

PsxMipsGteRunner::Result PsxMipsGteRunner::runBlock(const uint8_t *ram, size_t ramBytes,
                                                    uint32_t startPc, int maxSteps,
                                                    PsxGteEngine &gte, EmuHooks *hooks)
{
    Result result{};
    if (!ram || ramBytes < 4 || maxSteps <= 0)
        return result;

    uint32_t gpr[32]{};
    uint32_t pc = startPc & ~3u;

    for (int step = 0; step < maxSteps; ++step) {
        const uint32_t insn = fetchInsn(ram, ramBytes, pc);
        pc += 4;
        if (insn == 0)
            break;

        const uint32_t op = insn >> 26;

        if (op == 0x26) { // LWC2 (primary opcode 100110)
            const int cop2Rt = static_cast<int>((insn >> 16) & 0x1Fu);
            const int base = static_cast<int>((insn >> 21) & 0x1Fu);
            const uint32_t offset = signExtendImm16(insn);
            const uint32_t addr = gpr[base] + offset;
            if (addr + 4 <= ramBytes)
                gte.loadWordToReg(cop2Rt, fetchInsn(ram, ramBytes, addr));
            ++result.stepsExecuted;
            continue;
        }

        if (op == kCop2Opcode) {
            const uint32_t rs = (insn >> 21) & 0x1Fu;
            const uint32_t rt = (insn >> 16) & 0x1Fu;
            const uint32_t rd = (insn >> 11) & 0x1Fu;

            if (rs == 0x04) { // MTC2 — rt=GPR, rd=cop2 reg
                gte.writeReg(static_cast<int>(rd), gpr[rt]);
            } else if (rs == 0x06) { // CTC2
                gte.writeReg(static_cast<int>(rd), gpr[rt]);
            } else if (rs == 0x01) { // GTE command (RTPS/RTPT/…)
                if (gte.executeGteCommand(insn)) {
                    ++result.rtpsEvents;
                    if (hooks && hooks->isCaptureEnabled()) {
                        MatrixRecord matrix = gte.matrixRecord();
                        hooks->onGteMatrix(matrix);
                    }
                }
            }
            ++result.stepsExecuted;
            continue;
        }

        if (op == 0x0F) { // SPECIAL
            const uint32_t funct = insn & 0x3Fu;
            if (funct == 0x08 || funct == 0x09) // JR/JALR
                break;
        }

        if (op == 0x00) { // SPECIAL
            const uint32_t rs = (insn >> 21) & 0x1Fu;
            const uint32_t rt = (insn >> 16) & 0x1Fu;
            const uint32_t rd = (insn >> 11) & 0x1Fu;
            const uint32_t funct = insn & 0x3Fu;
            const uint32_t shamt = (insn >> 6) & 0x1Fu;

            switch (funct) {
            case 0x00: // SLL
                gpr[rd] = gpr[rt] << shamt;
                break;
            case 0x21: // ADDU
                gpr[rd] = gpr[rs] + gpr[rt];
                break;
            case 0x25: // OR
                gpr[rd] = gpr[rs] | gpr[rt];
                break;
            case 0x08: // JR
            case 0x09: // JALR
                ++result.stepsExecuted;
                return result;
            default:
                break;
            }
            ++result.stepsExecuted;
            continue;
        }

        if (op == 0x0D) { // ORI
            const uint32_t rs = (insn >> 21) & 0x1Fu;
            const uint32_t rt = (insn >> 16) & 0x1Fu;
            const uint32_t imm = insn & 0xFFFFu;
            gpr[rt] = gpr[rs] | imm;
            ++result.stepsExecuted;
            continue;
        }

        if (op == 0x0F) { // LUI
            const uint32_t rt = (insn >> 16) & 0x1Fu;
            const uint32_t imm = insn & 0xFFFFu;
            gpr[rt] = imm << 16;
            ++result.stepsExecuted;
            continue;
        }

        if (op == 0x09) { // ADDIU
            const uint32_t rs = (insn >> 21) & 0x1Fu;
            const uint32_t rt = (insn >> 16) & 0x1Fu;
            gpr[rt] = gpr[rs] + signExtendImm16(insn);
            ++result.stepsExecuted;
            continue;
        }

        if (op == 0x23) { // LW
            const uint32_t rs = (insn >> 21) & 0x1Fu;
            const uint32_t rt = (insn >> 16) & 0x1Fu;
            const uint32_t addr = gpr[rs] + signExtendImm16(insn);
            if (addr + 4 <= ramBytes)
                gpr[rt] = fetchInsn(ram, ramBytes, addr);
            ++result.stepsExecuted;
            continue;
        }

        if (op == 0x04 || op == 0x05 || op == 0x06 || op == 0x07 || op == 0x01 || op == 0x02) {
            // branches — stop basic block
            break;
        }

        if (psxLooksLikeGp0Opcode(insn))
            break;

        ++result.stepsExecuted;
    }

    return result;
}
