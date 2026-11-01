// Pure pieces of the native Kimodo runtime, pinned against values captured from Kimodo's own Python
// (kimodo e1c219c: sanitize.py, model/diffusion.py, motion_rep/smooth_root.py, geometry.py).

#include "KimodoMath.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace AIMotion::KimodoMath;

TEST(KimodoMath, SanitizeMatchesPython)
{
    EXPECT_EQ(sanitizeText(QStringLiteral("walk")), QStringLiteral("Walk."));
    EXPECT_EQ(sanitizeText(QStringLiteral("  a person   runs forward!!  ")), QStringLiteral("A person runs forward! !"));
    EXPECT_EQ(sanitizeText(QStringLiteral("Someone waves with the right hand!!")), QStringLiteral("Someone waves with the right hand! !"));
    EXPECT_EQ(sanitizeText(QStringLiteral("sit down slowly, then stand up")), QStringLiteral("Sit down slowly, then stand up."));
    EXPECT_EQ(sanitizeText(QStringLiteral("--hello world--")), QStringLiteral("Hello world."));
    EXPECT_EQ(sanitizeText(QStringLiteral("!!!")), QStringLiteral("!"));
    EXPECT_EQ(sanitizeText(QStringLiteral("JUMP. then SIT")), QStringLiteral("Jump. Then sit."));
    EXPECT_EQ(sanitizeText(QStringLiteral("he says \"hi\"")), QStringLiteral("He says \"hi\"."));
    EXPECT_EQ(sanitizeText(QStringLiteral("a b c?d e")), QStringLiteral("A b c? D e."));
    EXPECT_EQ(sanitizeText(QStringLiteral("   ")), QString());
}

TEST(KimodoMath, Llm2VecPromptPoolsThePromptAndEot)
{
    const LlmPrompt p = llm2vecPrompt(QStringLiteral("Walk."));
    EXPECT_EQ(p.full, QStringLiteral("<|start_header_id|>user<|end_header_id|>\n\nWalk.<|eot_id|>"));
    EXPECT_EQ(p.pooled, QStringLiteral("Walk.<|eot_id|>"));
}

namespace {

// kimodo/model/diffusion.py get_beta_schedule (cosine) + cumprod, float32 like the torch buffers.
std::vector<float> cosineBase(int n)
{
    const auto ab = [](double t) { const double c = std::cos((t + 0.008) / 1.008 * M_PI / 2.0); return c * c; };
    std::vector<float> acp(static_cast<size_t>(n));
    float cum = 1.0f;
    for (int i = 0; i < n; ++i) {
        const float beta = float(std::min(1.0 - ab(double(i + 1) / n) / ab(double(i) / n), 0.999));
        cum *= (1.0f - beta);
        acp[size_t(i)] = cum;
    }
    return acp;
}

} // namespace

TEST(KimodoMath, DdimScheduleMatchesKimodo)
{
    const std::vector<float> base = cosineBase(1000);
    EXPECT_NEAR(base[0], 0.9999586939811707, 1e-7);
    EXPECT_NEAR(base[999], 2.4287349909002387e-09, 1e-12);

    const DdimSchedule s20 = ddimSchedule(base, 20);
    const std::vector<int64_t> want = {0, 53, 105, 158, 210, 263, 315, 368, 421, 473, 526, 578, 631, 684, 736, 789, 841, 894, 946, 999};
    EXPECT_EQ(s20.timesteps, want);
    EXPECT_NEAR(s20.alphasCumprod[0], 0.9999586939811707, 1e-6);
    EXPECT_NEAR(s20.alphasCumprod[5], 0.8309721946716309, 1e-5);
    EXPECT_NEAR(s20.alphasCumprod[19], 2.434341173085386e-09, 1e-10);
    EXPECT_FLOAT_EQ(s20.alphasCumprodPrev[0], 1.0f);
    EXPECT_NEAR(s20.alphasCumprodPrev[5], 0.8881219625473022, 1e-5);
    EXPECT_NEAR(s20.alphasCumprodPrev[19], 0.006806911434978247, 1e-6);

    const DdimSchedule s100 = ddimSchedule(base, 100);
    EXPECT_EQ(s100.timesteps[1], 10);
    EXPECT_EQ(s100.timesteps[95], 959);
    EXPECT_EQ(s100.timesteps[99], 999);
}

TEST(KimodoMath, DdimStepAtTheLastStepReturnsThePrediction)
{
    // At step 0 alpha_bar_prev = 1: x_{t-1} is exactly the clean prediction.
    const DdimSchedule s = ddimSchedule(cosineBase(1000), 10);
    std::vector<float> x = {0.3f, -1.2f};
    const std::vector<float> pred = {0.5f, 0.25f};
    ddimStep(s, 0, pred.data(), x.data(), x.size());
    EXPECT_NEAR(x[0], 0.5f, 1e-6);
    EXPECT_NEAR(x[1], 0.25f, 1e-6);
}

TEST(KimodoMath, SmoothSignalMatchesKimodo)
{
    const int T = 37;
    std::vector<std::array<float, 2>> x(static_cast<size_t>(T));
    for (int t = 0; t < T; ++t)
        x[size_t(t)] = {float(0.05 * t + 0.03 * std::sin(t * 0.9)), float(0.02 * std::cos(t * 1.3))};
    const auto y = smoothSignal(x, 0.06f);
    ASSERT_EQ(y.size(), size_t(T));
    const struct { int i; double a, b; } want[] = {
        {0, 0.06219957023859024, 0.0029110759496688843},  {1, 0.11015245318412781, 0.002662800019606948},
        {10, 0.5417933464050293, 0.0004400657780934125},  {17, 0.8777124881744385, -0.0012265348341315985},
        {35, 1.7545523643493652, -0.00519526656717062},   {36, 1.7906067371368408, -0.005354589782655239},
    };
    for (const auto& w : want) {
        EXPECT_NEAR(y[size_t(w.i)][0], w.a, 2e-5) << "frame " << w.i;
        EXPECT_NEAR(y[size_t(w.i)][1], w.b, 2e-5) << "frame " << w.i;
    }
}

TEST(KimodoMath, QuaternionConversionsMatchPyTorch3D)
{
    const float rz90[9] = {0, -1, 0, 1, 0, 0, 0, 0, 1};
    const auto q = quatFromMatrixWxyz(rz90);
    EXPECT_NEAR(q[0], 0.70710677f, 1e-6);
    EXPECT_NEAR(q[3], 0.70710677f, 1e-6);
    const float ry180[9] = {-1, 0, 0, 0, 1, 0, 0, 0, -1};
    const auto q2 = quatFromMatrixWxyz(ry180);
    EXPECT_NEAR(q2[2], 1.0f, 1e-6);
    float back[9];
    matrixFromQuatWxyz(q, back);
    for (int k = 0; k < 9; ++k) EXPECT_NEAR(back[k], rz90[k], 1e-6);
}
