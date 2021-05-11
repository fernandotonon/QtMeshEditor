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

        Ogre::Skeleton::BoneIterator it = sk->getBoneIterator();
        while (it.hasMoreElements())
        {
            Ogre::Bone* bone = it.getNext();

            if(bone->getParent()==NULL)
            {
                bone->setPosition(bone->getPosition()*_scale);
            }
        }
        it = sk->getBoneIterator();
        while (it.hasMoreElements())
        {
            Ogre::Bone* bone = it.getNext();

            if(bone->getParent()!=NULL)
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
        Ogre::Skeleton::BoneIterator it = sk->getBoneIterator();

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

        while (it.hasMoreElements())
        {
            Ogre::Bone* bone = it.getNext();

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
        Ogre::Skeleton::BoneIterator it = sk->getBoneIterator();

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

        while (it.hasMoreElements())
        {
            Ogre::Bone* bone = it.getNext();

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
            Ogre::AnimationStateIterator iter=set->getAnimationStateIterator();
            while(iter.hasMoreElements())
            {
              iter.getNext()->setEnabled(false);
            }
        }

        //Update the screen
        Manager::getSingleton()->getRoot()->renderOneFrame();
        QCoreApplication::processEvents();

        Ogre::XMLSkeletonSerializer ss;
        Ogre::Skeleton* sk = _ent->getSkeleton();
        ss.exportSkeleton(sk,"_temp.xml");

        QFile* xmlFile = new QFile("_temp.xml");

        if(!xmlFile->open(QFile::ReadOnly|QFile::Text))
        {
            return false;
        }

        QByteArray xmlArray = xmlFile->readAll();

        xmlArray.replace(QString("<animation name=\""+_oldName+"\"").toStdString().data(),QString("<animation name=\""+_newName+"\"").toStdString().data());

        xmlFile->close();

        xmlFile = new QFile("_temp.xml");

        if(!xmlFile->open(QFile::WriteOnly|QFile::Truncate))
        {
            return false;
        }

        xmlFile->write(xmlArray);
        xmlFile->close();

        sk->unload();

        _ent->getAllAnimationStates()->removeAllAnimationStates();
        _ent->getMesh().getPointer()->removeAllAnimations();

        for(int c = _ent->getSkeleton()->getNumAnimations()-1; c >=0; --c)
        {
            _ent->getSkeleton()->removeAnimation(_ent->getSkeleton()->getAnimation(c)->getName().data());
        }

        ss.importSkeleton("_temp.xml",sk);

        _ent->refreshAvailableAnimationState();
        _ent->_updateAnimation();
        _ent->getMesh().getPointer()->_dirtyState();

        //Delete the temp file
        xmlFile = new QFile("_temp.xml");

        xmlFile->remove();

        return true;
    }
    return false;
}
