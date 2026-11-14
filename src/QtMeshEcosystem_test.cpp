#include <gtest/gtest.h>

#include "QtMeshEcosystem.h"

#include <QFile>
#include <QImage>
#include <QSet>
#include <QUrl>

using namespace QtMeshEcosystem;

namespace {
const QList<Link> kAllLinks = {Link::Website, Link::GitHub, Link::Cloud, Link::Games,
                               Link::Sponsor, Link::Contribute, Link::Issues};

// Hosts a showcase or ecosystem link may point at. Anything else is a link
// nobody verified, which the screens must never show.
bool isOfficialHost(const QString& host)
{
    return host == QLatin1String("qtmesh.dev") || host == QLatin1String("games.qtmesh.dev")
        || host == QLatin1String("github.com") || host == QLatin1String("fernandotonon.github.io")
        || host == QLatin1String("www.roblox.com");
}
} // namespace

TEST(QtMeshEcosystem, EveryLinkIsHttpsOnAnOfficialHostWithAUniqueId)
{
    QSet<QString> ids;
    for (Link link : kAllLinks) {
        const QUrl u = url(link);
        EXPECT_TRUE(u.isValid()) << linkId(link).toStdString();
        EXPECT_EQ(u.scheme(), QStringLiteral("https")) << u.toString().toStdString();
        EXPECT_TRUE(isOfficialHost(u.host())) << u.toString().toStdString();
        EXPECT_FALSE(linkId(link).isEmpty());
        ids.insert(linkId(link));
    }
    EXPECT_EQ(ids.size(), kAllLinks.size());
}

TEST(QtMeshEcosystem, KeyDestinationsMatchTheOfficialSites)
{
    EXPECT_EQ(url(Link::Games), QUrl(QStringLiteral("https://games.qtmesh.dev")));
    EXPECT_EQ(url(Link::Website), QUrl(QStringLiteral("https://qtmesh.dev")));
    EXPECT_EQ(url(Link::GitHub),
              QUrl(QStringLiteral("https://github.com/fernandotonon/QtMeshEditor")));
    // .github/FUNDING.yml: github: [fernandotonon]
    EXPECT_EQ(url(Link::Sponsor), QUrl(QStringLiteral("https://github.com/sponsors/fernandotonon")));
}

TEST(QtMeshEcosystem, ShowcaseGamesAreCompleteAndTheirThumbnailsAreCompiledIn)
{
    const auto& games = showcaseGames();
    ASSERT_GE(games.size(), 4);
    QSet<QString> ids;
    bool sawWeb = false, sawRoblox = false;
    for (const auto& g : games) {
        SCOPED_TRACE(g.id.toStdString());
        EXPECT_FALSE(g.title.isEmpty());
        EXPECT_FALSE(g.blurb.isEmpty());
        const QUrl u(g.url);
        EXPECT_EQ(u.scheme(), QStringLiteral("https"));
        EXPECT_TRUE(isOfficialHost(u.host()));
        sawWeb |= g.platform == QLatin1String("Web");
        sawRoblox |= g.platform == QLatin1String("Roblox");
        // Offline rendering: the thumbnail must be a real image in the qrc.
        ASSERT_TRUE(QFile::exists(g.thumbnail)) << g.thumbnail.toStdString();
        const QImage img(g.thumbnail);
        EXPECT_FALSE(img.isNull());
        EXPECT_LE(QFile(g.thumbnail).size(), 96 * 1024) << "keep showcase thumbnails small";
        ids.insert(g.id);
    }
    EXPECT_EQ(ids.size(), games.size());
    EXPECT_TRUE(sawWeb);
    EXPECT_TRUE(sawRoblox);
}

TEST(QtMeshEcosystem, PlatformsListAllFourAndOnlyLinkVerifiedDestinations)
{
    QStringList names;
    for (const auto& p : platforms()) {
        names.append(p.name);
        EXPECT_FALSE(p.status.isEmpty());
        if (!p.url.isEmpty())
            EXPECT_TRUE(isOfficialHost(QUrl(p.url).host())) << p.url.toStdString();
    }
    EXPECT_EQ(names, (QStringList{"Web", "Roblox", "Steam", "Android"}));
    // Every platform is live: no "in progress" / "in testing" disclaimers.
    for (const auto& p : platforms()) {
        EXPECT_FALSE(p.url.isEmpty()) << p.name.toStdString();
        EXPECT_FALSE(p.status.contains(QLatin1String("progress"), Qt::CaseInsensitive));
        EXPECT_FALSE(p.status.contains(QLatin1String("testing"), Qt::CaseInsensitive));
    }
}

TEST(QtMeshEcosystem, OpenUrlRefusesAnythingButHttps)
{
    // Rejected before any browser is launched.
    EXPECT_FALSE(openUrl(QUrl(QStringLiteral("http://qtmesh.dev")), "t", "test"));
    EXPECT_FALSE(openUrl(QUrl(QStringLiteral("file:///etc/passwd")), "t", "test"));
    EXPECT_FALSE(openUrl(QUrl(), "t", "test"));
}

TEST(QtMeshEcosystem, IdentityStrings)
{
    EXPECT_EQ(tagline(), QStringLiteral("Create. Refine. Play."));
    EXPECT_TRUE(description().startsWith(QStringLiteral("QtMesh Editor is an open-source 3D creation")));
}
