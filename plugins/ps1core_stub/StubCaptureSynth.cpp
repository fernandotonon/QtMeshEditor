#include "StubCaptureSynth.h"

#include "CaptureTypes.h"
#include "EmuHooks.h"
#include "PsxVramColor.h"

#include <QVector>
#include <QtGlobal>

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
    const uint32_t matrixId = hooks->onGteMatrix(matrix);

    auto emitPrim = [&](PrimKind kind, uint8_t count, int x0, int y0) {
        PrimRecord prim{};
        prim.kind = kind;
        prim.vertexCount = count;
        prim.matrixId = matrixId;
        prim.tpage = 0x100;
        prim.clut = 0x200;
        prim.verts[0].x = x0;
        prim.verts[0].y = y0;
        prim.verts[0].r = 200;
        prim.verts[0].g = 40;
        prim.verts[0].b = 40;
        prim.verts[0].u = 8;
        prim.verts[0].v = 8;
        if (count >= 2) {
            prim.verts[1].x = x0 + 32;
            prim.verts[1].y = y0;
        }
        if (count >= 3) {
            prim.verts[2].x = x0 + 16;
            prim.verts[2].y = y0 + 24;
        }
        if (count >= 4) {
            prim.verts[3].x = x0 + 32;
            prim.verts[3].y = y0 + 24;
        }
        hooks->onGpuPrim(prim);
    };

    emitPrim(PrimKind::MonoTri, 3, 16, 16);
    emitPrim(PrimKind::ShadedTri, 3, 64, 16);
    emitPrim(PrimKind::TexturedTri, 3, 112, 16);
    emitPrim(PrimKind::MonoQuad, 4, 16, 64);
    emitPrim(PrimKind::ShadedQuad, 4, 64, 64);
    emitPrim(PrimKind::TexturedQuad, 4, 112, 64);
    emitPrim(PrimKind::Sprite, 2, 160, 64);

    DrawModeRecord mode{};
    mode.drawModeBits = 0x1234;
    mode.tpage = 0x100;
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
