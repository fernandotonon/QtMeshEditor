#ifndef CAPTURETYPES_H
#define CAPTURETYPES_H

#include <cstdint>

/** PS1 GPU primitive kinds decoded from GP0 (#418). */
enum class PrimKind : uint8_t {
    MonoTri = 0,
    ShadedTri,
    TexturedTri,
    MonoQuad,
    ShadedQuad,
    TexturedQuad,
    Sprite,
};

struct PsxVertex {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    int16_t u = 0;
    int16_t v = 0;
};

/** One draw call lifted from the GPU command stream. */
struct PrimRecord {
    PrimKind kind = PrimKind::MonoTri;
    uint8_t vertexCount = 0;
    PsxVertex verts[4]{};
    uint16_t tpage = 0;
    uint16_t clut = 0;
    uint8_t semiTrans = 0;
    uint32_t drawModeBits = 0;
    uint32_t matrixId = 0;
};

/** GP0 0xE1–0xE6 drawing environment snapshot. */
struct DrawModeRecord {
    uint32_t drawModeBits = 0;
    uint16_t tpage = 0;
    uint16_t clut = 0;
    uint8_t textureWindow = 0;
    uint8_t maskSetting = 0;
};

/** 3×3 fixed-point rotation + translation from the GTE (#419). */
struct Matrix3 {
    int32_t m[3][3]{};
};

struct MatrixRecord {
    Matrix3 rt{};
    int32_t tr[3]{};
    int32_t ofx = 0;
    int32_t ofy = 0;
    int32_t h = 0;
    uint64_t hash = 0;
};

#endif // CAPTURETYPES_H
