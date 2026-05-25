#include "StubCaptureSynth.h"

#include "CaptureTypes.h"
#include "EmuHooks.h"
#include "PsxVramColor.h"

#include <QVector>
#include <QtGlobal>

namespace {

// PS1 GP0 packet builders (psx-spx / nocash) — opcode goes in the **low** byte
// per QtMeshEditor's `psxGp0OpcodeByte` convention. Each builder appends raw
// 32-bit words to the FIFO buffer so the stub feeds the same code path the
// real libretro plugin uses (#662 — issue acceptance item 2).

uint32_t cmdWord(uint8_t opcode, uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint32_t>(opcode)
           | (static_cast<uint32_t>(r) << 8)
           | (static_cast<uint32_t>(g) << 16)
           | (static_cast<uint32_t>(b) << 24);
}

uint32_t posWord(int16_t x, int16_t y)
{
    return (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16)
           | static_cast<uint32_t>(static_cast<uint16_t>(x));
}

uint32_t uvWord(uint8_t u, uint8_t v, uint16_t clutOrTpage)
{
    return (static_cast<uint32_t>(clutOrTpage) << 16)
           | (static_cast<uint32_t>(v) << 8)
           | static_cast<uint32_t>(u);
}

uint32_t colorWord(uint8_t r, uint8_t g, uint8_t b)
{
    return (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8)
           | static_cast<uint32_t>(r);
}

void emitMonoTri(QVector<uint32_t> &fifo, int16_t x0, int16_t y0)
{
    fifo.append(cmdWord(0x20, 200, 40, 40));
    fifo.append(posWord(x0, y0));
    fifo.append(posWord(x0 + 32, y0));
    fifo.append(posWord(x0 + 16, y0 + 24));
}

void emitShadedTri(QVector<uint32_t> &fifo, int16_t x0, int16_t y0)
{
    fifo.append(cmdWord(0x30, 200, 40, 40));
    fifo.append(posWord(x0, y0));
    fifo.append(colorWord(40, 200, 40));
    fifo.append(posWord(x0 + 32, y0));
    fifo.append(colorWord(40, 40, 200));
    fifo.append(posWord(x0 + 16, y0 + 24));
}

void emitTexturedTri(QVector<uint32_t> &fifo, int16_t x0, int16_t y0, uint16_t clut,
                     uint16_t tpage)
{
    fifo.append(cmdWord(0x24, 200, 40, 40));
    fifo.append(posWord(x0, y0));
    fifo.append(uvWord(8, 8, clut));
    fifo.append(posWord(x0 + 32, y0));
    fifo.append(uvWord(40, 8, tpage));
    fifo.append(posWord(x0 + 16, y0 + 24));
    fifo.append(uvWord(24, 32, tpage));
}

void emitMonoQuad(QVector<uint32_t> &fifo, int16_t x0, int16_t y0)
{
    fifo.append(cmdWord(0x28, 200, 40, 40));
    fifo.append(posWord(x0, y0));
    fifo.append(posWord(x0 + 32, y0));
    fifo.append(posWord(x0 + 16, y0 + 24));
    fifo.append(posWord(x0 + 32, y0 + 24));
}

void emitShadedQuad(QVector<uint32_t> &fifo, int16_t x0, int16_t y0)
{
    fifo.append(cmdWord(0x38, 200, 40, 40));
    fifo.append(posWord(x0, y0));
    fifo.append(colorWord(40, 200, 40));
    fifo.append(posWord(x0 + 32, y0));
    fifo.append(colorWord(40, 40, 200));
    fifo.append(posWord(x0 + 16, y0 + 24));
    fifo.append(colorWord(200, 200, 40));
    fifo.append(posWord(x0 + 32, y0 + 24));
}

void emitTexturedQuad(QVector<uint32_t> &fifo, int16_t x0, int16_t y0, uint16_t clut,
                      uint16_t tpage)
{
    fifo.append(cmdWord(0x2C, 200, 40, 40));
    fifo.append(posWord(x0, y0));
    fifo.append(uvWord(8, 8, clut));
    fifo.append(posWord(x0 + 32, y0));
    fifo.append(uvWord(40, 8, tpage));
    fifo.append(posWord(x0 + 16, y0 + 24));
    fifo.append(uvWord(24, 32, tpage));
    fifo.append(posWord(x0 + 32, y0 + 24));
    fifo.append(uvWord(40, 32, tpage));
}

void emitSprite(QVector<uint32_t> &fifo, int16_t x0, int16_t y0, uint16_t clut)
{
    fifo.append(cmdWord(0x64, 200, 40, 40));
    fifo.append(posWord(x0, y0));
    fifo.append(posWord(x0 + 32, y0));
    fifo.append(uvWord(8, 8, clut));
}

} // namespace

