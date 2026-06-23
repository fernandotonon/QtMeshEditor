#ifndef CLOUD_DEEP_LINK_H
#define CLOUD_DEEP_LINK_H

#include <QString>
#include <QUrl>

struct CloudDeepLinkTarget {
    QString ownerSlug;
    QString projectSlug;

    bool isValid() const { return !ownerSlug.isEmpty() && !projectSlug.isEmpty(); }
};

/// Parses `qtmesh://cloud/open?owner=…&project=…` launch URLs from the website.
class CloudDeepLink {
public:
    CloudDeepLink() = delete;

    static constexpr QLatin1StringView kLaunchTokenPrefix{"qtmesh-cloud://"};

    static bool parseUrl(const QString& urlString, CloudDeepLinkTarget* out);
    static bool parseUrl(const QUrl& url, CloudDeepLinkTarget* out);

    static QString encodeLaunchToken(const QString& ownerSlug, const QString& projectSlug);
    static bool decodeLaunchToken(const QString& token, CloudDeepLinkTarget* out);
};

#endif // CLOUD_DEEP_LINK_H
