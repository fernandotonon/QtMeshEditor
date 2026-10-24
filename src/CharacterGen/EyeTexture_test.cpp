#include <gtest/gtest.h>

#include "EyeTexture.h"

#include <cmath>

using namespace CharacterGen;

namespace {
float lum(QRgb p) { return 0.299f * qRed(p) + 0.587f * qGreen(p) + 0.114f * qBlue(p); }
// the pixel at polar texture radius rho (0..1) and angle a
QRgb at(const QImage& img, float rho, float a)
{
    const int x = int(std::lround((0.5f + 0.5f * rho * std::cos(a)) * float(img.width() - 1)));
    const int y = int(std::lround((0.5f + 0.5f * rho * std::sin(a)) * float(img.height() - 1)));
    return img.pixel(x, y);
}
}  // namespace

TEST(EyeSphere, IsAClosedUnitSphereWithTheIrisPoleOnMinusY)
{
    const EyeSphere s = makeEyeSphere(16, 24);
    const size_t n = s.positions.size() / 3;
    ASSERT_GT(n, size_t(0));
    ASSERT_EQ(s.uvs.size(), n * 2);
    for (size_t i = 0; i < n; ++i) {
        const float x = s.positions[i * 3], y = s.positions[i * 3 + 1], z = s.positions[i * 3 + 2];
        EXPECT_NEAR(std::sqrt(x * x + y * y + z * z), 1.0f, 1e-4f);
        EXPECT_GE(s.uvs[i * 2], 0.0f); EXPECT_LE(s.uvs[i * 2], 1.0f);
        EXPECT_GE(s.uvs[i * 2 + 1], 0.0f); EXPECT_LE(s.uvs[i * 2 + 1], 1.0f);
    }
    // vertex 0 is the iris pole: at -Y, in the middle of the texture
    EXPECT_NEAR(s.positions[1], -1.0f, 1e-5f);
    EXPECT_NEAR(s.uvs[0], 0.5f, 1e-5f);
    EXPECT_NEAR(s.uvs[1], 0.5f, 1e-5f);
    ASSERT_EQ(s.indices.size() % 3, size_t(0));
    for (uint32_t i : s.indices) EXPECT_LT(i, n);
}

TEST(EyeSphere, TrianglesFaceOutward)
{
    const EyeSphere s = makeEyeSphere(16, 24);
    int inward = 0;
    for (size_t t = 0; t + 2 < s.indices.size(); t += 3) {
        const float* a = &s.positions[s.indices[t] * 3];
        const float* b = &s.positions[s.indices[t + 1] * 3];
        const float* c = &s.positions[s.indices[t + 2] * 3];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float centroid[3] = {a[0] + b[0] + c[0], a[1] + b[1] + c[1], a[2] + b[2] + c[2]};
        const float area2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
        if (area2 < 1e-12f) continue;   // the degenerate sliver at a pole
        if (n[0] * centroid[0] + n[1] * centroid[1] + n[2] * centroid[2] < 0) ++inward;
    }
    EXPECT_EQ(inward, 0);
}

TEST(EyeSphere, UvRadiusIsMonotonicAndSpansTheDisc)
{
    EXPECT_FLOAT_EQ(eyeUvRadius(0.0f), 0.0f);
    EXPECT_NEAR(eyeUvRadius(3.14159265f), 1.0f, 1e-5f);
    float prev = -1;
    for (int i = 0; i <= 100; ++i) {
        const float r = eyeUvRadius(3.14159265f * float(i) / 100.0f);
        EXPECT_GE(r, prev);
        prev = r;
    }
    EXPECT_LT(eyePupilRadius(), eyeIrisRadius());
}

TEST(EyeTexture, PupilIsBlackIrisHasTheColourAndTheScleraIsPale)
{
    const Rgb colour{0.20f, 0.45f, 0.70f};
    const QImage img = bakeEyeTexture(colour, 256);
    ASSERT_EQ(img.size(), QSize(256, 256));
    EXPECT_LT(lum(img.pixel(128, 128)), 20.0f) << "pupil";
    // mid-iris, averaged around the ring: bluer than red, and clearly coloured
    double r = 0, g = 0, b = 0;
    const float rho = (eyePupilRadius() + eyeIrisRadius()) * 0.5f + 0.02f;
    for (int k = 0; k < 24; ++k) {
        const QRgb p = at(img, rho, 6.2831853f * float(k) / 24.0f);
        r += qRed(p); g += qGreen(p); b += qBlue(p);
    }
    EXPECT_GT(b, r) << "an iris asked to be blue is bluer than red";
    // the sclera: outside the iris, pale
    double sl = 0;
    for (int k = 0; k < 24; ++k) sl += lum(at(img, eyeIrisRadius() * 1.5f, 6.2831853f * float(k) / 24.0f));
    EXPECT_GT(sl / 24.0, 170.0);
}

