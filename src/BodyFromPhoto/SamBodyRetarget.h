#pragma once

#include "../AnimationRetargeter.h"
#include "SamBodySequence.h"

#include <QString>
#include <QStringList>

#include <vector>

namespace Ogre { class Skeleton; }

/// #1100: drive an arbitrary humanoid rig from SAM 3D Body's MHR joint rotations through the project's INCOMPATIBLE-skeleton
/// retargeter (#523), instead of the 22-role CMU path.
///
/// The CMU path takes deltas against the clip's FIRST FRAME and assumes the rig's bind pose is that frame. For a dance (or any
/// take that does not start in the rig's rest pose) that discards the whole starting pose, and it knows nothing about the
/// A-pose / T-pose difference between rigs. The retargeter does: the MHR rest pose (all-zero parameters) is the source rest,
/// each mapped bone's world delta from that rest is carried into the target frame and applied to the target's bind, with the
/// per-bone direction alignment that lets an A-pose source drive a T-pose target. Names are mapped automatically (the same
/// role matcher the rest of the motion pipeline uses), and MHR's finger joints come along for free.
namespace SamBody {

/// Rest skeleton as plain data, WITHOUT the helper joints (see excludedFromMapping): their children hang off the nearest kept
/// ancestor. `restPos`: 127*3 metres, `restQuat`: 127*4 (x,y,z,w) GLOBAL rotations, both for the all-zero
/// pose. Empty RigDesc on malformed input.
Retarget::RigDesc mhrRigDesc(const std::vector<float>& restPos, const std::vector<float>& restQuat);

/// One frame of LOCAL transforms from `jointQuat` (127*4 global rotations of the FULL MHR skeleton, x,y,z,w) on `rig`: rotations are re-expressed
/// against each parent, positions are the rig's bind positions (the bone lengths do not change).
Retarget::LocalPose mhrLocalPose(const Retarget::RigDesc& rig, const std::vector<float>& jointQuat);

/// SAM's pose is CAMERA-relative: a camera that looks down on the person (a phone held high, a turntable render) comes back as a body
/// leaning forward. Measured on 12 rendered frames of a dance: SAM's joint directions were within 10 degrees of the truth after ONE
/// global rotation of 27 degrees, and 27 degrees off without it. `uprightCorrection` returns the world rotation that stands the
/// clip's average torso axis (root -> neck) upright; `jointPosFrames` are Body::jointPos per frame. Identity when there is nothing
/// to average. Rotates about the horizontal axis only, so the facing direction (yaw) is untouched.
Ogre::Quaternion uprightCorrection(const std::vector<std::vector<float>>& jointPosFrames);

/// Pre-multiply every joint's global rotation (127*4, x,y,z,w) by the world rotation `c`.
void applyWorldRotation(std::vector<float>& jointQuat, const Ogre::Quaternion& c);

struct RetargetOutcome {
    bool ok = false;
    QString error;
    int mappedBones = 0;
    int fingerBones = 0;                      ///< mapped bones whose target name carries a finger
    QStringList unmappedTarget;               ///< target bones nothing was mapped onto (a quick rig-compatibility read)
    Retarget::RetargetReport report;
};

/// Standard (Mixamo-style) spelling of an MHR joint, which the project's role matcher understands; the name itself for joints with
/// no counterpart. Used only to build the bone map.
std::string mhrAlias(const std::string& mhrJointName);

/// Source names that must not take part in the auto-map: twist/corrective helpers, face and world joints.
bool excludedFromMapping(const std::string& mhrJointName);

/// Same, but at explicit `times` (seconds, ascending, one per frame): pose keyframes from photos are not uniformly spaced.
RetargetOutcome retargetToSkeleton(Ogre::Skeleton* skeleton, const Retarget::RigDesc& mhr,
                                   const std::vector<Retarget::LocalPose>& frames, const std::vector<float>& times,
                                   const std::string& clip, bool replace);

/// The whole pipeline for a solved sequence: MHR rest skeleton, upright correction (SAM's pose is camera-relative), local poses,
/// retarget, write the clip. `fps` > 0 spaces the frames uniformly (video / camera takes); 0 uses `seq.times` (pose photos).
RetargetOutcome retargetSequenceToSkeleton(Ogre::Skeleton* skeleton, const MhrSequence& seq, int fps, const std::string& clip,
                                           bool replace);

/// Retarget `frames` (one LocalPose per sample on `mhr`, uniformly spaced at `fps`) onto `skeleton` as the skeletal animation
/// `clip` (replaced when it exists and `replace` is set). Rotation-only: the target keeps its own bone lengths.
RetargetOutcome retargetToSkeleton(Ogre::Skeleton* skeleton, const Retarget::RigDesc& mhr,
                                   const std::vector<Retarget::LocalPose>& frames, int fps, const std::string& clip,
                                   bool replace);

} // namespace SamBody
