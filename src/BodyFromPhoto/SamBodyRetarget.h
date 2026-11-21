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

/// SAM's pose is camera-relative, and there is deliberately NO upright correction: a camera pitch and a genuinely leaning subject
/// (a rifle aimer) look identical from the torso axis, and standing the average torso upright made a leaning walk look tilted back.
/// The rig keeps SAM's pose as predicted.

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
/// `rootMotion` = move the root by the SOURCE root's displacement (the frames' root `pos`), scaled by the rigs' height ratio;
/// false = rotation-only.
RetargetOutcome retargetToSkeleton(Ogre::Skeleton* skeleton, const Retarget::RigDesc& mhr,
                                   const std::vector<Retarget::LocalPose>& frames, const std::vector<float>& times,
                                   const std::string& clip, bool replace, bool rootMotion = false);

/// Root travel from the decoder's per-frame camera translation, in the MHR frame (Y up), relative to the first frame. The
/// MHR frame is the camera frame with Y and Z negated, so a static camera's view of the person moving by d is (dx, -dy, -dz).
/// Monocular depth is the noisy axis (it comes from the person's apparent size), so it is smoothed harder than x/y:
/// centred moving averages of `radiusXY` / `radiusZ` frames. A frame with a non-finite value keeps the previous offset.
std::vector<Ogre::Vector3> rootTrajectory(const std::vector<std::array<float, 3>>& camT, int radiusXY = 1, int radiusZ = 4);

/// The whole pipeline for a solved sequence: MHR rest skeleton, local poses,
/// retarget, write the clip. `fps` > 0 spaces the frames uniformly (video / camera takes); 0 uses `seq.times` (pose photos).
/// `rootMotion` = translate the root from `seq.camT` (rootTrajectory): right for a static camera; pose photos should pass false
/// (framing differences between photos are not travel).
RetargetOutcome retargetSequenceToSkeleton(Ogre::Skeleton* skeleton, const MhrSequence& seq, int fps, const std::string& clip,
                                           bool replace, bool rootMotion = false);

/// Retarget `frames` (one LocalPose per sample on `mhr`, uniformly spaced at `fps`) onto `skeleton` as the skeletal animation
/// `clip` (replaced when it exists and `replace` is set). Rotation-only: the target keeps its own bone lengths.
RetargetOutcome retargetToSkeleton(Ogre::Skeleton* skeleton, const Retarget::RigDesc& mhr,
                                   const std::vector<Retarget::LocalPose>& frames, int fps, const std::string& clip,
                                   bool replace, bool rootMotion = false);

} // namespace SamBody
