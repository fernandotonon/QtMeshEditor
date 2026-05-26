#include "GteInverse.h"

#include "GteCapture.h"

#include <cmath>
#include <gtest/gtest.h>

namespace {

constexpr int32_t kFixedUnit = 1 << 12;

int32_t toFixed(double v)
{
    return static_cast<int32_t>(std::lround(v * kFixedUnit));
}

MatrixRecord identityMatrix()
{
    MatrixRecord m{};
    m.rt.m[0][0] = kFixedUnit;
    m.rt.m[1][1] = kFixedUnit;
    m.rt.m[2][2] = kFixedUnit;
    m.h = 256;
    return m;
}

// Builds a 12.4 fixed-point rotation matrix from intrinsic Euler angles
// (X then Y then Z) without translation/projection metadata — tests fill those
// fields explicitly.
MatrixRecord rotationMatrixXYZ(double rxRad, double ryRad, double rzRad)
{
    const double cx = std::cos(rxRad);
    const double sx = std::sin(rxRad);
    const double cy = std::cos(ryRad);
    const double sy = std::sin(ryRad);
    const double cz = std::cos(rzRad);
    const double sz = std::sin(rzRad);

    // R = Rz * Ry * Rx (standard right-handed XYZ Euler, model -> camera basis).
    const double r00 = cz * cy;
    const double r01 = cz * sy * sx - sz * cx;
    const double r02 = cz * sy * cx + sz * sx;
    const double r10 = sz * cy;
    const double r11 = sz * sy * sx + cz * cx;
    const double r12 = sz * sy * cx - cz * sx;
    const double r20 = -sy;
    const double r21 = cy * sx;
    const double r22 = cy * cx;

    MatrixRecord m{};
    m.rt.m[0][0] = toFixed(r00);
    m.rt.m[0][1] = toFixed(r01);
    m.rt.m[0][2] = toFixed(r02);
    m.rt.m[1][0] = toFixed(r10);
    m.rt.m[1][1] = toFixed(r11);
    m.rt.m[1][2] = toFixed(r12);
    m.rt.m[2][0] = toFixed(r20);
    m.rt.m[2][1] = toFixed(r21);
    m.rt.m[2][2] = toFixed(r22);
    m.h = 300;
    m.ofx = 160 << 16;
    m.ofy = 120 << 16;
    return m;
}

} // namespace

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

// Pre-#675 the forward/inverse math only round-tripped on an identity-on-diagonal RT
// (the existing ModelToScreenRoundTripsThroughScreenToModel above), so any real rotation
// silently fell through to psxScreenToWorld and produced a flat-XY blob.  These tests
// pin the *real* GTE math down on three independent rotation cases.

TEST(GteInverseTest, ModelToScreenRoundTripsRotated90DegY)
{
    // 90° rotation around the Y axis: model (x, y, z) -> camera (z, y, -x).
    MatrixRecord matrix = rotationMatrixXYZ(0.0, M_PI / 2.0, 0.0);
    matrix.tr[0] = 0;
    matrix.tr[1] = 0;
    matrix.tr[2] = 6000; // pull the model away from the camera so IR[2] != 0.

    ASSERT_TRUE(GteCapture::looksOrthonormalRotation(matrix));

    constexpr int kModelX = 1000;
    constexpr int kModelY = -2000;
    constexpr int kModelZ = 500;

    int sx = 0, sy = 0, sz = 0;
    ASSERT_TRUE(GteInverse::modelToScreen(matrix, kModelX, kModelY, kModelZ, sx, sy, sz));

    float mx = 0.0f, my = 0.0f, mz = 0.0f;
    ASSERT_TRUE(GteInverse::screenToModel(matrix, sx, sy, sz, mx, my, mz));

    EXPECT_NEAR(mx, static_cast<float>(kModelX), 2.0f);
    EXPECT_NEAR(my, static_cast<float>(kModelY), 2.0f);
    EXPECT_NEAR(mz, static_cast<float>(kModelZ), 2.0f);
}

