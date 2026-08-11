/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "NodeAnimCommands.h"

#include "../Manager.h"
#include "../NodeAnimationManager.h"
#include "../SentryReporter.h"

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

namespace {

// Same epsilon as NodeAnimationManager uses for "same keyframe":
// 1ms granularity matches the dope-sheet slider's 2-3 decimal
// places, so users can't accidentally create back-to-back keys.
constexpr double kKeyframeMergeEpsilon = 1e-3;

Ogre::SceneManager* sceneMgr()
{
    auto* mgr = Manager::getSingletonPtr();
    return mgr ? mgr->getSceneMgr() : nullptr;
}

// Walk every NodeAnimationTrack on `anim` and snapshot the per-key
// transform values. Used by DeleteClipCommand to make the operation
// reversible — we capture before redo() drops the live clip.
std::vector<NodeTrackSnapshot> snapshotAllTracks(Ogre::Animation* anim)
{
    std::vector<NodeTrackSnapshot> out;
    if (!anim) return out;
    const auto& tracks = anim->_getNodeTrackList();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        Ogre::NodeAnimationTrack* t = it->second;
        if (!t) continue;
        NodeTrackSnapshot snap;
        if (auto* node = t->getAssociatedNode())
            snap.nodeName = QString::fromStdString(node->getName());
        for (unsigned short k = 0; k < t->getNumKeyFrames(); ++k) {
            auto* kf = static_cast<Ogre::TransformKeyFrame*>(t->getKeyFrame(k));
            if (!kf) continue;
            NodeKeyframeSnapshot ks;
            ks.time = static_cast<double>(kf->getTime());
            ks.translate = kf->getTranslate();
            ks.rotation = kf->getRotation();
            ks.scale = kf->getScale();
            snap.keys.push_back(ks);
        }
        out.push_back(std::move(snap));
    }
    return out;
}

// Recreate a clip from a NodeTrackSnapshot list. Routes through
// NodeAnimationManager so the per-clip handle allocator stays
// authoritative (and we don't accidentally clash with handles the
// user creates later via add-from-Inspector).
void rebuildClipFromSnapshot(const QString& name,
                             double length,
                             const std::vector<NodeTrackSnapshot>& tracks)
{
    auto* m = NodeAnimationManager::instance();
    if (!m) return;
    if (!m->createClip(name, length)) return;
    for (const auto& tr : tracks) {
        for (const auto& k : tr.keys) {
            m->addKeyframe(name, tr.nodeName, k.time,
                           k.translate, k.rotation, k.scale);
        }
    }
}

// Find an existing keyframe within `kKeyframeMergeEpsilon` of `time`
// on the given track; null when there's no overlap. Mirrors the
// manager's same-key detection so the command and the manager agree
// on which keyframe is "the same one."
Ogre::TransformKeyFrame* findKeyframeNear(Ogre::NodeAnimationTrack* track, double time)
{
    if (!track) return nullptr;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (!kf) continue;
        if (std::abs(kf->getTime() - time) < kKeyframeMergeEpsilon)
            return kf;
    }
    return nullptr;
}

} // namespace

// ──────────────── CreateNodeAnimClipCommand ─────────────────────────

CreateNodeAnimClipCommand::CreateNodeAnimClipCommand(const QString& name,
                                                     double length,
                                                     QUndoCommand* parent)
    : QUndoCommand(parent), mName(name), mLength(length)
{
    setText(QStringLiteral("Create node clip \"%1\"").arg(name));
}

void CreateNodeAnimClipCommand::redo()
{
    SentryReporter::addBreadcrumb("scene.anim.node.cmd",
        QStringLiteral("redo: create clip '%1'").arg(mName));
    if (auto* m = NodeAnimationManager::instance())
        m->createClip(mName, mLength);
}

void CreateNodeAnimClipCommand::undo()
{
    SentryReporter::addBreadcrumb("scene.anim.node.cmd",
        QStringLiteral("undo: create clip '%1'").arg(mName));
    if (auto* m = NodeAnimationManager::instance())
        m->deleteClip(mName);
}

// ──────────────── DeleteNodeAnimClipCommand ─────────────────────────

