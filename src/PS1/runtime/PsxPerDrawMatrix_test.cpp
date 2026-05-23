#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/MeshTopologyHash.h"
#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/CaptureSnapshot.h"
#include "PS1/runtime/GteInverse.h"
#include "PS1/runtime/Gp0HookDispatch.h"
#include "PS1/runtime/MeshReconstructionStats.h"
#include "PS1/runtime/PsxCaptureFilters.h"
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

PrimRecord triAtScreenCoords(int x0, int y0, int z0, int x1, int y1, int z1, int x2, int y2, int z2,
                              uint32_t matrixId)
{
    PrimRecord prim{};
    prim.kind = PrimKind::MonoTri;
    prim.vertexCount = 3;
    prim.matrixId = matrixId;
    prim.verts[0] = {x0, y0, z0, 255, 0, 0, 0, 0};
    prim.verts[1] = {x1, y1, z1, 0, 255, 0, 0, 0};
    prim.verts[2] = {x2, y2, z2, 0, 0, 255, 0, 0};
    return prim;
}

void appendProjectedTri(CaptureSnapshot &snap, const MatrixRecord &matrix, uint32_t matrixId,
                        int mx0, int my0, int mz0, int mx1, int my1, int mz1, int mx2, int my2,
                        int mz2)
{
    int sx0 = 0;
    int sy0 = 0;
    int sz0 = 0;
    int sx1 = 0;
    int sy1 = 0;
    int sz1 = 0;
    int sx2 = 0;
    int sy2 = 0;
    int sz2 = 0;
    ASSERT_TRUE(GteInverse::modelToScreen(matrix, mx0, my0, mz0, sx0, sy0, sz0));
    ASSERT_TRUE(GteInverse::modelToScreen(matrix, mx1, my1, mz1, sx1, sy1, sz1));
    ASSERT_TRUE(GteInverse::modelToScreen(matrix, mx2, my2, mz2, sx2, sy2, sz2));
    PrimRecord prim = triAtScreenCoords(sx0, sy0, sz0, sx1, sy1, sz1, sx2, sy2, sz2, matrixId);
    ASSERT_TRUE(PsxCaptureFilters::isOnScreenPrim(prim));
    snap.prims.append(prim);
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
    CaptureSnapshot snap;
    snap.matrices.append(identityMatrix());

    PrimRecord prim{};
    prim.kind = PrimKind::MonoTri;
    prim.vertexCount = 3;
    prim.matrixId = 0;
    prim.verts[0] = {80, 80, 4096, 255, 0, 0, 0, 0};
    prim.verts[1] = {112, 80, 4096, 0, 255, 0, 0, 0};
    prim.verts[2] = {96, 104, 4096, 0, 0, 255, 0, 0};
    ASSERT_TRUE(PsxCaptureFilters::isOnScreenPrim(prim));
    snap.prims.append(prim);

    MeshReconstructionStats stats;
    const ReconstructedCaptureSet captureSet =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, &stats);
    EXPECT_FALSE(captureSet.isEmpty());
    EXPECT_EQ(stats.primsTotal, 1);
    EXPECT_EQ(stats.primsWithMatrixId, 1);
    EXPECT_GE(stats.gteInverseVertices, 3);
    EXPECT_EQ(stats.gteInversePercent(), 100);
    EXPECT_TRUE(stats.hasBounds());
    EXPECT_FALSE(stats.slabLike);
}

#endif // ENABLE_PS1_RIP
