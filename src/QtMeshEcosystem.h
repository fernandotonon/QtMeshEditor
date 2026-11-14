#ifndef QTMESH_ECOSYSTEM_H
#define QTMESH_ECOSYSTEM_H

#include <QList>
#include <QString>
#include <QUrl>

/**
 * @brief Offline catalog of the QtMesh ecosystem shown by the Welcome and
 *        About screens: official links, showcase games, and their platforms.
 *
 * Pure data, so it renders without a network connection. Every URL here is
 * an OFFICIAL destination checked against games.qtmesh.dev / the repository
 * (FUNDING.yml for Sponsors). Platform entries point at games.qtmesh.dev,
 * which lists every game with its Steam / Google Play / Roblox / web link.
 *
 * Thumbnails are small (384x216 JPEG) and compiled into the binary under
 * :/ecosystem/, so nothing is downloaded when a screen opens.
 */
namespace QtMeshEcosystem {

/// Stable ids for every outbound link (also the breadcrumb payload).
enum class Link {
    Website,        ///< https://qtmesh.dev
    GitHub,         ///< the QtMeshEditor repository
    Cloud,          ///< QtMesh Cloud (lives on qtmesh.dev)
    Games,          ///< https://games.qtmesh.dev
    Sponsor,        ///< GitHub Sponsors (from .github/FUNDING.yml)
    Contribute,     ///< CONTRIBUTING.md on GitHub
    Issues,         ///< bug reports / feature requests
};

struct Game {
    QString id;          ///< slug on games.qtmesh.dev
    QString title;
    QString blurb;       ///< one line, from the games site
    QString platform;    ///< "Web" or "Roblox"
    QString url;         ///< verified play / experience URL
    QString thumbnail;   ///< qrc path (":/ecosystem/<id>.jpg")
};

struct Platform {
    QString name;        ///< Steam / Web / Roblox / Android
    QString status;      ///< short status line shown under the name
    QString url;         ///< empty when no verified official destination exists
};

QString linkId(Link link);
QUrl url(Link link);

/// Showcase games, in display order (web first, then Roblox).
const QList<Game>& showcaseGames();

/// The four distribution platforms, with links only where verified.
const QList<Platform>& platforms();

/// "Create. Refine. Play." — the shared identity line.
QString tagline();

/// One-paragraph description used by the About screen.
QString description();

/// Opens @p url in the system browser and records a "ui.action" breadcrumb
/// naming the link id and the surface it was clicked from (no personal data;
/// Sentry itself honours the user's telemetry consent).
bool openUrl(const QUrl& url, const QString& linkName, const QString& surface);
bool open(Link link, const QString& surface);

} // namespace QtMeshEcosystem

#endif // QTMESH_ECOSYSTEM_H
