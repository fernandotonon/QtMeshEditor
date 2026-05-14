#include <gtest/gtest.h>
#include <cmath>
#include "Euler.h"

using namespace Ogre;

TEST(EulerTest, IdentityForwardRightUp)
{
    Euler e;
    EXPECT_TRUE(e.forward().positionEquals(Vector3::NEGATIVE_UNIT_Z, 1e-4f));
    EXPECT_TRUE(e.right().positionEquals(Vector3::UNIT_X, 1e-4f));
    EXPECT_TRUE(e.up().positionEquals(Vector3::UNIT_Y, 1e-4f));
}

TEST(EulerTest, ToQuaternionMatchesYawPitchRollOrder)
{
    Euler e(Radian(0.5f), Radian(0.25f), Radian(0.1f));
    Quaternion q = e.toQuaternion();
    Quaternion expected = Quaternion(e.yaw(), Vector3::UNIT_Y)
        * Quaternion(e.pitch(), Vector3::UNIT_X)
        * Quaternion(e.roll(), Vector3::UNIT_Z);
    EXPECT_NEAR(std::abs(q.Dot(expected)), 1.0f, 1e-4f);
}

TEST(EulerTest, QuaternionRoundTrip)
{
    Euler e(Degree(33).valueRadians(), Degree(-12).valueRadians(), Degree(77).valueRadians());
    Quaternion q = e.toQuaternion();
    Euler e2(q);
    EXPECT_NEAR(std::abs(q.Dot(e2.toQuaternion())), 1.0f, 1e-3f);
}

TEST(EulerTest, DirectionSetsYawPitch)
{
    Euler e;
    e.direction(Vector3(0, 0, -1));
    EXPECT_GT(e.forward().dotProduct(Vector3::NEGATIVE_UNIT_Z), 0.99f);
}

TEST(EulerTest, RelativeYawPitchRoll)
{
    Euler e;
    e.yaw(Radian(0.1f)).pitch(Radian(0.2f)).roll(Radian(0.3f));
    EXPECT_NEAR(e.yaw().valueRadians(), 0.1f, 1e-5f);
    EXPECT_NEAR(e.pitch().valueRadians(), 0.2f, 1e-5f);
    EXPECT_NEAR(e.roll().valueRadians(), 0.3f, 1e-5f);
}

TEST(EulerTest, NormaliseWrapsLargeYaw)
{
    Euler e;
    e.setYaw(Radian(5.0f));
    e.normalise(true, false, false);
    EXPECT_LT(std::abs(e.yaw().valueRadians()), Math::PI + 0.01f);
}

TEST(EulerTest, ConstructFromQuaternionDecomposesYPR)
{
    Quaternion q = Quaternion(Radian(0.3f), Vector3::UNIT_Y)
                 * Quaternion(Radian(0.2f), Vector3::UNIT_X)
                 * Quaternion(Radian(0.1f), Vector3::UNIT_Z);
    Euler e(q);
    EXPECT_NEAR(e.yaw().valueRadians(),   0.3f, 1e-4f);
    EXPECT_NEAR(e.pitch().valueRadians(), 0.2f, 1e-4f);
    EXPECT_NEAR(e.roll().valueRadians(),  0.1f, 1e-4f);
}

TEST(EulerTest, ConstructFromMatrixRoundTrip)
{
    Quaternion q(Radian(0.2f), Vector3::UNIT_Y);
    Matrix3 m;
    q.ToRotationMatrix(m);
    Euler e(m);
    EXPECT_NEAR(e.yaw().valueRadians(), 0.2f, 1e-4f);
    EXPECT_NEAR(e.pitch().valueRadians(), 0.0f, 1e-4f);
    EXPECT_NEAR(e.roll().valueRadians(), 0.0f, 1e-4f);
}

TEST(EulerTest, RealConstructorBuildsRadians)
{
    Euler e(1.0f, 2.0f, 3.0f);
    EXPECT_NEAR(e.yaw().valueRadians(),   1.0f, 1e-6f);
    EXPECT_NEAR(e.pitch().valueRadians(), 2.0f, 1e-6f);
    EXPECT_NEAR(e.roll().valueRadians(),  3.0f, 1e-6f);
}

TEST(EulerTest, SettersUpdateValues)
{
    Euler e;
    e.setYaw(Radian(1.0f));
    e.setPitch(Radian(2.0f));
    e.setRoll(Radian(3.0f));
    EXPECT_NEAR(e.yaw().valueRadians(),   1.0f, 1e-6f);
    EXPECT_NEAR(e.pitch().valueRadians(), 2.0f, 1e-6f);
    EXPECT_NEAR(e.roll().valueRadians(),  3.0f, 1e-6f);
}

