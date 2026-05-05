#include "ResampleCurveCommand.h"

#include "SkeletonResolver.h"
#include "../CurveEditModel.h"
#include "../CurveResampler.h"

#include <Ogre.h>
#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>

#include <QObject>
#include <QString>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr float kEpsilon = 0.001f;

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

double readChannel(const Ogre::TransformKeyFrame* kf, const std::string& ch) {
    if (ch == "tx") return kf->getTranslate().x;
    if (ch == "ty") return kf->getTranslate().y;
    if (ch == "tz") return kf->getTranslate().z;
    if (ch == "rw") return kf->getRotation().w;
    if (ch == "rx") return kf->getRotation().x;
    if (ch == "ry") return kf->getRotation().y;
    if (ch == "rz") return kf->getRotation().z;
    if (ch == "sx") return kf->getScale().x;
    if (ch == "sy") return kf->getScale().y;
    if (ch == "sz") return kf->getScale().z;
    return 0.0;
}

void writeChannel(Ogre::TransformKeyFrame* kf, const std::string& ch, double v) {
    const float fv = static_cast<float>(v);
    if (ch == "tx") { auto t = kf->getTranslate(); t.x = fv; kf->setTranslate(t); return; }
    if (ch == "ty") { auto t = kf->getTranslate(); t.y = fv; kf->setTranslate(t); return; }
    if (ch == "tz") { auto t = kf->getTranslate(); t.z = fv; kf->setTranslate(t); return; }
    if (ch == "rw") { auto r = kf->getRotation();  r.w = fv; kf->setRotation(r);  return; }
    if (ch == "rx") { auto r = kf->getRotation();  r.x = fv; kf->setRotation(r);  return; }
    if (ch == "ry") { auto r = kf->getRotation();  r.y = fv; kf->setRotation(r);  return; }
    if (ch == "rz") { auto r = kf->getRotation();  r.z = fv; kf->setRotation(r);  return; }
    if (ch == "sx") { auto s = kf->getScale();     s.x = fv; kf->setScale(s);     return; }
    if (ch == "sy") { auto s = kf->getScale();     s.y = fv; kf->setScale(s);     return; }
    if (ch == "sz") { auto s = kf->getScale();     s.z = fv; kf->setScale(s);     return; }
}

ResampleCurveCommand::KeyframeSnapshot snapshotKf(const Ogre::TransformKeyFrame* kf) {
    return { kf->getTime(), kf->getTranslate(), kf->getRotation(), kf->getScale() };
}

QVariantList toVariantList(const std::vector<double>& v) {
    QVariantList out;
    out.reserve(static_cast<int>(v.size()));
    for (double x : v) out.append(x);
    return out;
}

} // namespace

ResampleCurveCommand::ResampleCurveCommand(std::string entityName,
                                             std::string animationName,
                                             std::string boneName,
                                             std::string channel,
                                             float t0, float t1,
                                             QUndoCommand* parent)
    : QUndoCommand(parent)
    , mEntityName(std::move(entityName))
    , mAnimationName(std::move(animationName))
    , mBoneName(std::move(boneName))
    , mChannel(std::move(channel))
    , mT0(t0)
    , mT1(t1)
{
    setText(QObject::tr("Resample curve"));
}

bool ResampleCurveCommand::captureBefore()
{
    auto* track = resolveTrack(mEntityName, mAnimationName, mBoneName);
    if (!track) return false;

    mBefore.clear();
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        const float t = kf->getTime();
        // Anchors at t0 and t1 are preserved; we only snapshot strict
        // interior keyframes so undo restores them and so the redo
        // path knows which range to overwrite.
        if (t > mT0 + kEpsilon && t < mT1 - kEpsilon) {
            mBefore.push_back(snapshotKf(kf));
        }
    }
    return true;
}

bool ResampleCurveCommand::applySnapshot(const std::vector<KeyframeSnapshot>& snap)
{
    auto* track = resolveTrack(mEntityName, mAnimationName, mBoneName);
    if (!track) return false;

    // Strip every interior keyframe in (t0, t1). Iterate downward so
    // index shifts on remove don't mis-skip.
    for (int i = static_cast<int>(track->getNumKeyFrames()) - 1; i >= 0; --i) {
        auto* kf = track->getKeyFrame(static_cast<unsigned short>(i));
        const float t = kf->getTime();
        if (t > mT0 + kEpsilon && t < mT1 - kEpsilon) {
            track->removeKeyFrame(static_cast<unsigned short>(i));
        }
    }

    // Re-insert the snapshotted frames. createNodeKeyFrame keeps the
    // track sorted by time on its own.
    for (const auto& s : snap) {
        auto* kf = track->createNodeKeyFrame(s.time);
        kf->setTranslate(s.translate);
        kf->setRotation(s.rotation);
        kf->setScale(s.scale);
    }
    track->_keyFrameDataChanged();
    return true;
}

