#ifndef UPDATERINSTALLER_H
#define UPDATERINSTALLER_H

#include <QString>

/**
 * @brief Platform install + relauncher orchestration (#446–448).
 *
 * Prepares a verified artifact for install, writes a manifest, and spawns the
 * platform relauncher before the main app exits.
 */
namespace UpdaterInstaller {

enum class ArtifactKind {
    Unknown,
    Zip,
    TarGz,
    TarXz,
    Dmg,
    AppImage,
};

struct InstallContext {
    QString stagedArtifactPath;
    QString releaseTag;
    QString installRoot;
    QString executablePath;
    qint64 parentPid = 0;
};

struct InstallPlan {
    bool ok = false;
    QString errorMessage;
    QString manifestPath;
    ArtifactKind artifactKind = ArtifactKind::Unknown;
};

ArtifactKind detectArtifactKind(const QString& fileName);
QString resolveInstallRoot(const QString& executablePath = QString());
QString relauncherExecutablePath();
InstallPlan prepareInstall(const InstallContext& context);
bool launchRelauncher(const InstallPlan& plan);
bool isInstallLocationWritable(const InstallContext& context);

#ifdef QTMESH_UNIT_TESTS
QString resolveInstallRootForTest(const QString& applicationFilePath);
#endif

} // namespace UpdaterInstaller

#endif // UPDATERINSTALLER_H