TEST(EulerTest, OrientationSetsAllAtOnce)
{
    Euler e;
    e.orientation(Radian(0.1f), Radian(0.2f), Radian(0.3f));
    EXPECT_NEAR(e.yaw().valueRadians(),   0.1f, 1e-6f);
    EXPECT_NEAR(e.pitch().valueRadians(), 0.2f, 1e-6f);
    EXPECT_NEAR(e.roll().valueRadians(),  0.3f, 1e-6f);
}

TEST(EulerTest, RotateAddsRelativeYPR)
{
    Euler e(Radian(0.1f), Radian(0.2f), Radian(0.3f));
    e.rotate(Radian(0.5f), Radian(-0.1f), Radian(0.05f));
    EXPECT_NEAR(e.yaw().valueRadians(),   0.6f,   1e-5f);
    EXPECT_NEAR(e.pitch().valueRadians(), 0.1f,   1e-5f);
    EXPECT_NEAR(e.roll().valueRadians(),  0.35f,  1e-5f);
}

TEST(EulerTest, NormaliseDoesNothingIfAlreadyInRange)
{
    Euler e(Radian(0.5f), Radian(0.5f), Radian(0.5f));
    Euler before = e;
    e.normalise();
    EXPECT_NEAR(e.yaw().valueRadians(),   before.yaw().valueRadians(),   1e-6f);
    EXPECT_NEAR(e.pitch().valueRadians(), before.pitch().valueRadians(), 1e-6f);
    EXPECT_NEAR(e.roll().valueRadians(),  before.roll().valueRadians(),  1e-6f);
}

TEST(EulerTest, NormaliseWrapsNegativeAngles)
{
    Euler e;
    e.setYaw(Radian(-Math::PI * 2.0f - 0.5f));
    e.normalise(true, false, false);
    EXPECT_LE(e.yaw().valueRadians(), Math::PI);
    EXPECT_GE(e.yaw().valueRadians(), -Math::PI);
}

TEST(EulerTest, LimitYawClampsRange)
{
    Euler e(Radian(2.0f), Radian(0.0f), Radian(0.0f));
    e.limitYaw(Radian(1.0f));
    EXPECT_LE(std::abs(e.yaw().valueRadians()), 1.0f + 1e-5f);
}

TEST(EulerTest, LimitPitchClampsRange)
{
    Euler e(Radian(0.0f), Radian(2.5f), Radian(0.0f));
    e.limitPitch(Radian(1.0f));
    EXPECT_LE(std::abs(e.pitch().valueRadians()), 1.0f + 1e-5f);
}

TEST(EulerTest, LimitRollClampsRange)
{
    Euler e(Radian(0.0f), Radian(0.0f), Radian(-2.5f));
    e.limitRoll(Radian(1.0f));
    EXPECT_LE(std::abs(e.roll().valueRadians()), 1.0f + 1e-5f);
}

TEST(EulerTest, OperatorAddSubtract)
{
    Euler a(Radian(0.1f), Radian(0.2f), Radian(0.3f));
    Euler b(Radian(1.0f), Radian(2.0f), Radian(3.0f));
    Euler sum = a + b;
    EXPECT_NEAR(sum.yaw().valueRadians(),   1.1f, 1e-6f);
    EXPECT_NEAR(sum.pitch().valueRadians(), 2.2f, 1e-6f);
    EXPECT_NEAR(sum.roll().valueRadians(),  3.3f, 1e-6f);

    Euler diff = b - a;
    EXPECT_NEAR(diff.yaw().valueRadians(),   0.9f, 1e-6f);
    EXPECT_NEAR(diff.pitch().valueRadians(), 1.8f, 1e-6f);
    EXPECT_NEAR(diff.roll().valueRadians(),  2.7f, 1e-6f);
}

TEST(EulerTest, OperatorScalarMultiply)
{
    Euler e(Radian(0.1f), Radian(0.2f), Radian(0.3f));
    Euler scaled = e * 2.0f;
    EXPECT_NEAR(scaled.yaw().valueRadians(),   0.2f, 1e-6f);
    EXPECT_NEAR(scaled.pitch().valueRadians(), 0.4f, 1e-6f);
    EXPECT_NEAR(scaled.roll().valueRadians(),  0.6f, 1e-6f);

    Euler scaled2 = 2.0f * e;
    EXPECT_NEAR(scaled2.yaw().valueRadians(), 0.2f, 1e-6f);
}

