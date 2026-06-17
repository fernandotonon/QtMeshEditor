#ifndef GITHUBRELEASEPARSER_H
#define GITHUBRELEASEPARSER_H

#include "UpdateVersion.h"

#include <QMetaType>
#include <QJsonObject>
#include <QList>
#include <QString>

/**
 * @brief Pure-data parser for GitHub Releases API JSON (#441).
 *
 * Keeps network and UI out of the parsing path so unit tests can feed
 * canned fixtures without a GL context or live HTTP.
 */
namespace GitHubReleaseParser {

enum class Channel {
    Stable,
    Beta,
};

struct ReleaseAsset {
    QString name;
    QString browserDownloadUrl;
    qint64 size = 0;
};

struct ParsedRelease {
    QString tagName;
    QString name;
    QString body;
    QString htmlUrl;
    bool prerelease = false;
    QString publishedAt;
    QList<ReleaseAsset> assets;
    bool valid = false;
};

struct CheckResult {
    bool parseOk = false;
    ParsedRelease release;
    UpdateVersion::Comparison comparison = UpdateVersion::Comparison::Invalid;
    QString errorMessage;
};

/// Parse one release object from the `/releases` array.
ParsedRelease parseReleaseObject(const QJsonObject& obj);

/**
 * @brief Pick the newest release for @p channel and compare against @p localVersion.
 *
 * Stable channel skips `prerelease: true` entries. Beta channel prefers the
 * first prerelease in API order (newest-first), falling back to the newest
 * stable release when no prerelease exists.
 */
CheckResult pickLatestForChannel(const QByteArray& releasesJson,
                                 Channel channel,
                                 const QString& localVersion);

QString channelToString(Channel channel);
Channel channelFromString(const QString& slug, bool* ok = nullptr);

} // namespace GitHubReleaseParser

Q_DECLARE_METATYPE(GitHubReleaseParser::Channel)

#endif // GITHUBRELEASEPARSER_H
