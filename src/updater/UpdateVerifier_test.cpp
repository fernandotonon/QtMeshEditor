#include <gtest/gtest.h>

#include "UpdateVerifier.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QString>

namespace {

QString fixturePath(const char* name)
{
    return QDir(QStringLiteral(QTMESH_UT_SOURCE_ROOT))
        .filePath(QStringLiteral("tests/fixtures/updater/%1").arg(QString::fromUtf8(name)));
}

} // namespace

TEST(UpdateVerifier, ComputesSha256Hex)
{
    const QString path = fixturePath("release-3.5.3-readme.md");
    QString error;
    const QString digest = UpdateVerifier::sha256HexOfFile(path, &error);
    ASSERT_FALSE(digest.isEmpty()) << error.toStdString();
    EXPECT_EQ(digest.size(), 64);
    EXPECT_TRUE(UpdateVerifier::verifySha256Hex(path, digest, &error));
}

TEST(UpdateVerifier, RejectsSha256Mismatch)
{
    const QString path = fixturePath("release-3.5.3-readme.md");
    QString error;
    EXPECT_FALSE(UpdateVerifier::verifySha256Hex(path, QStringLiteral("00"), &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(UpdateVerifier, ReadsSha256ManifestEntry)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());

    const QString artifact = temp.path() + QStringLiteral("/payload.bin");
    ASSERT_TRUE(QFile::copy(fixturePath("release-3.5.3-readme.md"), artifact));

    const QString manifest = temp.path() + QStringLiteral("/SHA256SUMS");
    QFile manifestFile(manifest);
    ASSERT_TRUE(manifestFile.open(QIODevice::WriteOnly | QIODevice::Text));

    QString error;
    const QString digest = UpdateVerifier::sha256HexOfFile(artifact, &error);
    ASSERT_FALSE(digest.isEmpty()) << error.toStdString();
    manifestFile.write(QStringLiteral("%1  payload.bin\n").arg(digest).toUtf8());
    manifestFile.close();

    const UpdateVerifier::Outcome out =
        UpdateVerifier::verifySha256FromManifest(artifact, manifest, QStringLiteral("payload.bin"));
    EXPECT_TRUE(out.ok) << out.errorMessage.toStdString();
}