DeleteNodeAnimClipCommand::DeleteNodeAnimClipCommand(const QString& name,
                                                     QUndoCommand* parent)
    : QUndoCommand(parent), mName(name)
{
    setText(QStringLiteral("Delete node clip \"%1\"").arg(name));
    if (auto* scene = sceneMgr()) {
        const std::string sn = name.toStdString();
        if (scene->hasAnimation(sn)) {
            auto* anim = scene->getAnimation(sn);
            mLength = static_cast<double>(anim->getLength());
            mTracks = snapshotAllTracks(anim);
        }
    }
}

void DeleteNodeAnimClipCommand::redo()
{
    SentryReporter::addBreadcrumb("scene.anim.node.cmd",
        QStringLiteral("redo: delete clip '%1'").arg(mName));
    if (auto* m = NodeAnimationManager::instance())
        m->deleteClip(mName);
}

void DeleteNodeAnimClipCommand::undo()
{
    SentryReporter::addBreadcrumb("scene.anim.node.cmd",
        QStringLiteral("undo: delete clip '%1' (restore %2 track(s))")
            .arg(mName).arg(mTracks.size()));
    rebuildClipFromSnapshot(mName, mLength, mTracks);
}

// ──────────────── SetNodeKeyframeCommand ────────────────────────────

SetNodeKeyframeCommand::SetNodeKeyframeCommand(const QString& clipName,
                                               const QString& nodeName,
                                               double time,
                                               const Ogre::Vector3& translate,
                                               const Ogre::Quaternion& rotation,
                                               const Ogre::Vector3& scale,
                                               QUndoCommand* parent)
    : QUndoCommand(parent),
      mClipName(clipName),
      mNodeName(nodeName)
{
    mNew.time = time;
    mNew.translate = translate;
    mNew.rotation = rotation;
    mNew.scale = scale;
    setText(QStringLiteral("Keyframe \"%1\"@%2s on '%3'")
                .arg(clipName).arg(time, 0, 'f', 2).arg(nodeName));

    // Snapshot the prior state at construction so undo can restore
    // it exactly. Three cases:
    //   - No clip / no node / no track → mPriorKeyframe stays empty
    //     and mTrackCreatedByRedo will be set true on redo (we'll
    //     detect after-the-fact that the track came into existence
    //     because of us, not before).
    //   - Track exists but no key within epsilon → mPriorKeyframe
    //     empty, mTrackCreatedByRedo stays false. Undo deletes the
    //     single keyframe we added but keeps the (now-empty) track.
    //   - Key within epsilon exists → mPriorKeyframe holds its TRS;
    //     undo restores those values.
    if (auto* scene = sceneMgr()) {
        const std::string sclip = clipName.toStdString();
        if (scene->hasAnimation(sclip)) {
            auto* anim = scene->getAnimation(sclip);
            // We can't ask the manager for the handle without
            // triggering a (lazy) allocation. So we just scan all
            // tracks for one associated with our node — same idea
            // the snapshot helper uses.
            const auto& tracks = anim->_getNodeTrackList();
            for (auto it = tracks.begin(); it != tracks.end(); ++it) {
                auto* t = it->second;
                if (!t || !t->getAssociatedNode()) continue;
                if (QString::fromStdString(t->getAssociatedNode()->getName()) != nodeName)
                    continue;
                if (auto* kf = findKeyframeNear(t, time)) {
                    NodeKeyframeSnapshot prior;
                    prior.time = static_cast<double>(kf->getTime());
                    prior.translate = kf->getTranslate();
                    prior.rotation = kf->getRotation();
                    prior.scale = kf->getScale();
                    mPriorKeyframe = prior;
                }
                break;
            }
        }
    }
}

void SetNodeKeyframeCommand::redo()
{
    SentryReporter::addBreadcrumb("scene.anim.node.cmd",
        QStringLiteral("redo: keyframe '%1':'%2'@%3")
            .arg(mClipName, mNodeName).arg(mNew.time, 0, 'f', 3));
    auto* m = NodeAnimationManager::instance();
    if (!m) return;

    // Detect whether the track was created as a side effect of this
    // call. We compare track-list size before and after — if it
    // grew, the track is ours and undo should drop it entirely
    // (rather than leaving an empty NodeAnimationTrack stub).
    Ogre::Animation* anim = nullptr;
    if (auto* scene = sceneMgr()) {
        const std::string sclip = mClipName.toStdString();
        if (scene->hasAnimation(sclip)) anim = scene->getAnimation(sclip);
    }
    const size_t tracksBefore = anim ? anim->_getNodeTrackList().size() : 0;

    m->addKeyframe(mClipName, mNodeName, mNew.time,
                   mNew.translate, mNew.rotation, mNew.scale);

    const size_t tracksAfter = anim ? anim->_getNodeTrackList().size() : 0;
    mTrackCreatedByRedo = (tracksAfter > tracksBefore);
}

