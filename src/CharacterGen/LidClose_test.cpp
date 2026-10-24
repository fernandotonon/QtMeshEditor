#include <gtest/gtest.h>

#include "LidClose.h"

#include <cmath>

using namespace CharacterGen;

namespace {
constexpr int kNx = 41;
constexpr int kNy = 29;
constexpr float kCx = 0.03f;   // the left eye (+x)

struct Sheet {
    std::vector<float> positions;
    std::vector<int32_t> faces;
    std::vector<uint8_t> dropped;
    int vertices() const { return int(positions.size() / 3); }
};

// A flat sheet (z = 0) around the left eye with an almond hole: the triangles whose centroid is inside the ellipse are dropped.
Sheet sheet()
{
    Sheet s;
    const float w = 0.08f;
    const float h = 0.04f;
    for (int j = 0; j < kNy; ++j)
        for (int i = 0; i < kNx; ++i) {
            s.positions.push_back(kCx - w / 2 + w * float(i) / float(kNx - 1));
            s.positions.push_back(-h / 2 + h * float(j) / float(kNy - 1));
            s.positions.push_back(0.0f);
        }
    auto idx = [](int i, int j) { return int32_t(j * kNx + i); };
    for (int j = 0; j + 1 < kNy; ++j)
        for (int i = 0; i + 1 < kNx; ++i)
            for (const auto& t : {std::array<int32_t, 3>{idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)}, std::array<int32_t, 3>{idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)}}) {
                s.faces.insert(s.faces.end(), t.begin(), t.end());
                float x = 0;
                float y = 0;
                for (int32_t v : t) { x += s.positions[size_t(v) * 3]; y += s.positions[size_t(v) * 3 + 1]; }
                x = x / 3 - kCx;
                y /= 3;
                const float a = 0.014f;
                const float b = 0.0065f;
                s.dropped.push_back((x * x) / (a * a) + (y * y) / (b * b) < 1.0f ? 1 : 0);
            }
    return s;
}

// A shape table of 4 channels (blinkL, blinkR, wideL, wideR): blinkL lowers the upper half around the left eye by 2 mm, wideL lifts it.
std::vector<float> shapes(const Sheet& s)
{
    const size_t stride = size_t(s.vertices()) * 3;
    std::vector<float> t(4 * stride, 0.0f);
    for (int v = 0; v < s.vertices(); ++v) {
        const float x = s.positions[size_t(v) * 3] - kCx;
        const float y = s.positions[size_t(v) * 3 + 1];
        if (y > 0.0f && std::fabs(x) < 0.02f) {
            t[0 * stride + size_t(v) * 3 + 1] = -0.2f;   // cm
            t[2 * stride + size_t(v) * 3 + 1] = 0.1f;
        }
    }
    return t;
}

LidChannels channels() { LidChannels c; c.blink = {{0, 1}}; c.wide = {{2, 3}}; return c; }
std::array<LashEye, 2> leftEyeOnly() { return {LashEye{{kCx, 0.0f, -0.012f}, true}, LashEye{}}; }

double distanceToPolyline(const std::vector<float>& pos, const std::vector<uint32_t>& line, const std::array<double, 3>& q)
{
    double best = 1e30;
    for (size_t k = 0; k + 1 < line.size(); ++k) {
        std::array<double, 3> a{};
        std::array<double, 3> e{};
        for (size_t c = 0; c < 3; ++c) { a[c] = pos[size_t(line[k]) * 3 + c]; e[c] = double(pos[size_t(line[k + 1]) * 3 + c]) - a[c]; }
        const double len2 = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
        double t = 0;
        if (len2 > 1e-18) t = std::min(1.0, std::max(0.0, ((q[0] - a[0]) * e[0] + (q[1] - a[1]) * e[1] + (q[2] - a[2]) * e[2]) / len2));
        double d2 = 0;
        for (size_t c = 0; c < 3; ++c) { const double d = a[c] + e[c] * t - q[c]; d2 += d * d; }
        best = std::min(best, std::sqrt(d2));
    }
    return best;
}
}  // namespace

