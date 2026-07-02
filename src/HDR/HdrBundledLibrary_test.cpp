#include "HDR/HdrBundledLibrary.h"

#include <gtest/gtest.h>

#include <QDir>

TEST(HdrBundledLibraryTest, CatalogHasFiveEntries)
{
    const QStringList names = HdrBundledLibrary::catalogFileNames();
    EXPECT_EQ(5, names.size());
    EXPECT_TRUE(names.contains(QStringLiteral("studio_neutral.hdr")));
    EXPECT_TRUE(names.contains(QStringLiteral("flat_grey.hdr")));
}

TEST(HdrBundledLibraryTest, FindByBaseNameAcceptsWithOrWithoutExtension)
{
    EXPECT_NE(nullptr, HdrBundledLibrary::findByBaseName(QStringLiteral("studio_neutral")));
    EXPECT_NE(nullptr, HdrBundledLibrary::findByBaseName(QStringLiteral("studio_neutral.hdr")));
    EXPECT_EQ(nullptr, HdrBundledLibrary::findByBaseName(QStringLiteral("not_a_real_hdri")));
}

TEST(HdrBundledLibraryTest, FlatGreyCannotBeDownloaded)
{
    QString error;
    EXPECT_FALSE(HdrBundledLibrary::downloadHdri(QStringLiteral("flat_grey"), &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(HdrBundledLibraryTest, UnknownNameFailsDownload)
{
    QString error;
    EXPECT_FALSE(HdrBundledLibrary::downloadHdri(QStringLiteral("missing_hdri"), &error));
    EXPECT_TRUE(error.contains(QStringLiteral("Unknown HDRI")));
}

TEST(HdrBundledLibraryTest, UserHdriDirectoryIsWritable)
{
    const QString dir = HdrBundledLibrary::userHdriDirectory();
    EXPECT_FALSE(dir.isEmpty());
    EXPECT_TRUE(QDir(dir).exists());
}
