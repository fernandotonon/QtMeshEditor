#include <gtest/gtest.h>

#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include "TextureAtlasPacker.h"

// Coverage companion to TextureAtlasPacker_test.cpp. Targets the branches
// the original suite leaves untested:
//   - pack(): negative-padding rejection guard (cpp lines 51-53)
//   - packToFile(): failure propagation when pack() fails (ok=false)
//   - packToFile(): unsupported-extension save-failure branch (cpp 194-198)
// Distinct suite name (TextureAtlasPackerCoverageTest) and distinct file
// to avoid ODR / duplicate-registration clashes with the existing suite.

using namespace TextureAtlasPacker;

namespace {

// Mirror the byte-order-safe helper from the existing suite: Format_ARGB32
// + qRgba() fill is correct on both endiannesses (RGBA8888 fill is not).
QString writeSolidPngCov(const QTemporaryDir& dir,
                         const QString& name,
                         int w, int h, QRgb colour)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(colour);
    const QString path = dir.filePath(name);
    [&]() { ASSERT_TRUE(img.save(path, "PNG")) << path.toStdString(); }();
    return path;
}

} // namespace

// --- pack(): negative padding guard -----------------------------------------

TEST(TextureAtlasPackerCoverageTest, NegativePaddingReturnsError)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;
    spec.sourcePaths << writeSolidPngCov(dir, "a.png", 8, 8, qRgba(255, 0, 0, 255));
    spec.atlasWidth = 64;
    spec.atlasHeight = 64;
    spec.padding = -1;

    AtlasResult r = pack(spec);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("Padding must be non-negative"))
        << r.error.toStdString();
    EXPECT_TRUE(r.image.isNull());
    EXPECT_TRUE(r.tiles.isEmpty());
}

TEST(TextureAtlasPackerCoverageTest, LargeNegativePaddingReturnsError)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;
    spec.sourcePaths << writeSolidPngCov(dir, "a.png", 8, 8, qRgba(0, 255, 0, 255));
    spec.atlasWidth = 128;
    spec.atlasHeight = 128;
    spec.padding = -100;

    AtlasResult r = pack(spec);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("Padding must be non-negative"))
        << r.error.toStdString();
}

// The padding guard must precede the per-image load loop: even if the source
// path is bogus, the negative-padding error should be the one reported (it is
// checked first in pack()).
TEST(TextureAtlasPackerCoverageTest, NegativePaddingTakesPrecedenceOverBadInput)
{
    AtlasSpec spec;
    spec.sourcePaths << QStringLiteral("/nonexistent/path/does-not-exist.png");
    spec.atlasWidth = 64;
    spec.atlasHeight = 64;
    spec.padding = -5;

    AtlasResult r = pack(spec);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("Padding must be non-negative"))
        << r.error.toStdString();
}

// Zero padding is valid (boundary just above the rejected range).
TEST(TextureAtlasPackerCoverageTest, ZeroPaddingIsAccepted)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;
    spec.sourcePaths << writeSolidPngCov(dir, "a.png", 8, 8, qRgba(0, 0, 255, 255));
    spec.atlasWidth = 64;
    spec.atlasHeight = 64;
    spec.padding = 0;

    AtlasResult r = pack(spec);
    EXPECT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.tiles.size(), 1);
    EXPECT_EQ(r.tiles[0].x, 0);
    EXPECT_EQ(r.tiles[0].y, 0);
}

// --- packToFile(): failure propagation when pack() fails ---------------------

TEST(TextureAtlasPackerCoverageTest, PackToFilePropagatesPackFailureEmptyInput)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;   // no source paths -> pack() fails before any save

    const QString outPath = dir.filePath("atlas.png");
    AtlasResult r = packToFile(spec, outPath);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
    // pack() failed: the file must not have been written.
    EXPECT_FALSE(QFile::exists(outPath));
}

TEST(TextureAtlasPackerCoverageTest, PackToFilePropagatesPackFailureNegativePadding)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;
    spec.sourcePaths << writeSolidPngCov(dir, "a.png", 8, 8, qRgba(255, 0, 0, 255));
    spec.atlasWidth = 64;
    spec.atlasHeight = 64;
    spec.padding = -1;   // pack() rejects -> packToFile returns early

    const QString outPath = dir.filePath("never_written.png");
    AtlasResult r = packToFile(spec, outPath);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("Padding must be non-negative"))
        << r.error.toStdString();
    EXPECT_FALSE(QFile::exists(outPath));
}

// --- packToFile(): save-failure branch (unsupported / unwritable target) -----

// An unknown/garbage extension forces QImage::save() to fail, exercising the
// cpp 194-198 branch that flips ok=false, clears the image, and sets the
// "Failed to save atlas to ..." error.
TEST(TextureAtlasPackerCoverageTest, PackToFileUnsupportedExtensionReportsSaveFailure)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;
    spec.sourcePaths << writeSolidPngCov(dir, "a.png", 8, 8, qRgba(0, 255, 0, 255));
    spec.atlasWidth = 16;
    spec.atlasHeight = 16;
    spec.padding = 0;

    // ".xyzzy" is not a format Qt's image plugins recognise -> save() fails.
    const QString outPath = dir.filePath("atlas.xyzzy");
    AtlasResult r = packToFile(spec, outPath);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("Failed to save atlas")) << r.error.toStdString();
    // The failure branch clears the image.
    EXPECT_TRUE(r.image.isNull());
    EXPECT_FALSE(QFile::exists(outPath));
}

// Writing to a path inside a directory that does not exist also makes save()
// fail even with a valid PNG extension (separate trigger for the same branch).
TEST(TextureAtlasPackerCoverageTest, PackToFileUnwritableDirReportsSaveFailure)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;
    spec.sourcePaths << writeSolidPngCov(dir, "a.png", 8, 8, qRgba(0, 0, 255, 255));
    spec.atlasWidth = 16;
    spec.atlasHeight = 16;
    spec.padding = 0;

    // A nested directory that was never created -> the PNG handler cannot
    // open the file for writing.
    const QString outPath = dir.filePath("no_such_subdir/inner/atlas.png");
    AtlasResult r = packToFile(spec, outPath);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("Failed to save atlas")) << r.error.toStdString();
    EXPECT_TRUE(r.image.isNull());
}

// Sanity: the happy path still works alongside the failure cases (guards
// against a save-format regression masking the failure-branch tests).
TEST(TextureAtlasPackerCoverageTest, PackToFileSucceedsWithValidPng)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    AtlasSpec spec;
    spec.sourcePaths << writeSolidPngCov(dir, "a.png", 8, 8, qRgba(255, 0, 0, 255));
    spec.atlasWidth = 16;
    spec.atlasHeight = 16;
    spec.padding = 0;

    const QString outPath = dir.filePath("ok_atlas.png");
    AtlasResult r = packToFile(spec, outPath);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_FALSE(r.image.isNull());
    EXPECT_TRUE(QFile::exists(outPath));
}
