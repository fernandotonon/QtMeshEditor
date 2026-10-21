#include <gtest/gtest.h>

#include "CharacterAssets.h"

#include <QDir>
#include <QTemporaryDir>

TEST(CharacterAssets, DirFollowsTheEnvironmentOverrideAndEmptyDirIsNotPresent)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray old = qgetenv("QTMESH_CHARACTER_DIR");
    qputenv("QTMESH_CHARACTER_DIR", tmp.path().toUtf8());
    EXPECT_EQ(QDir(CharacterGen::CharacterAssets::dir()).absolutePath(), QDir(tmp.path()).absolutePath());
    EXPECT_FALSE(CharacterGen::CharacterAssets::present());
    if (old.isEmpty()) qunsetenv("QTMESH_CHARACTER_DIR"); else qputenv("QTMESH_CHARACTER_DIR", old);
}

TEST(CharacterAssets, OfflineGuardReportsMissingFilesWithoutFetching)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray oldDir = qgetenv("QTMESH_CHARACTER_DIR");
    qputenv("QTMESH_CHARACTER_DIR", tmp.path().toUtf8());
    qputenv("QTMESH_CHARACTER_NO_DOWNLOAD", "1");
    QString err;
    EXPECT_FALSE(CharacterGen::CharacterAssets::ensureBlocking(&err));
    EXPECT_FALSE(err.isEmpty());
    qunsetenv("QTMESH_CHARACTER_NO_DOWNLOAD");
    if (oldDir.isEmpty()) qunsetenv("QTMESH_CHARACTER_DIR"); else qputenv("QTMESH_CHARACTER_DIR", oldDir);
}
