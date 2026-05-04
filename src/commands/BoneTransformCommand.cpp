#include "BoneTransformCommand.h"

#include <OgreSkeletonInstance.h>
#include <OgreBone.h>

BoneTransformCommand::BoneTransformCommand(Ogre::SkeletonInstance* skeleton,
                                           std::string boneName,
                                           const Ogre::Vector3& beforePos,
                                           const Ogre::Quaternion& beforeOrient,
                                           const Ogre::Vector3& beforeScale,
                                           const Ogre::Vector3& afterPos,
                                           const Ogre::Quaternion& afterOrient,
                                           const Ogre::Vector3& afterScale,
                                           bool bindMode,
                                           QUndoCommand* parent)
    : QUndoCommand(parent)
    , mSkeleton(skeleton)
    , mBoneName(std::move(boneName))
    , mBeforePos(beforePos)
    , mBeforeOrient(beforeOrient)
    , mBeforeScale(beforeScale)
    , mAfterPos(afterPos)
    , mAfterOrient(afterOrient)
    , mAfterScale(afterScale)
    , mBindMode(bindMode)
{
    setText(bindMode ? QStringLiteral("Bone bind-pose edit")
                     : QStringLiteral("Bone transform"));
}

void BoneTransformCommand::apply(const Ogre::Vector3& p,
                                 const Ogre::Quaternion& o,
                                 const Ogre::Vector3& s)
{
    if (!mSkeleton || !mSkeleton->hasBone(mBoneName)) return;
    Ogre::Bone* bone = mSkeleton->getBone(mBoneName);
    bone->setPosition(p);
    bone->setOrientation(o);
    bone->setScale(s);
    // For bind-pose edits, the durable artifact is the bone's initial
    // state (used by Skeleton::reset → resetToInitialState). Capturing
    // the new local as the new initial keeps undo/redo round-tripping
    // the bind pose, not just the transient local TRS.
    if (mBindMode) bone->setInitialState();
    bone->needUpdate();
}

void BoneTransformCommand::undo() { apply(mBeforePos, mBeforeOrient, mBeforeScale); }
void BoneTransformCommand::redo() { apply(mAfterPos,  mAfterOrient,  mAfterScale);  }
