#ifndef PSXGP0OPCODE_H
#define PSXGP0OPCODE_H

#include <cstdint>

/**
 * Decodes the GP0 command byte from a RAM packet header.
 * Linked OT/DMA tags (bit0 set) carry the opcode in bits 24-31; drawing-environment
 * commands keep the opcode in the low byte (0xE1 already has bit0 set).
 */
inline uint8_t psxGp0OpcodeByte(uint32_t word)
{
    const uint8_t low = static_cast<uint8_t>(word & 0xFF);
    if (low >= 0xE1 && low <= 0xE6)
        return low;

    const uint8_t tagOpcode = static_cast<uint8_t>((word >> 24) & 0xFF);
    if ((word & 1u) != 0u && tagOpcode >= 0x20 && tagOpcode <= 0x7F)
        return tagOpcode;

    if ((word & 1u) != 0u) {
        const uint8_t high = static_cast<uint8_t>((word >> 8) & 0xFF);
        if (high >= 0x20 && high <= 0x7F)
            return high;
    }
    return low;
}

#endif // PSXGP0OPCODE_H