TEST(EulerTest, OperatorMultiplyEulersProducesCombinedQuaternion)
{
    Euler a(Radian(0.5f), Radian(0.0f), Radian(0.0f));
    Euler b(Radian(0.3f), Radian(0.0f), Radian(0.0f));
    Quaternion combined = a * b;
    // Combined rotation around Y of 0.5 + 0.3 = 0.8 (approximately, for pure-Y).
    Euler result(combined);
    EXPECT_NEAR(result.yaw().valueRadians(), 0.8f, 1e-3f);
}

TEST(EulerTest, OperatorMultiplyVectorAppliesRotation)
{
    Euler e(Radian(Math::HALF_PI), Radian(0.0f), Radian(0.0f));
    Vector3 forward = e * Vector3(0, 0, -1);
    // 90° yaw rotates -Z to either -X or +X (Ogre convention).
    EXPECT_NEAR(std::abs(forward.x), 1.0f, 1e-4f);
    EXPECT_NEAR(forward.y, 0.0f, 1e-4f);
}

TEST(EulerTest, OperatorEqualityAndInequality)
{
    Euler a(Radian(0.1f), Radian(0.2f), Radian(0.3f));
    Euler b(Radian(0.1f), Radian(0.2f), Radian(0.3f));
    Euler c(Radian(0.4f), Radian(0.2f), Radian(0.3f));
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
    EXPECT_TRUE(a != c);
    EXPECT_FALSE(a != b);
}

TEST(EulerTest, AssignmentOperatorEuler)
{
    Euler a(Radian(0.1f), Radian(0.2f), Radian(0.3f));
    Euler b;
    b = a;
    EXPECT_TRUE(a == b);
}

TEST(EulerTest, AssignmentOperatorQuaternion)
{
    Quaternion q(Radian(0.4f), Vector3::UNIT_Y);
    Euler e;
    e = q;
    EXPECT_NEAR(e.yaw().valueRadians(), 0.4f, 1e-4f);
}

TEST(EulerTest, AssignmentOperatorMatrix3)
{
    Quaternion q(Radian(0.5f), Vector3::UNIT_Y);
    Matrix3 m;
    q.ToRotationMatrix(m);
    Euler e;
    e = m;
    EXPECT_NEAR(e.yaw().valueRadians(), 0.5f, 1e-4f);
}

TEST(EulerTest, SameOrientationTrueForEquivalentRotations)
{
    Euler a(Radian(0.5f), Radian(0.0f), Radian(0.0f));
    Euler b(Radian(0.5f + Math::TWO_PI), Radian(0.0f), Radian(0.0f));
    EXPECT_TRUE(sameOrientation(a, b));
}

TEST(EulerTest, ImplicitConversionToQuaternionWorks)
{
    Euler e(Radian(0.5f), Radian(0.0f), Radian(0.0f));
    Quaternion q = e;     // implicit operator Quaternion()
    EXPECT_NEAR(q.getYaw().valueRadians(), 0.5f, 1e-4f);
}

TEST(EulerTest, DirectionSetYawOnlyLeavesPitchUntouched)
{
    Euler e(Radian(0.0f), Radian(0.5f), Radian(0.0f));
    Radian origPitch = e.pitch();
    e.direction(Vector3(1, 0, -1), /*setYaw=*/true, /*setPitch=*/false);
    EXPECT_NEAR(e.pitch().valueRadians(), origPitch.valueRadians(), 1e-6f);
}

TEST(EulerTest, DirectionSetPitchOnlyLeavesYawUntouched)
{
    Euler e(Radian(0.5f), Radian(0.0f), Radian(0.0f));
    Radian origYaw = e.yaw();
    e.direction(Vector3(0, 1, -1), /*setYaw=*/false, /*setPitch=*/true);
    EXPECT_NEAR(e.yaw().valueRadians(), origYaw.valueRadians(), 1e-6f);
}

TEST(EulerTest, RotationToProducesRelativeDelta)
{
    Euler current;
    current.direction(Vector3(0, 0, -1)); // facing -Z
    Euler delta = current.rotationTo(Vector3(1, 0, 0));
    // Applying delta to current should orient us toward +X.
    Euler combined = current + delta;
    combined.normalise();
    Vector3 fwd = combined.forward();
    EXPECT_GT(fwd.x, 0.9f);  // mostly facing +X
}
