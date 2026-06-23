#include "CloudUploadPlanner.h"

#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <QFile>

TEST(CloudUploadPlanner, MakesStableSlug)
{
    EXPECT_EQ(CloudUploadPlanner::makeProjectSlug(QStringLiteral(" My PS1 Asset Pack!! ")),
              QStringLiteral("my-ps1-asset-pack"));
    EXPECT_EQ(CloudUploadPlanner::makeProjectSlug(QStringLiteral("###"), QStringLiteral("Fallback Name")),
              QStringLiteral("fallback-name"));
}

TEST(CloudUploadPlanner, TruncatesSlugWithoutTrailingDash)
{
    const QString slug = CloudUploadPlanner::makeProjectSlug(
        QStringLiteral("abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz"));
    EXPECT_LE(slug.size(), 64);
    EXPECT_FALSE(slug.endsWith(QLatin1Char('-')));
}

TEST(CloudUploadPlanner, CollisionSuffixStaysWithinLimit)
{
    const QString longName = QStringLiteral(
        "abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz");
    const QString retrySlug = CloudUploadPlanner::makeProjectSlug(
        QStringLiteral("%1-%2").arg(longName, QStringLiteral("20260529140530")));
    EXPECT_LE(retrySlug.size(), 64);
    EXPECT_FALSE(retrySlug.endsWith(QLatin1Char('-')));
}

TEST(CloudUploadPlanner, InfersKnownAssetRoles)
{
    EXPECT_EQ(CloudUploadPlanner::inferAssetRole(QStringLiteral("model.FBX")), QStringLiteral("model"));
    EXPECT_EQ(CloudUploadPlanner::inferAssetRole(QStringLiteral("rig.skeleton")), QStringLiteral("skeleton"));
    EXPECT_EQ(CloudUploadPlanner::inferAssetRole(QStringLiteral("walk.anim")), QStringLiteral("animation"));
    EXPECT_EQ(CloudUploadPlanner::inferAssetRole(QStringLiteral("wall.TIM")), QStringLiteral("texture"));
    EXPECT_EQ(CloudUploadPlanner::inferAssetRole(QStringLiteral("stage.RSD")), QStringLiteral("sidecar"));
    EXPECT_EQ(CloudUploadPlanner::inferAssetRole(QStringLiteral("metadata.json")), QStringLiteral("metadata"));
    EXPECT_EQ(CloudUploadPlanner::inferAssetRole(QStringLiteral("notes.txt")), QStringLiteral("file"));
}

TEST(CloudUploadPlanner, BuildsDescriptorsFromPaths)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("cube.obj"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("data"), 4);
    file.close();

    const auto descriptors = CloudUploadPlanner::buildAssetFileDescriptors({path});
    ASSERT_EQ(descriptors.size(), 1);
    EXPECT_EQ(descriptors.first().path, path);
    EXPECT_EQ(descriptors.first().uploadName, QStringLiteral("cube.obj"));
    EXPECT_EQ(descriptors.first().role, QStringLiteral("model"));
    EXPECT_EQ(descriptors.first().sizeBytes, 4);
    EXPECT_FALSE(descriptors.first().mimeType.isEmpty());
}

TEST(CloudUploadPlanner, MainFileAlwaysIncludedEvenWhenExcluded)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString main = dir.filePath(QStringLiteral("hero.fbx"));
    QFile file(main);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("x");

    const QStringList selected = CloudUploadPlanner::selectedPathsForUpload(
        main, {}, {QStringLiteral("**/*")});
    ASSERT_EQ(selected.size(), 1);
    EXPECT_EQ(selected.first(), QFileInfo(main).absoluteFilePath());
}
