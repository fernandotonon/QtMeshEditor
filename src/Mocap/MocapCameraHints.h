#ifndef MOCAPCAMERAHINTS_H
#define MOCAPCAMERAHINTS_H

#include "../updater/InstallFlavor.h"

#include <QCoreApplication>
#include <QString>

#ifdef Q_OS_LINUX
#include <QtGlobal>
#endif

namespace MocapCameraHints {

inline bool runningAsSnap()
{
#ifdef Q_OS_LINUX
    return InstallFlavor::detect(QCoreApplication::applicationFilePath())
        == InstallFlavor::Flavor::Snap;
#else
    return false;
#endif
}

inline QString snapConnectHint()
{
    if (!runningAsSnap())
        return {};
    return QStringLiteral(
        "\n\nIf you installed via Snap, connect the camera interface:\n"
        "  snap connect qtmesheditor:camera");
}

inline QString permissionDeniedMessage()
{
#ifdef Q_OS_MACOS
    return QStringLiteral(
        "Camera access was not granted. If no prompt appeared, enable it for "
        "QtMeshEditor in System Settings → Privacy & Security → Camera, then "
        "click Preview again.");
#elif defined(Q_OS_LINUX)
    if (runningAsSnap()) {
        return QStringLiteral(
            "Camera access was not granted. Connect the camera interface, then "
            "click Preview again:\n"
            "  snap connect qtmesheditor:camera");
    }
    return QStringLiteral(
        "Camera access was not granted. Allow camera access for QtMeshEditor "
        "in your system privacy settings (or via xdg-desktop-portal), then "
        "click Preview again.");
#else
    return QStringLiteral(
        "Camera access was not granted. Allow camera access for QtMeshEditor, "
        "then click Preview again.");
#endif
}

}  // namespace MocapCameraHints

#endif  // MOCAPCAMERAHINTS_H