TEST(GteInverseTest, ModelToScreenRoundTripsArbitrary3DRotation)
{
    // 30° pitch + 45° yaw + 15° roll — the same kind of mixed rotation real games author.
    MatrixRecord matrix = rotationMatrixXYZ(M_PI / 6.0, M_PI / 4.0, M_PI / 12.0);
    matrix.tr[0] = 250;
    matrix.tr[1] = -100;
    matrix.tr[2] = 5000;

    ASSERT_TRUE(GteCapture::looksOrthonormalRotation(matrix));

    constexpr int kModelX = 800;
    constexpr int kModelY = 1200;
    constexpr int kModelZ = -400;

    int sx = 0, sy = 0, sz = 0;
    ASSERT_TRUE(GteInverse::modelToScreen(matrix, kModelX, kModelY, kModelZ, sx, sy, sz));

    float mx = 0.0f, my = 0.0f, mz = 0.0f;
    ASSERT_TRUE(GteInverse::screenToModel(matrix, sx, sy, sz, mx, my, mz));

    // Floating-point + screen-space integer rounding contributes ~3 units of drift.
    EXPECT_NEAR(mx, static_cast<float>(kModelX), 4.0f);
    EXPECT_NEAR(my, static_cast<float>(kModelY), 4.0f);
    EXPECT_NEAR(mz, static_cast<float>(kModelZ), 4.0f);
}

// PS1 GP0 polygon packets only carry 2D screen-space XY. The GTE writes Z into
// a separate SZ FIFO that the CPU drains *before* building the GP0 word, so
// PsxVertex::z stays 0 for any capture sourced from GP0 alone. Without depth,
// the pinhole inverse degenerates: IR[0]/IR[1] collapse to 0 and every vertex
// of a given matrix tag maps to the same model-space point (~RT^T·(-TR)/4096).
// vertexFromPsx must then fall back to psxScreenToWorld so the mesh is at
// least visible — not silently emit a zero-extent collapse. This is the
// regression that bit the first build of #675.
TEST(GteInverseTest, ScreenToModelRefusesZeroDepth)
{
    MatrixRecord matrix = rotationMatrixXYZ(0.0, M_PI / 4.0, 0.0);
    matrix.tr[0] = 1000;
    matrix.tr[1] = -500;
    matrix.tr[2] = 5000;

    float mx = 0.0f, my = 0.0f, mz = 0.0f;
    EXPECT_FALSE(GteInverse::screenToModel(matrix, 160, 120, 0, mx, my, mz));
    EXPECT_FALSE(GteInverse::screenToModel(matrix, 200, 80, 0, mx, my, mz));
    EXPECT_FALSE(GteInverse::screenToModel(matrix, -50, 50, 0, mx, my, mz));
}

// Companion to the test above: if multiple vertices share a matrix and sz=0
// were accepted, all of them would collapse onto a single model-space point
// (the mesh becomes invisible because every reconstructed sub-mesh has zero
// extent). With the guard in place those calls fail and the caller is forced
// to use psxScreenToWorld, which preserves the per-vertex screen XY spread.
TEST(GteInverseTest, ScreenToModelWithZeroDepthDoesNotCollapseVertices)
{
    MatrixRecord matrix = rotationMatrixXYZ(M_PI / 12.0, M_PI / 8.0, 0.0);
    matrix.tr[0] = 256;
    matrix.tr[1] = 128;
    matrix.tr[2] = 4096;

    float mx0 = 0.0f, my0 = 0.0f, mz0 = 0.0f;
    float mx1 = 0.0f, my1 = 0.0f, mz1 = 0.0f;
    EXPECT_FALSE(GteInverse::screenToModel(matrix, 100, 100, 0, mx0, my0, mz0));
    EXPECT_FALSE(GteInverse::screenToModel(matrix, 200, 150, 0, mx1, my1, mz1));
    // Failure path leaves outputs untouched — confirm the caller's fallback
    // is what produces vertex-to-vertex divergence, not the inverse itself.
    EXPECT_EQ(mx0, 0.0f);
    EXPECT_EQ(my0, 0.0f);
    EXPECT_EQ(mz0, 0.0f);
    EXPECT_EQ(mx1, 0.0f);
    EXPECT_EQ(my1, 0.0f);
    EXPECT_EQ(mz1, 0.0f);
}