void SetNodeKeyframeCommand::undo()
{
    SentryReporter::addBreadcrumb("scene.anim.node.cmd",
        QStringLiteral("undo: keyframe '%1':'%2'@%3")
            .arg(mClipName, mNodeName).arg(mNew.time, 0, 'f', 3));
    auto* scene = sceneMgr();
    if (!scene) return;
    const std::string sclip = mClipName.toStdString();
    if (!scene->hasAnimation(sclip)) return;
    auto* anim = scene->getAnimation(sclip);
    if (!anim) return;

    // Find the track for our node.
    Ogre::NodeAnimationTrack* track = nullptr;
    unsigned short handle = 0;
    const auto& tracks = anim->_getNodeTrackList();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        auto* t = it->second;
        if (!t || !t->getAssociatedNode()) continue;
        if (QString::fromStdString(t->getAssociatedNode()->getName()) == mNodeName) {
            track = t;
            handle = it->first;
            break;
        }
    }
    if (!track) return;

    if (mPriorKeyframe.has_value()) {
        // Restore the prior values in place.
        if (auto* kf = findKeyframeNear(track, mNew.time)) {
            kf->setTranslate(mPriorKeyframe->translate);
            kf->setRotation(mPriorKeyframe->rotation);
            kf->setScale(mPriorKeyframe->scale);
        }
    } else {
        // We added a fresh key. Drop it. Ogre's NodeAnimationTrack
        // exposes removeKeyFrame(index) — find the index by scanning.
        for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
            auto* kf = track->getKeyFrame(i);
            if (kf && std::abs(kf->getTime() - mNew.time) < kKeyframeMergeEpsilon) {
                track->removeKeyFrame(i);
                break;
            }
        }
        // If we created the track in redo, drop the whole (now-empty)
        // track so the clip returns to its pre-redo state. Also tell
        // the manager to forget its `{clip, node} → handle` entry —
        // otherwise the stale handle would shadow a later addKeyframe
        // for a different node, corrupting that node's animation.
        // Same class of bug as Codex P1 on PR #584 (hash-truncation
        // collisions), just in the undo path.
        if (mTrackCreatedByRedo && track->getNumKeyFrames() == 0) {
            anim->destroyNodeTrack(handle);
            if (auto* m = NodeAnimationManager::instance())
                m->forgetTrackHandle(mClipName, mNodeName);
        }
    }
}

namespace {

// Shared by the move + delete commands: resolve the NodeAnimationTrack
// for `nodeName` on `clipName`. Null when the clip/track is missing.
Ogre::NodeAnimationTrack* trackForNode(const QString& clipName,
                                       const QString& nodeName)
{
    auto* scene = sceneMgr();
    if (!scene) return nullptr;
    const std::string sclip = clipName.toStdString();
    if (!scene->hasAnimation(sclip)) return nullptr;
    auto* anim = scene->getAnimation(sclip);
    if (!anim) return nullptr;
    const auto& tracks = anim->_getNodeTrackList();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        auto* t = it->second;
        if (!t || !t->getAssociatedNode()) continue;
        if (QString::fromStdString(t->getAssociatedNode()->getName()) == nodeName)
            return t;
    }
    return nullptr;
}

// Re-time the keyframe nearest `from` to `to` while keeping its TRS.
// Ogre keeps keyframes time-sorted internally, so we remove-and-re-add
// rather than mutate the time in place (setTime exists but doesn't
// resort, which would corrupt interpolation).
void retimeKeyframe(Ogre::NodeAnimationTrack* track, double from, double to)
{
    if (!track) return;
    auto* src = findKeyframeNear(track, from);
    if (!src) return;
    const Ogre::Vector3 t = src->getTranslate();
    const Ogre::Quaternion r = src->getRotation();
    const Ogre::Vector3 s = src->getScale();
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        if (track->getKeyFrame(i) == src) { track->removeKeyFrame(i); break; }
    }
    auto* dst = track->createNodeKeyFrame(static_cast<Ogre::Real>(to));
    if (!dst) return;
    dst->setTranslate(t);
    dst->setRotation(r);
    dst->setScale(s);
}

} // namespace

