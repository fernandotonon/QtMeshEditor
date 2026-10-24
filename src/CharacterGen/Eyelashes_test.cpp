#include <gtest/gtest.h>

#include "Eyelashes.h"

#include <cmath>

using namespace CharacterGen;

namespace {
// A flat sheet in the xy plane (z = 0), 6 cm x 4 cm, with an almond hole around (cx, 0): the triangles whose centroid is inside the
// ellipse are "dropped". Returns positions in metres.
struct Sheet {
    std::vector<float> positions;
    std::vector<int32_t> faces;
    std::vector<uint8_t> dropped;
};

Sheet sheetWithHole(float cx)
{
    Sheet s;
    const int nx = 41, ny = 29;
    const float w = 0.06f, h = 0.04f;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            s.positions.push_back(-w / 2 + w * float(i) / float(nx - 1));
            s.positions.push_back(-h / 2 + h * float(j) / float(ny - 1));
            s.positions.push_back(0.0f);
        }
    auto idx = [&](int i, int j) { return int32_t(j * nx + i); };
    for (int j = 0; j + 1 < ny; ++j)
        for (int i = 0; i + 1 < nx; ++i)
            for (const auto& t : {std::array<int32_t, 3>{idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)}, std::array<int32_t, 3>{idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)}}) {
                s.faces.insert(s.faces.end(), t.begin(), t.end());
                float x = 0, y = 0;
                for (int32_t v : t) { x += s.positions[size_t(v) * 3]; y += s.positions[size_t(v) * 3 + 1]; }
                x = x / 3 - cx; y /= 3;
                const float a = 0.014f, b = 0.0065f;   // 28 mm x 13 mm opening
                s.dropped.push_back((x * x) / (a * a) + (y * y) / (b * b) < 1.0f ? 1 : 0);
            }
    return s;
}
}  // namespace

TEST(Eyelashes, BuildsAnUpperAndALowerStripPerEyeRootedOnTheLidMargin)
{
    // two openings: the left eye at +x, the right eye at -x (a sheet wide enough for both)
    Sheet s = sheetWithHole(0.0f);
    const std::array<LashEye, 2> eyes = {LashEye{{0.0f, 0.0f, -0.012f}, true}, LashEye{{0.0f, 0.0f, -0.012f}, false}};
    const LashMesh m = buildLashes(s.positions, s.faces, s.dropped, eyes);
    EXPECT_EQ(m.upperStrips, 1);
    EXPECT_EQ(m.lowerStrips, 1);
    ASSERT_FALSE(m.indices.empty());
    const size_t nv = m.positions.size() / 3;
    ASSERT_EQ(m.sourceVertex.size(), nv);
    ASSERT_EQ(m.normals.size(), nv * 3);
    ASSERT_EQ(m.uvs.size(), nv * 2);
    for (uint32_t i : m.indices) EXPECT_LT(i, nv);
    // vertices come in (root, tip) pairs: the root IS the body vertex, the tip is up to a lash length away
    for (size_t i = 0; i + 1 < nv; i += 2) {
        const uint32_t src = m.sourceVertex[i];
        ASSERT_LT(src, s.positions.size() / 3);
        for (int k = 0; k < 3; ++k) EXPECT_FLOAT_EQ(m.positions[i * 3 + size_t(k)], s.positions[size_t(src) * 3 + size_t(k)]);
        EXPECT_EQ(m.sourceVertex[i + 1], src) << "the tip follows the same body vertex";
        float d2 = 0;
        for (int k = 0; k < 3; ++k) { const float d = m.positions[(i + 1) * 3 + size_t(k)] - m.positions[i * 3 + size_t(k)]; d2 += d * d; }
        EXPECT_GT(std::sqrt(d2), 0.0015f);
        EXPECT_LT(std::sqrt(d2), 0.0125f);
        EXPECT_FLOAT_EQ(m.uvs[i * 2 + 1], 1.0f);
        EXPECT_FLOAT_EQ(m.uvs[(i + 1) * 2 + 1], 0.0f);
    }
    // upper lashes rise above the margin, lower ones drop below it: the first strip is the upper lid
    float upperRise = 0, lowerDrop = 0;
    const size_t half = nv / 2;
    for (size_t i = 0; i + 1 < half; i += 2) upperRise += m.positions[(i + 1) * 3 + 1] - m.positions[i * 3 + 1];
    for (size_t i = half; i + 1 < nv; i += 2) lowerDrop += m.positions[i * 3 + 1] - m.positions[(i + 1) * 3 + 1];
    EXPECT_GT(upperRise, 0.0f);
    EXPECT_GT(lowerDrop, 0.0f);
    // the lashes point away from the eyeball (eyeball behind the sheet at -z): forward = +z
    float forward = 0;
    for (size_t i = 0; i + 1 < nv; i += 2) forward += m.positions[(i + 1) * 3 + 2] - m.positions[i * 3 + 2];
    EXPECT_GT(forward, 0.0f);
}

