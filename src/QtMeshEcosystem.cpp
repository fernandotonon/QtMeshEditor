#include "QtMeshEcosystem.h"
#include "SentryReporter.h"

#include <QDesktopServices>

namespace QtMeshEcosystem {

QString linkId(Link link)
{
    switch (link) {
    case Link::Website:    return QStringLiteral("website");
    case Link::GitHub:     return QStringLiteral("github");
    case Link::Cloud:      return QStringLiteral("cloud");
    case Link::Games:      return QStringLiteral("games");
    case Link::Sponsor:    return QStringLiteral("sponsor");
    case Link::Contribute: return QStringLiteral("contribute");
    case Link::Issues:     return QStringLiteral("issues");
    }
    return QString();
}

QUrl url(Link link)
{
    switch (link) {
    case Link::Website:
    case Link::Cloud:
        return QUrl(QStringLiteral("https://qtmesh.dev"));
    case Link::GitHub:
        return QUrl(QStringLiteral("https://github.com/fernandotonon/QtMeshEditor"));
    case Link::Games:
        return QUrl(QStringLiteral("https://games.qtmesh.dev"));
    case Link::Sponsor:
        return QUrl(QStringLiteral("https://github.com/sponsors/fernandotonon"));
    case Link::Contribute:
        return QUrl(QStringLiteral(
            "https://github.com/fernandotonon/QtMeshEditor/blob/master/CONTRIBUTING.md"));
    case Link::Issues:
        return QUrl(QStringLiteral("https://github.com/fernandotonon/QtMeshEditor/issues"));
    }
    return QUrl();
}

const QList<Game>& showcaseGames()
{
    // Titles, blurbs and URLs as published on games.qtmesh.dev.
    static const QList<Game> games = {
        { QStringLiteral("scrapfall"), QStringLiteral("Scrapfall"),
          QStringLiteral("Top-down horde shooter against swarms of industrial robots."),
          QStringLiteral("Web"),
          QStringLiteral("https://fernandotonon.github.io/scrapfall-web/"),
          QStringLiteral(":/ecosystem/scrapfall.jpg") },
        { QStringLiteral("ironfang"), QStringLiteral("Ironfang: First Siege"),
          QStringLiteral("A micro-RTS: gather iron, raise a goblin army, topple the fortress."),
          QStringLiteral("Web"),
          QStringLiteral("https://fernandotonon.github.io/Ironfang/"),
          QStringLiteral(":/ecosystem/ironfang.jpg") },
        { QStringLiteral("mall-chase"), QStringLiteral("Mall Chase"),
          QStringLiteral("Chase a thief across a busy three-floor mall."),
          QStringLiteral("Web"),
          QStringLiteral("https://fernandotonon.github.io/mall-chase-web/"),
          QStringLiteral(":/ecosystem/mall-chase.jpg") },
        { QStringLiteral("bite-by-bite"), QStringLiteral("Bite by Bite"),
          QStringLiteral("Stealth puzzles with a squad of specialised zombies."),
          QStringLiteral("Web"),
          QStringLiteral("https://fernandotonon.github.io/Bite-by-Bite/"),
          QStringLiteral(":/ecosystem/bite-by-bite.jpg") },
        { QStringLiteral("data-core-clash"), QStringLiteral("Data Core Clash"),
          QStringLiteral("Team objective: capture network nodes, steal the enemy Data Core."),
          QStringLiteral("Roblox"),
          QStringLiteral("https://www.roblox.com/games/109556118216781"),
          QStringLiteral(":/ecosystem/data-core-clash.jpg") },
        { QStringLiteral("mothership-raiders"), QStringLiteral("Mothership Raiders"),
          QStringLiteral("Team sci-fi PvP: dogfight, then board the enemy mothership."),
          QStringLiteral("Roblox"),
          QStringLiteral("https://www.roblox.com/games/90569045808714"),
          QStringLiteral(":/ecosystem/mothership-raiders.jpg") },
    };
    return games;
}

const QList<Platform>& platforms()
{
    static const QList<Platform> list = {
        { QStringLiteral("Web"), QStringLiteral("Play in the browser"),
          QStringLiteral("https://games.qtmesh.dev") },
        { QStringLiteral("Roblox"), QStringLiteral("Multiplayer experiences"),
          QStringLiteral("https://games.qtmesh.dev") },
        { QStringLiteral("Steam"), QStringLiteral("PC releases"),
          QStringLiteral("https://games.qtmesh.dev") },
        { QStringLiteral("Android"), QStringLiteral("On Google Play"),
          QStringLiteral("https://games.qtmesh.dev") },
    };
    return list;
}

QString tagline()
{
    return QStringLiteral("Create. Refine. Play.");
}

QString description()
{
    return QStringLiteral(
        "QtMeshEditor is an open-source 3D creation and asset pipeline tool "
        "combining modeling, asset conversion, AI-assisted 3D generation, rigging, "
        "and animation into a unified development workflow.");
}

bool openUrl(const QUrl& target, const QString& linkName, const QString& surface)
{
    if (!target.isValid() || target.scheme() != QLatin1String("https"))
        return false;
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Ecosystem link: %1 (%2)").arg(linkName, surface));
    return QDesktopServices::openUrl(target);
}

bool open(Link link, const QString& surface)
{
    return openUrl(url(link), linkId(link), surface);
}

} // namespace QtMeshEcosystem
