/*/////////////////////////////////////////////////////////////////////////////////
/// A QtMeshEditor file
///
/// Copyright (c) HogPog Team (www.hogpog.com.br)
///
/// The MIT License
///
/// Permission is hereby granted, free of charge, to any person obtaining a copy
/// of this software and associated documentation files (the "Software"), to deal
/// in the Software without restriction, including without limitation the rights
/// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
/// copies of the Software, and to permit persons to whom the Software is
/// furnished to do so, subject to the following conditions:
///
/// The above copyright notice and this permission notice shall be included in
/// all copies or substantial portions of the Software.
///
/// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
/// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
/// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
/// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
/// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
/// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
/// THE SOFTWARE.
////////////////////////////////////////////////////////////////////////////////*/

#include "SkeletonTransform.h"

#include <QDebug>
#include <QFile>
#include <QCoreApplication>
#include <OgreAnimationState.h>

#include "OgreXML/OgreXMLSkeletonSerializer.h"

#include "Manager.h"

SkeletonTransform::SkeletonTransform()
{
}

void SkeletonTransform::scaleSkeleton(const Ogre::Entity *_ent, const Ogre::Vector3 &_scale)
{
    if(_ent->hasSkeleton())
    {
        Ogre::Skeleton* sk = _ent->getSkeleton();

        ///Process Bone
        //Disable all animations
        Ogre::AnimationStateSet *set=_ent->getAllAnimationStates();
        if(set)
        {
            Ogre::AnimationStateIterator iter=set->getAnimationStateIterator();
            while(iter.hasMoreElements())
            {
              iter.getNext()->setEnabled(false);
            }
        }

        Manager::getSingleton()->getRoot()->renderOneFrame();

        auto bones = sk->getBones();
        for(const auto &bone : bones)
        {
            if(bone->getParent()==nullptr)
            {
                bone->setPosition(bone->getPosition()*_scale);
            }
        }
        for(const auto &bone : bones)
        {
            if(bone->getParent()!=nullptr)
            {
                bone->_setDerivedPosition(bone->_getDerivedPosition()*_scale);
            }
        }

        sk->setBindingPose();
    }
}

void SkeletonTransform::translateSkeleton(const Ogre::Entity *_ent, const Ogre::Vector3 &_translate)
{
    if(_ent->hasSkeleton())
    {
        Ogre::Skeleton* sk = _ent->getSkeleton();

        ///Process Bone

        //Disable all animations
        Ogre::AnimationStateSet *set=_ent->getAllAnimationStates();
        if(set)
        {
            Ogre::AnimationStateIterator iter=set->getAnimationStateIterator();
            while(iter.hasMoreElements())
            {
              iter.getNext()->setEnabled(false);
            }
        }

        Manager::getSingleton()->getRoot()->renderOneFrame();

        auto bones = sk->getBones();
        for(const auto &bone : bones)
        {
            if(bone->getParent()==NULL)
            {
                if(_translate.isZeroLength())
                    continue;

                bone->translate(_translate);

                sk->setBindingPose();
            }
        }
    }
}

void SkeletonTransform::rotateSkeleton(const Ogre::Entity *_ent, const Ogre::Vector3 &_rotate)
{
    if(_ent->hasSkeleton())
    {
        Ogre::Skeleton* sk = _ent->getSkeleton();

        ///Process Bone

        //Disable all animations
        Ogre::AnimationStateSet *set=_ent->getAllAnimationStates();
        if(set)
        {
            Ogre::AnimationStateIterator iter=set->getAnimationStateIterator();
            while(iter.hasMoreElements())
            {
              iter.getNext()->setEnabled(false);
            }
        }

        Manager::getSingleton()->getRoot()->renderOneFrame();

        auto bones = sk->getBones();
        for(const auto &bone : bones)
        {
            if(bone->getParent()==NULL)
            {
                if(_rotate.x!=0)
                    bone->rotate(Ogre::Quaternion(Ogre::Degree(_rotate.x), Ogre::Vector3::UNIT_Y),Ogre::Node::TS_WORLD);
                else if(_rotate.y!=0)
                    bone->rotate(Ogre::Quaternion(Ogre::Degree(_rotate.y), Ogre::Vector3::UNIT_Z),Ogre::Node::TS_WORLD);
                else if(_rotate.z!=0)
                    bone->rotate(Ogre::Quaternion(Ogre::Degree(_rotate.z), Ogre::Vector3::UNIT_X),Ogre::Node::TS_WORLD);
                else
                    continue;

                sk->setBindingPose();
            }
        }
    }
}

bool SkeletonTransform::renameAnimation(Ogre::Entity *_ent, const QString &_oldName, const QString &_newName)
{
    if(!_newName.size())
        return false;

    if(_ent && _ent->getSkeleton()->hasAnimation(_oldName.toStdString().data()))
    {
        //Disable all animations
        Ogre::AnimationStateSet *set = _ent->getAllAnimationStates();
        if(set)
        {
            for(const auto &animState: set->getAnimationStates())
                animState.second->setEnabled(false);
        }

        //Update the screen
        Manager::getSingleton()->getRoot()->renderOneFrame();
        QCoreApplication::processEvents();

        // Clone the current animation
        auto currentAnim = _ent->getSkeleton()->getAnimation(_oldName.toStdString());
        auto newAnim = _ent->getSkeleton()->createAnimation(_newName.toStdString(),currentAnim->getLength());
        unsigned short numTracks = currentAnim->getNumNodeTracks();
        for(unsigned short j=0; j<numTracks+1000; j++)
        {
            if(!currentAnim->hasNodeTrack(j)) continue;

            Ogre::NodeAnimationTrack* track = currentAnim->getNodeTrack(j);
            if(!track)
                continue;

            Ogre::NodeAnimationTrack* newTrack = newAnim->createNodeTrack(track->getHandle());
            newTrack->setAssociatedNode(track->getAssociatedNode());

            unsigned short numKeyFrames = track->getNumKeyFrames();
            for(unsigned short k=0; k<numKeyFrames; k++)
            {
                Ogre::TransformKeyFrame* keyFrame = track->getNodeKeyFrame(k);
                if(!keyFrame)
                    continue;

                Ogre::TransformKeyFrame* newKeyFrame = newTrack->createNodeKeyFrame(keyFrame->getTime());
                newKeyFrame->setTranslate(keyFrame->getTranslate());
                newKeyFrame->setRotation(keyFrame->getRotation());
                newKeyFrame->setScale(keyFrame->getScale());

                // print the keyframe
               /* Ogre::LogManager::getSingleton().logMessage("Animation: " + newAnim->getName());
                Ogre::LogManager::getSingleton().logMessage("Keyframe: " + Ogre::StringConverter::toString(keyFrame->getTime()));
                Ogre::LogManager::getSingleton().logMessage("Translate: " + Ogre::StringConverter::toString(keyFrame->getTranslate()));
                Ogre::LogManager::getSingleton().logMessage("Rotation: " + Ogre::StringConverter::toString(keyFrame->getRotation()));
                Ogre::LogManager::getSingleton().logMessage("Scale: " + Ogre::StringConverter::toString(keyFrame->getScale()));*/
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
    return false;
}
