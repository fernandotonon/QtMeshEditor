#ifndef CREATURE_MOTION_EXTRACT_H
#define CREATURE_MOTION_EXTRACT_H

#include "QuadrupedSkeleton.h"

#include <QString>

#include <array>
#include <vector>

namespace Ogre { class Entity; }

// Extract skeletal animation from a CREATURE rig onto a canonical creature
// skeleton (#1073), as world-frame quaternions — the same convention the
// motion library stores for humanoids.
//
// Deliberately much simpler than AnimationMerger::extractCanonicalClips
// (~1000 lines). That function spends most of its size INFERRING a humanoid
// rig's conventions: toe chirality, hip/head up axis, shoulder-derived left
// axis, calm-frame selection — all needed because scraped humanoid rigs
// disagree wildly about their frames.
//
// Creature rigs in this corpus do not have that problem: the packs that
// carry semantic bone names (Quaternius animal/dinosaur/monster) share one
// naming convention AND one axis convention, and the ones that do not are
// refused outright by CreatureSkeleton::detectBodyPlan rather than guessed
// at. So this extractor reads the rig directly instead of reverse-
// engineering it, which is both correct here and far less to go wrong.
namespace CreatureMotionExtract {

struct Clip {
    QString animation;          ///< source animation name
    QString bodyPlan;           ///< "quadruped" | "wingedBiped"
    int frames = 0;
    int fps = 30;
    int resolvedRoles = 0;
    /// frames × jointCount × [x,y,z,w] world-space orientations; unresolved
    /// roles hold identity so the array is always rectangular.
    std::vector<std::vector<std::array<float, 4>>> quats;
    /// Per-role REFERENCE world orientation, sampled at the rig's bind pose.
    /// The retarget applies each frame as a delta against this, so a target
    /// creature of different proportions still reads correctly.
    std::vector<std::array<float, 4>> restWorld;
    /// Per-frame ROOT world translation, as a delta from frame 0, expressed
    /// in units of the source rig's HIP HEIGHT (root bind height above the
    /// mesh floor).
    ///
    /// Rotation alone cannot express a jump (the body rises), a death (it
    /// topples to the ground) or a lunge: with translation discarded the body
    /// pivots about a root pinned at its bind position and folds through its
    /// own legs. Measured across the library, death clips rotate the root by a
    /// median of 105 deg, so this is most of what those clips ARE.
    ///
    /// It is stored SCALE-NORMALISED because that is the whole reason
    /// translation was originally dropped: a horse's leap in raw world units
    /// would fling a pug into orbit. Dividing by hip height here and
    /// multiplying by the TARGET's hip height on replay makes the motion
    /// proportional to the creature, which is what "the same jump" means
    /// across body sizes.
    std::vector<std::array<float, 3>> rootOffset;
    /// Source rig hip height used for the normalisation above (world units).
    float hipHeight = 1.0f;
};

struct Result {
    bool ok = false;
    QString error;
    CreatureSkeleton::BodyPlan plan = CreatureSkeleton::BodyPlan::Quadruped;
    std::vector<Clip> clips;
};

/// Extract every skeletal animation on `entity` (or just `onlyAnimation`).
/// Detects the body plan from the rig's bone names and REFUSES rather than
/// guessing when no plan matches — an unnamed rig ("Bone.001", …) or a
/// humanoid must not be retargeted as a creature.
Result extract(Ogre::Entity* entity, int fps = 30,
               const QString& onlyAnimation = QString());

} // namespace CreatureMotionExtract

#endif // CREATURE_MOTION_EXTRACT_H
