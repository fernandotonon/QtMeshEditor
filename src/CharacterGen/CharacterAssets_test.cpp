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

#include "CharacterController.h"

TEST(CharacterController, ShapeLabelsAndSeedCoefficients)
{
    CharacterController* c = CharacterController::instance();
    EXPECT_FALSE(c->shapeLabel(0).isEmpty());
    EXPECT_NE(c->shapeLabel(0), c->shapeLabel(20));
    EXPECT_FALSE(c->shapeHint(30).isEmpty());
    EXPECT_EQ(c->shapeLabel(99), QStringLiteral("Shape 100"));   // out of range: a plain numbered name, never a crash
    const QVariantList a = c->seedBody(5, 1.0), b = c->seedBody(5, 1.0), z = c->seedBody(-1, 1.0);
    ASSERT_EQ(a.size(), 45);
    EXPECT_EQ(a, b) << "a seed must reproduce";
    for (const QVariant& v : z) EXPECT_EQ(v.toDouble(), 0.0);
    double sum = 0;
    for (const QVariant& v : a) sum += std::fabs(v.toDouble());
    EXPECT_GT(sum, 0.1);
    EXPECT_FALSE(c->hasCharacter());   // nothing generated in this process
    CharacterController::kill();
}
