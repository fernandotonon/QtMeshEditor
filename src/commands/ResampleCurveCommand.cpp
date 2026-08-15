#include "ResampleCurveCommand.h"

#include "SkeletonResolver.h"
#include "../CurveEditModel.h"
#include "../CurveResampler.h"
#include "../Manager.h"

#include <Ogre.h>
#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>
#include <OgreSceneManager.h>

#include <QObject>
#include <QString>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr float kEpsilon = 0.001f;

// Resolve a node-clip track: the clip is a SceneManager-owned
// Ogre::Animation named `animName`; the track is the one whose
// associated node is named `boneName` (the node name). Mirrors the
// resolution walk NodeAnimationManager's curve/query helpers use — the
// m_trackHandles cache can be stale after undo, so we match by node
// name off _getNodeTrackList(). (#520)
Ogre::NodeAnimationTrack* resolveNodeTrack(const std::string& animName,
                                           const std::string& nodeName)
{
    if (animName.empty() || nodeName.empty()) return nullptr;
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    Ogre::SceneManager* scene = mgr->getSceneMgr();
    if (!scene || !scene->hasAnimation(animName)) return nullptr;
    Ogre::Animation* anim = scene->getAnimation(animName);
    const auto& tracks = anim->_getNodeTrackList();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        Ogre::NodeAnimationTrack* t = it->second;
        if (t && t->getAssociatedNode()
            && t->getAssociatedNode()->getName() == nodeName)
            return t;
    }
    return nullptr;
}

Ogre::NodeAnimationTrack* resolveTrack(const std::string& entityName,
                                       const std::string& animName,
                                       const std::string& boneName,
                                       bool isNodeClip)
{
    if (isNodeClip) return resolveNodeTrack(animName, boneName);
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
                                             double toleranceMul,
                                             int fixedFps,
                                             bool isNodeClip,
                                             QUndoCommand* parent)
    : QUndoCommand(parent)
    , mEntityName(std::move(entityName))
    , mAnimationName(std::move(animationName))
    , mBoneName(std::move(boneName))
    , mChannel(std::move(channel))
    , mT0(t0)
    , mT1(t1)
    , mToleranceMul(toleranceMul)
    , mFixedFps(fixedFps)
    , mIsNodeClip(isNodeClip)
{
    setText(QObject::tr("Resample curve"));
}

