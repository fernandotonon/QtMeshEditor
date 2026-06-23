#include "CloudDeepLink.h"

#include <QUrlQuery>

bool CloudDeepLink::parseUrl(const QString& urlString, CloudDeepLinkTarget* out)
{
    if (!out)
        return false;
    *out = {};
    return parseUrl(QUrl(urlString), out);
}

bool CloudDeepLink::parseUrl(const QUrl& url, CloudDeepLinkTarget* out)
{
    if (!out)
        return false;
    *out = {};

    if (!url.isValid() || url.scheme().compare(QLatin1String("qtmesh"), Qt::CaseInsensitive) != 0)
        return false;
    if (url.host().compare(QLatin1String("cloud"), Qt::CaseInsensitive) != 0)
        return false;

    const QString path = url.path();
    if (!path.isEmpty() && path != QLatin1String("/") && path != QLatin1String("/open"))
        return false;

    const QUrlQuery query(url);
    out->ownerSlug = query.queryItemValue(QStringLiteral("owner")).trimmed();
    out->projectSlug = query.queryItemValue(QStringLiteral("project")).trimmed();
    return out->isValid();
}

QString CloudDeepLink::encodeLaunchToken(const QString& ownerSlug, const QString& projectSlug)
{
    const QString owner = ownerSlug.trimmed();
    const QString project = projectSlug.trimmed();
    if (owner.isEmpty() || project.isEmpty())
        return {};
    return QString(kLaunchTokenPrefix) + owner + QLatin1Char('/') + project;
}

bool CloudDeepLink::decodeLaunchToken(const QString& token, CloudDeepLinkTarget* out)
{
    if (!out)
        return false;
    *out = {};
    const QString prefix = QString(kLaunchTokenPrefix);
    if (!token.startsWith(prefix))
        return false;

    const QString remainder = token.mid(prefix.size());
    const int slash = remainder.indexOf(QLatin1Char('/'));
    if (slash <= 0)
        return false;

    out->ownerSlug = remainder.left(slash).trimmed();
    out->projectSlug = remainder.mid(slash + 1).trimmed();
    return out->isValid();
}
