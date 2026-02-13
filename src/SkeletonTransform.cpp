/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

#include "SkeletonTransform.h"

#include <QDebug>
#include <QFile>
#include <QCoreApplication>
#include <OgreAnimationState.h>

#include "OgreXML/OgreXMLSkeletonSerializer.h"

#include "Manager.h"

namespace {

void disableAnimationsAndRender(const Ogre::Entity *ent)
{
    auto *set = ent->getAllAnimationStates();
    if(set)
    {
        for(const auto &[name, state] : set->getAnimationStates())
        {
            state->setEnabled(false);
        }
    }
    Manager::getSingleton()->getRoot()->renderOneFrame();
}

Ogre::Quaternion buildRotationQuat(const Ogre::Vector3 &rotate)
{
    if(rotate.x != 0)
        return {Ogre::Degree(rotate.x), Ogre::Vector3::UNIT_Y};
    if(rotate.y != 0)
        return {Ogre::Degree(rotate.y), Ogre::Vector3::UNIT_Z};
    if(rotate.z != 0)
        return {Ogre::Degree(rotate.z), Ogre::Vector3::UNIT_X};
    return Ogre::Quaternion::IDENTITY;
}

} // anonymous namespace

void SkeletonTransform::scaleSkeleton(const Ogre::Entity *_ent, const Ogre::Vector3 &_scale)
{
    if(!_ent->hasSkeleton()) return;

    auto *sk = _ent->getSkeleton();
    disableAnimationsAndRender(_ent);

    const auto &bones = sk->getBones();
    for(const auto &bone : bones)
    {
        if(bone->getParent() == nullptr)
            bone->setPosition(bone->getPosition() * _scale);
    }
    for(const auto &bone : bones)
    {
        if(bone->getParent() != nullptr)
            bone->_setDerivedPosition(bone->_getDerivedPosition() * _scale);
    }

    sk->setBindingPose();
}

void SkeletonTransform::translateSkeleton(const Ogre::Entity *_ent, const Ogre::Vector3 &_translate)
{
    if(!_ent->hasSkeleton()) return;

    auto *sk = _ent->getSkeleton();
    disableAnimationsAndRender(_ent);

    const auto &bones = sk->getBones();
    for(const auto &bone : bones)
    {
        if(bone->getParent() != nullptr) continue;
        if(_translate.isZeroLength()) continue;

        bone->translate(_translate);
    }
    sk->setBindingPose();
}

void SkeletonTransform::rotateSkeleton(const Ogre::Entity *_ent, const Ogre::Vector3 &_rotate)
{
    rotateSkeleton(_ent, buildRotationQuat(_rotate));
}

void SkeletonTransform::rotateSkeleton(const Ogre::Entity *_ent, const Ogre::Quaternion &_quat)
{
    if(!_ent->hasSkeleton()) return;
    if(_quat == Ogre::Quaternion::IDENTITY) return;

    auto *sk = _ent->getSkeleton();
    disableAnimationsAndRender(_ent);

    // Use the same pivot as MeshTransform::rotateMesh (mesh bounding box center)
    auto meshCenter = _ent->getMesh()->getBounds().getCenter();

    const auto &bones = sk->getBones();
    for(const auto &bone : bones)
    {
        if(bone->getParent() != nullptr) continue;

        // Rotate position around mesh center (matches vertex rotation pivot)
        bone->setPosition(_quat * (bone->getPosition() - meshCenter) + meshCenter);
        // Rotate orientation
        bone->rotate(_quat, Ogre::Node::TS_WORLD);
    }
    sk->setBindingPose();
}

bool SkeletonTransform::renameAnimation(Ogre::Entity *_ent, const QString &_oldName, const QString &_newName)
{
    if(_newName.isEmpty())
        return false;

    if(!_ent || !_ent->getSkeleton()->hasAnimation(_oldName.toStdString().data()))
        return false;

    disableAnimationsAndRender(_ent);
    QCoreApplication::processEvents();

    // Clone the current animation
    auto *currentAnim = _ent->getSkeleton()->getAnimation(_oldName.toStdString());
    auto *newAnim = _ent->getSkeleton()->createAnimation(_newName.toStdString(), currentAnim->getLength());
    const int trackSearchLimit = static_cast<int>(currentAnim->getNumNodeTracks()) + 1000;
    for(int j = 0; j < trackSearchLimit; j++)
    {
        if(!currentAnim->hasNodeTrack(j)) continue;

        auto *track = currentAnim->getNodeTrack(j);
        if(!track) continue;

        auto *newTrack = newAnim->createNodeTrack(track->getHandle());
        newTrack->setAssociatedNode(track->getAssociatedNode());

        const auto numKeyFrames = track->getNumKeyFrames();
        for(unsigned short k = 0; k < numKeyFrames; k++)
        {
            const auto *keyFrame = track->getNodeKeyFrame(k);
            if(!keyFrame) continue;

            auto *newKeyFrame = newTrack->createNodeKeyFrame(keyFrame->getTime());
            newKeyFrame->setTranslate(keyFrame->getTranslate());
            newKeyFrame->setRotation(keyFrame->getRotation());
            newKeyFrame->setScale(keyFrame->getScale());
        }
    }

    //Remove the old animation
    _ent->getSkeleton()->removeAnimation(_oldName.toStdString());

    // Update the animations
    _ent->refreshAvailableAnimationState();
    _ent->_updateAnimation();
    _ent->getMesh().get()->_dirtyState();

    //Update the screen
    Manager::getSingleton()->getRoot()->renderOneFrame();
    QCoreApplication::processEvents();

    return true;
}
