#ifndef MOCAPBODYDRIVEDEBUG_H
#define MOCAPBODYDRIVEDEBUG_H

#ifdef ENABLE_MOCAP

#include "MocapLiveTypes.h"

#include <cstddef>

namespace Ogre {
class Entity;
class SkeletonInstance;
}

class BodyRetargeter;

// stderr diagnostics when QTMESH_MOCAP_DEBUG=1 — compare PoseIK FK joints vs
// the driven skeleton so live mocap mismatches are easy to spot in a terminal.
namespace MocapBodyDriveDebug {

void logFrame(
    Ogre::Entity* entity,
    Ogre::SkeletonInstance* skel,
    const BodyLiveFrame& body,
    const BodyRetargeter* retargeter,
    double timeSec,
    bool bodyNeutralReady,
    bool haveNeutralRef,
    int warmupFrames,
    int warmupTarget,
    size_t localsApplied);

}  // namespace MocapBodyDriveDebug

#endif  // ENABLE_MOCAP
#endif  // MOCAPBODYDRIVEDEBUG_H
