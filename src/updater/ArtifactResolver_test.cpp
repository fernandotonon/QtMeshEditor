#include <gtest/gtest.h>

#include "ArtifactResolver.h"

using GitHubReleaseParser::ReleaseAsset;

namespace {

ReleaseAsset makeAsset(const QString& name, const QString& url = QString())
{
    ReleaseAsset asset;
    asset.name = name;
    asset.browserDownloadUrl = url.isEmpty()
        ? QStringLiteral("https://example.com/%1").arg(name)
        : url;
    asset.size = 1024;
    return asset;
}

} // namespace

#if defined(Q_OS_WIN)

TEST(ArtifactResolver, PicksWindowsZipWithSignature)
{
    QList<ReleaseAsset> assets = {
        makeAsset(QStringLiteral("qtmesheditor_amd64.deb")),
        makeAsset(QStringLiteral("QtMeshEditor-3.5.3-bin-Windows.zip")),
        makeAsset(QStringLiteral("QtMeshEditor-3.5.3-bin-Windows.zip.minisig")),
        makeAsset(QStringLiteral("SHA256SUMS")),
    };

    const ArtifactResolver::ResolvedArtifact resolved =
        ArtifactResolver::resolveForCurrentPlatform(assets, InstallFlavor::Flavor::Portable);

    ASSERT_TRUE(resolved.ok) << resolved.errorMessage.toStdString();
    EXPECT_EQ(resolved.fileName, QStringLiteral("QtMeshEditor-3.5.3-bin-Windows.zip"));
    EXPECT_FALSE(resolved.signatureUrl.isEmpty());
    EXPECT_FALSE(resolved.sha256SumsUrl.isEmpty());
}

#elif defined(Q_OS_MACOS)

TEST(ArtifactResolver, PicksMacDmgWithSignature)
{
    QList<ReleaseAsset> assets = {
        makeAsset(QStringLiteral("QtMeshEditor-3.5.3-MacOS.dmg")),
        makeAsset(QStringLiteral("QtMeshEditor-3.5.3-MacOS.dmg.minisig")),
    };

    const ArtifactResolver::ResolvedArtifact resolved =
        ArtifactResolver::resolveForCurrentPlatform(assets, InstallFlavor::Flavor::Portable);

    ASSERT_TRUE(resolved.ok) << resolved.errorMessage.toStdString();
    EXPECT_EQ(resolved.fileName, QStringLiteral("QtMeshEditor-3.5.3-MacOS.dmg"));
}

#elif defined(Q_OS_LINUX)

TEST(ArtifactResolver, RejectsDebOnlyReleaseForPortableLinux)
{
    QList<ReleaseAsset> assets = {
        makeAsset(QStringLiteral("qtmesheditor_amd64.deb")),
        makeAsset(QStringLiteral("qtmesheditor_amd64.deb.minisig")),
    };

    const ArtifactResolver::ResolvedArtifact resolved =
        ArtifactResolver::resolveForCurrentPlatform(assets, InstallFlavor::Flavor::Portable);

    EXPECT_FALSE(resolved.ok);
    EXPECT_FALSE(resolved.errorMessage.isEmpty());
}

TEST(ArtifactResolver, PicksLinuxTarballWithSignature)
{
    QList<ReleaseAsset> assets = {
        makeAsset(QStringLiteral("QtMeshEditor-3.5.3-linux-x86_64.tar.gz")),
        makeAsset(QStringLiteral("QtMeshEditor-3.5.3-linux-x86_64.tar.gz.minisig")),
    };

    const ArtifactResolver::ResolvedArtifact resolved =
        ArtifactResolver::resolveForCurrentPlatform(assets, InstallFlavor::Flavor::Portable);

    ASSERT_TRUE(resolved.ok) << resolved.errorMessage.toStdString();
    EXPECT_TRUE(resolved.fileName.contains(QStringLiteral("linux")));
}

#endif

TEST(ArtifactResolver, RejectsPackageManagedFlavor)
{
    QList<ReleaseAsset> assets = {
        makeAsset(QStringLiteral("QtMeshEditor-3.5.3-bin-Windows.zip")),
        makeAsset(QStringLiteral("QtMeshEditor-3.5.3-bin-Windows.zip.minisig")),
    };

    const ArtifactResolver::ResolvedArtifact resolved =
        ArtifactResolver::resolveForCurrentPlatform(assets, InstallFlavor::Flavor::Homebrew);

    EXPECT_FALSE(resolved.ok);
    EXPECT_TRUE(resolved.errorMessage.contains(QStringLiteral("portable")));
}

TEST(ArtifactResolver, RequiresSignatureSidecar)
{
    QList<ReleaseAsset> assets;
#if defined(Q_OS_WIN)
    assets.append(makeAsset(QStringLiteral("QtMeshEditor-3.5.3-bin-Windows.zip")));
#elif defined(Q_OS_MACOS)
    assets.append(makeAsset(QStringLiteral("QtMeshEditor-3.5.3-MacOS.dmg")));
#else
    assets.append(makeAsset(QStringLiteral("QtMeshEditor-3.5.3-linux-x86_64.tar.gz")));
#endif

    const ArtifactResolver::ResolvedArtifact resolved =
        ArtifactResolver::resolveForCurrentPlatform(assets, InstallFlavor::Flavor::Portable);

    EXPECT_FALSE(resolved.ok);
    EXPECT_TRUE(resolved.errorMessage.contains(QStringLiteral("minisig")));
}