TEST(EyeTexture, DeterministicAndColourDependent)
{
    const QImage a = bakeEyeTexture({0.4f, 0.25f, 0.1f}, 96, 5);
    const QImage a2 = bakeEyeTexture({0.4f, 0.25f, 0.1f}, 96, 5);
    const QImage b = bakeEyeTexture({0.2f, 0.5f, 0.25f}, 96, 5);
    const QImage c = bakeEyeTexture({0.4f, 0.25f, 0.1f}, 96, 6);
    EXPECT_EQ(a, a2);
    EXPECT_NE(a, b);
    EXPECT_NE(a, c) << "another seed, another fibre pattern";
}

TEST(EyeTexture, PngEncodes)
{
    const QByteArray png = bakeEyeTexturePng({0.3f, 0.3f, 0.3f}, 64);
    ASSERT_FALSE(png.isEmpty());
    QImage back;
    ASSERT_TRUE(back.loadFromData(png, "PNG"));
    EXPECT_EQ(back.size(), QSize(64, 64));
}

TEST(EyeSphere, HasRingsExactlyOnThePupilEdgeAndTheLimbus)
{
    const EyeSphere s = makeEyeSphere();
    auto hasRingAt = [&](float angle) {
        int hits = 0;
        for (size_t i = 0; i < s.positions.size() / 3; ++i) {
            const float theta = std::acos(std::min(1.0f, std::max(-1.0f, -s.positions[i * 3 + 1])));
            if (std::fabs(theta - angle) < 1e-4f) ++hits;
        }
        return hits >= 24;   // one vertex per segment
    };
    EXPECT_TRUE(hasRingAt(eyePupilAngle()));
    EXPECT_TRUE(hasRingAt(eyeIrisAngle()));
}

TEST(EyeDilate, MovesThePupilEdgeAndKeepsTheLimbusAndTheBackFixed)
{
    auto on = [](float theta, float phi) { return std::array<float, 3>{std::sin(theta) * std::cos(phi), -std::cos(theta), std::sin(theta) * std::sin(phi)}; };
    auto angleOf = [](const std::array<float, 3>& p) { return std::acos(std::min(1.0f, std::max(-1.0f, -p[1]))); };
    const float pupil = eyePupilAngle(), iris = eyeIrisAngle();
    // the pupil edge lands on the requested angle, keeping its azimuth
    const auto edge = dilateEyePoint(on(pupil, 1.0f), 0.34f);
    EXPECT_NEAR(angleOf(edge), 0.34f, 1e-4f);
    EXPECT_NEAR(std::atan2(edge[2], edge[0]), 1.0f, 1e-4f);
    // a constriction pulls it towards the pole
    EXPECT_LT(angleOf(dilateEyePoint(on(pupil, 0.3f), 0.10f)), pupil);
    // the limbus, and everything behind it, does not move
    for (float t : {iris, iris + 0.3f, 2.0f, 3.0f}) {
        const auto p = on(t, 2.0f);
        const auto q = dilateEyePoint(p, 0.34f);
        for (int k = 0; k < 3; ++k) EXPECT_NEAR(p[size_t(k)], q[size_t(k)], 1e-5f);
    }
    // the iris band is monotonic: points keep their order from the pole to the limbus
    float prev = -1;
    for (int i = 0; i <= 20; ++i) {
        const float a = angleOf(dilateEyePoint(on(iris * float(i) / 20.0f, 0.5f), 0.34f));
        EXPECT_GE(a, prev - 1e-6f);
        prev = a;
    }
    // the identity request changes nothing
    const auto same = dilateEyePoint(on(0.1f, 0.2f), pupil);
    const auto orig = on(0.1f, 0.2f);
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(same[size_t(k)], orig[size_t(k)], 1e-5f);
}
