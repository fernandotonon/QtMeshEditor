#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/GteCapture.h"

TEST(CaptureBufferTest, MatrixDedupeByHash)
{
    CaptureBuffer buffer;
    buffer.beginFrame();

    MatrixRecord a{};
    a.rt.m[0][0] = 4096;
    a.tr[0] = 10;
    a.h = 256;

    MatrixRecord b = a;
    b.tr[0] = 99;

    const uint32_t id0 = buffer.addMatrix(a);
    const uint32_t id1 = buffer.addMatrix(a);
    const uint32_t id2 = buffer.addMatrix(b);

    EXPECT_EQ(id0, id1);
    EXPECT_NE(id0, id2);
    EXPECT_EQ(buffer.matrices().size(), 2);

    PrimRecord prim{};
    prim.matrixId = id0;
    buffer.addPrim(prim);
    prim.matrixId = id0;
    buffer.addPrim(prim);
    prim.matrixId = id2;
    buffer.addPrim(prim);

    buffer.endFrame();
    EXPECT_TRUE(buffer.hasCameraMatrix());
    EXPECT_EQ(buffer.cameraMatrixId(), id0);
}

TEST(GteCaptureTest, HashStableForIdenticalMatrices)
{
    MatrixRecord m{};
    m.rt.m[1][1] = 2048;
    m.ofx = 160;
    m.ofy = 120;
    m.h = 512;
    EXPECT_EQ(GteCapture::hashMatrix(m), GteCapture::hashMatrix(m));
    EXPECT_TRUE(GteCapture::matricesEqual(m, m));
}

#endif // ENABLE_PS1_RIP
