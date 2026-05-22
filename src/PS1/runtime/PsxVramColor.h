#ifndef PSXVRAMCOLOR_H
#define PSXVRAMCOLOR_H

#include <cstdint>

/** PS1 VRAM 16-bit BGR555 (+ STP in bit 15) helpers shared by rip + TIM paths. */
namespace PsxVramColor {

/** GP0 draw-mode bit 11: when clear, texel 0x0000 is transparent; when set, opaque black. */
inline bool drawModeMasksZeroAsTransparent(uint32_t drawModeBits)
{
    return (drawModeBits & (1u << 11)) == 0;
}

inline void bgr555ToRgba(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b, uint8_t &a,
                         bool treatZeroAsTransparent = true)
{
    const uint8_t rr = static_cast<uint8_t>(c & 0x1F);
    const uint8_t gg = static_cast<uint8_t>((c >> 5) & 0x1F);
    const uint8_t bb = static_cast<uint8_t>((c >> 10) & 0x1F);
    r = static_cast<uint8_t>((rr * 255 + 15) / 31);
    g = static_cast<uint8_t>((gg * 255 + 15) / 31);
    b = static_cast<uint8_t>((bb * 255 + 15) / 31);
    const bool stp = (c & 0x8000) != 0;
    if (treatZeroAsTransparent && c == 0)
        a = 0;
    else if (stp)
        a = 128;
    else
        a = 255;
}

inline uint16_t rgbaToBgr555(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    const uint16_t r5 = static_cast<uint16_t>(r >> 3);
    const uint16_t g5 = static_cast<uint16_t>(g >> 3);
    const uint16_t b5 = static_cast<uint16_t>(b >> 3);
    const uint16_t stp = (a < 128) ? 1u : 0u;
    return static_cast<uint16_t>((stp << 15) | (b5 << 10) | (g5 << 5) | r5);
}

} // namespace PsxVramColor

#endif // PSXVRAMCOLOR_H
