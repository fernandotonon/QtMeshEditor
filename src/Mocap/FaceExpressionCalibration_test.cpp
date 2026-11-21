#include <gtest/gtest.h>

#include "FaceExpressionCalibration.h"
#include "FaceCapCanonicalData.h"

#include <cstring>

namespace {

int channel(const char* name)
{
    for (int c = 0; c < FaceCalibration::kChannels; ++c)
        if (std::strcmp(FaceCap::kBlendshapeNames[size_t(c)], name) == 0)
            return c;
    return -1;
}

std::vector<std::array<float, FaceCalibration::kChannels>> take(int frames)
{
    return std::vector<std::array<float, FaceCalibration::kChannels>>(size_t(frames), std::array<float, 52>{});
}

} // namespace

TEST(FaceExpressionCalibration, BiasedBlinkRestsAtZeroAndARealBlinkStillCloses)
{
    // A performer looking down at a phone: blink reads ~0.5 all take, one real blink reaches 0.95.
    const int blink = channel("eyeBlinkLeft");
    ASSERT_GE(blink, 0);
    auto w = take(20);
    for (auto& f : w) f[size_t(blink)] = 0.5f;
    w[10][size_t(blink)] = 0.95f;
    const std::vector<bool> confident(20, true);

    const auto report = FaceCalibration::calibrate(w, confident);
    EXPECT_TRUE(report.baselineApplied);
    EXPECT_NEAR(w[0][size_t(blink)], 0.0f, 1e-6f);
    EXPECT_NEAR(w[10][size_t(blink)], 0.9f, 1e-4f);   // (0.95-0.5)/(1-0.5)
}

TEST(FaceExpressionCalibration, HeldSmileIsNotTreatedAsBias)
{
    const int smile = channel("mouthSmileLeft");
    ASSERT_GE(smile, 0);
    EXPECT_FALSE(FaceCalibration::hasBaseline(smile));
    auto w = take(20);
    for (auto& f : w) f[size_t(smile)] = 0.7f;
    FaceCalibration::calibrate(w, std::vector<bool>(20, true));
    EXPECT_NEAR(w[5][size_t(smile)], 0.7f, 1e-6f);
}

TEST(FaceExpressionCalibration, UnderReportedJawGetsItsGainAndClamps)
{
    const int jaw = channel("jawOpen");
    ASSERT_GE(jaw, 0);
    const float gain = FaceCalibration::channelGain(jaw);
    EXPECT_GT(gain, 1.0f);
    auto w = take(4);
    w[1][size_t(jaw)] = 0.2f;
    w[2][size_t(jaw)] = 0.9f;
    FaceCalibration::calibrate(w, std::vector<bool>(4, true));
    EXPECT_NEAR(w[1][size_t(jaw)], 0.2f * gain, 1e-5f);
    EXPECT_FLOAT_EQ(w[2][size_t(jaw)], 1.0f);
}

TEST(FaceExpressionCalibration, FewFramesSkipTheBaselineAndUnconfidentFramesAreUntouched)
{
    const int blink = channel("eyeBlinkRight");
    auto w = take(4);
    for (auto& f : w) f[size_t(blink)] = 0.5f;
    std::vector<bool> confident{true, true, false, true};
    const auto report = FaceCalibration::calibrate(w, confident);
    EXPECT_FALSE(report.baselineApplied);   // 3 confident frames < minFramesForBaseline
    EXPECT_NEAR(w[0][size_t(blink)], 0.5f, 1e-6f);
    EXPECT_NEAR(w[2][size_t(blink)], 0.5f, 1e-6f);
}

TEST(FaceExpressionCalibration, DisabledCalibrationWithUnitStrengthKeysTheRawOutput)
{
    const int jaw = channel("jawOpen");
    const int blink = channel("eyeBlinkLeft");
    auto w = take(20);
    for (auto& f : w) { f[size_t(jaw)] = 0.3f; f[size_t(blink)] = 0.5f; }
    FaceCalibration::Options o;
    o.baseline = false;
    o.gains = false;
    FaceCalibration::calibrate(w, std::vector<bool>(20, true), o);
    EXPECT_NEAR(w[3][size_t(jaw)], 0.3f, 1e-6f);
    EXPECT_NEAR(w[3][size_t(blink)], 0.5f, 1e-6f);
}

TEST(FaceExpressionCalibration, StrengthScalesEveryChannel)
{
    const int smile = channel("mouthSmileRight");
    auto w = take(2);
    w[0][size_t(smile)] = 0.4f;
    FaceCalibration::Options o;
    o.strength = 1.5;
    FaceCalibration::calibrate(w, std::vector<bool>(2, true), o);
    EXPECT_NEAR(w[0][size_t(smile)], 0.6f, 1e-5f);
}
