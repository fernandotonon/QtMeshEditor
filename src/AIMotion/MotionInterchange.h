#pragma once

// The motion interchange layer between external motion models (NVIDIA Kimodo
// today, MotionBricks next) and QtMeshEditor's animation system.
//
// A model sidecar writes `qtmesh-motion-interchange` NPZ files (format
// version 1, see ai/kimodo/generate.py for the producer): parent-relative
// rotation matrices per frame, the root's world position, foot contacts, and
// the SOURCE skeleton itself (names, parents, rest joint positions). Carrying
// the skeleton in the file means nothing here hard-codes SOMA geometry; what
// IS versioned in code is the name mapping (SomaSkeleton.h).
//
// Pure data (Qt + Ogre math only): testable without a scene.

#include "../AnimationRetargeter.h"

#include <OgreQuaternion.h>
#include <OgreVector.h>

#include <QJsonObject>
#include <QString>

#include <string>
#include <vector>

namespace AIMotion {

constexpr int kInterchangeVersion = 1;

struct Motion {
    std::vector<std::string> names;
    std::vector<int> parents;                          ///< parent index, -1 for the root; parent < child
    std::vector<Ogre::Vector3> restJoints;             ///< rest-pose GLOBAL positions, metres (y up, +z forward)
    float fps = 30.0f;
    std::vector<std::vector<Ogre::Quaternion>> localRot;   ///< [frame][joint], identity = the rest pose
    std::vector<Ogre::Vector3> rootPos;                    ///< [frame] root world position, metres
    std::vector<std::vector<float>> contacts;              ///< [frame][channel], may be empty
    QJsonObject meta;                                       ///< source/model/seed/settings (result.json)

    int jointCount() const { return int(names.size()); }
    int frameCount() const { return int(localRot.size()); }
    float duration() const { return frameCount() > 1 ? float(frameCount() - 1) / fps : 0.0f; }
    int indexOf(const std::string& name) const;
};

/// Read one interchange NPZ. False + `error` on a malformed file (wrong
/// version, shape mismatch, parents out of order, non-rotation matrices).
bool loadInterchange(const QString& npzPath, Motion* out, QString* error = nullptr);

/// Rotation matrix (row-major 3x3) -> quaternion. Re-orthonormalised first, so
/// float noise in the producer cannot turn into a scaled quaternion.
Ogre::Quaternion quatFromMatrix(const float m[9]);

/// The source skeleton as a retarget RigDesc: rest rotations are identity,
/// so each bind position is the offset from the parent's rest position.
Retarget::RigDesc rigDesc(const Motion& m);

/// One LocalPose per frame on rigDesc(m): the frame's rotations, bind
/// offsets, and the root's world position.
std::vector<Retarget::LocalPose> localPoses(const Motion& m);

/// Resample to `fps` keeping the duration (first and last frames are kept
/// exactly; rotations slerp along the shortest path, root/contacts lerp).
Motion resample(const Motion& m, float fps);

} // namespace AIMotion