bool ResampleCurveCommand::resampleAndWrite()
{
    auto* track = resolveTrack(mEntityName, mAnimationName, mBoneName);
    if (!track) return false;

    // Find anchor keyframes at t0 and t1 so we can read all 10 channels'
    // values at the segment endpoints — used to fill the non-resampled
    // channels of the new interior keyframes via linear interpolation.
    Ogre::TransformKeyFrame* kfA = nullptr;
    Ogre::TransformKeyFrame* kfB = nullptr;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - mT0) <= kEpsilon) kfA = kf;
        if (std::fabs(kf->getTime() - mT1) <= kEpsilon) kfB = kf;
    }
    if (!kfA || !kfB) return false;

    const Ogre::Vector3    tA = kfA->getTranslate(), tB = kfB->getTranslate();
    const Ogre::Quaternion rA = kfA->getRotation(),  rB = kfB->getRotation();
    const Ogre::Vector3    sA = kfA->getScale(),     sB = kfB->getScale();

    // Build the channel time/value arrays the resampler needs from
    // EVERY keyframe on the track (the curve evaluator interpolates
    // across the full series, not just the segment endpoints).
    std::vector<double> kfTimesD, kfValuesD;
    kfTimesD.reserve(track->getNumKeyFrames());
    kfValuesD.reserve(track->getNumKeyFrames());
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        kfTimesD.push_back(kf->getTime());
        kfValuesD.push_back(readChannel(kf, mChannel));
    }
    const QVariantList kfTimes  = toVariantList(kfTimesD);
    const QVariantList kfValues = toVariantList(kfValuesD);

    auto samples = CurveResampler::resampleSegment(
        CurveEditModel::instance(),
        QString::fromStdString(mEntityName),
        QString::fromStdString(mAnimationName),
        QString::fromStdString(mBoneName),
        QString::fromStdString(mChannel),
        mT0, mT1, kfTimes, kfValues);
    if (samples.empty()) return false;

    // Drop the closing endpoint sample if it lands on t1 — that anchor
    // already exists; we only insert strictly interior frames.
    while (!samples.empty()
           && std::fabs(samples.back().time - mT1) <= kEpsilon) {
        samples.pop_back();
    }

    // Strip prior interior keyframes before re-inserting.
    for (int i = static_cast<int>(track->getNumKeyFrames()) - 1; i >= 0; --i) {
        auto* kf = track->getKeyFrame(static_cast<unsigned short>(i));
        const float t = kf->getTime();
        if (t > mT0 + kEpsilon && t < mT1 - kEpsilon) {
            track->removeKeyFrame(static_cast<unsigned short>(i));
        }
    }

    // Insert resampled frames. Non-resampled channels get linearly
    // interpolated TRS between the two anchor keyframes — that
    // preserves the segment's other-channel shape without forcing a
    // separate resample for each channel.
    mAfter.clear();
    mAfter.reserve(samples.size());
    const float duration = mT1 - mT0;
    for (const auto& s : samples) {
        const float t = static_cast<float>(s.time);
        const float u = duration > 0.0f
                        ? std::clamp((t - mT0) / duration, 0.0f, 1.0f)
                        : 0.0f;
        Ogre::Vector3    tr = tA + (tB - tA) * u;
        Ogre::Quaternion ro = Ogre::Quaternion::Slerp(u, rA, rB, true);
        Ogre::Vector3    sc = sA + (sB - sA) * u;

        auto* kf = track->createNodeKeyFrame(t);
        kf->setTranslate(tr);
        kf->setRotation(ro);
        kf->setScale(sc);
        // Overwrite the resampled channel with the curve's value at t.
        writeChannel(kf, mChannel, s.value);
        mAfter.push_back(snapshotKf(kf));
    }

    track->_keyFrameDataChanged();
    return true;
}

void ResampleCurveCommand::redo()
{
    if (!mCaptured) {
        if (!captureBefore()) return;
        mCaptured = true;
        // First redo: actually resample. Subsequent redos replay the
        // captured `mAfter` snapshot for determinism.
        resampleAndWrite();
        return;
    }
    applySnapshot(mAfter);
}

void ResampleCurveCommand::undo()
{
    applySnapshot(mBefore);
}