// ──────────────── MoveNodeKeyframeCommand ───────────────────────────

MoveNodeKeyframeCommand::MoveNodeKeyframeCommand(const QString& clipName,
                                                 const QString& nodeName,
                                                 double oldTime,
                                                 double newTime,
                                                 QUndoCommand* parent)
    : QUndoCommand(parent),
      mClipName(clipName),
      mNodeName(nodeName),
      mOldTime(oldTime),
      mNewTime(newTime)
{
    setText(QStringLiteral("Move node keyframe '%1'@%2→%3s")
                .arg(nodeName).arg(oldTime, 0, 'f', 2).arg(newTime, 0, 'f', 2));
}

void MoveNodeKeyframeCommand::redo()
{
    SentryReporter::addBreadcrumb("scene.anim.node.cmd",
        QStringLiteral("redo: move kf '%1':'%2' %3→%4")
            .arg(mClipName, mNodeName).arg(mOldTime, 0, 'f', 3).arg(mNewTime, 0, 'f', 3));
    retimeKeyframe(trackForNode(mClipName, mNodeName), mOldTime, mNewTime);
    if (auto* m = NodeAnimationManager::instance())
        m->emitKeyframesChanged(mClipName);
}

void MoveNodeKeyframeCommand::undo()
{
    SentryReporter::addBreadcrumb("scene.anim.node.cmd",
        QStringLiteral("undo: move kf '%1':'%2' %3→%4")
            .arg(mClipName, mNodeName).arg(mNewTime, 0, 'f', 3).arg(mOldTime, 0, 'f', 3));
    retimeKeyframe(trackForNode(mClipName, mNodeName), mNewTime, mOldTime);
    if (auto* m = NodeAnimationManager::instance())
        m->emitKeyframesChanged(mClipName);
}

// ──────────────── DeleteNodeKeyframeCommand ─────────────────────────

DeleteNodeKeyframeCommand::DeleteNodeKeyframeCommand(const QString& clipName,
                                                     const QString& nodeName,
                                                     double time,
                                                     QUndoCommand* parent)
    : QUndoCommand(parent),
      mClipName(clipName),
      mNodeName(nodeName)
{
    if (auto* track = trackForNode(clipName, nodeName)) {
        if (auto* kf = findKeyframeNear(track, time)) {
            mSnapshot.time = static_cast<double>(kf->getTime());
            mSnapshot.translate = kf->getTranslate();
            mSnapshot.rotation = kf->getRotation();
            mSnapshot.scale = kf->getScale();
            mValid = true;
        }
    }
    setText(QStringLiteral("Delete node keyframe '%1'@%2s")
                .arg(nodeName).arg(time, 0, 'f', 2));
}

void DeleteNodeKeyframeCommand::redo()
{
    if (!mValid) return;
    SentryReporter::addBreadcrumb("scene.anim.node.cmd",
        QStringLiteral("redo: delete kf '%1':'%2'@%3")
            .arg(mClipName, mNodeName).arg(mSnapshot.time, 0, 'f', 3));
    auto* track = trackForNode(mClipName, mNodeName);
    if (!track) return;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = track->getKeyFrame(i);
        if (kf && std::abs(kf->getTime() - mSnapshot.time) < kKeyframeMergeEpsilon) {
            track->removeKeyFrame(i);
            break;
        }
    }
    if (auto* m = NodeAnimationManager::instance())
        m->emitKeyframesChanged(mClipName);
}

void DeleteNodeKeyframeCommand::undo()
{
    if (!mValid) return;
    SentryReporter::addBreadcrumb("scene.anim.node.cmd",
        QStringLiteral("undo: delete kf '%1':'%2'@%3")
            .arg(mClipName, mNodeName).arg(mSnapshot.time, 0, 'f', 3));
    // Re-add through the manager so a track destroyed by removing its
    // last keyframe is recreated on the correct handle.
    if (auto* m = NodeAnimationManager::instance())
        m->addKeyframe(mClipName, mNodeName, mSnapshot.time,
                       mSnapshot.translate, mSnapshot.rotation, mSnapshot.scale);
}
