// Audio2Face (#1019) audio preprocessing. Pure data — no Qt, no audio file.
#include <gtest/gtest.h>

#include "AudioToFace/AudioPreprocess.h"

#include <cmath>
#include <vector>

using namespace AudioToFace;

// The window is CENTRED on the frame it predicts, not started at it. This is
// the single easiest thing to get wrong here, and getting it wrong does not
// crash — it shifts the entire performance half a second late against the
// audio, which reads as "the lipsync is bad" rather than as a bug.
TEST(AudioPreprocess, WindowIsCentredOnTheFrameNotStartedAtIt)
{
    // A ramp, so every sample is identifiable by value.
    std::vector<float> s(40000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = float(i);

    const int64_t centre = 20000;
    const auto w = windowAt(s, centre);
    ASSERT_EQ(int(w.size()), kWindowSamples);

    // The sample at the window's offset must be the frame's own sample.
    EXPECT_FLOAT_EQ(w[size_t(kWindowOffset)], float(centre))
        << "the frame's sample must sit at buffer_ofs, not at index 0";
    // And the window must reach BACK before the frame.
    EXPECT_FLOAT_EQ(w[0], float(centre - kWindowOffset))
        << "the window must start kWindowOffset samples EARLY";
    EXPECT_LT(w[0], float(centre)) << "there must be leading context";
}

// The first and last half-second of a take still need usable windows.
TEST(AudioPreprocess, WindowsNearTheEndsAreZeroPaddedNotTruncated)
{
    std::vector<float> s(1000, 1.0f);

    const auto first = windowAt(s, 0);
    ASSERT_EQ(int(first.size()), kWindowSamples) << "always a full window";
    EXPECT_FLOAT_EQ(first[0], 0.0f) << "before the start must be silence";
    EXPECT_FLOAT_EQ(first[size_t(kWindowOffset)], 1.0f) << "the frame itself is real audio";

    const auto last = windowAt(s, 999);
    ASSERT_EQ(int(last.size()), kWindowSamples);
    EXPECT_FLOAT_EQ(last[size_t(kWindowSamples - 1)], 0.0f) << "past the end must be silence";

    // Entirely outside the audio is all silence, not garbage.
    const auto beyond = windowAt(s, 100000);
    for (float v : beyond) EXPECT_FLOAT_EQ(v, 0.0f);
}

TEST(AudioPreprocess, StereoIsAveragedNotJustLeftChannel)
{
    // Left silent, right carrying the voice — taking channel 0 would yield
    // a silent take and a motionless face.
    const std::vector<float> lr{0.0f, 1.0f,  0.0f, 0.5f,  0.0f, -1.0f};
    const auto mono = toMono(lr, 2);
    ASSERT_EQ(mono.size(), 3u);
    EXPECT_FLOAT_EQ(mono[0], 0.5f);
    EXPECT_FLOAT_EQ(mono[1], 0.25f);
    EXPECT_FLOAT_EQ(mono[2], -0.5f);

    // Mono passes through untouched.
    const std::vector<float> m{0.1f, 0.2f};
    EXPECT_EQ(toMono(m, 1), m);
}

TEST(AudioPreprocess, ResampleHitsTheTargetRateAndPreservesDuration)
{
    // 1 second at 48 kHz -> 1 second at 16 kHz.
    std::vector<float> s(48000, 0.0f);
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = std::sin(2.0f * 3.14159265f * 200.0f * float(i) / 48000.0f);

    const auto out = resampleTo16k(s, 48000);
    EXPECT_NEAR(double(out.size()), 16000.0, 2.0) << "duration must be preserved";
    for (float v : out) {
        EXPECT_TRUE(std::isfinite(v));
        EXPECT_LE(std::abs(v), 1.01f) << "linear interpolation must not overshoot";
    }

    // Already at the target rate: untouched, not resampled through a no-op
    // that would still cost a pass and introduce rounding.
    std::vector<float> at16(100, 0.25f);
    EXPECT_EQ(resampleTo16k(at16, kSampleRate), at16);
}

TEST(AudioPreprocess, FrameCentresMatchTheRequestedRate)
{
    // 2 seconds at 16 kHz, 30 fps -> ~60 frames, one every 533 samples.
    const auto c = frameCentres(size_t(kSampleRate) * 2, 30.0);
    ASSERT_GE(c.size(), 59u);
    ASSERT_LE(c.size(), 61u);
    EXPECT_EQ(c.front(), 0);
    EXPECT_NEAR(double(c[1] - c[0]), double(kSampleRate) / 30.0, 1.0);
    for (size_t i = 1; i < c.size(); ++i)
        EXPECT_GT(c[i], c[i-1]) << "centres must be strictly increasing";
    EXPECT_LT(c.back(), int64_t(kSampleRate) * 2) << "never past the end of the audio";

    EXPECT_TRUE(frameCentres(0, 30.0).empty());
    EXPECT_TRUE(frameCentres(16000, 0.0).empty()) << "a zero frame rate must not divide by zero";
    EXPECT_TRUE(frameCentres(16000, -5.0).empty());
}

// The network's response scales with input level, so a quiet take barely
// opens the mouth. Normalising fixes that — but amplifying silence would turn
// the noise floor into input and make the face mumble through a pause.
TEST(AudioPreprocess, PeakNormalisationLiftsQuietAudioButLeavesSilenceAlone)
{
    std::vector<float> quiet{0.01f, -0.02f, 0.015f};
    const auto lifted = normalisePeak(quiet, 0.95f);
    float peak = 0.0f;
    for (float v : lifted) peak = std::max(peak, std::abs(v));
    EXPECT_NEAR(peak, 0.95f, 1e-5);
    // The SHAPE must be preserved, only the scale changes.
    EXPECT_NEAR(lifted[0] / lifted[1], quiet[0] / quiet[1], 1e-4);

    const std::vector<float> silence(100, 0.0f);
    EXPECT_EQ(normalisePeak(silence, 0.95f), silence) << "silence must not be amplified";

    // Disabled by a zero/negative target.
    EXPECT_EQ(normalisePeak(quiet, 0.0f), quiet);
}
