/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef ANIMCONSTRAINTS_H
#define ANIMCONSTRAINTS_H

// Animation constraints (#525, epic #517 Slice H) — pure data + maths.
//
// A constraint drives a bone's or scene node's transform from another object
// every frame, AFTER the animation has been sampled:
//
//   look-at        rotate so an aim axis points at the target
//   ik             analytical 2-bone IK: the owner (the END bone, e.g. a hand)
//                  reaches the target by rotating its parent and grandparent;
//                  an optional pole object picks the bend direction
//   parent-of      follow the target as if parented to it (offset captured
//                  when the constraint was added), independent of the scene
//                  hierarchy
//   copy-rotation  take the target's world rotation
//   copy-position  take the target's world position (per-axis mask)
//   limit-rotation clamp the owner's LOCAL Euler XYZ angles (knees that don't
//                  bend backwards)
//
// Each has an `influence` (0..1): the result is blended with the incoming
// value. Constraints on one owner form a STACK evaluated from the bottom up,
// so the top-most constraint is applied last and wins where they conflict.
//
// Everything here is maths on plain Ogre value types and JSON, so it is
// unit-tested headless. The scene binding (per-frame evaluation, bake,
// undo, sidecar) is ConstraintManager.

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <OgreQuaternion.h>
#include <OgreVector.h>

#include <vector>

namespace AnimCon {

enum class Type { LookAt, IK, ParentOf, CopyRotation, CopyPosition, LimitRotation };

QString typeId(Type t);                        // look-at|ik|parent-of|copy-rotation|copy-position|limit-rotation
bool typeFromId(const QString& id, Type* out); // also accepts aim, ik2, child-of, …
QStringList typeIds();
QString typeLabel(Type t);
/// Types that need a target object (all but limit-rotation).
bool needsTarget(Type t);
/// Types a scene node can own (IK needs a bone chain).
bool nodeCanOwn(Type t);

/// A bone (`entity` + `bone`) or a scene node (`entity` = node name, `bone`
/// empty). String form: `node:Name` / `bone:Entity/Bone` (bone names may
/// contain ':' — only the FIRST ':' splits the kind).
struct Ref {
    QString object;
    QString bone;
    bool isBone() const { return !bone.isEmpty(); }
    bool isEmpty() const { return object.isEmpty(); }
    bool operator==(const Ref& o) const { return object == o.object && bone == o.bone; }
};
QString formatRef(const Ref& r);
bool parseRef(const QString& text, Ref* out, QString* error = nullptr);

enum class Axis { X, Y, Z, NegX, NegY, NegZ };
QString axisId(Axis a);                         // x|y|z|-x|-y|-z
bool axisFromId(const QString& id, Axis* out);
Ogre::Vector3 axisVector(Axis a);

struct Transform {
    Ogre::Vector3 position = Ogre::Vector3::ZERO;
    Ogre::Quaternion rotation = Ogre::Quaternion::IDENTITY;
    Ogre::Vector3 scale = Ogre::Vector3::UNIT_SCALE;
};
/// a ∘ b (b expressed in a's space).
Transform compose(const Transform& a, const Transform& b);
Transform inverse(const Transform& t);

struct Constraint {
    QString id;
    QString name;
    Type type = Type::LookAt;
    Ref owner;
    Ref target;
    Ref pole;                        ///< IK only; empty = keep the current bend plane
    bool enabled = true;
    double influence = 1.0;          ///< 0..1

    // look-at
    Axis aimAxis = Axis::NegZ;          ///< Ogre forward (cameras/lights look down -Z)
    Axis upAxis = Axis::Y;

    // copy-position
    bool useX = true, useY = true, useZ = true;

    // parent-of: owner world = target world ∘ offset (captured when added)
    Transform offset;
    bool hasOffset = false;

    // limit-rotation (degrees, local Euler XYZ)
    bool limitX = true, limitY = true, limitZ = true;
    Ogre::Vector3 minDeg = Ogre::Vector3(-45, -45, -45);
    Ogre::Vector3 maxDeg = Ogre::Vector3(45, 45, 45);
};

QString displayName(const Constraint& c);

// ---------------------------------------------------------------------------
// Solvers (world space unless said otherwise)
// ---------------------------------------------------------------------------

/// World rotation whose `aim` axis points from `from` to `to`, with the
/// owner's `up` axis as close as possible to `worldUp`. Returns `current`
/// when the two points coincide.
Ogre::Quaternion aimRotation(const Ogre::Vector3& from, const Ogre::Vector3& to, Axis aim, Axis up,
                             const Ogre::Vector3& worldUp, const Ogre::Quaternion& current);

/// Analytical 2-bone IK. A = root joint (e.g. shoulder), B = mid joint
/// (elbow), C = end (wrist), T = target, P = pole point (only its direction
/// from A matters; pass `hasPole=false` to keep the current bend plane).
/// Returns the WORLD delta rotations to pre-multiply onto the root and mid
/// bones' world orientations. `reached` is false when the target is out of
/// reach (the chain then points straight at it).
struct IkSolution {
    Ogre::Quaternion rootDelta = Ogre::Quaternion::IDENTITY;
    Ogre::Quaternion midDelta = Ogre::Quaternion::IDENTITY;
    Ogre::Vector3 newMid = Ogre::Vector3::ZERO;
    Ogre::Vector3 newEnd = Ogre::Vector3::ZERO;
    bool reached = true;
    bool valid = false;
};
IkSolution solveTwoBone(const Ogre::Vector3& A, const Ogre::Vector3& B, const Ogre::Vector3& C,
                        const Ogre::Vector3& T, const Ogre::Vector3& P, bool hasPole);

/// Clamp a LOCAL rotation's Euler XYZ angles (degrees) on the enabled axes.
Ogre::Quaternion limitRotation(const Ogre::Quaternion& local, const Constraint& c);

/// influence blend: lerp positions, slerp (shortest) rotations.
Ogre::Vector3 blend(const Ogre::Vector3& from, const Ogre::Vector3& to, double t);
Ogre::Quaternion blend(const Ogre::Quaternion& from, const Ogre::Quaternion& to, double t);

// ---------------------------------------------------------------------------
// JSON — schema `qtmesh-anim-constraints-v1`
// ---------------------------------------------------------------------------

QString schemaId();
QJsonObject toJson(const Constraint& c);
bool fromJson(const QJsonObject& o, Constraint* out, QString* error = nullptr);
QJsonObject toDocument(const std::vector<Constraint>& cs);
bool fromDocument(const QJsonObject& doc, std::vector<Constraint>* out, QString* error = nullptr);

/// `key=value` overrides (CLI / MCP / panel): influence, enabled, aim, up,
/// x, y, z (copy-position axes), limit_x|y|z, min_x|y|z, max_x|y|z, target,
/// pole, name. Unknown keys → error.
bool applyParam(Constraint* c, const QString& key, const QString& value, QString* error = nullptr);

QString uniqueId(const std::vector<Constraint>& existing);

} // namespace AnimCon

#endif // ANIMCONSTRAINTS_H
