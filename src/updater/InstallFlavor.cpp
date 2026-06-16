#include "InstallFlavor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace InstallFlavor {

namespace {

QString normalizedExePath(const QString& applicationFilePath)
{
    const QString raw = applicationFilePath.isEmpty()
        ? QCoreApplication::applicationFilePath()
        : applicationFilePath;
    return QDir::cleanPath(QFileInfo(raw).absoluteFilePath());
}

bool pathContains(const QString& path, QLatin1String needle)
{
    return path.contains(needle, Qt::CaseInsensitive);
}

#ifdef Q_OS_LINUX
Flavor detectLinux(const QString& path)
{
    if (QFile::exists(QStringLiteral("/.dockerenv")))
        return Flavor::Docker;

    if (pathContains(path, QLatin1String("/snap/qtmesheditor/"))
        || path.startsWith(QStringLiteral("/snap/bin/qtmesheditor"), Qt::CaseInsensitive))
        return Flavor::Snap;

    if (pathContains(path, QLatin1String("/var/lib/flatpak/"))
        || pathContains(path, QLatin1String("/.local/share/flatpak/")))
        return Flavor::Flatpak;

    const QFileInfo info(path);
    const QString dir = info.absolutePath();
    if (dir == QStringLiteral("/usr/bin") || dir == QStringLiteral("/bin"))
        return Flavor::Debian;

    if (pathContains(path, QLatin1String("/opt/QtMeshEditor"))
        || pathContains(path, QLatin1String("/opt/qtmesheditor")))
        return Flavor::Portable;

    return Flavor::Unknown;
}
#endif

#ifdef Q_OS_MACOS
Flavor detectMacOS(const QString& path)
{
    // Homebrew cask installs under /Applications or /opt/homebrew/Caskroom/.
    if (path.startsWith(QStringLiteral("/Applications/Homebrew/"), Qt::CaseInsensitive)
        || pathContains(path, QLatin1String("/Caskroom/qtmesheditor/"))
        || pathContains(path, QLatin1String("/opt/homebrew/Caskroom/qtmesheditor/")))
        return Flavor::Homebrew;

    // Sentinel some brew workflows drop inside the bundle (issue #440 spike).
    if (path.contains(QStringLiteral(".app"), Qt::CaseInsensitive)) {
        const int appIdx = path.indexOf(QStringLiteral(".app"), 0, Qt::CaseInsensitive);
        const QString bundleRoot = path.left(appIdx + 4);
        if (QFile::exists(bundleRoot + QStringLiteral("/Contents/Resources/.brew")))
            return Flavor::Homebrew;
    }

    if (path.startsWith(QStringLiteral("/Applications/QtMeshEditor.app"), Qt::CaseInsensitive)
        || pathContains(path, QLatin1String("/Applications/QtMeshEditor.app/")))
        return Flavor::Portable;

    return Flavor::Unknown;
}
#endif

#ifdef Q_OS_WIN
bool registryHasUninstallEntry(const wchar_t* subKey)
{
    HKEY key = nullptr;
    const LONG opened =
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, KEY_READ | KEY_WOW64_64KEY, &key);
    if (opened != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

Flavor detectWindows(const QString& path)
{
    Q_UNUSED(path);

    if (registryHasUninstallEntry(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\QtMeshEditor"))
        return Flavor::Portable; // MSI/custom installer — still self-managed, not WinGet

    const QString startMenu = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    if (pathContains(startMenu, QLatin1String("WinGet"))
        || pathContains(path, QLatin1String("WindowsApps"))
        || registryHasUninstallEntry(
               L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FernandoTonon.QtMeshEditor"))
        return Flavor::WinGet;

    // Zip portable installs typically live under Downloads or a user-chosen folder.
    if (pathContains(path, QLatin1String("\\QtMeshEditor\\"))
        || path.endsWith(QStringLiteral("QtMeshEditor.exe"), Qt::CaseInsensitive))
        return Flavor::Portable;

    return Flavor::Unknown;
}
#endif

} // namespace

QString toSlug(Flavor flavor)
{
    switch (flavor) {
    case Flavor::Portable: return QStringLiteral("portable");
    case Flavor::Homebrew: return QStringLiteral("homebrew");
    case Flavor::WinGet: return QStringLiteral("winget");
    case Flavor::Snap: return QStringLiteral("snap");
    case Flavor::Flatpak: return QStringLiteral("flatpak");
    case Flavor::Debian: return QStringLiteral("debian");
    case Flavor::Docker: return QStringLiteral("docker");
    case Flavor::Unknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

QString displayName(Flavor flavor)
{
    switch (flavor) {
    case Flavor::Portable: return QStringLiteral("Portable");
    case Flavor::Homebrew: return QStringLiteral("Homebrew");
    case Flavor::WinGet: return QStringLiteral("WinGet");
    case Flavor::Snap: return QStringLiteral("Snap");
    case Flavor::Flatpak: return QStringLiteral("Flatpak");
    case Flavor::Debian: return QStringLiteral("Debian package");
    case Flavor::Docker: return QStringLiteral("Docker");
    case Flavor::Unknown: return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

bool isPackageManagerManaged(Flavor flavor)
{
    return flavor != Flavor::Portable && flavor != Flavor::Unknown;
}

QString updateCommandHint(Flavor flavor)
{
    switch (flavor) {
    case Flavor::Homebrew:
        return QStringLiteral("brew upgrade --cask qtmesheditor");
    case Flavor::WinGet:
        return QStringLiteral("winget upgrade FernandoTonon.QtMeshEditor");
    case Flavor::Snap:
        return QStringLiteral("sudo snap refresh qtmesheditor");
    case Flavor::Flatpak:
        return QStringLiteral("flatpak update io.github.fernandotonon.QtMeshEditor");
    case Flavor::Debian:
        return QStringLiteral("sudo apt update && sudo apt install qtmesheditor");
    case Flavor::Docker:
        return QStringLiteral("docker pull ghcr.io/fernandotonon/qtmesh");
    case Flavor::Portable:
    case Flavor::Unknown:
        break;
    }
    return {};
}

Flavor detect(const QString& applicationFilePath)
{
    const QString path = normalizedExePath(applicationFilePath);
#if defined(Q_OS_LINUX)
    return detectLinux(path);
#elif defined(Q_OS_MACOS)
    return detectMacOS(path);
#elif defined(Q_OS_WIN)
    return detectWindows(path);
#else
    Q_UNUSED(path);
    return Flavor::Unknown;
#endif
}

} // namespace InstallFlavor
