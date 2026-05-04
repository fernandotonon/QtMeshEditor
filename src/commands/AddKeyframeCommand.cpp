#include "AddKeyframeCommand.h"

#include "SkeletonResolver.h"

#include <OgreSkeletonInstance.h>
#include <OgreBone.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>

#include <cmath>

namespace { constexpr float kKeyframeEpsilon = 0.001f; }

AddKeyframeCommand::AddKeyframeCommand(std::string entityName,
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
    , mEntityName(std::move(entityName))
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
    Ogre::SkeletonInstance* skel = SkeletonResolver::resolve(mEntityName);
    if (!skel || !skel->hasAnimation(mAnimationName)) return nullptr;
    if (!skel->hasBone(mBoneName)) return nullptr;
    Ogre::Animation* anim = skel->getAnimation(mAnimationName);
    Ogre::Bone* bone = skel->getBone(mBoneName);
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
    Ogre::SkeletonInstance* skel = SkeletonResolver::resolve(mEntityName);
    if (!skel || !skel->hasAnimation(mAnimationName)) return;
    if (!skel->hasBone(mBoneName)) return;
    Ogre::Animation* anim = skel->getAnimation(mAnimationName);
    Ogre::Bone* bone = skel->getBone(mBoneName);

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
    Ogre::SkeletonInstance* skel = SkeletonResolver::resolve(mEntityName);
    if (!skel) return;
    Ogre::NodeAnimationTrack* track = findTrack();
    if (!track) return;

    if (mMode == Mode::TrackCreated) {
        // Lazy-created track: destroy it entirely so the bone returns
        // to its "no track" state (curve doesn't drive it during
        // playback, bone holds its bind/manual pose).
        Ogre::Animation* anim = skel->getAnimation(mAnimationName);
        Ogre::Bone* bone = skel->getBone(mBoneName);
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
