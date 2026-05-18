#include "GteInverse.h"

#include <gtest/gtest.h>

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
