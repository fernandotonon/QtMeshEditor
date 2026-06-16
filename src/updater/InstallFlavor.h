#ifndef INSTALLFLAVOR_H
#define INSTALLFLAVOR_H

#include <QString>

/**
 * @brief How the running binary was installed — gates the in-app auto-updater.
 *
 * Package-manager flavors must defer to their own update channel (brew, winget,
 * snap, apt, flatpak, docker pull). Only @ref InstallFlavor::Portable receives
 * download → verify → install → restart. See docs/AUTO_UPDATER_DESIGN.md (#440).
 */
namespace InstallFlavor {

enum class Flavor {
    Portable,   ///< Direct-download zip/dmg/tarball or /opt/QtMeshEditor portable tree
    Homebrew,   ///< macOS Homebrew cask
    WinGet,     ///< Windows WinGet package
    Snap,       ///< Linux snap (qtmesheditor)
    Flatpak,    ///< Linux Flatpak install
    Debian,     ///< Linux .deb / apt install under /usr
    Docker,     ///< Container image (/.dockerenv)
    Unknown,    ///< Dev/CI build tree, ambiguous layout — no self-update
};

/// Canonical lowercase slug for Sentry breadcrumbs and settings keys.
QString toSlug(Flavor flavor);

/// User-facing label for the updater dialog.
QString displayName(Flavor flavor);

/// True for every flavor except @ref Flavor::Portable.
bool isPackageManagerManaged(Flavor flavor);

/**
 * @brief One-line shell command the updater dialog shows instead of downloading.
 *
 * Empty for @ref Flavor::Portable and @ref Flavor::Unknown.
 */
QString updateCommandHint(Flavor flavor);

/**
 * @brief Detect install flavor from the application binary path.
 *
 * @p applicationFilePath is usually QCoreApplication::applicationFilePath().
 * Normalised to absolute form internally. Pure function aside from optional
 * registry / codesign probes on Windows and macOS.
 */
Flavor detect(const QString& applicationFilePath);

} // namespace InstallFlavor

#endif // INSTALLFLAVOR_H
