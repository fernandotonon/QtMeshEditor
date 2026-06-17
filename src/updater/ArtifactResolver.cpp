#include "ArtifactResolver.h"

#include <QRegularExpression>

namespace ArtifactResolver {

namespace {

const GitHubReleaseParser::ReleaseAsset* findAssetByName(
    const QList<GitHubReleaseParser::ReleaseAsset>& assets,
    const QRegularExpression& pattern)
{
    for (const GitHubReleaseParser::ReleaseAsset& asset : assets) {
        if (pattern.match(asset.name).hasMatch()) {
            return &asset;
        }
    }
    return nullptr;
}

const GitHubReleaseParser::ReleaseAsset* findAssetExact(
    const QList<GitHubReleaseParser::ReleaseAsset>& assets,
    const QString& name)
{
    for (const GitHubReleaseParser::ReleaseAsset& asset : assets) {
        if (asset.name.compare(name, Qt::CaseInsensitive) == 0) {
            return &asset;
        }
    }
    return nullptr;
}

QRegularExpression primaryArtifactPattern()
{
#if defined(Q_OS_WIN)
    return QRegularExpression(QStringLiteral("^.+-bin-Windows\\.zip$"),
                              QRegularExpression::CaseInsensitiveOption);
#elif defined(Q_OS_MACOS)
    return QRegularExpression(QStringLiteral("^.+-MacOS\\.dmg$"),
                              QRegularExpression::CaseInsensitiveOption);
#elif defined(Q_OS_LINUX)
    return QRegularExpression(
        QStringLiteral("^.+(-linux|_linux)[-_].+\\.(AppImage|tar\\.gz|tar\\.xz)$"),
        QRegularExpression::CaseInsensitiveOption);
#else
    return QRegularExpression();
#endif
}

} // namespace

ResolvedArtifact resolveForCurrentPlatform(
    const QList<GitHubReleaseParser::ReleaseAsset>& assets,
    InstallFlavor::Flavor flavor)
{
    ResolvedArtifact result;

    if (!InstallFlavor::isPackageManagerManaged(flavor) && flavor != InstallFlavor::Flavor::Portable
        && flavor != InstallFlavor::Flavor::Unknown) {
        result.errorMessage = QStringLiteral("In-app download is only supported for portable installs");
        return result;
    }

    const QRegularExpression pattern = primaryArtifactPattern();
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS) && !defined(Q_OS_LINUX)
    result.errorMessage = QStringLiteral("Auto-update downloads are not supported on this platform");
    return result;
#endif

    if (!pattern.isValid()) {
        result.errorMessage = QStringLiteral("Auto-update downloads are not supported on this platform");
        return result;
    }

    const GitHubReleaseParser::ReleaseAsset* primary = findAssetByName(assets, pattern);
    if (!primary) {
#if defined(Q_OS_LINUX)
        result.errorMessage =
            QStringLiteral("No portable Linux update package found in this release "
                           "(expected AppImage or .tar.gz; .deb installs use apt)");
#else
        result.errorMessage = QStringLiteral("No update package found for this platform in the release");
#endif
        return result;
    }

    result.fileName = primary->name;
    result.downloadUrl = primary->browserDownloadUrl;

    const QString signatureName = primary->name + QStringLiteral(".minisig");
    if (const GitHubReleaseParser::ReleaseAsset* sig = findAssetExact(assets, signatureName)) {
        result.signatureFileName = sig->name;
        result.signatureUrl = sig->browserDownloadUrl;
    } else {
        result.errorMessage =
            QStringLiteral("Release is missing signature sidecar: %1").arg(signatureName);
        return result;
    }

    if (const GitHubReleaseParser::ReleaseAsset* sums =
            findAssetExact(assets, QStringLiteral("SHA256SUMS"))) {
        result.sha256SumsFileName = sums->name;
        result.sha256SumsUrl = sums->browserDownloadUrl;
    }

    result.ok = true;
    return result;
}

} // namespace ArtifactResolver
