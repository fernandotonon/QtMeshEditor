// Audio2Face (#1019) PCA reconstruction. Pure data — a tiny synthetic basis
// pins every rule without the 200 MB model archive.
#include <gtest/gtest.h>

#include "AudioToFace/A2FCoefficients.h"

#include <cmath>
#include <vector>

using AudioToFace::CoefficientLayout;
using AudioToFace::PcaBasis;
using AudioToFace::reconstructPca;
using AudioToFace::reconstructSkin;
using AudioToFace::reconstructTongue;

namespace {

// 2 shapes over 2 vertices (6 values). The mean is deliberately NON-ZERO,
// because treating it as zero is the easy mistake and the tests must catch it.
PcaBasis tinyBasis()
{
    PcaBasis b;
    b.shapeCount = 2;
    b.valueCount = 6;
    b.mean   = {0.5f, 0.0f, 0.0f,  0.5f, 0.0f, 0.0f};
    b.matrix = {
        1.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,   // shape 0: +X on both verts
        0.0f, 2.0f, 0.0f,  0.0f, 2.0f, 0.0f,   // shape 1: +2Y on both verts
    };
    return b;
}

}  // namespace

TEST(A2FCoefficients, LayoutSumsToTheNetworkOutputSize)
{
    // 272 + 10 + 15 + 4 = 301, straight from the shipped network_info.json.
    // If this ever disagrees the reconstruction reads the wrong slice and the
    // face deforms from tongue/jaw numbers.
    CoefficientLayout l;
    EXPECT_EQ(l.total(), 301);
    EXPECT_EQ(l.skinShapes, 272);
    EXPECT_EQ(l.tongueShapes, 10);
    EXPECT_EQ(l.jawValues, 15);
    EXPECT_EQ(l.eyeValues, 4);
}

TEST(A2FCoefficients, ReconstructsMeanPlusWeightedShapes)
{
    const auto b = tinyBasis();
    const auto out = reconstructPca(b, {1.0f, 0.5f});
    ASSERT_EQ(out.size(), 6u);
    // vert0: mean(0.5,0,0) + 1*(1,0,0) + 0.5*(0,2,0) = (1.5, 1.0, 0)
    EXPECT_FLOAT_EQ(out[0], 1.5f);
    EXPECT_FLOAT_EQ(out[1], 1.0f);
    EXPECT_FLOAT_EQ(out[2], 0.0f);
    EXPECT_FLOAT_EQ(out[3], 1.5f);
    EXPECT_FLOAT_EQ(out[4], 1.0f);
}

// The mean is the average FACE, not zero. Omitting it offsets every frame by
// that average expression, so the character never returns to neutral between
// words — a subtle, constant wrongness rather than an obvious break.
TEST(A2FCoefficients, ZeroCoefficientsGiveTheMeanNotZero)
{
    const auto b = tinyBasis();
    const auto out = reconstructPca(b, {0.0f, 0.0f});
    ASSERT_EQ(out.size(), 6u);
    EXPECT_FLOAT_EQ(out[0], 0.5f) << "all-zero coefficients must return the MEAN";
    EXPECT_FLOAT_EQ(out[3], 0.5f);
    for (float v : out) EXPECT_TRUE(std::isfinite(v));
}

// The skin block is read from a full 301-vector in place. Reading it with the
// wrong offset silently deforms the face using tongue coefficients.
TEST(A2FCoefficients, OffsetSelectsTheRequestedBlock)
{
    const auto b = tinyBasis();
    // Pretend shapes 0,1 live at index 3 of a longer vector.
    const std::vector<float> full{9.0f, 9.0f, 9.0f, 1.0f, 0.5f};
    const auto out = reconstructPca(b, full, 3);
    ASSERT_EQ(out.size(), 6u);
    EXPECT_FLOAT_EQ(out[0], 1.5f) << "must read from the offset, not the start";
    EXPECT_FLOAT_EQ(out[1], 1.0f);
}

// A short coefficient vector means the caller mis-sized the network output.
// Reconstructing what fits would deform part of the face and leave the rest at
// the mean, which presents as a rig fault rather than a data fault.
TEST(A2FCoefficients, ShortInputIsRefusedRatherThanPartiallyApplied)
{
    const auto b = tinyBasis();
    EXPECT_TRUE(reconstructPca(b, {1.0f}).empty())       << "needs 2 coefficients";
    EXPECT_TRUE(reconstructPca(b, {}).empty());
    EXPECT_TRUE(reconstructPca(b, {1.0f, 1.0f}, 1).empty())
        << "offset 1 leaves only one coefficient available";
    EXPECT_TRUE(reconstructPca(b, {1.0f, 1.0f}, -1).empty()) << "negative offset";
}

TEST(A2FCoefficients, InvalidBasisIsRefused)
{
    PcaBasis b;                                  // all-empty
    EXPECT_FALSE(b.valid());
    EXPECT_TRUE(reconstructPca(b, {1.0f}).empty());

    PcaBasis ragged = tinyBasis();
    ragged.matrix.pop_back();                    // matrix no longer shapeCount*valueCount
    EXPECT_FALSE(ragged.valid()) << "a truncated basis must not be usable";
    EXPECT_TRUE(reconstructPca(ragged, {1.0f, 1.0f}).empty());

    PcaBasis badMean = tinyBasis();
    badMean.mean.pop_back();
    EXPECT_FALSE(badMean.valid()) << "mean must match valueCount";
}

// The skin/tongue helpers must require a FULL network output, not just enough
// for their own block — a caller passing a truncated tensor has a bug that
// should surface here rather than as odd motion.
TEST(A2FCoefficients, BlockHelpersRequireAFullNetworkOutput)
{
    const auto b = tinyBasis();
    const std::vector<float> tooShort(300, 0.0f);
    EXPECT_TRUE(reconstructSkin(b, tooShort).empty());
    EXPECT_TRUE(reconstructTongue(b, tooShort).empty());

    std::vector<float> full(301, 0.0f);
    full[0] = 1.0f;                     // first SKIN coefficient
    full[272] = 1.0f;                   // first TONGUE coefficient
    const auto skin = reconstructSkin(b, full);
    ASSERT_EQ(skin.size(), 6u);
    EXPECT_FLOAT_EQ(skin[0], 1.5f) << "skin reads from index 0";

    const auto tongue = reconstructTongue(b, full);
    ASSERT_EQ(tongue.size(), 6u);
    EXPECT_FLOAT_EQ(tongue[0], 1.5f) << "tongue reads from index skinShapes";
}