void stubEmitCaptureSample(EmuHooks *hooks)
{
    if (!hooks || !hooks->isCaptureEnabled())
        return;

    hooks->onFrameBegin();

    MatrixRecord matrix{};
    matrix.rt.m[0][0] = 1 << 12;
    matrix.rt.m[1][1] = 1 << 12;
    matrix.rt.m[2][2] = 1 << 12;
    matrix.h = 256;
    hooks->onGteMatrix(matrix);

    // Build the seven-flavor capture as one contiguous GP0 FIFO stream and
    // submit it via submitGp0Words so the stub exercises the same direct-hook
    // path as a real in-core mednafen GP0 dispatch would (#662).
    QVector<uint32_t> fifo;
    fifo.reserve(96);

    // 0xE4 drawing offset (0,0) — 2-word packet (opcode + xy) per the parser's
    // convention. Stamps submit-matrix OFX/OFY (#658).
    fifo.append(static_cast<uint32_t>(0xE4));
    fifo.append(posWord(0, 0));

    constexpr uint16_t kClut = 0x200;
    constexpr uint16_t kTpage = 0x100;

    emitMonoTri(fifo, 16, 16);
    emitShadedTri(fifo, 64, 16);
    emitTexturedTri(fifo, 112, 16, kClut, kTpage);
    emitMonoQuad(fifo, 16, 64);
    emitShadedQuad(fifo, 64, 64);
    emitTexturedQuad(fifo, 112, 64, kClut, kTpage);
    emitSprite(fifo, 160, 64, kClut);

    hooks->submitGp0Words(fifo.constData(), static_cast<size_t>(fifo.size()));

    // Mirror the legacy DrawMode emission so tests/observers that watch
    // `onDrawMode` still see a final mode update.
    DrawModeRecord mode{};
    mode.drawModeBits = 0x1234;
    mode.tpage = kTpage;
    hooks->onDrawMode(mode);

    hooks->onFrameEnd();
}

void stubFillVramPattern(EmuHooks *hooks, std::uint64_t frameIndex, bool forceMirror)
{
    if (!hooks || (!forceMirror && !hooks->isCaptureEnabled()))
        return;

    QVector<uint16_t> clutRow(16);
    for (int i = 0; i < 16; ++i) {
        const uint8_t v = static_cast<uint8_t>((i * 16 + static_cast<int>(frameIndex % 16)) & 0xFF);
        clutRow[i] = PsxVramColor::rgbaToBgr555(v, static_cast<uint8_t>(255 - v), 128, 255);
    }
    hooks->onVramWrite(0, 480, 16, 1, clutRow.constData());

    QVector<uint16_t> block4(64 * 32);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 64; ++x) {
            uint16_t word = 0;
            for (int nibble = 0; nibble < 4; ++nibble) {
                const int idx = (x * 4 + nibble + y + static_cast<int>(frameIndex)) % 16;
                word |= static_cast<uint16_t>(idx << (nibble * 4));
            }
            block4[y * 64 + x] = word;
        }
    }
    hooks->onVramWrite(0, 0, 64, 32, block4.constData());

    QVector<uint16_t> block8(128 * 32);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 128; ++x) {
            const uint8_t lo = static_cast<uint8_t>((x + y + static_cast<int>(frameIndex)) & 0xFF);
            const uint8_t hi = static_cast<uint8_t>((x * 2 + y + static_cast<int>(frameIndex)) & 0xFF);
            block8[y * 128 + x] = static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
        }
    }
    hooks->onVramWrite(64, 0, 128, 32, block8.constData());

    QVector<uint16_t> block15(64 * 32);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 64; ++x) {
            const uint8_t r = static_cast<uint8_t>((x * 4 + y) & 0xFF);
            const uint8_t g = static_cast<uint8_t>((32 + y + static_cast<int>(frameIndex)) & 0xFF);
            const uint8_t b = static_cast<uint8_t>((x * 2 + y) & 0xFF);
            block15[y * 64 + x] = PsxVramColor::rgbaToBgr555(r, g, b, (x & 1) ? 128 : 255);
        }
    }
    hooks->onVramWrite(0, 256, 64, 32, block15.constData());
}
