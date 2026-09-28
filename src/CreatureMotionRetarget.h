#ifndef CREATURE_MOTION_RETARGET_H
#define CREATURE_MOTION_RETARGET_H

#include "QuadrupedSkeleton.h"

#include <QString>

#include <array>
#include <vector>

namespace Ogre { class Skeleton; }

// Apply a canonical CREATURE clip onto a creature rig (#1073) — the
// counterpart of AnimationMerger::applyMotionClip for non-humanoid skeletons.
//
// Uses the same world-delta principle as the humanoid world-frame path, which
// is what makes it body-plan agnostic:
//
//     dWorld(f) = clip(f) · clip(0)^-1
//
// i.e. how far a joint has turned IN WORLD SPACE since the clip's first
// frame. That delta is then transported into the target bone's parent-world
// frame and composed onto its bind pose. Because it is a delta, the source
// and target may have different proportions, bone lengths and rest poses —
// only the ROLE correspondence has to hold, which is exactly what the
// canonical creature skeleton provides.
//
// The delta is expressed in WORLD space, so it is already rig-independent:
// two rigs whose rest poses differ still agree on "the shin swung 40 deg
// about X". Conjugating it by the source rest pose was TRIED and is wrong --
// it re-introduces the source's axes and visibly breaks the legs. What the
// delta does NOT survive is being applied to the WRONG BONE, which is what
// role resolution must get right (see roleSpecificity).
//
// Rotation only: translation and scale are left at the bind pose, so a
// long-legged horse clip on a short-legged pug does not stretch the pug.
namespace CreatureMotionRetarget {

struct Result {
    bool ok = false;
    QString error;
    int tracksWritten = 0;    ///< target bones that received keyframes
    int rolesResolved = 0;    ///< canonical roles matched on the TARGET rig
    int frames = 0;
    float length = 0.0f;      ///< seconds
};

/// Write `clipQuats` (frames x jointCount x [x,y,z,w], WORLD orientations on
/// `plan`'s canonical skeleton) onto `skel` as a new animation `animName`.
///
/// The target rig must match `plan`: applying quadruped data to a rig that is
/// not a quadruped is refused rather than approximated, because the failure
/// is silent — four legs driven through two roles still animates, it just
/// animates wrongly.
Result apply(Ogre::Skeleton* skel,
             const std::string& animName,
             CreatureSkeleton::BodyPlan plan,
             const std::vector<std::vector<std::array<float, 4>>>& clipQuats,
             int fps);

} // namespace CreatureMotionRetarget

#endif // CREATURE_MOTION_RETARGET_H
