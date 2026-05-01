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
    EXPECT_NEAR(std::abs(q.dotProduct(expected)), 1.0f, 1e-4f);
}

TEST(EulerTest, QuaternionRoundTrip)
{
    Euler e(Degree(33).valueRadians(), Degree(-12).valueRadians(), Degree(77).valueRadians());
    Quaternion q = e.toQuaternion();
    Euler e2(q);
    EXPECT_NEAR(std::abs(q.dotProduct(e2.toQuaternion())), 1.0f, 1e-3f);
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
