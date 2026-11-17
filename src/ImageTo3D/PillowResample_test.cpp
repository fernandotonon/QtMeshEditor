#include "PillowResample.h"

#include <gtest/gtest.h>

#include <QImage>

#include <vector>

// Expected bytes come from Pillow 13.0:
//   Image.fromarray(pattern(w, h), 'RGBA').resize((ow, oh), Image.Resampling.LANCZOS)
// The resample must match bit for bit: the TRELLIS.2 1024 texture flow
// reacts to differences of a level or two in its input.

namespace {

QImage pattern(int w, int h)
{
    static const int alpha[5] = {0, 255, 128, 40, 200};
    QImage img(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            line[x * 4 + 0] = uchar((x * 37 + y * 11) % 256);
            line[x * 4 + 1] = uchar((x * 5 + y * 53) % 256);
            line[x * 4 + 2] = uchar((x * x + y * 7) % 256);
            line[x * 4 + 3] = uchar(alpha[(x + 2 * y) % 5]);
        }
    }
    return img;
}

std::vector<int> bytes(const QImage& img)
{
    std::vector<int> out;
    for (int y = 0; y < img.height(); ++y) {
        const uchar* line = img.constScanLine(y);
        for (int x = 0; x < img.width() * 4; ++x)
            out.push_back(line[x]);
    }
    return out;
}

} // namespace

TEST(PillowResample, FitLongSideMatchesUpstreamRounding)
{
    EXPECT_EQ(PillowResample::fitLongSide(QSize(1024, 1536)), QSize(682, 1024));
    EXPECT_EQ(PillowResample::fitLongSide(QSize(4000, 3000)), QSize(1024, 768));
    EXPECT_EQ(PillowResample::fitLongSide(QSize(800, 600)), QSize(800, 600));
    EXPECT_EQ(PillowResample::fitLongSide(QSize(1024, 10)), QSize(1024, 10));
}

TEST(PillowResample, HorizontalFirstMatchesPillow)
{
    const QImage out = PillowResample::resizeLikePillow(pattern(12, 7), QSize(5, 3));
    const std::vector<int> expected = {
        34, 40, 6, 119, 128, 49, 14, 123, 174, 60, 34, 127, 45, 80, 67, 124, 126, 98, 108, 117,
        52, 156, 20, 135, 161, 176, 31, 123, 158, 189, 51, 124, 57, 183, 83, 125, 162, 142, 128, 127,
        85, 64, 39, 111, 192, 94, 46, 127, 103, 91, 66, 126, 79, 104, 100, 122, 188, 62, 143, 130};
    EXPECT_EQ(out.format(), QImage::Format_RGBA8888);
    EXPECT_EQ(bytes(out), expected);
}

TEST(PillowResample, VerticalFirstWhenHeightShrinksMoreMatchesPillow)
{
    // 40 -> 9 rows vs 6 -> 5 columns: Pillow runs the vertical pass first.
    const QImage out = PillowResample::resizeLikePillow(pattern(6, 40), QSize(5, 9));
    const std::vector<int> expected = {
        23, 113, 14, 121, 68, 70, 11, 130, 108, 121, 18, 122, 158, 101, 27, 121, 217, 121, 35, 130,
        69, 134, 40, 125, 114, 101, 45, 123, 160, 143, 49, 124, 210, 135, 56, 126, 120, 140, 68, 123,
        118, 140, 73, 125, 164, 104, 76, 124, 230, 146, 79, 125, 95, 140, 87, 125, 20, 146, 97, 125,
        170, 148, 104, 124, 214, 112, 106, 125, 125, 158, 111, 124, 48, 142, 118, 125, 90, 152, 129, 124,
        222, 156, 137, 124, 89, 118, 136, 125, 37, 170, 141, 124, 95, 112, 148, 125, 139, 135, 160, 124,
        150, 164, 168, 124, 36, 142, 169, 125, 104, 113, 174, 124, 140, 81, 179, 125, 195, 158, 189, 124,
        38, 138, 195, 125, 106, 90, 201, 124, 142, 132, 206, 125, 213, 90, 224, 124, 212, 163, 234, 125,
        114, 144, 246, 122, 149, 93, 227, 128, 221, 138, 234, 123, 165, 97, 200, 126, 22, 167, 109, 123,
        154, 158, 66, 135, 227, 109, 86, 112, 132, 151, 30, 133, 2, 103, 29, 121, 82, 186, 11, 130};
    EXPECT_EQ(bytes(out), expected);
}

TEST(PillowResample, PremultiplyRoundTripKeepsOpaqueAndClearPixels)
{
    std::vector<uint8_t> px = {10, 20, 30, 255, 40, 50, 60, 0, 200, 100, 50, 128};
    PillowResample::premultiply(px);
    EXPECT_EQ(px[0], 10);    // opaque unchanged
    EXPECT_EQ(px[4], 0);     // alpha 0 -> black
    EXPECT_EQ(px[8], 100);   // MULDIV255(200, 128)
    PillowResample::unpremultiply(px);
    EXPECT_EQ(px[8], 199);   // floor(255 * 100 / 128): Pillow truncates
}

TEST(PillowResample, RejectsBadInput)
{
    EXPECT_TRUE(PillowResample::lanczosRgba8({1, 2, 3}, 1, 1, 1, 1).empty());
    EXPECT_TRUE(PillowResample::resizeLikePillow(QImage(), QSize(4, 4)).isNull());
}
