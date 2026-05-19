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
