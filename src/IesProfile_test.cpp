#include <QRegularExpression>

#include <gtest/gtest.h>

#include "IesProfile.h"

#include <QFile>
#include <QTemporaryDir>

namespace
{

QByteArray sampleIesBytes()
{
    return QByteArray(
        "IESNA:LM-63-2002\n"
        "TILT=NONE\n"
        "1 1 1 3 1 1 1 0 0 0\n"
        "1 1 1 0 0\n"
        "0 45 90\n"
        "0\n"
        "1000 500 100\n");
}

} // namespace

TEST(IesProfileTest, SampleFixtureHasExpectedLayout)
{
    const QByteArray bytes = sampleIesBytes();
    const QList<QByteArray> raw = bytes.split('\n');
    ASSERT_GE(raw.size(), 3);
    EXPECT_TRUE(QString::fromUtf8(raw[1]).contains(QStringLiteral("TILT"), Qt::CaseInsensitive));
    const QStringList headerParts =
        QString::fromUtf8(raw[2]).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    EXPECT_EQ(headerParts.size(), 10);
}

TEST(IesProfileTest, ParsesMinimalTypeC)
{
    QString error;
    const IesProfile profile = IesProfile::parseBytes(sampleIesBytes(), &error);
    ASSERT_TRUE(profile.valid) << error.toStdString();
    EXPECT_EQ(profile.verticalAnglesDeg.size(), 3);
    EXPECT_GT(profile.maxCandela, 0.0f);
    EXPECT_GT(profile.beamAngleDeg, 0.0f);

    const QVector<float> slice = profile.polarSlice();
    ASSERT_EQ(slice.size(), 3);
    EXPECT_FLOAT_EQ(slice[0], 1.0f);
    EXPECT_LT(slice[2], slice[0]);
}

TEST(IesProfileTest, ParseFileReadsFromDisk)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("sample.ies"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(sampleIesBytes()), sampleIesBytes().size());
    file.close();

    QString error;
    const IesProfile profile = IesProfile::parseFile(path, &error);
    ASSERT_TRUE(profile.valid) << error.toStdString();
}

TEST(IesProfileTest, MissingTiltFails)
{
    QString error;
    const IesProfile profile = IesProfile::parseBytes(QByteArray("NOT AN IES FILE\n"), &error);
    EXPECT_FALSE(profile.valid);
    EXPECT_FALSE(error.isEmpty());
}

TEST(IesProfileTest, HorizontalMajorCandelaLayout)
{
    const QByteArray bytes =
        "IESNA:LM-63-2002\n"
        "TILT=NONE\n"
        "1 1 1 2 2 1 1 1 0 0 0\n"
        "1 1 1 0 0\n"
        "0 90\n"
        "0 180\n"
        "1000 2000 3000 4000\n";

    QString error;
    const IesProfile profile = IesProfile::parseBytes(bytes, &error);
    ASSERT_TRUE(profile.valid) << error.toStdString();
    ASSERT_EQ(profile.verticalAnglesDeg.size(), 2);
    ASSERT_EQ(profile.candela.size(), 4);

    const QVector<float> slice = profile.polarSlice();
    ASSERT_EQ(slice.size(), 2);
    EXPECT_FLOAT_EQ(slice[0], 0.25f);
    EXPECT_FLOAT_EQ(slice[1], 0.5f);
}
