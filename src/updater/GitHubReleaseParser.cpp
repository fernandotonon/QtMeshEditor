#include "GitHubReleaseParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

namespace GitHubReleaseParser {

namespace {

ReleaseAsset parseAssetObject(const QJsonObject& obj)
{
    ReleaseAsset asset;
    asset.name = obj.value(QStringLiteral("name")).toString();
    asset.browserDownloadUrl = obj.value(QStringLiteral("browser_download_url")).toString();
    asset.size = obj.value(QStringLiteral("size")).toDouble();
    return asset;
}

bool releaseMatchesChannel(const ParsedRelease& release, Channel channel)
{
    switch (channel) {
    case Channel::Stable:
        return release.valid && !release.prerelease;
    case Channel::Beta:
        return release.valid && release.prerelease;
    }
    return false;
}

} // namespace

ParsedRelease parseReleaseObject(const QJsonObject& obj)
{
    ParsedRelease release;
    release.tagName = obj.value(QStringLiteral("tag_name")).toString();
    release.name = obj.value(QStringLiteral("name")).toString();
    release.body = obj.value(QStringLiteral("body")).toString();
    release.htmlUrl = obj.value(QStringLiteral("html_url")).toString();
    release.prerelease = obj.value(QStringLiteral("prerelease")).toBool(false);
    release.publishedAt = obj.value(QStringLiteral("published_at")).toString();

    const QJsonArray assets = obj.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue& value : assets) {
        if (!value.isObject()) {
            continue;
        }
        release.assets.append(parseAssetObject(value.toObject()));
    }

    release.valid = !release.tagName.isEmpty() && !release.htmlUrl.isEmpty();
    return release;
}

CheckResult pickLatestForChannel(const QByteArray& releasesJson,
                                 Channel channel,
                                 const QString& localVersion)
{
    CheckResult result;

    const QJsonDocument doc = QJsonDocument::fromJson(releasesJson);
    if (!doc.isArray()) {
        result.errorMessage = QStringLiteral("GitHub releases response is not a JSON array");
        return result;
    }

    const QJsonArray releases = doc.array();
    ParsedRelease chosen;
    ParsedRelease stableFallback;

    for (const QJsonValue& value : releases) {
        if (!value.isObject()) {
            continue;
        }
        const ParsedRelease parsed = parseReleaseObject(value.toObject());
        if (!parsed.valid) {
            continue;
        }

        if (channel == Channel::Stable && !parsed.prerelease) {
            chosen = parsed;
            break;
        }

        if (channel == Channel::Beta) {
            if (parsed.prerelease && !chosen.valid) {
                chosen = parsed;
            }
            if (!parsed.prerelease && !stableFallback.valid) {
                stableFallback = parsed;
            }
        }
    }

    if (channel == Channel::Beta && !chosen.valid && stableFallback.valid) {
        chosen = stableFallback;
    }

    if (!chosen.valid) {
        result.errorMessage = QStringLiteral("No matching release found for channel");
        return result;
    }

    result.parseOk = true;
    result.release = chosen;
    const auto compareMode = channel == Channel::Beta ? UpdateVersion::CompareMode::WithPrerelease
                                                      : UpdateVersion::CompareMode::ReleaseOnly;
    result.comparison = UpdateVersion::compare(localVersion, chosen.tagName, compareMode);
    return result;
}

QString channelToString(Channel channel)
{
    switch (channel) {
    case Channel::Stable:
        return QStringLiteral("stable");
    case Channel::Beta:
        return QStringLiteral("beta");
    }
    return QStringLiteral("stable");
}

Channel channelFromString(const QString& slug, bool* ok)
{
    const QString normalized = slug.trimmed().toLower();
    if (normalized == QStringLiteral("stable") || normalized.isEmpty()) {
        if (ok) {
            *ok = true;
        }
        return Channel::Stable;
    }
    if (normalized == QStringLiteral("beta")) {
        if (ok) {
            *ok = true;
        }
        return Channel::Beta;
    }
    if (ok) {
        *ok = false;
    }
    return Channel::Stable;
}

} // namespace GitHubReleaseParser