TEST(GteInverseTest, ModelToScreenForwardChangesAllAxesForRotatedMatrix)
{
    // Pre-#675 the forward math was diagonal-only, so a Y rotation produced the same
    // screen X for a model X-shift — verifying the matrix multiply actually rotates.
    MatrixRecord matrix = rotationMatrixXYZ(0.0, M_PI / 2.0, 0.0);
    matrix.tr[2] = 4000;

    int sx0 = 0, sy0 = 0, sz0 = 0;
    int sx1 = 0, sy1 = 0, sz1 = 0;
    ASSERT_TRUE(GteInverse::modelToScreen(matrix, 0, 0, 0, sx0, sy0, sz0));
    ASSERT_TRUE(GteInverse::modelToScreen(matrix, 1000, 0, 0, sx1, sy1, sz1));

    // 90° Y rotation: model X-shift becomes camera-Z shift (depth), so the projected
    // depth must change and screen X must stay essentially fixed.
    EXPECT_NE(sz1, sz0);
    EXPECT_NEAR(sx1, sx0, 5);
}

TEST(GteCaptureTest, LooksOrthonormalRotationAcceptsIdentity)
{
    EXPECT_TRUE(GteCapture::looksOrthonormalRotation(identityMatrix()));
}

TEST(GteCaptureTest, LooksOrthonormalRotationAcceptsRealRotations)
{
    EXPECT_TRUE(GteCapture::looksOrthonormalRotation(rotationMatrixXYZ(0.0, M_PI / 2.0, 0.0)));
    EXPECT_TRUE(GteCapture::looksOrthonormalRotation(rotationMatrixXYZ(M_PI / 4.0, 0.0, 0.0)));
    EXPECT_TRUE(
        GteCapture::looksOrthonormalRotation(rotationMatrixXYZ(M_PI / 6.0, M_PI / 4.0, M_PI / 12.0)));
}

TEST(GteCaptureTest, LooksOrthonormalRotationRejectsScaledRotation)
{
    // Each entry doubled — rows are now |row|^2 == (2*4096)^2 = 4 * 4096^2, well outside tolerance.
    MatrixRecord matrix = rotationMatrixXYZ(0.0, M_PI / 4.0, 0.0);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            matrix.rt.m[r][c] *= 2;
    EXPECT_FALSE(GteCapture::looksOrthonormalRotation(matrix));
}

TEST(GteCaptureTest, LooksOrthonormalRotationRejectsReflection)
{
    // Flip the sign of the last row -> determinant becomes -4096^3 (reflection).
    MatrixRecord matrix = identityMatrix();
    matrix.rt.m[2][2] = -kFixedUnit;
    EXPECT_FALSE(GteCapture::looksOrthonormalRotation(matrix));
}

TEST(GteCaptureTest, LooksOrthonormalRotationRejectsRandomFill)
{
    MatrixRecord matrix{};
    // Arbitrary but stable bit pattern — rows are neither unit-length nor orthogonal.
    matrix.rt.m[0][0] = 1234;
    matrix.rt.m[0][1] = 5678;
    matrix.rt.m[0][2] = -42;
    matrix.rt.m[1][0] = 999;
    matrix.rt.m[1][1] = 100;
    matrix.rt.m[1][2] = 7777;
    matrix.rt.m[2][0] = -500;
    matrix.rt.m[2][1] = 2000;
    matrix.rt.m[2][2] = 3000;
    EXPECT_FALSE(GteCapture::looksOrthonormalRotation(matrix));
}
