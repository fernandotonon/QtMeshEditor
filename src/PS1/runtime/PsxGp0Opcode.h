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

/** libgpu getaddr(): byte address of the next DR tag from bits 2–23 (addr >> 2). */
inline uint32_t psxGp0TagNextByteAddr(uint32_t tag)
{
    return ((tag >> 2) & 0x3FFFFFu) << 2;
}

inline bool psxLooksLikeGp0Opcode(uint32_t word)
{
    const uint8_t cmd = psxGp0OpcodeByte(word);
    if (cmd >= 0x20 && cmd <= 0x3F)
        return true;
    if (cmd >= 0x60 && cmd <= 0x7F)
        return true;
    if (cmd >= 0xE1 && cmd <= 0xE6)
        return true;
    if (cmd == 0xA0 || cmd == 0xC0)
        return true;
    return false;
}

#endif // PSXGP0OPCODE_H
