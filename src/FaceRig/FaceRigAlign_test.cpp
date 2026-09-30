#include <gtest/gtest.h>

#include "FaceRig/FaceRigAlign.h"

#include <array>
#include <cmath>
#include <vector>

namespace {

using V3 = std::array<float, 3>;

V3 rotY(const V3& p, float deg)
{
    const float a = deg * 3.14159265358979f / 180.0f;
    const float c = std::cos(a), s = std::sin(a);
    return { c*p[0] + s*p[2], p[1], -s*p[0] + c*p[2] };
}
V3 rotX(const V3& p, float deg)
{
    const float a = deg * 3.14159265358979f / 180.0f;
    const float c = std::cos(a), s = std::sin(a);
    return { p[0], c*p[1] - s*p[2], s*p[1] + c*p[2] };
}

// A non-planar "face" constellation: eyes, nose tip, chin, forehead, ears.
std::vector<V3> faceConstellation()
{
    return { {-0.3f, 0.3f, 0.8f}, {0.3f, 0.3f, 0.8f},   // eyes
             {0.0f, 0.0f, 1.0f},                          // nose tip
             {0.0f, -0.6f, 0.7f},                          // chin
             {0.0f, 0.7f, 0.6f},                           // forehead
             {-0.9f, 0.0f, 0.0f}, {0.9f, 0.0f, 0.0f},      // ears
             {0.0f, -0.3f, 0.85f} };                       // mouth
}

}  // namespace

TEST(FaceRigAlign, RecoversYaw180)
{
    const auto src = faceConstellation();
    std::vector<V3> dst;
    for (const auto& p : src) dst.push_back(rotY(p, 180.0f));
    const auto a = FaceRig::alignPoints(src, dst);
    ASSERT_TRUE(a.ok);
    EXPECT_NEAR(a.angleDeg, 180.0f, 0.5f);
    EXPECT_NEAR(a.scale, 1.0f, 1e-3f);
    EXPECT_LT(a.residual, 1e-3f);
    // R maps the template's +Z face onto -Z.
    const V3 f = FaceRig::rotateVec(a.R, {0, 0, 1}, false);
    EXPECT_NEAR(f[2], -1.0f, 1e-3f);
    EXPECT_NEAR(f[0], 0.0f, 1e-3f);
}

TEST(FaceRigAlign, RecoversYawPlusPitchAndScale)
{
    const auto src = faceConstellation();
    std::vector<V3> dst;
    for (const auto& p : src) {
        V3 q = rotX(rotY(p, 90.0f), 20.0f);
        for (auto& c : q) c = c * 2.5f + 10.0f;      // scale + translate
        dst.push_back(q);
    }
    const auto a = FaceRig::alignPoints(src, dst);
    ASSERT_TRUE(a.ok);
    EXPECT_NEAR(a.scale, 2.5f, 1e-2f);
    EXPECT_LT(a.residual, 1e-3f);
    // Round trip every point through the recovered similarity.
    for (size_t i = 0; i < src.size(); ++i) {
        const V3 f = FaceRig::rotateVec(a.R, src[i], false);
        for (int k = 0; k < 3; ++k)
            EXPECT_NEAR(a.scale * f[size_t(k)] + a.translation[size_t(k)], dst[i][size_t(k)], 2e-3f);
    }
    // R^T undoes R.
    const V3 back = FaceRig::rotateVec(a.R, FaceRig::rotateVec(a.R, {0.2f, -0.4f, 0.9f}, false), true);
    EXPECT_NEAR(back[0], 0.2f, 1e-4f); EXPECT_NEAR(back[1], -0.4f, 1e-4f); EXPECT_NEAR(back[2], 0.9f, 1e-4f);
}

// The whole point: a correctly paired constellation must score low under ANY
// rotation, while a MIRRORED pairing (left/right swapped) must still score
// high — no proper rotation explains a reflection.
TEST(FaceRigAlign, ResidualIsRotationInvariantButRejectsMirroring)
{
    const auto src = faceConstellation();
    std::vector<V3> turned, mirrored;
    for (const auto& p : src) {
        turned.push_back(rotY(p, 180.0f));
        mirrored.push_back({ -p[0], p[1], p[2] });
    }
    const double rTurned   = FaceRig::rotationAwareResidual(src, turned);
    const double rMirrored = FaceRig::rotationAwareResidual(src, mirrored);
    EXPECT_LT(rTurned, 1e-3);
    EXPECT_GT(rMirrored, 0.1) << "a reflection must not look like a good match";
    EXPECT_GT(rMirrored, rTurned * 100.0);
}

