#ifndef ARTIFACTRESOLVER_H
#define ARTIFACTRESOLVER_H

#include "GitHubReleaseParser.h"
#include "InstallFlavor.h"

#include <QList>
#include <QString>

/**
 * @brief Pick the release artifact matching the running platform (#444).
 *
 * Pure-data resolver — no network. Only @ref InstallFlavor::Portable receives
 * an artifact; package-managed flavors should never call this.
 */
namespace ArtifactResolver {

struct ResolvedArtifact {
    bool ok = false;
    QString fileName;
    QString downloadUrl;
    QString signatureFileName;
    QString signatureUrl;
    QString sha256SumsFileName;
    QString sha256SumsUrl;
    QString errorMessage;
};

/**
 * @brief Resolve the portable update artifact for the current OS/arch.
 *
 * @p assets is the `assets[]` array from the chosen GitHub release.
 */
ResolvedArtifact resolveForCurrentPlatform(const QList<GitHubReleaseParser::ReleaseAsset>& assets,
                                           InstallFlavor::Flavor flavor);

} // namespace ArtifactResolver

#endif // ARTIFACTRESOLVER_H
