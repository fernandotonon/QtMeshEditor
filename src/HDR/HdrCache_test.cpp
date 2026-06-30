#include <gtest/gtest.h>

#include "HDR/HdrCache.h"
#include "HDR/HdrIblPrecompute.h"

#include <QTemporaryDir>
#include <QFile>
#include <QStandardPaths>

namespace {

HdrIbl::IblBakeResult makeTinyBakeResult()
{
    HdrIbl::IblBakeResult result;
    result.irradiance.faceSize = HdrIbl::kIrradianceFaceSize;
    const size_t irrPixels = static_cast<size_t>(HdrIbl::kIrradianceFaceSize)
                             * static_cast<size_t>(HdrIbl::kIrradianceFaceSize) * 3u;
    for (auto& face : result.irradiance.faces)
        face.assign(irrPixels, 0.25f);

    result.prefilter.mips.resize(static_cast<size_t>(HdrIbl::kPrefilterMipCount));
    for (int mip = 0; mip < HdrIbl::kPrefilterMipCount; ++mip) {
        const int faceSize = std::max(1, HdrIbl::kPrefilterBaseFaceSize >> mip);
        result.prefilter.mips[static_cast<size_t>(mip)].faceSize = faceSize;
        result.prefilter.mips[static_cast<size_t>(mip)].faces.faceSize = faceSize;
        const size_t pixels = static_cast<size_t>(faceSize) * static_cast<size_t>(faceSize) * 3u;
        for (auto& face : result.prefilter.mips[static_cast<size_t>(mip)].faces.faces)
            face.assign(pixels, 0.5f);
    }

    result.brdfLut.size = HdrIbl::kBrdfLutSize;
    result.brdfLut.rg.assign(static_cast<size_t>(HdrIbl::kBrdfLutSize)
                                 * static_cast<size_t>(HdrIbl::kBrdfLutSize) * 2u,
                             0.1f);
    return result;
}

} // namespace

TEST(HdrCacheTest, SaveLoadRoundTrip)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qputenv("XDG_DATA_HOME", tmp.path().toUtf8());

    const QString cacheKey =
        QStringLiteral("abc123deadbeefabc123deadbeefabc123de");
    const HdrIbl::IblBakeResult original = makeTinyBakeResult();

    QString saveError;
    ASSERT_TRUE(HdrCache::save(cacheKey, original, saveError)) << saveError.toStdString();
    EXPECT_TRUE(HdrCache::isValid(cacheKey));

    HdrIbl::IblBakeResult loaded;
    QString loadError;
    ASSERT_TRUE(HdrCache::load(cacheKey, loaded, loadError)) << loadError.toStdString();
    EXPECT_EQ(loaded.irradiance.faceSize, original.irradiance.faceSize);
    EXPECT_EQ(loaded.prefilter.mips.size(), original.prefilter.mips.size());
    EXPECT_EQ(loaded.brdfLut.size, original.brdfLut.size);
    EXPECT_NEAR(loaded.irradiance.faces[0][0], 0.25f, 1e-5f);
}

TEST(HdrCacheTest, TruncatedFile_IsRejectedGracefully)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qputenv("XDG_DATA_HOME", tmp.path().toUtf8());

    const QString cacheKey =
        QStringLiteral("c0ffee00c0ffee00c0ffee00c0ffee00c0ffee00");
    const HdrIbl::IblBakeResult original = makeTinyBakeResult();
    QString saveError;
    ASSERT_TRUE(HdrCache::save(cacheKey, original, saveError));

    QFile truncated(HdrCache::entryDirectory(cacheKey) + QStringLiteral("/irradiance.bin"));
    ASSERT_TRUE(truncated.open(QIODevice::WriteOnly | QIODevice::Truncate));
    truncated.write("bad");
    truncated.close();

    HdrIbl::IblBakeResult loaded;
    QString loadError;
    EXPECT_FALSE(HdrCache::load(cacheKey, loaded, loadError));
    EXPECT_FALSE(loadError.isEmpty());
}
