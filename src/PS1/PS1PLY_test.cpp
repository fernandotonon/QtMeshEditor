#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "PS1/PS1PLY.h"

TEST(PS1PLY, IsPsyqPlyFile_TrueWhenHeaderPresent)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("m.ply"));
    const QByteArray data =
        "# comment\n"
        "@PLY940102\n"
        "3 4 1\n";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write(data), data.size());
    f.close();
    EXPECT_TRUE(PS1PLY::isPsyqPlyFile(path));
}

// Some exporters (e.g. RSD toolchains) prefix with "#PLY Mesh Data" before @PLY940102.
TEST(PS1PLY, IsPsyqPlyFile_TrueWithPlyMeshDataCommentPrefix)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("planetish.ply"));
    const QByteArray data =
        "#PLY Mesh Data\n"
        "@PLY940102\n"
        "3 3 1\n"
        "0 0 0\n"
        "1 0 0\n"
        "0 1 0\n"
        "0 0 1\n"
        "0 0 1\n"
        "0 0 1\n"
        "0 0 2 1 0 0 2 1 0\n";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write(data), data.size());
    f.close();
    EXPECT_TRUE(PS1PLY::isPsyqPlyFile(path));
}

TEST(PS1PLY, IsPsyqPlyFile_FalseForStanfordPly)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("stanford.ply"));
    const QByteArray data = "ply\nformat ascii 1.0\n";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write(data), data.size());
    f.close();
    EXPECT_FALSE(PS1PLY::isPsyqPlyFile(path));
}
