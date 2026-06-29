#include <gtest/gtest.h>

#include "HdrEquirectLoader.h"

#include "MinimalEXRWriter.h"

#include <QTemporaryDir>
#include <QFile>
#include <cmath>

using namespace HdrEquirect;

namespace {

FloatImage makeTopRedBottomBlueEquirect()
{
    FloatImage img;
    img.width = 8;
    img.height = 4;
    img.rgb.resize(static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 3u);
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(img.width)
                              + static_cast<size_t>(x)) * 3u;
            if (y >= img.height / 2) {
                img.rgb[i + 0] = 1.f;
                img.rgb[i + 1] = 0.f;
                img.rgb[i + 2] = 0.f;
            } else {
                img.rgb[i + 0] = 0.f;
                img.rgb[i + 1] = 0.f;
                img.rgb[i + 2] = 1.f;
            }
        }
    }
    return img;
}

} // namespace

TEST(HdrEquirectLoaderTest, BakeSynthetic8x4_PolarFacesHaveExpectedMeans)
{
    const FloatImage equirect = makeTopRedBottomBlueEquirect();
    CubemapFaces faces;
    QString error;
    ASSERT_TRUE(bakeEquirectToCubemap(equirect, 4, faces, error)) << error.toStdString();

    // Ogre face order: +X, -X, +Y, -Y, +Z, -Z
    const RgbMean plusY = faceMeanRgb(faces.faces[2], faces.faceSize);
    const RgbMean minusY = faceMeanRgb(faces.faces[3], faces.faceSize);

    EXPECT_GT(plusY.r, plusY.b);
    EXPECT_GT(minusY.b, minusY.r);
}

TEST(HdrEquirectLoaderTest, BakeConstantEquirect_AllFacesMatch)
{
    FloatImage equirect;
    equirect.width = 8;
    equirect.height = 4;
    equirect.rgb.assign(static_cast<size_t>(8 * 4 * 3), 0.f);
    for (size_t i = 0; i < equirect.rgb.size(); i += 3) {
        equirect.rgb[i + 0] = 0.75f;
        equirect.rgb[i + 1] = 0.50f;
        equirect.rgb[i + 2] = 0.25f;
    }

    CubemapFaces faces;
    QString error;
    ASSERT_TRUE(bakeEquirectToCubemap(equirect, 4, faces, error)) << error.toStdString();

    for (int face = 0; face < 6; ++face) {
        const RgbMean mean = faceMeanRgb(faces.faces[static_cast<size_t>(face)], faces.faceSize);
        EXPECT_NEAR(mean.r, 0.75f, 0.08f);
        EXPECT_NEAR(mean.g, 0.50f, 0.08f);
        EXPECT_NEAR(mean.b, 0.25f, 0.08f);
    }
}

TEST(HdrEquirectLoaderTest, Sha1HexOfFile_IsStable)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("probe.bin"));
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("hdr-environment-cache-key");
    f.close();

    const QString a = sha1HexOfFile(path);
    const QString b = sha1HexOfFile(path);
    EXPECT_FALSE(a.isEmpty());
    EXPECT_EQ(a, b);
}

#ifdef ENABLE_OPENEXR
TEST(HdrEquirectLoaderTest, LoadExr_RoundTripsFloatRgb)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const int w = 16;
    const int h = 8;
    std::vector<float> rgb(static_cast<size_t>(w * h * 3));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w)
                              + static_cast<size_t>(x)) * 3u;
            rgb[i + 0] = static_cast<float>(x) / static_cast<float>(w);
            rgb[i + 1] = static_cast<float>(y) / static_cast<float>(h);
            rgb[i + 2] = 0.5f;
        }
    }

    const QString path = tmp.filePath(QStringLiteral("env.exr"));
    ASSERT_TRUE(MinimalEXR::writeRGB32F(path, w, h, rgb));

    FloatImage loaded;
    QString error;
    ASSERT_TRUE(loadFromFile(path, loaded, error)) << error.toStdString();
    EXPECT_EQ(loaded.width, w);
    EXPECT_EQ(loaded.height, h);
    EXPECT_EQ(static_cast<int>(loaded.rgb.size()), w * h * 3);
    EXPECT_NEAR(loaded.rgb[0], 0.f, 1e-4f);
    EXPECT_NEAR(loaded.rgb[1], 0.f, 1e-4f);
    EXPECT_NEAR(loaded.rgb[2], 0.5f, 1e-4f);
}
#endif
