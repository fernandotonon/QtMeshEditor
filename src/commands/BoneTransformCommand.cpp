#include "BoneTransformCommand.h"

#include "SkeletonResolver.h"

#include <OgreSkeletonInstance.h>
#include <OgreBone.h>

BoneTransformCommand::BoneTransformCommand(std::string entityName,
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
    , mEntityName(std::move(entityName))
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
    Ogre::SkeletonInstance* skel = SkeletonResolver::resolve(mEntityName);
    if (!skel || !skel->hasBone(mBoneName)) return;
    Ogre::Bone* bone = skel->getBone(mBoneName);
    bone->setPosition(p);
    bone->setOrientation(o);
    bone->setScale(s);
    // For bind-pose edits, the durable artifact is the bone's initial
    // state (used by Skeleton::reset → resetToInitialState). Capturing
    // the new local as the new initial keeps undo/redo round-tripping
    // the bind pose, not just the transient local TRS.
    if (mBindMode) bone->setInitialState();
    // needUpdate(true) propagates to children, so the whole subtree's
    // derived transforms are invalidated. Without `true`, child TagPoints
    // (e.g. SkeletonDebug bone visuals) keep stale derived poses until
    // the next animation tick.
    bone->needUpdate(true);
}

void BoneTransformCommand::undo() { apply(mBeforePos, mBeforeOrient, mBeforeScale); }
void BoneTransformCommand::redo() { apply(mAfterPos,  mAfterOrient,  mAfterScale);  }
