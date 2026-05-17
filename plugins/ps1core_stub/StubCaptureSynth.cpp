#include "StubCaptureSynth.h"

#include "CaptureTypes.h"
#include "EmuHooks.h"

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
