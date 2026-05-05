#include "DecimateTrackCommand.h"

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

Ogre::NodeAnimationTrack* resolveTrack(const std::string& entityName,
                                       const std::string& animName,
                                       const std::string& boneName)
{
    if (animName.empty() || boneName.empty()) return nullptr;
    Ogre::SkeletonInstance* skel = SkeletonResolver::resolve(entityName);
    if (!skel) return nullptr;
    if (!skel->hasAnimation(animName) || !skel->hasBone(boneName)) return nullptr;
    Ogre::Animation* anim = skel->getAnimation(animName);
    Ogre::Bone* bone = skel->getBone(boneName);
    if (!anim->hasNodeTrack(bone->getHandle())) return nullptr;
    return anim->getNodeTrack(bone->getHandle());
}

} // namespace

DecimateTrackCommand::DecimateTrackCommand(std::string entityName,
                                            std::string animationName,
                                            std::string boneName,
                                            int targetFps,
                                            QUndoCommand* parent)
    : QUndoCommand(parent)
    , mEntityName(std::move(entityName))
    , mAnimationName(std::move(animationName))
    , mBoneName(std::move(boneName))
    , mTargetFps(targetFps)
{
    setText(QObject::tr("Reduce keyframes"));
}

bool DecimateTrackCommand::snapshotTrack(std::vector<KeyframeSnapshot>& out)
{
    auto* track = resolveTrack(mEntityName, mAnimationName, mBoneName);
    if (!track) return false;
    out.clear();
    out.reserve(track->getNumKeyFrames());
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        out.push_back({ kf->getTime(),
                        kf->getTranslate(),
                        kf->getRotation(),
                        kf->getScale() });
    }
    return true;
}

bool DecimateTrackCommand::replaceTrack(const std::vector<KeyframeSnapshot>& snap)
{
    auto* track = resolveTrack(mEntityName, mAnimationName, mBoneName);
    if (!track) return false;

    // Strip every keyframe (downward to keep indices valid).
    for (int i = static_cast<int>(track->getNumKeyFrames()) - 1; i >= 0; --i) {
        track->removeKeyFrame(static_cast<unsigned short>(i));
    }
    for (const auto& s : snap) {
        auto* kf = track->createNodeKeyFrame(s.time);
        kf->setTranslate(s.translate);
        kf->setRotation(s.rotation);
        kf->setScale(s.scale);
    }
    track->_keyFrameDataChanged();
    return true;
}

std::vector<DecimateTrackCommand::KeyframeSnapshot>
DecimateTrackCommand::decimate(const std::vector<KeyframeSnapshot>& dense) const
{
    if (dense.size() <= 2 || mTargetFps <= 0) return dense;
    const float minGap = 1.0f / static_cast<float>(mTargetFps);
    constexpr float kEps = 1e-4f;

    std::vector<KeyframeSnapshot> kept;
    kept.reserve(dense.size());
    kept.push_back(dense.front());
    float lastKept = dense.front().time;
    for (size_t i = 1; i + 1 < dense.size(); ++i) {
        if (dense[i].time - lastKept >= minGap - kEps) {
            kept.push_back(dense[i]);
            lastKept = dense[i].time;
        }
    }
    // Always keep the final keyframe so the animation length doesn't
    // change. Drop the previous-kept if it landed within the gap of
    // the final frame to avoid an artifact close to the end.
    const auto& last = dense.back();
    if (!kept.empty() && last.time - kept.back().time < minGap - kEps
        && kept.size() > 1) {
        kept.pop_back();
    }
    kept.push_back(last);
    return kept;
}

void DecimateTrackCommand::redo()
{
    if (!mCaptured) {
        if (!snapshotTrack(mBefore)) return;
        mAfter = decimate(mBefore);
        if (mAfter.size() >= mBefore.size()) {
            // Nothing to drop — leave the track alone and don't flip
            // mCaptured so a later redo can retry against a denser
            // snapshot if the track changed.
            return;
        }
        if (!replaceTrack(mAfter)) return;
        mCaptured = true;
        return;
    }
    replaceTrack(mAfter);
}

void DecimateTrackCommand::undo()
{
    replaceTrack(mBefore);
}
