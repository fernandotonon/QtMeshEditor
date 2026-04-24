#pragma once

#include <OgreQuaternion.h>
#include <OgreVector3.h>

namespace TransformMath {

// Builds a quaternion matching the editor's rotate-vector convention:
// only one axis is expected to be non-zero at a time (X->UNIT_Y, Y->UNIT_Z, Z->UNIT_X).
inline Ogre::Quaternion buildRotationQuat(const Ogre::Vector3 &rotate)
{
    if(rotate.x != 0)
        return {Ogre::Degree(rotate.x), Ogre::Vector3::UNIT_Y};
    if(rotate.y != 0)
        return {Ogre::Degree(rotate.y), Ogre::Vector3::UNIT_Z};
    if(rotate.z != 0)
        return {Ogre::Degree(rotate.z), Ogre::Vector3::UNIT_X};
    return Ogre::Quaternion::IDENTITY;
}

} // namespace TransformMath

