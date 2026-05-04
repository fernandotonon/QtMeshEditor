#include "AddKeyframeCommand.h"

#include <OgreSkeleton.h>
#include <OgreBone.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>

#include <cmath>

namespace { constexpr float kKeyframeEpsilon = 0.001f; }

AddKeyframeCommand::AddKeyframeCommand(Ogre::Skeleton* skeleton,
                                       std::string animationName,
                                       std::string boneName,
                                       float time,
                                       Mode mode,
                                       const Ogre::Vector3& beforeTranslate,
                                       const Ogre::Quaternion& beforeRotation,
                                       const Ogre::Vector3& beforeScale,
                                       const Ogre::Vector3& afterTranslate,
                                       const Ogre::Quaternion& afterRotation,
                                       const Ogre::Vector3& afterScale,
                                       QUndoCommand* parent)
    : QUndoCommand(parent)
    , mSkeleton(skeleton)
    , mAnimationName(std::move(animationName))
    , mBoneName(std::move(boneName))
    , mTime(time)
    , mMode(mode)
    , mBeforeTranslate(beforeTranslate)
    , mBeforeRotation(beforeRotation)
    , mBeforeScale(beforeScale)
    , mAfterTranslate(afterTranslate)
    , mAfterRotation(afterRotation)
    , mAfterScale(afterScale)
{
    setText(QStringLiteral("Add keyframe"));
}

Ogre::NodeAnimationTrack* AddKeyframeCommand::findTrack() const
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

Ogre::TransformKeyFrame* AddKeyframeCommand::findKeyframe(Ogre::NodeAnimationTrack* track) const
{
    if (!track) return nullptr;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - mTime) <= kKeyframeEpsilon) return kf;
    }
    return nullptr;
}

void AddKeyframeCommand::redo()
{
    if (!mSkeleton || !mSkeleton->hasAnimation(mAnimationName)) return;
    Ogre::Animation* anim = mSkeleton->getAnimation(mAnimationName);
    if (!mSkeleton->hasBone(mBoneName)) return;
    Ogre::Bone* bone = mSkeleton->getBone(mBoneName);

    Ogre::NodeAnimationTrack* track = findTrack();
    if (!track && mMode == Mode::TrackCreated)
        track = anim->createNodeTrack(bone->getHandle(), bone);
    if (!track) return;

    Ogre::TransformKeyFrame* kf = findKeyframe(track);
    if (!kf) kf = track->createNodeKeyFrame(mTime);
    kf->setTranslate(mAfterTranslate);
    kf->setRotation(mAfterRotation);
    kf->setScale(mAfterScale);
}

void AddKeyframeCommand::undo()
{
    Ogre::NodeAnimationTrack* track = findTrack();
    if (!track) return;

    if (mMode == Mode::TrackCreated) {
        // Lazy-created track: destroy it entirely so the bone returns
        // to its "no track" state (curve doesn't drive it during
        // playback, bone holds its bind/manual pose).
        Ogre::Animation* anim = mSkeleton->getAnimation(mAnimationName);
        Ogre::Bone* bone = mSkeleton->getBone(mBoneName);
        anim->destroyNodeTrack(bone->getHandle());
        return;
    }

    Ogre::TransformKeyFrame* kf = findKeyframe(track);
    if (!kf) return;

    if (mMode == Mode::KeyframeCreated) {
        // Find the keyframe's index and remove it.
        for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
            auto* candidate = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
            if (std::fabs(candidate->getTime() - mTime) <= kKeyframeEpsilon) {
                track->removeKeyFrame(i);
                return;
            }
        }
    } else {
        // KeyframeUpdated: restore the pre-edit TRS in place.
        kf->setTranslate(mBeforeTranslate);
        kf->setRotation(mBeforeRotation);
        kf->setScale(mBeforeScale);
    }
}
