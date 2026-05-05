#include "SetKeyframeValueCommand.h"

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
constexpr float kEpsilon = 0.001f;

Ogre::TransformKeyFrame* findKeyframe(const std::string& entityName,
                                       const std::string& animName,
                                       const std::string& boneName,
                                       float time)
{
    if (animName.empty() || boneName.empty()) return nullptr;
    Ogre::SkeletonInstance* skel = SkeletonResolver::resolve(entityName);
    if (!skel) return nullptr;
    if (!skel->hasAnimation(animName) || !skel->hasBone(boneName)) return nullptr;
    Ogre::Animation* anim = skel->getAnimation(animName);
    Ogre::Bone* bone = skel->getBone(boneName);
    if (!anim->hasNodeTrack(bone->getHandle())) return nullptr;
    auto* track = anim->getNodeTrack(bone->getHandle());
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - time) <= kEpsilon) return kf;
    }
    return nullptr;
}

// Read or write one of the 10 scalar channels on a TransformKeyFrame.
// Returns true on a known channel id.
bool getChannel(const Ogre::TransformKeyFrame* kf, const std::string& ch, double& out) {
    if (ch == "tx") { out = kf->getTranslate().x; return true; }
    if (ch == "ty") { out = kf->getTranslate().y; return true; }
    if (ch == "tz") { out = kf->getTranslate().z; return true; }
    if (ch == "rw") { out = kf->getRotation().w;  return true; }
    if (ch == "rx") { out = kf->getRotation().x;  return true; }
    if (ch == "ry") { out = kf->getRotation().y;  return true; }
    if (ch == "rz") { out = kf->getRotation().z;  return true; }
    if (ch == "sx") { out = kf->getScale().x;     return true; }
    if (ch == "sy") { out = kf->getScale().y;     return true; }
    if (ch == "sz") { out = kf->getScale().z;     return true; }
    return false;
}

bool setChannel(Ogre::TransformKeyFrame* kf, const std::string& ch, double v) {
    if (ch == "tx") { auto t = kf->getTranslate(); t.x = static_cast<float>(v); kf->setTranslate(t); return true; }
    if (ch == "ty") { auto t = kf->getTranslate(); t.y = static_cast<float>(v); kf->setTranslate(t); return true; }
    if (ch == "tz") { auto t = kf->getTranslate(); t.z = static_cast<float>(v); kf->setTranslate(t); return true; }
    if (ch == "rw") { auto r = kf->getRotation();  r.w = static_cast<float>(v); kf->setRotation(r); return true; }
    if (ch == "rx") { auto r = kf->getRotation();  r.x = static_cast<float>(v); kf->setRotation(r); return true; }
    if (ch == "ry") { auto r = kf->getRotation();  r.y = static_cast<float>(v); kf->setRotation(r); return true; }
    if (ch == "rz") { auto r = kf->getRotation();  r.z = static_cast<float>(v); kf->setRotation(r); return true; }
    if (ch == "sx") { auto s = kf->getScale();     s.x = static_cast<float>(v); kf->setScale(s); return true; }
    if (ch == "sy") { auto s = kf->getScale();     s.y = static_cast<float>(v); kf->setScale(s); return true; }
    if (ch == "sz") { auto s = kf->getScale();     s.z = static_cast<float>(v); kf->setScale(s); return true; }
    return false;
}

} // namespace

SetKeyframeValueCommand::SetKeyframeValueCommand(std::string entityName,
                                                  std::string animationName,
                                                  std::string boneName,
                                                  std::string channel,
                                                  float time,
                                                  double newValue,
                                                  QUndoCommand* parent)
    : QUndoCommand(parent)
    , mEntityName(std::move(entityName))
    , mAnimationName(std::move(animationName))
    , mBoneName(std::move(boneName))
    , mChannel(std::move(channel))
    , mTime(time)
    , mNewValue(newValue)
{
    setText(QObject::tr("Set keyframe value"));
}

bool SetKeyframeValueCommand::apply(double value)
{
    auto* kf = findKeyframe(mEntityName, mAnimationName, mBoneName, mTime);
    if (!kf) return false;
    if (!mCaptured) {
        // First-time apply: snapshot the original so undo can restore it.
        // QUndoStack::push() always calls redo() exactly once before any
        // undo, so this branch is the right place to capture.
        getChannel(kf, mChannel, mOldValue);
        mCaptured = true;
    }
    return setChannel(kf, mChannel, value);
}

void SetKeyframeValueCommand::redo() { apply(mNewValue); }
void SetKeyframeValueCommand::undo() { apply(mOldValue); }
