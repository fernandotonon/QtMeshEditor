#include "MoveKeyframeCommand.h"

#include "SkeletonResolver.h"

#include <Ogre.h>
#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>

#include <QObject>
#include <cmath>
#include <utility>

namespace {
constexpr float kEpsilon = 0.001f; // 1 ms — same tolerance as keyframe ticks

Ogre::NodeAnimationTrack* resolveTrack(const std::string& entityName,
                                       const std::string& animName,
                                       const std::string& boneName)
{
    if (animName.empty() || boneName.empty()) return nullptr;
    Ogre::SkeletonInstance* skel = SkeletonResolver::resolve(entityName);
    if (!skel) return nullptr;
    if (!skel->hasAnimation(animName)) return nullptr;
    if (!skel->hasBone(boneName)) return nullptr;
    Ogre::Animation* anim = skel->getAnimation(animName);
    Ogre::Bone* bone = skel->getBone(boneName);
    if (!anim || !bone) return nullptr;
    if (!anim->hasNodeTrack(bone->getHandle())) return nullptr;
    return anim->getNodeTrack(bone->getHandle());
}

} // namespace

MoveKeyframeCommand::MoveKeyframeCommand(std::string entityName,
                                         std::string animationName,
                                         std::string boneName,
                                         float oldTime,
                                         float newTime,
                                         QUndoCommand* parent)
    : QUndoCommand(parent)
    , mEntityName(std::move(entityName))
    , mAnimationName(std::move(animationName))
    , mBoneName(std::move(boneName))
    , mOldTime(oldTime)
    , mNewTime(newTime)
{
    setText(QObject::tr("Move keyframe"));
}

bool MoveKeyframeCommand::moveKeyframeTo(float searchTime, float targetTime)
{
    auto* track = resolveTrack(mEntityName, mAnimationName, mBoneName);
    if (!track) return false;

    // Find the keyframe at searchTime (± epsilon).
    int foundIdx = -1;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        Ogre::KeyFrame* kf = track->getKeyFrame(i);
        if (std::fabs(kf->getTime() - searchTime) <= kEpsilon) {
            foundIdx = static_cast<int>(i);
            break;
        }
    }
    if (foundIdx < 0) return false;

    // Reject if newTime collides with another existing keyframe.
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        if (static_cast<int>(i) == foundIdx) continue;
        if (std::fabs(track->getKeyFrame(i)->getTime() - targetTime) <= kEpsilon) {
            return false;
        }
    }

    // Ogre's KeyFrame has no setTime() — remove + recreate at the new time
    // and copy the T/R/S values across. Track auto-sorts on createNodeKeyFrame.
    auto* oldKf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(foundIdx));
    const Ogre::Vector3    t = oldKf->getTranslate();
    const Ogre::Quaternion r = oldKf->getRotation();
    const Ogre::Vector3    s = oldKf->getScale();

    track->removeKeyFrame(static_cast<unsigned short>(foundIdx));
    auto* newKf = track->createNodeKeyFrame(targetTime);
    newKf->setTranslate(t);
    newKf->setRotation(r);
    newKf->setScale(s);

    track->_keyFrameDataChanged();
    return true;
}

void MoveKeyframeCommand::redo()
{
    moveKeyframeTo(mOldTime, mNewTime);
}

void MoveKeyframeCommand::undo()
{
    moveKeyframeTo(mNewTime, mOldTime);
}