TEST(LidClose, TheRestPoseBecomesTheOpenPosePlusTheOldBlink)
{
    Sheet s = sheet();
    const std::vector<float> before = s.positions;
    const std::vector<float> table = shapes(s);
    relaxLids(s.positions, s.vertices(), s.faces, s.dropped, leftEyeOnly(), channels(), table);
    for (int v = 0; v < s.vertices(); ++v) {
        const float expected = before[size_t(v) * 3 + 1] + table[size_t(v) * 3 + 1] * 0.01f;
        EXPECT_NEAR(s.positions[size_t(v) * 3 + 1], expected, 1e-6f) << v;
    }
}

TEST(LidClose, TheBlinkClosesTheUpperMarginOntoTheLowerOne)
{
    Sheet s = sheet();
    const std::vector<float> table = shapes(s);
    const std::vector<float> tuned = relaxLids(s.positions, s.vertices(), s.faces, s.dropped, leftEyeOnly(), channels(), table);
    const LidMargins m = findLidMargins(s.positions, s.faces, s.dropped, leftEyeOnly())[0];
    ASSERT_TRUE(m.valid());
    const size_t stride = size_t(s.vertices()) * 3;
    double worst = 0;
    for (uint32_t u : m.upper) {
        std::array<double, 3> final{};
        for (size_t c = 0; c < 3; ++c) final[c] = double(s.positions[size_t(u) * 3 + c]) + double(tuned[size_t(u) * 3 + c]) * 0.01;   // channel 0 = blinkL
        worst = std::max(worst, distanceToPolyline(s.positions, m.lower, final));
    }
    EXPECT_LT(worst, 0.0004) << "every upper-margin vertex ends within 0.4 mm of the lower margin, corners included";
    // the lid really moved: the middle of the upper margin travels most of the opening
    const uint32_t mid = m.upper[m.upper.size() / 2];
    EXPECT_LT(tuned[size_t(mid) * 3 + 1], -0.3f) << "cm";
    (void)stride;
}

TEST(LidClose, WideIsReBasedAndTheRestIsUntouched)
{
    Sheet s = sheet();
    const std::vector<float> table = shapes(s);
    const std::vector<float> tuned = relaxLids(s.positions, s.vertices(), s.faces, s.dropped, leftEyeOnly(), channels(), table);
    const size_t stride = size_t(s.vertices()) * 3;
    for (size_t i = 0; i < stride; ++i) {
        EXPECT_FLOAT_EQ(tuned[2 * stride + i], table[2 * stride + i] - table[i]) << "wide left = old wide - old blink";
        EXPECT_FLOAT_EQ(tuned[3 * stride + i], table[3 * stride + i]) << "wide right is not re-based without a right eye shape";
    }
}

TEST(LidClose, LowerLidAndOtherSideDoNotMove)
{
    Sheet s = sheet();
    const std::vector<float> table = shapes(s);
    const std::vector<float> tuned = relaxLids(s.positions, s.vertices(), s.faces, s.dropped, leftEyeOnly(), channels(), table);
    const size_t stride = size_t(s.vertices()) * 3;
    for (int v = 0; v < s.vertices(); ++v) {
        if (s.positions[size_t(v) * 3 + 1] < -0.0045f)
            for (size_t c = 0; c < 3; ++c) EXPECT_NEAR(tuned[size_t(v) * 3 + c], 0.0f, 1e-4f) << "below the lid margin";
        for (size_t c = 0; c < 3; ++c) EXPECT_FLOAT_EQ(tuned[stride + size_t(v) * 3 + c], 0.0f) << "the right eye is not seated";
    }
}

TEST(LidClose, AnEyeWithoutMarginsOnlyGetsTheNewRestPose)
{
    Sheet s = sheet();
    std::fill(s.dropped.begin(), s.dropped.end(), uint8_t(0));   // no opening
    const std::vector<float> table = shapes(s);
    const std::vector<float> tuned = relaxLids(s.positions, s.vertices(), s.faces, s.dropped, leftEyeOnly(), channels(), table);
    for (size_t i = 0; i < size_t(s.vertices()) * 3; ++i) EXPECT_FLOAT_EQ(tuned[i], 0.0f);
}
