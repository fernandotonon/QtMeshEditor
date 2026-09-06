#include "AppStorage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSettings>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

void clearSnapEnv()
{
    qunsetenv("SNAP");
    qunsetenv("SNAP_USER_COMMON");
    qunsetenv("SNAP_USER_DATA");
}

} // namespace

TEST(AppStorageTest, PersistentRootFallsBackToAppDataWhenNotSnap)
{
    clearSnapEnv();
    EXPECT_FALSE(AppStorage::isSnap());
    EXPECT_EQ(AppStorage::persistentRoot(), AppStorage::revisionScopedRoot());
    EXPECT_TRUE(AppStorage::aiModelsRoot().endsWith(QStringLiteral("/ai_models"))
                || AppStorage::aiModelsRoot().endsWith(QStringLiteral("\\ai_models")));
}

TEST(AppStorageTest, PersistentRootUsesSnapUserCommon)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qputenv("SNAP", "/snap/qtmesheditor/x1");
    qputenv("SNAP_USER_COMMON", tmp.path().toUtf8());

    EXPECT_TRUE(AppStorage::isSnap());
    EXPECT_EQ(AppStorage::persistentRoot(),
              QDir(tmp.path()).filePath(QStringLiteral("QtMeshEditor")));
    EXPECT_EQ(AppStorage::aiModelsRoot(),
              QDir(AppStorage::persistentRoot())
                  .filePath(QStringLiteral("ai_models")));

    clearSnapEnv();
}

TEST(AppStorageTest, MigrateWritesMarkerAndIsIdempotent)
{
    QTemporaryDir common;
    ASSERT_TRUE(common.isValid());

    qputenv("SNAP", "/snap/qtmesheditor/x1");
    qputenv("SNAP_USER_COMMON", common.path().toUtf8());
    // Distinct SNAP_USER_DATA so sibling scan has a parent; no heavy dirs.
    qputenv("SNAP_USER_DATA",
            QDir(common.path()).filePath(QStringLiteral("../rev100")).toUtf8());

    const QString destAi = AppStorage::aiModelsRoot();
    ASSERT_TRUE(QDir().mkpath(destAi));
    QFile f(QDir(destAi).filePath(QStringLiteral("marker.gguf")));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    AppStorage::migrateHeavyDataFromRevisionScopedStorage();
    const QString marker = QDir(AppStorage::persistentRoot())
                               .filePath(QStringLiteral(".snap-persistent-root"));
    EXPECT_TRUE(QFileInfo::exists(marker));
    EXPECT_TRUE(QFileInfo::exists(f.fileName()));

    // Second call: marker present + no revision heavy dirs → early return.
    AppStorage::migrateHeavyDataFromRevisionScopedStorage();
    EXPECT_TRUE(QFileInfo::exists(marker));

    clearSnapEnv();
}

TEST(AppStorageTest, HeavySubdirNamesIncludeModelsAndGamification)
{
    EXPECT_TRUE(AppStorage::heavySubdirNames().contains(QStringLiteral("ai_models")));
    EXPECT_TRUE(AppStorage::heavySubdirNames().contains(QStringLiteral("sd_models")));
    EXPECT_TRUE(AppStorage::heavySubdirNames().contains(QStringLiteral("models")));
    EXPECT_TRUE(AppStorage::heavySubdirNames().contains(QStringLiteral("gamification")));
}

TEST(AppStorageTest, RetargetsLegacyModelsDirectorySettings)
{
    ASSERT_NE(QCoreApplication::instance(), nullptr);

    QTemporaryDir snapRoot;
    ASSERT_TRUE(snapRoot.isValid());
    const QString revData = QDir(snapRoot.path()).filePath(QStringLiteral("100"));
    const QString common = QDir(snapRoot.path()).filePath(QStringLiteral("common"));
    ASSERT_TRUE(QDir().mkpath(revData));
    ASSERT_TRUE(QDir().mkpath(common));

    qputenv("SNAP", "/snap/qtmesheditor/x1");
    qputenv("SNAP_USER_COMMON", common.toUtf8());
    qputenv("SNAP_USER_DATA", revData.toUtf8());

    const QString legacyLlm =
        QDir(revData).filePath(
            QStringLiteral(".local/share/QtMeshEditor/QtMeshEditor/models"));
    const QString legacySd =
        QDir(revData).filePath(
            QStringLiteral(".local/share/QtMeshEditor/QtMeshEditor/sd_models"));

    QSettings settings;
    settings.setValue(QStringLiteral("LLM/modelsDirectory"), legacyLlm);
    settings.setValue(QStringLiteral("StableDiffusion/modelsDirectory"), legacySd);
    settings.sync();

    AppStorage::migrateHeavyDataFromRevisionScopedStorage();

    settings.sync();
    EXPECT_EQ(settings.value(QStringLiteral("LLM/modelsDirectory")).toString(),
              AppStorage::llmModelsRoot());
    EXPECT_EQ(settings.value(QStringLiteral("StableDiffusion/modelsDirectory")).toString(),
              AppStorage::sdModelsRoot());

    // Explicit custom outside snap tree stays put.
    const QString custom = QStringLiteral("/opt/custom/sd_models");
    settings.setValue(QStringLiteral("LLM/modelsDirectory"), custom);
    settings.sync();
    AppStorage::migrateHeavyDataFromRevisionScopedStorage();
    settings.sync();
    EXPECT_EQ(settings.value(QStringLiteral("LLM/modelsDirectory")).toString(),
              custom);

    clearSnapEnv();
}

TEST(AppStorageTest, MigrateRenamesFromRevisionWhenDistinct)
{
    QTemporaryDir common;
    QTemporaryDir revision;
    ASSERT_TRUE(common.isValid());
    ASSERT_TRUE(revision.isValid());

    const QString srcAi =
        QDir(revision.path()).filePath(QStringLiteral("ai_models"));
    ASSERT_TRUE(QDir().mkpath(srcAi));
    QFile srcFile(QDir(srcAi).filePath(QStringLiteral("weights.gguf")));
    ASSERT_TRUE(srcFile.open(QIODevice::WriteOnly));
    srcFile.write("gguf");
    srcFile.close();

    const QString dstRoot =
        QDir(common.path()).filePath(QStringLiteral("QtMeshEditor"));
    const QString dstAi = QDir(dstRoot).filePath(QStringLiteral("ai_models"));
    ASSERT_TRUE(QDir().mkpath(dstRoot));
    ASSERT_TRUE(QFile::rename(srcAi, dstAi));
    EXPECT_TRUE(QFileInfo::exists(
        QDir(dstAi).filePath(QStringLiteral("weights.gguf"))));
    EXPECT_FALSE(QDir(srcAi).exists());

    EXPECT_TRUE(AppStorage::heavySubdirNames().contains(
        QStringLiteral("ai_models")));
}
