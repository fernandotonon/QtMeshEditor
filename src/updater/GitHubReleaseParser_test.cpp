#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QString>

#include "GitHubReleaseParser.h"

namespace {

using GitHubReleaseParser::Channel;
using GitHubReleaseParser::CheckResult;
using UpdateVersion::Comparison;

QString fixturePath(const char* name)
{
    return QDir(QStringLiteral(QTMESH_UT_SOURCE_ROOT))
        .filePath(QStringLiteral("tests/fixtures/updater/%1").arg(QString::fromUtf8(name)));
}

QByteArray loadFixture(const char* name)
{
    QFile file(fixturePath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

} // namespace

TEST(GitHubReleaseParser, ParsesReleaseFieldsAndAssets)
{
    const QByteArray json = loadFixture("github-releases-sample.json");
    ASSERT_FALSE(json.isEmpty());

    const QJsonDocument doc = QJsonDocument::fromJson(json);
    ASSERT_TRUE(doc.isArray());
    const auto release = GitHubReleaseParser::parseReleaseObject(doc.array().at(2).toObject());

    EXPECT_TRUE(release.valid);
    EXPECT_EQ(release.tagName, QStringLiteral("3.5.3"));
    EXPECT_EQ(release.name, QStringLiteral("3.5.3"));
    EXPECT_EQ(release.body, QStringLiteral("Stable release notes."));
    EXPECT_EQ(release.htmlUrl,
              QStringLiteral("https://github.com/fernandotonon/QtMeshEditor/releases/tag/3.5.3"));
    EXPECT_FALSE(release.prerelease);
    ASSERT_EQ(release.assets.size(), 2);
    EXPECT_EQ(release.assets.at(0).name,
              QStringLiteral("QtMeshEditor-3.5.3-bin-Windows.zip"));
}

TEST(GitHubReleaseParser, StableChannelSkipsPrereleases)
{
    const QByteArray json = loadFixture("github-releases-sample.json");
    const CheckResult result =
        GitHubReleaseParser::pickLatestForChannel(json, Channel::Stable, QStringLiteral("3.5.2"));

    ASSERT_TRUE(result.parseOk) << result.errorMessage.toStdString();
    EXPECT_EQ(result.release.tagName, QStringLiteral("3.5.3"));
    EXPECT_EQ(result.comparison, Comparison::Older);
}

TEST(GitHubReleaseParser, StableChannelReportsUpToDate)
{
    const QByteArray json = loadFixture("github-releases-sample.json");
    const CheckResult result =
        GitHubReleaseParser::pickLatestForChannel(json, Channel::Stable, QStringLiteral("3.5.3"));

    ASSERT_TRUE(result.parseOk);
    EXPECT_EQ(result.comparison, Comparison::Same);
}

TEST(GitHubReleaseParser, BetaChannelPrefersPrerelease)
{
    const QByteArray json = loadFixture("github-releases-sample.json");
    const CheckResult result =
        GitHubReleaseParser::pickLatestForChannel(json, Channel::Beta, QStringLiteral("3.5.3"));

    ASSERT_TRUE(result.parseOk);
    EXPECT_EQ(result.release.tagName, QStringLiteral("3.6.0-beta.2"));
    EXPECT_TRUE(result.release.prerelease);
    EXPECT_EQ(result.comparison, Comparison::Older);
}

TEST(GitHubReleaseParser, BetaChannelDetectsPrereleaseBump)
{
    const QByteArray json = loadFixture("github-releases-sample.json");
    const CheckResult result = GitHubReleaseParser::pickLatestForChannel(
        json, Channel::Beta, QStringLiteral("3.6.0-beta.1"));

    ASSERT_TRUE(result.parseOk);
    EXPECT_EQ(result.release.tagName, QStringLiteral("3.6.0-beta.2"));
    EXPECT_EQ(result.comparison, Comparison::Older);
}

TEST(GitHubReleaseParser, RejectsNonArrayPayload)
{
    const CheckResult result = GitHubReleaseParser::pickLatestForChannel(
        QByteArray("{\"tag_name\":\"3.5.3\"}"), Channel::Stable, QStringLiteral("3.5.0"));

    EXPECT_FALSE(result.parseOk);
    EXPECT_FALSE(result.errorMessage.isEmpty());
}
