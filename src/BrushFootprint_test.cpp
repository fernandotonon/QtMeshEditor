#include <gtest/gtest.h>

#include <QFile>
#include <QImage>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "BrushAssetLibrary.h"
#include "BrushFootprint.h"
#include "TexturePaintBuffer.h"

namespace {

BrushFootprint::ImageRgba makeDiskStamp(int size)
{
    BrushFootprint::ImageRgba img;
    img.width = size;
    img.height = size;
    img.pixels.assign(static_cast<size_t>(size) * static_cast<size_t>(size) * 4u, 0);
    const float cx = (size - 1) * 0.5f;
    const float cy = (size - 1) * 0.5f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = (x - cx) / cx;
            const float dy = (y - cy) / cy;
            const float r2 = dx * dx + dy * dy;
            const uint8_t a = r2 <= 1.0f ? 255 : 0;
            const size_t off = (static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)) * 4u;
            img.pixels[off + 0] = 255;
            img.pixels[off + 1] = 255;
            img.pixels[off + 2] = 255;
            img.pixels[off + 3] = a;
        }
    }
    return img;
}

} // namespace

TEST(BrushFootprintTest, StampSpacingScalesWithRadius)
{
    EXPECT_NEAR(BrushFootprint::stampSpacingUv(0.1f, 0.35f), 0.035f, 1e-5f);
    EXPECT_GE(BrushFootprint::stampSpacingUv(0.001f, 0.05f), 0.002f);
}

TEST(BrushFootprintTest, StampSpacingIsAlwaysPositive)
{
    EXPECT_GT(BrushFootprint::stampSpacingUv(0.1f, 0.0f), 0.0f);
    EXPECT_GT(BrushFootprint::stampSpacingUv(0.1f, -1.0f), 0.0f);
    EXPECT_GT(BrushFootprint::stampSpacingUv(0.0f, 0.35f), 0.0f);
}

TEST(BrushFootprintTest, RasterizedStampHasStrongCenterAlpha)
{
    const auto disk = makeDiskStamp(32);
    const auto raster = BrushFootprint::rasterizeStamp(disk, 16);
    ASSERT_FALSE(raster.empty());
    EXPECT_GT(raster.alpha[8 * raster.size + 8], 0.9f);
    EXPECT_LT(raster.alpha[0], 0.05f);
}

TEST(BrushFootprintTest, TilingSamplesRepeatInUvSpace)
{
    BrushFootprint::ImageRgba tile;
    tile.width = 2;
    tile.height = 2;
    tile.pixels = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255,
    };
    BrushFootprint::TilingSettings settings;
    settings.scale = 2.0f;
    const auto a = BrushFootprint::sampleTiling(tile, 0.1f, 0.1f, settings);
    const auto b = BrushFootprint::sampleTiling(tile, 0.6f, 0.1f, settings);
    EXPECT_NEAR(a.r, b.r, 0.05f);
}

TEST(BrushFootprintTest, PaintStampWritesPixels)
{
    TexturePaintBuffer buf(64, 64);
    const auto disk = makeDiskStamp(32);
    const auto raster = BrushFootprint::rasterizeStamp(disk, 16);
    const int changed = buf.paintStamp(
        Ogre::Vector2(0.5f, 0.5f), 0.1f, raster, 0.0f,
        [](float, float) { return Ogre::ColourValue(1, 0, 0, 1); }, 1.0f);
    EXPECT_GT(changed, 0);
    const auto center = buf.pixel(32, 32);
    EXPECT_GT(center.r, 0.5f);
}

TEST(BrushAssetLibraryTest, BundledCatalogCounts)
{
    EXPECT_EQ(BrushAssetLibrary::bundledStampNames().size(), 6u);
    EXPECT_EQ(BrushAssetLibrary::bundledTilingNames().size(), 4u);
}

TEST(BrushAssetLibraryTest, BundledAssetsResolveOnDisk)
{
    for (const auto& name : BrushAssetLibrary::bundledStampNames()) {
        const std::string path = BrushAssetLibrary::resolvePath(
            name, BrushAssetLibrary::AssetKind::Stamp);
        if (path.empty())
            GTEST_SKIP() << "Bundled stamp media not beside test binary";
        EXPECT_TRUE(QFile::exists(QString::fromUtf8(path.c_str()))) << name;
    }
    for (const auto& name : BrushAssetLibrary::bundledTilingNames()) {
        const std::string path = BrushAssetLibrary::resolvePath(
            name, BrushAssetLibrary::AssetKind::Tiling);
        if (path.empty())
            GTEST_SKIP() << "Bundled tiling media not beside test binary";
        EXPECT_TRUE(QFile::exists(QString::fromUtf8(path.c_str()))) << name;
    }
}

TEST(BrushAssetLibraryTest, ImportCustomStamp)
{
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = tmp.filePath(QStringLiteral("leaf.png"));
    QImage img(16, 16, QImage::Format_RGBA8888);
    img.fill(QColor(0, 255, 0, 128));
    ASSERT_TRUE(img.save(src));

    const std::string stored = BrushAssetLibrary::importAsset(
        src.toStdString(), BrushAssetLibrary::AssetKind::Stamp, "Leaf");
    ASSERT_FALSE(stored.empty());
    const auto loaded = BrushAssetLibrary::loadImage(stored);
    EXPECT_FALSE(loaded.empty());
    EXPECT_TRUE(BrushAssetLibrary::deleteCustom("Leaf", BrushAssetLibrary::AssetKind::Stamp));
    QStandardPaths::setTestModeEnabled(false);
}
