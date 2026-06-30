#include <gtest/gtest.h>

#include "HDR/HdrIblPrecompute.h"
#include "HDR/HdrEquirectLoader.h"

#include <array>
#include <cmath>

using namespace HdrEquirect;
using namespace HdrIbl;

namespace {

CubemapFaces makeConstantEnvironment(float r, float g, float b, int faceSize = 8)
{
    FloatImage equirect;
    equirect.width = 16;
    equirect.height = 8;
    equirect.rgb.assign(static_cast<size_t>(16 * 8 * 3), 0.f);
    for (size_t i = 0; i < equirect.rgb.size(); i += 3) {
        equirect.rgb[i + 0] = r;
        equirect.rgb[i + 1] = g;
        equirect.rgb[i + 2] = b;
    }

    CubemapFaces env;
    QString error;
    EXPECT_TRUE(bakeEquirectToCubemap(equirect, faceSize, env, error)) << error.toStdString();
    return env;
}

} // namespace

TEST(HdrIblPrecomputeTest, ConstantEnvironment_IrradianceMatchesReference)
{
    const CubemapFaces env = makeConstantEnvironment(0.8f, 0.4f, 0.2f);
    CubemapFaces irradiance;
    QString error;
    ASSERT_TRUE(bakeIrradiance(env, irradiance, error, 8, 64)) << error.toStdString();

    const std::array<float, 3> dir{0.f, 1.f, 0.f};
    std::array<float, 3> baked{};
    std::array<float, 3> reference{};
    ASSERT_TRUE(sampleIrradianceRgb(irradiance, dir, baked));
    ASSERT_TRUE(referenceIrradianceRgb(env, dir, reference, 64));

    EXPECT_NEAR(baked[0], reference[0], 0.12f);
    EXPECT_NEAR(baked[1], reference[1], 0.12f);
    EXPECT_NEAR(baked[2], reference[2], 0.12f);
}

TEST(HdrIblPrecomputeTest, BrdfLut_KnownPoint_HasExpectedShape)
{
    BrdfLut lut;
    QString error;
    ASSERT_TRUE(bakeBrdfLut(lut, error, 64, 128)) << error.toStdString();

    const int x = 32;
    const int y = 32;
    const size_t idx = (static_cast<size_t>(y) * 64u + static_cast<size_t>(x)) * 2u;
    const float scale = lut.rg[idx + 0];
    const float bias = lut.rg[idx + 1];

    EXPECT_GE(scale, 0.f);
    EXPECT_GE(bias, 0.f);
    EXPECT_TRUE(std::isfinite(scale));
    EXPECT_TRUE(std::isfinite(bias));
    EXPECT_GT(scale + bias, 0.f);
}

TEST(HdrIblPrecomputeTest, BakeAll_ProducesExpectedDimensions)
{
    const CubemapFaces env = makeConstantEnvironment(1.f, 1.f, 1.f, 16);
    IblBakeResult result;
    QString error;
    ASSERT_TRUE(bakeAll(env, result, error, 32)) << error.toStdString();

    EXPECT_EQ(result.irradiance.faceSize, kIrradianceFaceSize);
    EXPECT_EQ(static_cast<int>(result.prefilter.mips.size()), kPrefilterMipCount);
    EXPECT_EQ(result.prefilter.mips.front().faceSize, kPrefilterBaseFaceSize);
    EXPECT_EQ(result.brdfLut.size, kBrdfLutSize);
}
