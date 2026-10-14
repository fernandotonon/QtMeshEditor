/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "PoseMirror.h"

#include <QSet>

namespace PoseMirror {

QString planeId(Plane p)
{
    switch (p) {
    case Plane::X: return QStringLiteral("x");
    case Plane::Y: return QStringLiteral("y");
    case Plane::Z: return QStringLiteral("z");
    }
    return QStringLiteral("x");
}

bool planeFromId(const QString& id, Plane* out)
{
    const QString s = id.trimmed().toLower();
    Plane p;
    if (s == QLatin1String("x")) p = Plane::X;
    else if (s == QLatin1String("y")) p = Plane::Y;
    else if (s == QLatin1String("z")) p = Plane::Z;
    else return false;
    if (out) *out = p;
    return true;
}

Ogre::Vector3 planeNormal(Plane p)
{
    switch (p) {
    case Plane::X: return Ogre::Vector3::UNIT_X;
    case Plane::Y: return Ogre::Vector3::UNIT_Y;
    case Plane::Z: return Ogre::Vector3::UNIT_Z;
    }
    return Ogre::Vector3::UNIT_X;
}

Ogre::Vector3 mirrorPosition(const Ogre::Vector3& p, Plane plane)
{
    Ogre::Vector3 out = p;
    switch (plane) {
    case Plane::X: out.x = -out.x; break;
    case Plane::Y: out.y = -out.y; break;
    case Plane::Z: out.z = -out.z; break;
    }
    return out;
}

Ogre::Quaternion mirrorRotation(const Ogre::Quaternion& q, Plane plane)
{
    // Reflecting an orientation across a plane with unit normal n: keep the
    // rotation component ABOUT n, negate the two components perpendicular to
    // it. (Reflecting the rotation matrix directly would give determinant −1,
    // i.e. no longer a rotation.)
    switch (plane) {
    case Plane::X: return Ogre::Quaternion(q.w,  q.x, -q.y, -q.z);
    case Plane::Y: return Ogre::Quaternion(q.w, -q.x,  q.y, -q.z);
    case Plane::Z: return Ogre::Quaternion(q.w, -q.x, -q.y,  q.z);
    }
    return q;
}

BoneTransform mirrorTransform(const BoneTransform& t, const MirrorOptions& opts)
{
    BoneTransform out;
    out.translate = mirrorPosition(t.translate, opts.plane);
    out.rotation = mirrorRotation(t.rotation, opts.plane);
    out.scale = t.scale;
    if (opts.negateScaleAlongNormal) {
        switch (opts.plane) {
        case Plane::X: out.scale.x = -out.scale.x; break;
        case Plane::Y: out.scale.y = -out.scale.y; break;
        case Plane::Z: out.scale.z = -out.scale.z; break;
        }
    }
    return out;
}

Pose mirror(const Pose& pose,
            const BoneNaming::Map& naming,
            const MirrorOptions& opts,
            MirrorReport* report)
{
    Pose out;
    MirrorReport rep;

    // Destination names already written, so a second bone mapping onto the
    // same destination is REPORTED instead of silently overwriting the first
    // (the pre-slice-E `mirrored.insert(flipped, …)` lost data that way).
    QSet<QString> written;

    // Deterministic iteration order: QHash iteration order is unspecified, so
    // without sorting, WHICH bone wins a collision would vary between runs and
    // the collision report would be nondeterministic.
    QStringList names = pose.keys();
    names.sort();

    for (const QString& name : names) {
        const BoneTransform& src = pose.value(name);
        const QString other = naming.counterpart(name);

        if (other.isEmpty()) {
            // Centre bone: no counterpart to swap with.
            if (opts.mirrorCenterBonesInPlace) {
                out.insert(name, mirrorTransform(src, opts));
                written.insert(name);
                ++rep.centerBonesMirrored;
            } else {
                out.insert(name, src);
                written.insert(name);
                ++rep.untouched;
            }
            continue;
        }

        if (!pose.contains(other)) {
            // Sided bone whose partner isn't in this pose. Keep its own value
            // rather than dropping the bone, and report the partial mirror.
            rep.unpairedSided.push_back(name);
            if (!written.contains(name)) {
                out.insert(name, src);
                written.insert(name);
            }
            ++rep.untouched;
            continue;
        }

        if (written.contains(other)) {
            rep.collisions.push_back(name);
            continue;
        }
        out.insert(other, mirrorTransform(src, opts));
        written.insert(other);
        ++rep.pairedBonesMirrored;
    }

    if (report) *report = rep;
    return out;
}

QStringList mirrorSelection(const QStringList& boneNames,
                            const BoneNaming::Map& naming,
                            int* matched)
{
    QStringList out;
    int hits = 0;
    for (const QString& n : boneNames) {
        const QString other = naming.counterpart(n);
        if (other.isEmpty()) continue;
        ++hits;
        if (!out.contains(other)) out.push_back(other);
    }
    if (matched) *matched = hits;
    return out;
}

} // namespace PoseMirror
