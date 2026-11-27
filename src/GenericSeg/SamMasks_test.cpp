// SAM 2.1 automatic-mask post-processing (#1179): pure data, no model.

#include "GenericSeg/SamMasks.h"

#include <gtest/gtest.h>

using namespace GenericSeg;

namespace {

Raster rasterWithBlock(int size, int x0, int y0, int x1, int y1)
{
    Raster r;
    r.size = size;
    r.face.assign(size_t(size) * size, -1);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) r.face[size_t(y) * size + x] = 0;
    return r;
}

std::vector<float> logitsBox(int x0, int y0, int x1, int y1, float in = 5.0f, float out = -5.0f)
{
    std::vector<float> l(Sam::kLowRes * Sam::kLowRes, out);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) l[size_t(y) * Sam::kLowRes + x] = in;
    return l;
}

Sam::Candidate candidate(const std::vector<float>& logits, float score)
{
    Sam::Candidate c;
    c.logits = logits.data();
    c.score = score;
    EXPECT_TRUE(Sam::binarize(c));
    return c;
}

} // namespace

TEST(GenericSegSam, GridPointsCoverOnlyTheForegroundInInputPixels)
{
    // Mesh covers the left half of a 128 px image.
    const Raster r = rasterWithBlock(128, 0, 0, 64, 128);
    const auto pts = Sam::gridPoints(r, 8);
    EXPECT_EQ(pts.size(), 32u);   // 4 of 8 columns x 8 rows
    for (const auto& p : pts) {
        EXPECT_LT(p[0], Sam::kInputSize / 2.0f);
        EXPECT_GT(p[1], 0.0f);
        EXPECT_LT(p[1], float(Sam::kInputSize));
    }
    // First cell centre: (0.5/8) of the 1024 frame.
    EXPECT_FLOAT_EQ(pts[0][0], 64.0f);
    EXPECT_FLOAT_EQ(pts[0][1], 64.0f);
    EXPECT_TRUE(Sam::gridPoints(rasterWithBlock(64, 0, 0, 0, 0), 8).empty());
}

TEST(GenericSegSam, StabilityScoreIsIouOfTheTwoThresholds)
{
    std::vector<float> l = {3.0f, 0.5f, -0.5f, -3.0f};
    // > +1: 1 pixel, > -1: 3 pixels.
    EXPECT_FLOAT_EQ(Sam::stabilityScore(l.data(), 4, 1.0f), 1.0f / 3.0f);
    std::vector<float> none(4, -9.0f);
    EXPECT_FLOAT_EQ(Sam::stabilityScore(none.data(), 4, 1.0f), 0.0f);
}

TEST(GenericSegSam, BinarizeFindsAreaAndBox)
{
    const auto l = logitsBox(10, 20, 30, 25);
    const Sam::Candidate c = candidate(l, 0.9f);
    EXPECT_EQ(c.area, 20 * 5);
    EXPECT_EQ(c.box, (std::array<int, 4>{10, 20, 29, 24}));
    Sam::Candidate empty;
    const auto neg = logitsBox(0, 0, 0, 0);
    empty.logits = neg.data();
    EXPECT_FALSE(Sam::binarize(empty));
}

TEST(GenericSegSam, NmsKeepsTheBestOfNearDuplicatesAndDistinctMasks)
{
    const auto a = logitsBox(0, 0, 100, 100);
    const auto aNear = logitsBox(0, 0, 100, 98);     // IoU 0.98 with a
    const auto b = logitsBox(150, 150, 200, 200);    // disjoint
    const auto sub = logitsBox(0, 0, 50, 50);        // inside a, IoU 0.25
    std::vector<Sam::Candidate> c;
    c.push_back(candidate(aNear, 0.80f));
    c.push_back(candidate(a, 0.95f));
    c.push_back(candidate(b, 0.70f));
    c.push_back(candidate(sub, 0.90f));
    const auto kept = Sam::nms(c, 0.7f);
    EXPECT_EQ(kept, (std::vector<int>{1, 3, 2}));   // by score, aNear dropped
}

TEST(GenericSegSam, UpsampleKeepsTheMaskShape)
{
    const auto l = logitsBox(0, 0, 128, 256);   // left half
    const auto up = Sam::upsample(l.data(), 512);
    int left = 0, right = 0;
    for (int y = 0; y < 512; ++y)
        for (int x = 0; x < 512; ++x) (x < 256 ? left : right) += up[size_t(y) * 512 + x];
    EXPECT_EQ(left, 256 * 512);
    EXPECT_EQ(right, 0);
    // Downsampling works too (raster smaller than the mask).
    const auto down = Sam::upsample(l.data(), 64);
    EXPECT_EQ(down[0], 1);
    EXPECT_EQ(down[63], 0);
}
