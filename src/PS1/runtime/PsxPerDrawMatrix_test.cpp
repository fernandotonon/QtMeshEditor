#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/MeshTopologyHash.h"
#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/CaptureSnapshot.h"
#include "PS1/runtime/GteInverse.h"
#include "PS1/runtime/Gp0HookDispatch.h"
#include "PS1/runtime/MeshReconstructionStats.h"
#include "PS1/runtime/RipperHooks.h"

#include <atomic>
#include <cmath>

namespace {

uint32_t colorCmd(uint8_t opcode, uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint32_t>(opcode) | (static_cast<uint32_t>(r) << 8)
           | (static_cast<uint32_t>(g) << 16) | (static_cast<uint32_t>(b) << 24);
}

uint32_t pos(int x, int y)
{
    return static_cast<uint32_t>((y & 0xFFFF) << 16) | static_cast<uint32_t>(x & 0xFFFF);
}

MatrixRecord identityMatrix()
{
    MatrixRecord m{};
    m.rt.m[0][0] = 1 << 12;
    m.rt.m[1][1] = 1 << 12;
    m.rt.m[2][2] = 1 << 12;
    m.h = 256;
    m.ofx = 160 << 16;
    m.ofy = 120 << 16;
    return m;
}

PrimRecord triAtScreenCoords(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t matrixId)
{
    PrimRecord prim{};
    prim.kind = PrimKind::MonoTri;
    prim.vertexCount = 3;
    prim.matrixId = matrixId;
    prim.verts[0] = {x0, y0, 4096, 255, 0, 0, 0, 0};
    prim.verts[1] = {x1, y1, 4096, 0, 255, 0, 0, 0};
    prim.verts[2] = {x2, y2, 4096, 0, 0, 255, 0, 0};
    return prim;
}

} // namespace

TEST(PsxPerDrawMatrixTest, DrawEnvironmentFreezesSubmitMatrixId)
{
    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    MatrixRecord matrixA = identityMatrix();
    matrixA.tr[0] = 4096;
    MatrixRecord matrixB = identityMatrix();
    matrixB.tr[0] = 8192;

    const uint32_t idA = hooks.onGteMatrix(matrixA);
    const uint32_t drawEnv[] = {0xE1u | (0x100u << 16), 0u};
    hooks.submitGp0Words(drawEnv, 2);

    const uint32_t triWords[] = {
        colorCmd(0x20, 30, 20, 10),
        pos(8, 16),
        pos(40, 16),
        pos(24, 32),
    };
    hooks.submitGp0Words(triWords, 4);

    const uint32_t idB = hooks.onGteMatrix(matrixB);
    hooks.submitGp0Words(triWords, 4);

    hooks.submitGp0Words(drawEnv, 2);
    hooks.submitGp0Words(triWords, 4);

    ASSERT_EQ(buffer.prims().size(), 3);
    EXPECT_EQ(buffer.prims()[0].matrixId, idA);
    EXPECT_EQ(buffer.prims()[1].matrixId, idA);
    EXPECT_EQ(buffer.prims()[2].matrixId, idB);
}

TEST(PsxPerDrawMatrixTest, DrawingOffsetUpdatesSubmitMatrix)
{
    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    MatrixRecord matrix = identityMatrix();
    const uint32_t id0 = hooks.onGteMatrix(matrix);

    const uint32_t drawE4[] = {0xE4u | (0x100u << 16), pos(32, 64)};
    hooks.submitGp0Words(drawE4, 2);

    const uint32_t id1 = hooks.submitMatrixId();
    ASSERT_NE(id0, id1);
    ASSERT_LT(id1, static_cast<uint32_t>(buffer.matrices().size()));
    const MatrixRecord &updated = buffer.matrices()[static_cast<int>(id1)];
    EXPECT_EQ(updated.ofx, 32 << 16);
    EXPECT_EQ(updated.ofy, 64 << 16);
}

TEST(PsxPerDrawMatrixTest, ReconstructionReportsGteInverseStats)
{
    MatrixRecord matrixA = identityMatrix();
    matrixA.tr[0] = 2048;
    MatrixRecord matrixB = identityMatrix();
    matrixB.tr[0] = 4096;

    CaptureSnapshot snap;
    snap.matrices.append(matrixA);
    snap.matrices.append(matrixB);

    int sx = 0;
    int sy = 0;
    int sz = 0;
    ASSERT_TRUE(GteInverse::modelToScreen(matrixA, 1000, -500, 3000, sx, sy, sz));
    snap.prims.append(triAtScreenCoords(sx, sy, sx + 16, sy, sx + 8, sy + 12, 0));

    ASSERT_TRUE(GteInverse::modelToScreen(matrixB, 2000, -800, 3500, sx, sy, sz));
    snap.prims.append(triAtScreenCoords(sx, sy, sx + 16, sy, sx + 8, sy + 12, 1));

    MeshReconstructionStats stats;
    const ReconstructedCaptureSet captureSet =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, &stats);
    EXPECT_FALSE(captureSet.isEmpty());
    EXPECT_EQ(stats.primsTotal, 2);
    EXPECT_EQ(stats.primsWithMatrixId, 2);
    EXPECT_GE(stats.gteInverseVertices, 3);
    EXPECT_GE(stats.gteInversePercent(), 50);
    EXPECT_TRUE(stats.hasBounds());
    EXPECT_FALSE(stats.slabLike);
}

#endif // ENABLE_PS1_RIP
