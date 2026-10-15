/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef POSEMIRROR_H
#define POSEMIRROR_H

#include <QHash>
#include <QString>
#include <QStringList>

#include <OgreQuaternion.h>
#include <OgreVector.h>

#include "BoneNamingMap.h"

/**
 * @brief Pose mirroring across a symmetry plane (Skel slice E, #559).
 *
 * The operation half of slice E: `BoneNamingMap` answers "what is the
 * opposite-side bone called", this reflects the actual transforms and moves
 * them onto their counterparts.
 *
 * Pure data — takes and returns bone-name→TRS maps, touches no Ogre scene, no
 * skeleton, no singleton. That keeps the reflection math unit-testable
 * headlessly and lets three callers share one definition:
 * `PoseLibrary::mirrorPose` (#521 pose mirroring), `SkinWeightController`
 * weight-mirror (slice D #558), and the slice B mirror-chain (#556).
 *
 * ## The reflection
 *
 * Mirroring a pose across a plane is NOT the same as negating one component of
 * everything. A reflection is an improper (determinant −1) transform, so:
 *
 *   - **Translation** reflects normally: the component along the plane normal
 *     is negated.
 *   - **Rotation** cannot simply be reflected — a reflected rotation matrix has
 *     determinant −1 and is no longer a rotation. The correct conjugation for a
 *     plane with unit normal `n` keeps the quaternion's component about `n` and
 *     negates the two perpendicular ones. For n = +X that is
 *     `(w,x,y,z) → (w,x,−y,−z)`, which is exactly what the pre-slice-E
 *     `PoseLibrary::mirrorPose` hardcoded; `mirrorRotation` generalises it to
 *     any axis so the Y/Z planes the issue asks for behave correctly instead
 *     of silently reusing the X formula.
 *   - **Scale** is left ALONE. The old code negated scale.x "so the volume
 *     stays right", but a bone's local scale is not what carries the
 *     handedness flip — the rotation conjugation already does that. Negating a
 *     scale component on a mirrored pair yields inside-out normals on the
 *     mirrored half, and on a CENTRE bone (mirrored in place) it flips the
 *     bone every time the op runs, so mirroring twice would not be a no-op.
 *     `MirrorOptions::negateScaleAlongNormal` restores the legacy behaviour
 *     for anyone who depended on it.
 */
namespace PoseMirror {

/// The symmetry plane. Character rigs are overwhelmingly X-symmetric (the
/// sagittal plane), which is why it is the default everywhere.
enum class Plane {
    X = 0, ///< YZ plane, normal +X — the sagittal plane of a character rig
    Y,     ///< XZ plane, normal +Y
    Z,     ///< XY plane, normal +Z
};

/// Stable id for CLI/MCP/settings round-tripping.
QString planeId(Plane p);
/// Parse a plane id ("x"/"y"/"z"). Returns false on anything else rather than
/// defaulting — a typo silently mirroring across the wrong plane produces a
/// plausible-looking but wrong pose that is very hard to spot.
bool planeFromId(const QString& id, Plane* out);

/// Unit normal of the plane.
Ogre::Vector3 planeNormal(Plane p);

/// One bone's local transform. Mirrors `PoseLibrary`'s private snapshot shape
/// so conversion is a field copy.
struct BoneTransform {
    Ogre::Vector3 translate{Ogre::Vector3::ZERO};
    Ogre::Quaternion rotation{Ogre::Quaternion::IDENTITY};
    Ogre::Vector3 scale{Ogre::Vector3(1, 1, 1)};
};

/// bone name → local TRS.
using Pose = QHash<QString, BoneTransform>;

struct MirrorOptions {
    Plane plane = Plane::X;
    /// Reflect bones with no detected counterpart in place (the issue's
    /// "unpaired bones are reflected across the mirror plane in-place").
    /// Turning this off leaves centre bones exactly as they were, which is
    /// what you want when mirroring only one limb.
    bool mirrorCenterBonesInPlace = true;
    /// Legacy compatibility: also negate the scale component along the plane
    /// normal. Off by default — see the namespace comment for why.
    bool negateScaleAlongNormal = false;
};

struct MirrorReport {
    int pairedBonesMirrored = 0;  ///< bones moved onto a counterpart
    int centerBonesMirrored = 0;  ///< bones reflected in place
    int untouched = 0;            ///< left as-is
    /// Sided bones whose counterpart was absent from the pose. Their values
    /// are kept in place rather than dropped, and reported so a caller can
    /// tell the user the mirror was partial.
    QStringList unpairedSided;
    /// Two source bones that mapped onto the same destination. The FIRST wins
    /// and this lists the losers, because silently overwriting is how the
    /// pre-slice-E hash-insert lost data.
    QStringList collisions;
};

/// Reflect one position across the plane through the origin.
Ogre::Vector3 mirrorPosition(const Ogre::Vector3& p, Plane plane);

/// Reflect an orientation across the plane: keep the rotation component about
/// the plane normal, negate the two perpendicular components.
Ogre::Quaternion mirrorRotation(const Ogre::Quaternion& q, Plane plane);

/// Reflect one bone transform (no name mapping).
BoneTransform mirrorTransform(const BoneTransform& t, const MirrorOptions& opts);

/**
 * @brief Mirror a whole pose.
 *
 * For every bone: reflect its transform, then place the result under its
 * counterpart's name (so a left-hand gesture lands on the right hand). Bones
 * with no counterpart are reflected in place when
 * `opts.mirrorCenterBonesInPlace` is set.
 *
 * The returned pose contains an entry for every input bone that could be
 * placed; `report` (optional) explains what happened to the rest.
 */
Pose mirror(const Pose& pose,
            const BoneNaming::Map& naming,
            const MirrorOptions& opts = {},
            MirrorReport* report = nullptr);

/// Mirror-selection: map a set of selected bone names to their counterparts.
/// Bones with no counterpart are DROPPED (selecting a centre bone's "mirror"
/// would just reselect itself, which reads as the op having done nothing).
/// `matched` reports how many names actually flipped.
QStringList mirrorSelection(const QStringList& boneNames,
                            const BoneNaming::Map& naming,
                            int* matched = nullptr);

} // namespace PoseMirror

#endif // POSEMIRROR_H
