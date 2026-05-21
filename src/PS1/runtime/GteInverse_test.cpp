#include "GteInverse.h"

#include <cmath>
#include <gtest/gtest.h>

static MatrixRecord identityMatrix()
{
    MatrixRecord m{};
    m.rt.m[0][0] = 1 << 12;
    m.rt.m[1][1] = 1 << 12;
    m.rt.m[2][2] = 1 << 12;
    m.h = 256;
    return m;
}

TEST(GteInverseTest, ScreenToModelWithTranslation)
{
    MatrixRecord matrix = identityMatrix();
    matrix.tr[0] = 4096;
    matrix.tr[1] = 0;
    matrix.tr[2] = 0;

    float mx = 0.0f;
    float my = 0.0f;
    float mz = 0.0f;
    ASSERT_TRUE(GteInverse::screenToModel(matrix, 160, 120, 4096, mx, my, mz));
    EXPECT_TRUE(std::isfinite(mx));
    EXPECT_TRUE(std::isfinite(my));
    EXPECT_TRUE(std::isfinite(mz));
    EXPECT_NE(mx, 0.0f);
}

TEST(GteInverseTest, ModelToScreenRoundTripsThroughScreenToModel)
{
    MatrixRecord matrix = identityMatrix();
    matrix.ofx = 160 << 16;
    matrix.ofy = 120 << 16;
    matrix.tr[0] = 2048;

    constexpr int kModelX = 1000;
    constexpr int kModelY = -2000;
    constexpr int kModelZ = 3000;
    int sx = 0;
    int sy = 0;
    int sz = 0;
    ASSERT_TRUE(GteInverse::modelToScreen(matrix, kModelX, kModelY, kModelZ, sx, sy, sz));

    float mx = 0.0f;
    float my = 0.0f;
    float mz = 0.0f;
    ASSERT_TRUE(GteInverse::screenToModel(matrix, sx, sy, sz, mx, my, mz));

    int sx2 = 0;
    int sy2 = 0;
    int sz2 = 0;
    ASSERT_TRUE(GteInverse::modelToScreen(matrix, static_cast<int>(mx), static_cast<int>(my),
                                          static_cast<int>(mz), sx2, sy2, sz2));
    EXPECT_NEAR(sx, sx2, 2);
    EXPECT_NEAR(sy, sy2, 2);
    EXPECT_NEAR(sz, sz2, 2);
}

TEST(GteInverseTest, ScreenToWorldFlipsY)
{
    float wx = 0.0f;
    float wy = 0.0f;
    float wz = 0.0f;
    GteInverse::psxScreenToWorld(160.0f, 120.0f, 0.0f, wx, wy, wz);
    EXPECT_NEAR(wx, 0.0f, 1e-4f);
    EXPECT_NEAR(wy, 0.0f, 1e-4f);
    GteInverse::psxScreenToWorld(160.0f, 140.0f, 0.0f, wx, wy, wz);
    EXPECT_LT(wy, 0.0f);
}
