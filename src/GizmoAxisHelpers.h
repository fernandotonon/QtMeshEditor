#ifndef GIZMOAXISHELPERS_H
#define GIZMOAXISHELPERS_H

#include <Ogre.h>

namespace GizmoAxisHelpers {

enum class Axis {
    X,
    Y,
    Z,
    None
};

template <typename Fn>
inline void forEachAxis(Ogre::ManualObject* xAxis,
                        Ogre::ManualObject* yAxis,
                        Ogre::ManualObject* zAxis,
                        Fn&& fn)
{
    fn(xAxis);
    fn(yAxis);
    fn(zAxis);
}

template <typename Fn>
inline void forEachAxisIndexed(Ogre::ManualObject* xAxis,
                               Ogre::ManualObject* yAxis,
                               Ogre::ManualObject* zAxis,
                               Fn&& fn)
{
    fn(Axis::X, xAxis);
    fn(Axis::Y, yAxis);
    fn(Axis::Z, zAxis);
}

inline Axis axisFromObject(const Ogre::MovableObject* obj,
                           const Ogre::ManualObject* xAxis,
                           const Ogre::ManualObject* yAxis,
                           const Ogre::ManualObject* zAxis)
{
    if (obj == static_cast<const Ogre::MovableObject*>(xAxis)) {
        return Axis::X;
    }
    if (obj == static_cast<const Ogre::MovableObject*>(yAxis)) {
        return Axis::Y;
    }
    if (obj == static_cast<const Ogre::MovableObject*>(zAxis)) {
        return Axis::Z;
    }
    return Axis::None;
}

inline Ogre::Vector3 axisToUnitVector(Axis axis)
{
    switch (axis) {
    case Axis::X:
        return Ogre::Vector3::UNIT_X;
    case Axis::Y:
        return Ogre::Vector3::UNIT_Y;
    case Axis::Z:
        return Ogre::Vector3::UNIT_Z;
    case Axis::None:
        break;
    }

    return Ogre::Vector3::ZERO;
}

inline Ogre::AxisAlignedBox makeAxisBoundingBox(Axis axis,
                                                Ogre::Real axisMin,
                                                Ogre::Real axisMax,
                                                Ogre::Real sideExtent)
{
    Ogre::AxisAlignedBox boundingBox;
    switch (axis) {
    case Axis::X:
        boundingBox.setExtents(Ogre::Vector3(axisMin, -sideExtent, -sideExtent),
                               Ogre::Vector3(axisMax,  sideExtent,  sideExtent));
        break;
    case Axis::Y:
        boundingBox.setExtents(Ogre::Vector3(-sideExtent, axisMin, -sideExtent),
                               Ogre::Vector3( sideExtent, axisMax,  sideExtent));
        break;
    case Axis::Z:
        boundingBox.setExtents(Ogre::Vector3(-sideExtent, -sideExtent, axisMin),
                               Ogre::Vector3( sideExtent,  sideExtent, axisMax));
        break;
    case Axis::None:
        break;
    }
    return boundingBox;
}

template <typename OnX, typename OnY, typename OnZ, typename OnNone>
inline void dispatchAxis(Axis axis, OnX&& onX, OnY&& onY, OnZ&& onZ, OnNone&& onNone)
{
    switch (axis) {
    case Axis::X:
        onX();
        break;
    case Axis::Y:
        onY();
        break;
    case Axis::Z:
        onZ();
        break;
    case Axis::None:
        onNone();
        break;
    }
}

} // namespace GizmoAxisHelpers

#endif // GIZMOAXISHELPERS_H
