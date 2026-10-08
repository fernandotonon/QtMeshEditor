#ifndef CREATURE_MOTION_RETARGET_H
#define CREATURE_MOTION_RETARGET_H

#include "QuadrupedSkeleton.h"

#include <QString>

#include <array>
#include <vector>

namespace Ogre { class Skeleton; class Entity; }

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
// ROOT TRANSLATION is carried; every other joint is rotation-only, so a
// long-legged horse clip on a short-legged pug does not stretch the pug.
//
// Rotation alone cannot express a jump (the body rises), a death (it topples
// to the ground) or a lunge: with translation discarded the body pivots about
// a root pinned at its bind position and folds through its own legs. So the
// ROOT bone also receives a translation track, taken from the clip's
// hip-height-normalised `rootOffset` and multiplied by the TARGET's hip
// height -- "rose by one hip height" transfers across body sizes, raw world
// units do not. Pass `targetHipHeight`; 0 disables translation (the legacy
// rotation-only behaviour).
namespace CreatureMotionRetarget {

struct Result {
    bool ok = false;
    QString error;
    int tracksWritten = 0;    ///< target bones that received keyframes
    int rolesResolved = 0;    ///< canonical roles matched on the TARGET rig
    int frames = 0;
    float length = 0.0f;      ///< seconds
    /// True when the root received a translation track (clip carried the
    /// channel and a target hip height was supplied).
    bool rootTranslation = false;
};

/// Write `clipQuats` (frames x jointCount x [x,y,z,w], WORLD orientations on
/// `plan`'s canonical skeleton) onto `skel` as a new animation `animName`.
///
/// The target rig must match `plan`: applying quadruped data to a rig that is
/// not a quadruped is refused rather than approximated, because the failure
/// is silent — four legs driven through two roles still animates, it just
/// animates wrongly.
/// Measure a TARGET entity's hip height: the root bone's height above the
/// mesh floor, in world units. This is the yardstick the clip's normalised
/// `rootOffset` is multiplied by, so the same motion reads correctly on a
/// horse and on a pug. Returns 0 when the rig has no resolvable root, which
/// disables translation rather than guessing a scale.
float hipHeightOf(Ogre::Entity* entity, CreatureSkeleton::BodyPlan plan);

/// `srcRestWorld` is the SOURCE rig's per-role BIND world orientation, which
/// the clip's frames are deltas against. Supplying it matters: these clips do
/// NOT start at rest -- Horse|Walk's frame 0 is 29 deg into the stride -- so
/// deltaing against frame 0 re-centres the whole cycle on a mid-stride pose
/// and the legs sit bunched under the body in "weird positions" while the
/// measured range of motion still matches the source exactly. Empty falls
/// back to the legacy frame-0 behaviour.
Result apply(Ogre::Skeleton* skel,
             const std::string& animName,
             CreatureSkeleton::BodyPlan plan,
             const std::vector<std::vector<std::array<float, 4>>>& clipQuats,
             int fps,
             const std::vector<std::array<float, 4>>& srcRestWorld = {},
             const std::vector<std::array<float, 3>>& rootOffset = {},
             float targetHipHeight = 0.0f);

} // namespace CreatureMotionRetarget

#endif // CREATURE_MOTION_RETARGET_H
