#ifdef ENABLE_MOCAP

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

#include "Mocap/FaceCapCanonicalData.h"
#include "Mocap/FaceCapPose.h"

namespace {

using Quat = std::array<float, 4>;  // (x,y,z,w)

Quat axisAngle(float ax, float ay, float az, float rad)
{
    const float n = std::sqrt(ax * ax + ay * ay + az * az);
    const float s = std::sin(rad / 2) / n;
    return {ax * s, ay * s, az * s, std::cos(rad / 2)};
}

void rotate(const Quat& q, const float in[3], float out[3])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2 * (y * in[2] - z * in[1]);
    const float ty = 2 * (z * in[0] - x * in[2]);
    const float tz = 2 * (x * in[1] - y * in[0]);
    out[0] = in[0] + w * tx + (y * tz - z * ty);
    out[1] = in[1] + w * ty + (z * tx - x * tz);
    out[2] = in[2] + w * tz + (x * ty - y * tx);
}

double quatAngle(const Quat& a, const Quat& b)
{
    double dot = 0;
    for (int i = 0; i < 4; ++i)
        dot += a[i] * b[i];
    return 2.0 * std::acos(std::fmin(1.0, std::fabs(dot)));
}

// asymmetric, non-coplanar point cloud
std::vector<float> makeCloud()
{
    return {0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 1, 1, 0.5f, -2, 0.5f, 1};
}

}  // namespace

TEST(FaceCapPose, RecoversSyntheticRigidTransform)
{
    const std::vector<float> src = makeCloud();
    const int n = static_cast<int>(src.size() / 3);
    const Quat q = axisAngle(0.3f, 1.f, 0.2f, 0.7f);
    const float t[3] = {5.f, -3.f, 2.f};

    std::vector<float> dst(src.size());
    for (int i = 0; i < n; ++i) {
        float r[3];
        rotate(q, &src[i * 3], r);
        for (int k = 0; k < 3; ++k)
            dst[i * 3 + k] = r[k] + t[k];
    }
    const auto res = FaceCapPose::solve(src.data(), dst.data(), nullptr, n);
    ASSERT_TRUE(res.ok);
    EXPECT_LT(quatAngle(res.rotation, q), 1e-3);
    EXPECT_NEAR(res.scale, 1.f, 1e-4);
    for (int k = 0; k < 3; ++k)
        EXPECT_NEAR(res.translation[k], t[k], 1e-3);
}

TEST(FaceCapPose, RecoversScaleAndDiscardsItFromRotation)
{
    const std::vector<float> src = makeCloud();
    const int n = static_cast<int>(src.size() / 3);
    const Quat q = axisAngle(0.f, 1.f, 0.f, -0.4f);

    std::vector<float> dst(src.size());
    for (int i = 0; i < n; ++i) {
        float r[3];
        rotate(q, &src[i * 3], r);
        for (int k = 0; k < 3; ++k)
            dst[i * 3 + k] = 2.5f * r[k] + 1.f;
    }
    const auto res = FaceCapPose::solve(src.data(), dst.data(), nullptr, n);
    ASSERT_TRUE(res.ok);
    EXPECT_LT(quatAngle(res.rotation, q), 1e-3);
    EXPECT_NEAR(res.scale, 2.5f, 1e-3);
}

TEST(FaceCapPose, WeightsFocusTheFit)
{
    const std::vector<float> src = makeCloud();
    const int n = static_cast<int>(src.size() / 3);
    const Quat q = axisAngle(0.f, 0.f, 1.f, 0.5f);

    std::vector<float> dst(src.size());
    for (int i = 0; i < n; ++i) {
        float r[3];
        rotate(q, &src[i * 3], r);
        for (int k = 0; k < 3; ++k)
            dst[i * 3 + k] = r[k];
    }
    // corrupt the last point badly, but give it zero weight
    dst[(n - 1) * 3 + 0] += 100.f;
    std::vector<float> w(n, 1.f);
    w[n - 1] = 0.f;
    const auto res = FaceCapPose::solve(src.data(), dst.data(), w.data(), n);
    ASSERT_TRUE(res.ok);
    EXPECT_LT(quatAngle(res.rotation, q), 1e-3);
}

