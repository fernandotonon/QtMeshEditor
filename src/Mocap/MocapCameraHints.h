#ifndef MOCAPCAMERAHINTS_H
#define MOCAPCAMERAHINTS_H

#include <QString>

namespace MocapCameraHints {

bool runningAsSnap();

QString snapConnectHint();

QString permissionDeniedMessage();

}  // namespace MocapCameraHints

#endif  // MOCAPCAMERAHINTS_H