TEST(Eyelashes, NoLowerLashesWhenTheirLengthIsZeroAndNothingWithoutAnEye)
{
    Sheet s = sheetWithHole(0.0f);
    LashOptions o;
    o.lowerLength = 0.0f;
    const std::array<LashEye, 2> eyes = {LashEye{{0.0f, 0.0f, -0.012f}, true}, LashEye{}};
    const LashMesh m = buildLashes(s.positions, s.faces, s.dropped, eyes, o);
    EXPECT_EQ(m.upperStrips, 1);
    EXPECT_EQ(m.lowerStrips, 0);
    EXPECT_TRUE(buildLashes(s.positions, s.faces, s.dropped, {LashEye{}, LashEye{}}).indices.empty());
}

TEST(Eyelashes, NoOpeningMeansNoLashes)
{
    Sheet s = sheetWithHole(0.0f);
    std::fill(s.dropped.begin(), s.dropped.end(), uint8_t(0));
    const std::array<LashEye, 2> eyes = {LashEye{{0.0f, 0.0f, -0.012f}, true}, LashEye{}};
    EXPECT_TRUE(buildLashes(s.positions, s.faces, s.dropped, eyes).indices.empty());
}

TEST(Eyelashes, AnOpeningFarFromTheEyeballIsNotAnEyelid)
{
    Sheet s = sheetWithHole(0.0f);
    const std::array<LashEye, 2> eyes = {LashEye{{0.5f, 0.0f, -0.012f}, true}, LashEye{}};   // 50 cm away
    EXPECT_TRUE(buildLashes(s.positions, s.faces, s.dropped, eyes).indices.empty());
}

TEST(EyelashTexture, HasADarkOpaqueRootRowAndFadingStrandTips)
{
    const QImage img = bakeLashTexture({0.05f, 0.04f, 0.04f}, 256, 64);
    ASSERT_EQ(img.size(), QSize(256, 64));
    // the root is the top row (OBJ v = 1 after the importer flips V): opaque everywhere; the colour is the lash colour
    int opaqueTop = 0, opaqueBottom = 0;
    for (int x = 0; x < 256; ++x) {
        if (qAlpha(img.pixel(x, 0)) > 250) ++opaqueTop;
        if (qAlpha(img.pixel(x, 63)) > 127) ++opaqueBottom;
    }
    EXPECT_EQ(opaqueTop, 256);
    EXPECT_LT(opaqueBottom, 40) << "only a few strand tips reach the bottom row";
    EXPECT_EQ(qRed(img.pixel(10, 0)), int(std::lround(0.05f * 255)));
    // strands: somewhere in the middle there is both opaque and transparent texture
    int opaque = 0, clear = 0;
    for (int x = 0; x < 256; ++x) (qAlpha(img.pixel(x, 30)) > 127 ? opaque : clear)++;
    EXPECT_GT(opaque, 10);
    EXPECT_GT(clear, 100);
}

TEST(EyelashTexture, DeterministicColouredAndEncodes)
{
    EXPECT_EQ(bakeLashTexture({0.1f, 0.1f, 0.1f}, 128, 32, 3), bakeLashTexture({0.1f, 0.1f, 0.1f}, 128, 32, 3));
    EXPECT_NE(bakeLashTexture({0.1f, 0.1f, 0.1f}, 128, 32, 3), bakeLashTexture({0.1f, 0.1f, 0.1f}, 128, 32, 4));
    EXPECT_NE(bakeLashTexture({0.1f, 0.1f, 0.1f}, 128, 32, 3), bakeLashTexture({0.5f, 0.2f, 0.1f}, 128, 32, 3));
    const QByteArray png = bakeLashTexturePng({0.1f, 0.1f, 0.1f}, 64, 16);
    QImage back;
    ASSERT_TRUE(back.loadFromData(png, "PNG"));
    EXPECT_EQ(back.size(), QSize(64, 16));
}
