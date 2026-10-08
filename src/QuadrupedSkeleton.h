#ifndef QUADRUPED_SKELETON_H
#define QUADRUPED_SKELETON_H

#include <QString>
#include <QStringList>

// Canonical QUADRUPED skeleton for creature motion (#1073).
//
// The humanoid canonical skeleton (MotionInbetween, 22 joints: hips/spine/
// neck/head + 2 arms + 2 legs) cannot represent a four-legged animal. Its
// bone mapper matches "upleg" with no front/back distinction, so on a real
// quadruped rig FrontUpLeg.R and BackUpLeg.R BOTH resolve to the same
// canonical joint: four legs collapse onto two roles, the front legs end up
// driven by hind-leg rotations, and the tail has no role at all. That is the
// "body moves correctly, legs move sideways" failure.
//
// So creatures get their OWN canonical skeleton rather than a widened
// humanoid one — a widened one would change the humanoid channel count and
// invalidate the trained RMIB model (220 channels = 22 joints x 10 DoF).
//
// Layout (18 joints). Ordered spine-first, then limbs front-to-back, then
// tail, so a truncated/partial rig still fills a sensible prefix:
//
//   0  root        pelvis/hips — the anchor, like the humanoid hip
//   1  spine       lower back
//   2  chest       upper back / shoulder girdle
//   3  neck
//   4  head
//   5  frontUpLeg.L   6  frontLowLeg.L   7  frontFoot.L
//   8  frontUpLeg.R   9  frontLowLeg.R  10  frontFoot.R
//  11  backUpLeg.L   12  backLowLeg.L   13  backFoot.L
//  14  backUpLeg.R   15  backLowLeg.R   16  backFoot.R
//  17  tail1
//
// Deliberately NOT modelled yet: multi-segment tails beyond the first joint,
// wings, ears, horns. The reference corpus (Quaternius CC0 animal packs)
// animates Tail1..4 but the later segments are follow-through that a single
// tail root approximates; wings appear in too few assets to canonicalise
// without guessing. Both are additive later — the joint list is append-only
// so adding index 18+ does not renumber anything.
// Body plans a creature rig can take. Each has its own canonical joint list
// because the limb COUNT differs — a widened single skeleton would leave most
// joints unmapped for every creature and make "how many roles resolved" a
// useless plausibility signal.
//
// Only plans with enough semantically-named rigs in the corpus to validate
// against are modelled. A survey of 25 packs found only 11 with semantic bone
// names at all (the rest are "Bone.001", "Bone.002" …, which no name-based
// mapper can read), so birds/fish/insects are deliberately absent until
// either better-named assets or a geometric classifier exists — guessing a
// skeleton for them would produce exactly the silent mis-retarget this
// module was written to eliminate.
namespace CreatureSkeleton {

enum class BodyPlan {
    Quadruped,    ///< 4 legs + optional tail (horse, cow, dog, …)
    WingedBiped,  ///< 2 wings + 2 legs + optional tail (dragon, bat)
};

/// Detect the body plan a rig's bone names describe, or nullopt-ish: returns
/// Quadruped only when the quadruped plausibility test passes, WingedBiped
/// when wing bones are present with two legs. `ok` reports whether ANY plan
/// matched — a false means the caller must not take a creature path.
BodyPlan detectBodyPlan(const QStringList& boneNames, bool* ok);

namespace WingedBiped {
/// Canonical winged-biped joints (see kWingedJoints in the .cpp).
int jointCount();
QString jointName(int i);
int parentOf(int i);
int indexForBone(const QString& boneName);

/// See the quadruped overload.
int roleSpecificity(const QString& boneName);
/// Needs the spine anchor plus both wings — legs are optional (a dragon in
/// flight may have stubby or absent leg bones), but a rig with no wings is
/// not this plan.
bool isPlausible(const bool* resolvedRoles);
} // namespace WingedBiped

namespace QuadrupedSkeleton {

/// Number of canonical quadruped joints.
int jointCount();

/// Canonical joint name at `i` (0 <= i < jointCount()).
QString jointName(int i);

/// Parent joint index, or -1 for the root.
int parentOf(int i);

/// Map an arbitrary rig bone name to a canonical quadruped joint, or -1.
///
/// Case-insensitive; tolerates the common side spellings (.L/.R, _l/_r,
/// Left/Right) and rig prefixes. The FRONT/BACK distinction is what the
/// humanoid mapper lacks and is the whole point of this function, so it is
/// resolved BEFORE the generic leg matching.
int indexForBone(const QString& boneName);

/// How EXPLICIT a bone's claim to its role is (higher wins a tie).
///
/// Several bones can map to one role, and taking whichever the skeleton
/// lists first is wrong: the Quaternius rigs carry BOTH `FrontLeg.L` and
/// `FrontLowLeg.L`, and `FrontLeg.L` (a bare name with no segment word,
/// which indexForBone can only guess is the lower leg) is listed first. It
/// therefore stole the lower-leg role while the explicitly named
/// `FrontLowLeg.L` was dropped, so the clip's shin rotation drove the wrong
/// bone and swung the hoof sideways -- the reported "feet moving sideways".
///
/// 2 = names its segment outright ("FrontLowLeg", "FrontFoot", "Hips")
/// 1 = matched by a general keyword ("Body" -> spine)
/// 0 = a bare fallback with no segment word ("FrontLeg")
int roleSpecificity(const QString& boneName);

/// True when `resolved` of jointCount() roles is enough to retarget: the
/// spine chain plus at least three of the four legs. A rig that resolves
/// fewer is not a quadruped and must fall back rather than produce the
/// sideways-legs result this module exists to prevent.
bool isPlausibleQuadruped(const bool* resolvedRoles);

} // namespace QuadrupedSkeleton

} // namespace CreatureSkeleton

#endif // QUADRUPED_SKELETON_H