bool ResampleCurveCommand::captureBefore()
{
    auto* track = resolveTrack(mEntityName, mAnimationName, mBoneName, mIsNodeClip);
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
    auto* track = resolveTrack(mEntityName, mAnimationName, mBoneName, mIsNodeClip);
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
    auto* track = resolveTrack(mEntityName, mAnimationName, mBoneName, mIsNodeClip);
    if (!track) return false;

    // Snapshot every keyframe in the segment + endpoints. The
    // non-resampled channels of new keys lerp between the BRACKETING
    // snapshot pair for each output time, NOT just the segment
    // endpoints — otherwise a whole-clip resample (fixed-FPS bake)
    // would flatten the animation by interpolating only between t=0
    // and t=length, losing every intermediate pose.
    struct Sample {
        float            time;
        Ogre::Vector3    translate;
        Ogre::Quaternion rotation;
        Ogre::Vector3    scale;
    };
    std::vector<Sample> snap;
    snap.reserve(track->getNumKeyFrames());
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        const float t = kf->getTime();
        if (t >= mT0 - kEpsilon && t <= mT1 + kEpsilon) {
            snap.push_back({ t, kf->getTranslate(),
                             kf->getRotation(), kf->getScale() });
        }
    }
    if (snap.size() < 2) return false;

    // Build the channel time/value arrays the resampler needs from
    // the snapshot (so curve evaluation has the full series even
    // after the strip phase removes interior keyframes).
    std::vector<double> kfTimesD, kfValuesD;
    kfTimesD.reserve(snap.size());
    kfValuesD.reserve(snap.size());
    for (const auto& s : snap) {
        kfTimesD.push_back(s.time);
        // readChannel needs a TransformKeyFrame; we have a snapshot
        // struct — synthesize the channel value inline.
        const std::string& c = mChannel;
        double v = 0.0;
        if      (c == "tx") v = s.translate.x;
        else if (c == "ty") v = s.translate.y;
        else if (c == "tz") v = s.translate.z;
        else if (c == "rw") v = s.rotation.w;
        else if (c == "rx") v = s.rotation.x;
        else if (c == "ry") v = s.rotation.y;
        else if (c == "rz") v = s.rotation.z;
        else if (c == "sx") v = s.scale.x;
        else if (c == "sy") v = s.scale.y;
        else if (c == "sz") v = s.scale.z;
        kfValuesD.push_back(v);
    }
    const QVariantList kfTimes  = toVariantList(kfTimesD);
    const QVariantList kfValues = toVariantList(kfValuesD);

    auto samples = CurveResampler::resampleSegment(
        CurveEditModel::instance(),
        QString::fromStdString(mEntityName),
        QString::fromStdString(mAnimationName),
        QString::fromStdString(mBoneName),
        QString::fromStdString(mChannel),
        mT0, mT1, kfTimes, kfValues,
        mToleranceMul, mFixedFps);
    if (samples.empty()) return false;

    // Drop the closing endpoint sample if it lands on t1.
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

    // Helper: find the bracketing snapshot pair for time `t`. Returns
    // (lower, upper) snapshot pointers; on or beyond either end the
    // lower/upper collapses to that endpoint.
    auto bracket = [&snap](float t) -> std::pair<const Sample*, const Sample*> {
        const Sample* lo = &snap.front();
        const Sample* hi = &snap.back();
        for (size_t i = 0; i + 1 < snap.size(); ++i) {
            if (t >= snap[i].time - kEpsilon && t <= snap[i+1].time + kEpsilon) {
                lo = &snap[i];
                hi = &snap[i+1];
                break;
            }
        }
        return { lo, hi };
    };

    // Insert resampled frames. Non-resampled channels lerp between
    // the BRACKETING original keyframes for each output time so
    // intermediate poses survive the strip.
    mAfter.clear();
    mAfter.reserve(samples.size());
    for (const auto& s : samples) {
        const float t = static_cast<float>(s.time);
        const auto [lo, hi] = bracket(t);
        const float gap = hi->time - lo->time;
        const float u = gap > 1e-6f
                        ? std::clamp((t - lo->time) / gap, 0.0f, 1.0f)
                        : 0.0f;
        Ogre::Vector3    tr = lo->translate + (hi->translate - lo->translate) * u;
        Ogre::Quaternion ro = Ogre::Quaternion::Slerp(u, lo->rotation, hi->rotation, true);
        Ogre::Vector3    sc = lo->scale + (hi->scale - lo->scale) * u;

        auto* kf = track->createNodeKeyFrame(t);
        kf->setTranslate(tr);
        kf->setRotation(ro);
        kf->setScale(sc);
        // Overwrite the resampled channel with the curve's value at t.
        writeChannel(kf, mChannel, s.value);
        // writeChannel on a quaternion component (rw/rx/ry/rz) leaves
        // the rotation non-unit; Ogre's track interp slerps non-unit
        // quaternions incorrectly, producing visible rotation drift.
        if (mChannel.size() == 2 && mChannel[0] == 'r') {
            Ogre::Quaternion q = kf->getRotation();
            q.normalise();
            kf->setRotation(q);
        }
        mAfter.push_back(snapshotKf(kf));
    }

    track->_keyFrameDataChanged();
    return true;
}

void ResampleCurveCommand::redo()
{
    if (!mCaptured) {
        if (!captureBefore()) return;
        // First redo: resample. Only mark `mCaptured` once the
        // resample succeeds — otherwise a failed first redo would flip
        // every subsequent redo into the "replay mAfter" branch with an
        // empty mAfter, silently turning into a destructive no-op that
        // erases the user's interior keyframes on the next play.
        if (!resampleAndWrite()) return;
        mCaptured = true;
        return;
    }
    applySnapshot(mAfter);
}

void ResampleCurveCommand::undo()
{
    // If captureBefore failed (mCaptured stays false, mBefore empty),
    // a stack-driven undo would otherwise call applySnapshot({}) and
    // strip every interior keyframe in (t0, t1) with nothing to
    // re-insert — destroying the user's track.
    if (!mCaptured) return;
    applySnapshot(mBefore);
}
