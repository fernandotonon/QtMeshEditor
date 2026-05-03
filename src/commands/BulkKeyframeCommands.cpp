#include "BulkKeyframeCommands.h"

#include <Ogre.h>
#include <OgreSkeleton.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>

#include <QObject>
#include <cmath>
#include <utility>

namespace {
constexpr float kEpsilon = 0.001f;

Ogre::NodeAnimationTrack* resolveTrack(Ogre::Skeleton* skel,
                                       const std::string& animName,
                                       const std::string& boneName)
{
    if (!skel || animName.empty() || boneName.empty()) return nullptr;
    if (!skel->hasAnimation(animName)) return nullptr;
    if (!skel->hasBone(boneName)) return nullptr;
    Ogre::Animation* anim = skel->getAnimation(animName);
    Ogre::Bone* bone = skel->getBone(boneName);
    if (!anim || !bone) return nullptr;
    if (!anim->hasNodeTrack(bone->getHandle())) return nullptr;
    return anim->getNodeTrack(bone->getHandle());
}

int findKeyframeIndex(const Ogre::NodeAnimationTrack* track, float time) {
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        if (std::fabs(track->getKeyFrame(i)->getTime() - time) <= kEpsilon) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Move a single keyframe via the standard "remove + recreate" recipe (Ogre
// has no KeyFrame::setTime). Returns true when the keyframe was found and
// moved. Used by both bulk move and paste-undo.
bool relocateKeyframe(Ogre::NodeAnimationTrack* track, float fromT, float toT) {
    const int idx = findKeyframeIndex(track, fromT);
    if (idx < 0) return false;
    auto* oldKf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(idx));
    const Ogre::Vector3    t = oldKf->getTranslate();
    const Ogre::Quaternion r = oldKf->getRotation();
    const Ogre::Vector3    s = oldKf->getScale();
    track->removeKeyFrame(static_cast<unsigned short>(idx));
    auto* newKf = track->createNodeKeyFrame(toT);
    newKf->setTranslate(t);
    newKf->setRotation(r);
    newKf->setScale(s);
    track->_keyFrameDataChanged();
    return true;
}

} // namespace

// ── MoveKeyframesCommand ──────────────────────────────────────────────────────

MoveKeyframesCommand::MoveKeyframesCommand(Ogre::Skeleton* skeleton,
                                           std::string animationName,
                                           QVector<Item> items,
                                           float dt,
                                           QUndoCommand* parent)
    : QUndoCommand(parent)
    , mSkeleton(skeleton)
    , mAnimationName(std::move(animationName))
    , mItems(std::move(items))
    , mDt(dt)
{
    setText(QObject::tr("Move %n keyframe(s)", "", static_cast<int>(mItems.size())));
}

bool MoveKeyframesCommand::shiftAll(float fromOffset, float toOffset)
{
    // Apply the shift in two phases per track: first park each member at a
    // unique negative-time slot to avoid collisions during the rewrite, then
    // move from the parked slot to the final position. This lets a forward
    // shift survive cases where keyframe N+1's new time equals keyframe N's
    // current time on the same track.
    QVector<float> parkSlots;
    parkSlots.reserve(mItems.size());
    for (int i = 0; i < mItems.size(); ++i) {
        auto* track = resolveTrack(mSkeleton, mAnimationName, mItems[i].boneName);
        if (!track) { parkSlots.append(0.0f); continue; }
        const float src = mItems[i].originalTime + fromOffset;
        const float park = -1.0f - static_cast<float>(i); // unique negative slot
        relocateKeyframe(track, src, park);
        parkSlots.append(park);
    }
    for (int i = 0; i < mItems.size(); ++i) {
        auto* track = resolveTrack(mSkeleton, mAnimationName, mItems[i].boneName);
        if (!track) continue;
        const float dst = mItems[i].originalTime + toOffset;
        relocateKeyframe(track, parkSlots[i], dst);
    }
    return true;
}

void MoveKeyframesCommand::redo() { shiftAll(0.0f, mDt); }
void MoveKeyframesCommand::undo() { shiftAll(mDt, 0.0f); }

// ── PasteKeyframesCommand ─────────────────────────────────────────────────────

PasteKeyframesCommand::PasteKeyframesCommand(Ogre::Skeleton* skeleton,
                                             std::string animationName,
                                             QVector<Entry> entries,
                                             QUndoCommand* parent)
    : QUndoCommand(parent)
    , mSkeleton(skeleton)
    , mAnimationName(std::move(animationName))
    , mEntries(std::move(entries))
{
    mApplied.resize(mEntries.size());
    setText(QObject::tr("Paste %n keyframe(s)", "", static_cast<int>(mEntries.size())));
}

void PasteKeyframesCommand::redo()
{
    mPastedCount = 0;
    for (int i = 0; i < mEntries.size(); ++i) {
        const Entry& e = mEntries[i];
        mApplied[i] = false;
        auto* track = resolveTrack(mSkeleton, mAnimationName, e.boneName);
        if (!track) continue;
        if (findKeyframeIndex(track, e.time) >= 0) continue; // collision — skip
        auto* kf = track->createNodeKeyFrame(e.time);
        kf->setTranslate(Ogre::Vector3(e.tx, e.ty, e.tz));
        kf->setRotation(Ogre::Quaternion(e.rw, e.rx, e.ry, e.rz));
        kf->setScale(Ogre::Vector3(e.sx, e.sy, e.sz));
        track->_keyFrameDataChanged();
        mApplied[i] = true;
        ++mPastedCount;
    }
}

void PasteKeyframesCommand::undo()
{
    for (int i = 0; i < mEntries.size(); ++i) {
        if (!mApplied[i]) continue;
        const Entry& e = mEntries[i];
        auto* track = resolveTrack(mSkeleton, mAnimationName, e.boneName);
        if (!track) continue;
        const int idx = findKeyframeIndex(track, e.time);
        if (idx < 0) continue;
        track->removeKeyFrame(static_cast<unsigned short>(idx));
        track->_keyFrameDataChanged();
    }
    mPastedCount = 0;
}
