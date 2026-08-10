#ifdef ENABLE_MOCAP

#include "MocapCameraHints.h"

#include <QCoreApplication>

namespace MocapCameraHints {

void ensureMultimediaBackendSafe()
{
#ifdef Q_OS_LINUX
    static bool done = false;
    if (done)
        return;
    done = true;
    // Empty list disables all FFmpeg hw decode/encode backends (Qt docs).
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_DECODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", ",");
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES", ",");
#endif
}

bool runningAsSnap()
{
#ifdef Q_OS_LINUX
    return qEnvironmentVariable("SNAP") == QLatin1String("qtmesheditor");
#else
    return false;
#endif
}

QString snapConnectHint()
{
    if (!runningAsSnap())
        return {};
    return QCoreApplication::translate(
        "MocapCameraHints",
        "\n\nIf you installed via Snap, connect the camera interface:\n"
        "  snap connect qtmesheditor:camera");
}

QString permissionDeniedMessage()
{
#ifdef Q_OS_MACOS
    return QCoreApplication::translate(
        "MocapCameraHints",
        "Camera access was not granted. If no prompt appeared, enable it for "
        "QtMeshEditor in System Settings → Privacy & Security → Camera, then "
        "click Preview again.");
#elif defined(Q_OS_LINUX)
    if (runningAsSnap()) {
        return QCoreApplication::translate(
            "MocapCameraHints",
            "Camera access was not granted. Connect the camera interface, then "
            "click Preview again:\n"
            "  snap connect qtmesheditor:camera");
    }
    return QCoreApplication::translate(
        "MocapCameraHints",
        "Camera access was not granted. Allow camera access for QtMeshEditor "
        "in your system privacy settings (or via xdg-desktop-portal), then "
        "click Preview again.");
#else
    return QCoreApplication::translate(
        "MocapCameraHints",
        "Camera access was not granted. Allow camera access for QtMeshEditor, "
        "then click Preview again.");
#endif
}

}  // namespace MocapCameraHints

#endif  // ENABLE_MOCAP