TEST(FaceCapPose, DegenerateInputsReturnNotOk)
{
    const std::vector<float> src = makeCloud();
    EXPECT_FALSE(FaceCapPose::solve(src.data(), src.data(), nullptr, 2).ok);
    // all-zero weights
    std::vector<float> w(src.size() / 3, 0.f);
    EXPECT_FALSE(
        FaceCapPose::solve(src.data(), src.data(), w.data(),
                           static_cast<int>(src.size() / 3)).ok);
    // coincident points (zero variance)
    std::vector<float> same(src.size(), 1.f);
    EXPECT_FALSE(FaceCapPose::solve(same.data(), same.data(), nullptr,
                                    static_cast<int>(same.size() / 3)).ok);
}

TEST(FaceCapPose, HeadPoseIdentityForCanonicalLandmarks)
{
    // feed the canonical model itself as "screen landmarks" with the
    // documented (x, -y, -z) flip already inverted: identity comes back
    std::vector<float> landmarks(FaceCap::kCanonicalVertexCount * 3);
    for (int i = 0; i < FaceCap::kCanonicalVertexCount; ++i) {
        landmarks[i * 3 + 0] = FaceCap::kCanonicalFaceModel[i * 3 + 0];
        landmarks[i * 3 + 1] = -FaceCap::kCanonicalFaceModel[i * 3 + 1];
        landmarks[i * 3 + 2] = -FaceCap::kCanonicalFaceModel[i * 3 + 2];
    }
    const auto res = FaceCapPose::solveHeadPose(
        landmarks.data(), FaceCap::kCanonicalVertexCount);
    ASSERT_TRUE(res.ok);
    const Quat identity{0.f, 0.f, 0.f, 1.f};
    EXPECT_LT(quatAngle(res.rotation, identity), 1e-3);
    EXPECT_NEAR(res.scale, 1.f, 1e-4);
}

TEST(FaceCapPose, HeadPoseRecoversYaw)
{
    // rotate the canonical model 30 degrees about +Y (turn left), project to
    // the image frame (y,z flip), expect the same rotation back
    const Quat q = axisAngle(0.f, 1.f, 0.f, static_cast<float>(M_PI) / 6.f);
    std::vector<float> landmarks(FaceCap::kCanonicalVertexCount * 3);
    for (int i = 0; i < FaceCap::kCanonicalVertexCount; ++i) {
        float r[3];
        rotate(q, &FaceCap::kCanonicalFaceModel[i * 3], r);
        landmarks[i * 3 + 0] = r[0];
        landmarks[i * 3 + 1] = -r[1];
        landmarks[i * 3 + 2] = -r[2];
    }
    const auto res = FaceCapPose::solveHeadPose(
        landmarks.data(), FaceCap::kCanonicalVertexCount);
    ASSERT_TRUE(res.ok);
    EXPECT_LT(quatAngle(res.rotation, q), 1e-3);
}

TEST(FaceCapPose, HeadPoseRecoversPitch)
{
    // rotate the canonical model 20 degrees about +X (nod down), project to
    // the image frame (y,z flip), expect the same rotation back
    const Quat q = axisAngle(1.f, 0.f, 0.f, static_cast<float>(M_PI) / 9.f);
    std::vector<float> landmarks(FaceCap::kCanonicalVertexCount * 3);
    for (int i = 0; i < FaceCap::kCanonicalVertexCount; ++i) {
        float r[3];
        rotate(q, &FaceCap::kCanonicalFaceModel[i * 3], r);
        landmarks[i * 3 + 0] = r[0];
        landmarks[i * 3 + 1] = -r[1];
        landmarks[i * 3 + 2] = -r[2];
    }
    const auto res = FaceCapPose::solveHeadPose(
        landmarks.data(), FaceCap::kCanonicalVertexCount);
    ASSERT_TRUE(res.ok);
    EXPECT_LT(quatAngle(res.rotation, q), 1e-3);
}

#endif  // ENABLE_MOCAP
