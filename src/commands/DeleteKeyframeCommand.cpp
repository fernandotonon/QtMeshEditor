#include "DeleteKeyframeCommand.h"

#include <OgreSkeleton.h>
#include <OgreBone.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>

#include <cmath>

namespace { constexpr float kKeyframeEpsilon = 0.001f; }

DeleteKeyframeCommand::DeleteKeyframeCommand(Ogre::Skeleton* skeleton,
                                             std::string animationName,
                                             std::string boneName,
                                             float time,
                                             const Ogre::Vector3& translate,
                                             const Ogre::Quaternion& rotation,
                                             const Ogre::Vector3& scale,
                                             QUndoCommand* parent)
    : QUndoCommand(parent)
    , mSkeleton(skeleton)
    , mAnimationName(std::move(animationName))
    , mBoneName(std::move(boneName))
    , mTime(time)
    , mTranslate(translate)
    , mRotation(rotation)
    , mScale(scale)
{
    setText(QStringLiteral("Delete keyframe"));
}

Ogre::NodeAnimationTrack* DeleteKeyframeCommand::findTrack() const
{
    if (!mSkeleton || !mSkeleton->hasAnimation(mAnimationName)) return nullptr;
    Ogre::Animation* anim = mSkeleton->getAnimation(mAnimationName);
    if (!mSkeleton->hasBone(mBoneName)) return nullptr;
    Ogre::Bone* bone = mSkeleton->getBone(mBoneName);
    for (const auto& pair : anim->_getNodeTrackList()) {
        if (pair.second->getAssociatedNode()->getName() == bone->getName())
            return pair.second;
    }
    return nullptr;
}

void DeleteKeyframeCommand::redo()
{
    Ogre::NodeAnimationTrack* track = findTrack();
    if (!track) return;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - mTime) <= kKeyframeEpsilon) {
            track->removeKeyFrame(i);
            return;
        }
    }
}

void DeleteKeyframeCommand::undo()
{
    Ogre::NodeAnimationTrack* track = findTrack();
    if (!track) return;
    // Restore the keyframe with the captured TRS.
    auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->createNodeKeyFrame(mTime));
    kf->setTranslate(mTranslate);
    kf->setRotation(mRotation);
    kf->setScale(mScale);
}
