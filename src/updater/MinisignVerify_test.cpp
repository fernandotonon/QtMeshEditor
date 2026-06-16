#include <gtest/gtest.h>

#include "MinisignVerify.h"

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

// Public half of tests/fixtures/updater/minisign-test.key (secret key is NOT committed).
constexpr const char* kTestPublicKeyBase64 =
    "RWT6vKVd359GZ2DxI4QZgc51byx/fJXWLFmbSby/kTnRsIZfwj5gkI2A";

} // namespace

#if defined(Q_OS_LINUX)

TEST(MinisignVerify, AcceptsSignedReleaseReadmeFixture)
{
    const MinisignVerify::Outcome out = MinisignVerify::verifyFile(
        fixturePath("release-3.5.3-readme.md"),
        fixturePath("release-3.5.3-readme.md.minisig"),
        QString::fromUtf8(kTestPublicKeyBase64));

    ASSERT_EQ(out.result, MinisignVerify::Result::Ok) << out.errorMessage.toStdString();
    EXPECT_FALSE(out.trustedComment.isEmpty());
}

TEST(MinisignVerify, RejectsTamperedPayload)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());

    const QString tampered = temp.path() + QStringLiteral("/tampered.md");
    ASSERT_TRUE(QFile::copy(fixturePath("release-3.5.3-readme.md"), tampered));

    QFile file(tampered);
    ASSERT_TRUE(file.open(QIODevice::ReadWrite));
    file.seek(0);
    const char first = file.read(1).at(0);
    file.seek(0);
    file.write(QByteArray(1, static_cast<char>(first ^ 0x01)));
    file.close();

    const MinisignVerify::Outcome out = MinisignVerify::verifyFile(
        tampered,
        fixturePath("release-3.5.3-readme.md.minisig"),
        QString::fromUtf8(kTestPublicKeyBase64));

    EXPECT_EQ(out.result, MinisignVerify::Result::FileSignatureFailed);
}

TEST(MinisignVerify, RejectsWrongPublicKey)
{
    const MinisignVerify::Outcome out = MinisignVerify::verifyFile(
        fixturePath("release-3.5.3-readme.md"),
        fixturePath("release-3.5.3-readme.md.minisig"),
        QStringLiteral("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="));

    EXPECT_NE(out.result, MinisignVerify::Result::Ok);
    EXPECT_EQ(out.result, MinisignVerify::Result::InvalidPublicKey);
}

#else

TEST(MinisignVerify, UnsupportedOnWindowsInSpike)
{
    const MinisignVerify::Outcome out = MinisignVerify::verifyFile(
        fixturePath("release-3.5.3-readme.md"),
        fixturePath("release-3.5.3-readme.md.minisig"),
        QString::fromUtf8(kTestPublicKeyBase64));

    EXPECT_EQ(out.result, MinisignVerify::Result::Unsupported);
}

#endif
