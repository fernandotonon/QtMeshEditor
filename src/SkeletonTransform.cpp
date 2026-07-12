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
#include "AnimationMerger.h"

#include <QDebug>
#include <QFile>
#include <QCoreApplication>
#include <OgreAnimationState.h>

#include "OgreXML/OgreXMLSkeletonSerializer.h"

#include <string>
#include <vector>

#include "Manager.h"
#include "TransformMath.h"

void SkeletonTransform::scaleSkeleton(const Ogre::Entity *_ent, const Ogre::Vector3 &_scale)
{
    if(!_ent->hasSkeleton()) return;

    auto *sk = _ent->getSkeleton();
    sk->reset(true);

    // Scale every bone's local position so the hierarchy propagates correctly
    for(const auto &bone : sk->getBones())
        bone->setPosition(bone->getPosition() * _scale);

    sk->setBindingPose();

    // Mark animation state dirty so Ogre refreshes its skinning buffers
    if(auto *animSet = _ent->getAllAnimationStates())
        animSet->_notifyDirty();
}

void SkeletonTransform::translateSkeleton(const Ogre::Entity *_ent, const Ogre::Vector3 &_translate)
{
    if(!_ent->hasSkeleton()) return;

    auto *sk = _ent->getSkeleton();
    sk->reset(true);

    const auto &bones = sk->getBones();
    for(const auto &bone : bones)
    {
        if(bone->getParent() != nullptr) continue;
        if(_translate.isZeroLength()) continue;

        bone->translate(_translate);
    }
    sk->setBindingPose();

    if(auto *animSet = _ent->getAllAnimationStates())
        animSet->_notifyDirty();
}

void SkeletonTransform::rotateSkeleton(const Ogre::Entity *_ent, const Ogre::Vector3 &_rotate)
{
    auto pivot = _ent->getMesh()->getBounds().getCenter();
    rotateSkeleton(_ent, TransformMath::buildRotationQuat(_rotate), pivot);
}

void SkeletonTransform::rotateSkeleton(const Ogre::Entity *_ent, const Ogre::Quaternion &_quat,
                                       const Ogre::Vector3 &_pivot)
{
    if(!_ent->hasSkeleton()) return;
    if(_quat == Ogre::Quaternion::IDENTITY) return;

    auto *sk = _ent->getSkeleton();
    sk->reset(true);

    const auto &bones = sk->getBones();
    for(const auto &bone : bones)
    {
        if(bone->getParent() != nullptr) continue;

        // Rotate position around the same pivot used for mesh vertices
        bone->setPosition(_quat * (bone->getPosition() - _pivot) + _pivot);
        // Rotate orientation
        bone->rotate(_quat, Ogre::Node::TS_WORLD);
    }
    sk->setBindingPose();

    if(auto *animSet = _ent->getAllAnimationStates())
        animSet->_notifyDirty();
}

bool SkeletonTransform::renameAnimation(Ogre::Entity *_ent, const QString &_oldName, const QString &_newName)
{
    if(_newName.isEmpty())
        return false;

    if(!_ent || !_ent->getSkeleton()->hasAnimation(_oldName.toStdString().data()))
        return false;

    auto *sk = _ent->getSkeleton();

    // Save full animation state (enabled, time, loop) before the rename
    struct AnimStateInfo { bool enabled; Ogre::Real timePos; bool loop; };
    std::map<std::string, AnimStateInfo> savedStates;
    if(auto *animSet = _ent->getAllAnimationStates())
    {
        for(const auto &[name, state] : animSet->getAnimationStates())
            savedStates[name] = {state->getEnabled(), state->getTimePosition(), state->getLoop()};
    }

    // Clone the current animation with the new name
    auto *currentAnim = sk->getAnimation(_oldName.toStdString());
    auto *newAnim = sk->createAnimation(_newName.toStdString(), currentAnim->getLength());
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

    // #854: keep the arm-space widen/tuck value tied to the renamed clip
    // (the keyframes were copied above; migrate the tracked angle too).
    AnimationMerger::migrateArmSpaceKey(
        sk, _oldName.toStdString(), _newName.toStdString());

    //Remove the old animation
    sk->removeAnimation(_oldName.toStdString());

    // Rebuild animation states from the skeleton
    _ent->refreshAvailableAnimationState();

    // Remove stale animation state if the master skeleton re-created it
    auto *animSet = _ent->getAllAnimationStates();
    if(animSet && animSet->hasAnimationState(_oldName.toStdString()))
        animSet->removeAnimationState(_oldName.toStdString());

    // Restore animation states (mapping old name to new name)
    if(animSet)
    {
        for(const auto &[name, info] : savedStates)
        {
            auto stateName = (name == _oldName.toStdString()) ? _newName.toStdString() : name;
            if(animSet->hasAnimationState(stateName))
            {
                auto *state = animSet->getAnimationState(stateName);
                state->setEnabled(info.enabled);
                state->setTimePosition(info.timePos);
                state->setLoop(info.loop);
            }
        }
    }

    return true;
}
