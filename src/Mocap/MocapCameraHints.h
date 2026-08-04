#ifndef MOCAPCAMERAHINTS_H
#define MOCAPCAMERAHINTS_H

#include <QString>

namespace MocapCameraHints {

// Must run before the first Qt Multimedia call on Linux. The FFmpeg backend
// initializes VA-API during QPlatformMediaIntegration::instance(); on some
// NVIDIA + X11 setups that SIGSEGVs inside QMediaDevices::videoInputs().
void ensureMultimediaBackendSafe();

bool runningAsSnap();

QString snapConnectHint();

QString permissionDeniedMessage();

}  // namespace MocapCameraHints

#endif  // MOCAPCAMERAHINTS_H