TEST(FaceRigAlign, DegenerateInputsAreRejected)
{
    std::vector<V3> two = { {0,0,0}, {1,0,0} };
    EXPECT_FALSE(FaceRig::alignPoints(two, two).ok);
    EXPECT_GT(FaceRig::rotationAwareResidual(two, two), 1e8);
    std::vector<V3> line = { {0,0,0}, {1,0,0}, {2,0,0}, {3,0,0} };
    const auto a = FaceRig::alignPoints(line, line);
    // Collinear: either rejected, or (if the solver copes) an identity-ish fit.
    if (a.ok) EXPECT_LT(a.residual, 1e-2f);
}

TEST(FaceRigAlign, RotateInPlaceAboutPivotAndRotationToPlusZ)
{
    std::vector<float> xyz = { 1, 0, 0,   1, 1, 0 };
    const std::array<float, 9> R180{ -1, 0, 0,  0, 1, 0,  0, 0, -1 };
    FaceRig::rotateInPlace(xyz, R180, {1, 0, 0}, false);   // pivot = first point
    EXPECT_NEAR(xyz[0], 1.0f, 1e-6f); EXPECT_NEAR(xyz[2], 0.0f, 1e-6f);
    EXPECT_NEAR(xyz[3], 1.0f, 1e-6f); EXPECT_NEAR(xyz[4], 1.0f, 1e-6f);

    float deg = 0.0f;
    const auto Y = FaceRig::rotationToPlusZ({0, 0, -1}, 3.0f, &deg);
    EXPECT_NEAR(deg, 180.0f, 1e-3f);
    const V3 f = FaceRig::rotateVec(Y, {0, 0, -1}, false);
    EXPECT_NEAR(f[2], 1.0f, 1e-5f);
    const auto Yx = FaceRig::rotationToPlusZ({1, 0, 0}, 3.0f, &deg);
    EXPECT_NEAR(deg, 90.0f, 1e-3f);
    const V3 g = FaceRig::rotateVec(Yx, {1, 0, 0}, false);
    EXPECT_NEAR(g[2], 1.0f, 1e-5f); EXPECT_NEAR(g[0], 0.0f, 1e-5f);
    // Already facing +Z (or within the dead band): identity.
    const auto I = FaceRig::rotationToPlusZ({0.01f, 0, 1}, 3.0f, &deg);
    EXPECT_EQ(deg, 0.0f);
    EXPECT_NEAR(I[0], 1.0f, 1e-6f); EXPECT_NEAR(I[8], 1.0f, 1e-6f);
    // Z-up head facing -Y (the review case): pitch it onto +Z, and its up
    // (+Z) must land on +Y so the head is upright in the template frame.
    const auto P = FaceRig::rotationToPlusZ({0, -1, 0}, 3.0f, &deg);
    EXPECT_NEAR(deg, 90.0f, 1e-3f);
    const V3 pf = FaceRig::rotateVec(P, {0, -1, 0}, false);
    EXPECT_NEAR(pf[2], 1.0f, 1e-5f);
    const V3 pu = FaceRig::rotateVec(P, {0, 0, 1}, false);
    EXPECT_NEAR(pu[1], 1.0f, 1e-5f);
    // An arbitrary tilted direction still lands exactly on +Z.
    const V3 tilt{0.3f, 0.5f, -0.8f};
    const auto T = FaceRig::rotationToPlusZ(tilt, 3.0f, &deg);
    const V3 tf = FaceRig::rotateVec(T, tilt, false);
    const float tl = std::sqrt(tilt[0]*tilt[0] + tilt[1]*tilt[1] + tilt[2]*tilt[2]);
    EXPECT_NEAR(tf[0], 0.0f, 1e-5f); EXPECT_NEAR(tf[1], 0.0f, 1e-5f);
    EXPECT_NEAR(tf[2], tl, 1e-5f);
    // Degenerate: identity, no NaN.
    const auto Z = FaceRig::rotationToPlusZ({0, 0, 0}, 3.0f, &deg);
    EXPECT_EQ(deg, 0.0f); EXPECT_NEAR(Z[4], 1.0f, 1e-6f);
}
