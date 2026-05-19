#include "PsxDiscResolver.h"

#include <QTemporaryDir>
#include <QFile>

#include <gtest/gtest.h>

TEST(PsxDiscResolver, NeedsCueWrapperForIso)
{
    EXPECT_TRUE(PsxDiscResolver::needsCueWrapper(QStringLiteral("/games/test.iso")));
    EXPECT_TRUE(PsxDiscResolver::needsCueWrapper(QStringLiteral("/games/Crash.iso01.iso")));
    EXPECT_FALSE(PsxDiscResolver::needsCueWrapper(QStringLiteral("/games/disc.cue")));
}

TEST(PsxDiscResolver, ResolveMissingFails)
{
    const PsxDiscResolveResult result = PsxDiscResolver::resolve(QStringLiteral("/no/such/disc.iso"));
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.errorMessage.isEmpty());
}

TEST(PsxDiscResolver, GeneratesCueForIso)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());

    const QString isoPath = temp.filePath(QStringLiteral("game.iso"));
    {
        QFile iso(isoPath);
        ASSERT_TRUE(iso.open(QIODevice::WriteOnly));
        QByteArray sector(2048, '\0');
        sector[0] = '\x01';
        sector[1] = 'C';
        sector[2] = 'D';
        sector[3] = '0';
        sector[4] = '0';
        sector[5] = '1';
        for (int i = 0; i < 17; ++i)
            ASSERT_EQ(iso.write(sector), sector.size());
    }

    const PsxDiscResolveResult result = PsxDiscResolver::resolve(isoPath);
    ASSERT_TRUE(result.ok) << result.errorMessage.toUtf8().constData();
    EXPECT_TRUE(result.loadPath.endsWith(QStringLiteral("wrapper.cue")));

    QFile cue(result.loadPath);
    ASSERT_TRUE(cue.open(QIODevice::ReadOnly));
    const QByteArray text = cue.readAll();
    EXPECT_TRUE(text.contains("MODE1/2048"));
    EXPECT_TRUE(text.contains("game.iso"));
    EXPECT_TRUE(text.contains(QFileInfo(isoPath).absoluteFilePath().toUtf8()));
}

TEST(PsxDiscResolver, AcceptsCueWithLowercaseFileDirective)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());

    const QString binPath = temp.filePath(QStringLiteral("disc.bin"));
    {
        QFile bin(binPath);
        ASSERT_TRUE(bin.open(QIODevice::WriteOnly));
        bin.write(QByteArray(2048, '\0'));
    }

    const QString cuePath = temp.filePath(QStringLiteral("disc.cue"));
    QFile cue(cuePath);
    ASSERT_TRUE(cue.open(QIODevice::WriteOnly));
    cue.write("rem generated\nfile \"disc.bin\" binary\n  track 01 mode2/2352\n    index 01 00:00:00\n");
    cue.close();

    const PsxDiscResolveResult result = PsxDiscResolver::resolve(cuePath);
    ASSERT_TRUE(result.ok) << result.errorMessage.toUtf8().constData();
}

TEST(PsxDiscResolver, AcceptsCueWithLeadingRemLines)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());

    const QString binPath = temp.filePath(QStringLiteral("disc.bin"));
    {
        QFile bin(binPath);
        ASSERT_TRUE(bin.open(QIODevice::WriteOnly));
        bin.write(QByteArray(2048, '\0'));
    }

    const QString cuePath = temp.filePath(QStringLiteral("disc.cue"));
    QFile cue(cuePath);
    ASSERT_TRUE(cue.open(QIODevice::WriteOnly));
    cue.write("REM Redump metadata\nTITLE \"Game\"\nFILE \"disc.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
    cue.close();

    const PsxDiscResolveResult result = PsxDiscResolver::resolve(cuePath);
    ASSERT_TRUE(result.ok) << result.errorMessage.toUtf8().constData();
    EXPECT_EQ(result.loadPath, cuePath);
}

TEST(PsxDiscResolver, PassesThroughCue)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());

    const QString binPath = temp.filePath(QStringLiteral("disc.bin"));
    {
        QFile bin(binPath);
        ASSERT_TRUE(bin.open(QIODevice::WriteOnly));
        bin.write(QByteArray(2048, '\0'));
    }

    const QString cuePath = temp.filePath(QStringLiteral("disc.cue"));
    QFile cue(cuePath);
    ASSERT_TRUE(cue.open(QIODevice::WriteOnly));
    cue.write("FILE \"disc.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
    cue.close();

    const PsxDiscResolveResult result = PsxDiscResolver::resolve(cuePath);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.loadPath, cuePath);
}
