/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "NodeAnimationManager.h"

#include "Manager.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/NodeAnimCommands.h"
#include "commands/ResampleCurveCommand.h"
#include "commands/DecimateTrackCommand.h"

#include <QUndoStack>
#include <cmath>

#include <QCoreApplication>
#include <QHash>
#include <QThread>
#include <QVariantMap>

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

namespace {

// Per the project's singleton-on-main-thread convention (CLAUDE.md:
// "All run on the main thread."), assert any cross-thread access at
// lifecycle entry points so a regression surfaces loudly in debug
// builds.
inline void assertMainThread()
{
    Q_ASSERT(QCoreApplication::instance());
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
}

// Distance below which two keyframe times are treated as the same
// keyframe — 1ms matches the granularity of the dope-sheet slider
// (which displays seconds with 2-3 decimals) so users can't
// accidentally create back-to-back keys.
constexpr double kKeyframeMergeEpsilon = 1e-3;

// ── Curve-editor channel helpers (#520) ─────────────────────────────
// Deliberately mirror AnimationControlController's private readChannel /
// isKnownChannel / writeChannel / collectActiveChannels so a node clip's
// TRS curves edit IDENTICALLY to a skeletal bone's tracks. Kept local
// (not shared) because the two managers don't include each other and the
// helper bodies are tiny — duplicating is cheaper than a new header.

// Below-this magnitude a channel is treated as "at its identity value"
// (0 for translate/rotation-imaginary, 1 for scale) so an all-default
// track doesn't advertise ten always-on curves. Matches
// AnimationControlController::kChannelEpsilon.
constexpr float kChannelEpsilon = 1e-4f;

// Resolve channel id → scalar reader on a TransformKeyFrame.
double readChannel(const Ogre::TransformKeyFrame* kf, const QString& ch)
{
    const QString c = ch.toLower();
    if (c == "tx") return kf->getTranslate().x;
    if (c == "ty") return kf->getTranslate().y;
    if (c == "tz") return kf->getTranslate().z;
    if (c == "rw") return kf->getRotation().w;
    if (c == "rx") return kf->getRotation().x;
    if (c == "ry") return kf->getRotation().y;
    if (c == "rz") return kf->getRotation().z;
    if (c == "sx") return kf->getScale().x;
    if (c == "sy") return kf->getScale().y;
    if (c == "sz") return kf->getScale().z;
    return 0.0;
}

bool isKnownChannel(const QString& ch)
{
    static const QStringList kKnown = {
        QStringLiteral("tx"), QStringLiteral("ty"), QStringLiteral("tz"),
        QStringLiteral("rw"), QStringLiteral("rx"),
        QStringLiteral("ry"), QStringLiteral("rz"),
        QStringLiteral("sx"), QStringLiteral("sy"), QStringLiteral("sz"),
    };
    return kKnown.contains(ch.toLower());
}

// True when `ch` is one of the four rotation components (rw/rx/ry/rz).
bool isRotationChannel(const QString& ch)
{
    const QString c = ch.toLower();
    return c == "rw" || c == "rx" || c == "ry" || c == "rz";
}

// Write the requested scalar onto the keyframe's TRS without touching
// the other nine components. Rotation channels overwrite one quaternion
// component then NORMALISE — a raw component edit denormalises the quat,
// which would skew every interpolated slerp between it and its neighbours.
void writeChannel(Ogre::TransformKeyFrame* kf, const QString& ch, double v)
{
    const QString c = ch.toLower();
    const float fv = static_cast<float>(v);
    if (c == "tx") { auto t = kf->getTranslate(); t.x = fv; kf->setTranslate(t); return; }
    if (c == "ty") { auto t = kf->getTranslate(); t.y = fv; kf->setTranslate(t); return; }
    if (c == "tz") { auto t = kf->getTranslate(); t.z = fv; kf->setTranslate(t); return; }
    if (isRotationChannel(c)) {
        Ogre::Quaternion r = kf->getRotation();
        if      (c == "rw") r.w = fv;
        else if (c == "rx") r.x = fv;
        else if (c == "ry") r.y = fv;
        else                r.z = fv;   // rz
        // Renormalise so the quaternion stays unit-length; a degenerate
        // (all-zero) edit falls back to identity rather than NaN.
        if (r.Norm() > 1e-8f) r.normalise();
        else                  r = Ogre::Quaternion::IDENTITY;
        kf->setRotation(r);
        return;
    }
    if (c == "sx") { auto s = kf->getScale(); s.x = fv; kf->setScale(s); return; }
    if (c == "sy") { auto s = kf->getScale(); s.y = fv; kf->setScale(s); return; }
    if (c == "sz") { auto s = kf->getScale(); s.z = fv; kf->setScale(s); return; }
}

// The active-channel bool map for a node track (mirrors
// AnimationControlController::collectActiveChannels). A channel is
// "active" when at least one keyframe deviates from the identity value.
QVariantMap collectActiveChannels(const Ogre::NodeAnimationTrack* track)
{
    bool tx = false, ty = false, tz = false;
    bool rw = false, rx = false, ry = false, rz = false;
    bool sx = false, sy = false, sz = false;
    for (unsigned short i = 0; track && i < track->getNumKeyFrames(); ++i) {
        const auto* kf = static_cast<const Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        const Ogre::Vector3    t = kf->getTranslate();
        const Ogre::Quaternion r = kf->getRotation();
        const Ogre::Vector3    s = kf->getScale();
        if (std::fabs(t.x) > kChannelEpsilon) tx = true;
        if (std::fabs(t.y) > kChannelEpsilon) ty = true;
        if (std::fabs(t.z) > kChannelEpsilon) tz = true;
        // Sign-agnostic rotation identity check (w=±1 is identity).
        if (std::fabs(std::fabs(r.w) - 1.0f) > kChannelEpsilon) rw = true;
        if (std::fabs(r.x) > kChannelEpsilon) rx = true;
        if (std::fabs(r.y) > kChannelEpsilon) ry = true;
        if (std::fabs(r.z) > kChannelEpsilon) rz = true;
        if (std::fabs(s.x - 1.0f) > kChannelEpsilon) sx = true;
        if (std::fabs(s.y - 1.0f) > kChannelEpsilon) sy = true;
        if (std::fabs(s.z - 1.0f) > kChannelEpsilon) sz = true;
    }
    QVariantMap m;
    m[QStringLiteral("tx")] = tx; m[QStringLiteral("ty")] = ty; m[QStringLiteral("tz")] = tz;
    m[QStringLiteral("rw")] = rw; m[QStringLiteral("rx")] = rx;
    m[QStringLiteral("ry")] = ry; m[QStringLiteral("rz")] = rz;
    m[QStringLiteral("sx")] = sx; m[QStringLiteral("sy")] = sy; m[QStringLiteral("sz")] = sz;
    return m;
}

} // namespace

NodeAnimationManager* NodeAnimationManager::s_instance = nullptr;

NodeAnimationManager* NodeAnimationManager::instance()
{
    assertMainThread();
    if (!s_instance) s_instance = new NodeAnimationManager();
    return s_instance;
}

NodeAnimationManager* NodeAnimationManager::qmlInstance(QQmlEngine*, QJSEngine*)
{
    assertMainThread();
    return instance();
}

void NodeAnimationManager::kill()
{
    assertMainThread();
    if (!s_instance) return;
    delete s_instance;
    s_instance = nullptr;
}

NodeAnimationManager::NodeAnimationManager(QObject* parent) : QObject(parent)
{
    // Clean up node clips when their target SceneNode is destroyed, so a
    // deleted node can't leave a dangling track/AnimationState the render
    // loop would crash on.
    if (auto* mgr = Manager::getSingletonPtr())
        connect(mgr, &Manager::sceneNodeDestroyed,
                this, &NodeAnimationManager::onSceneNodeDestroyed);
}

NodeAnimationManager::~NodeAnimationManager() = default;

void NodeAnimationManager::onSceneNodeDestroyed(Ogre::SceneNode* node)
{
    assertMainThread();
    if (!node) return;
    auto* mgr = Manager::getSingletonPtr();
    auto* scene = mgr ? mgr->getSceneMgr() : nullptr;
    if (!scene) return;
    const QString nodeName = QString::fromStdString(node->getName());

    // Walk every SceneManager animation; drop tracks whose associated node is
    // the one being destroyed, then delete any clip left with no node tracks.
    std::vector<std::string> clipsToDelete;
    for (unsigned short ai = 0; ai < scene->getNumAnimations(); ++ai) {
        Ogre::Animation* anim = scene->getAnimation(ai);
        if (!anim) continue;
        // Collect handles targeting this node first (can't erase while iterating).
        std::vector<unsigned short> handles;
        const auto& tracks = anim->_getNodeTrackList();
        for (auto it = tracks.begin(); it != tracks.end(); ++it) {
            Ogre::NodeAnimationTrack* t = it->second;
            if (t && t->getAssociatedNode() == node)
                handles.push_back(it->first);
        }
        if (handles.empty()) continue;
        // Disable the driving state before mutating tracks so nothing applies
        // mid-teardown.
        if (scene->hasAnimationState(anim->getName())) {
            if (auto* st = scene->getAnimationState(anim->getName()))
                st->setEnabled(false);
        }
        for (unsigned short h : handles)
            anim->destroyNodeTrack(h);
        // Forget the handle cache for this (clip, node) pair.
        forgetTrackHandle(QString::fromStdString(anim->getName()), nodeName);
        if (anim->getNumNodeTracks() == 0)
            clipsToDelete.push_back(anim->getName());
    }

    for (const auto& n : clipsToDelete)
        deleteClip(QString::fromStdString(n));

    if (!clipsToDelete.empty() || !nodeName.isEmpty()) {
        // Refresh listeners (dope sheet / inspector) — a clip may have vanished
        // or lost its only track.
        emit clipsChanged();
    }
}

unsigned short NodeAnimationManager::trackHandleForNode(const QString& clipName,
                                                        const QString& nodeName)
{
    // Per-clip allocator: first sighting of (clipName, nodeName)
    // claims the next free handle inside the clip's Animation
    // (Ogre's hasNodeTrack(handle) is the source of truth — we walk
    // 0..N_handles to find the first gap). Subsequent calls find
    // the existing handle in m_trackHandles and reuse it, so
    // repeated addKeyframe writes go to the same NodeAnimationTrack.
    //
    // Replaces an earlier `qHash & 0xFFFF` strategy that silently
    // collapsed two different node names onto the same track when
    // their hashes collided — birthday paradox hits 50% at ~300
    // names on a 16-bit space (Codex P1 on PR #584).
    auto& clipMap = m_trackHandles[clipName];
    auto it = clipMap.find(nodeName);
    if (it != clipMap.end()) return *it;

    // Allocate the lowest unused handle in the clip's track table.
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return 0;
    auto* scene = mgr->getSceneMgr();
    if (!scene) return 0;
    const std::string sclip = clipName.toStdString();
    Ogre::Animation* anim = scene->hasAnimation(sclip) ? scene->getAnimation(sclip) : nullptr;

    unsigned short handle = 0;
    while (anim && anim->hasNodeTrack(handle)) {
        // Defensive cap — wrap at 65535 would happen automatically,
        // but at that point we're well past the realistic node count.
        if (handle == 65535) break;
        ++handle;
    }
    clipMap.insert(nodeName, handle);
    return handle;
}

Ogre::SceneNode* NodeAnimationManager::findSceneNode(const QString& nodeName)
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    auto* scene = mgr->getSceneMgr();
    if (!scene) return nullptr;
    const std::string sn = nodeName.toStdString();
    if (!scene->hasSceneNode(sn)) return nullptr;
    return scene->getSceneNode(sn);
}

bool NodeAnimationManager::createClip(const QString& name, double length)
{
    assertMainThread();
    if (name.isEmpty() || length <= 0.0) return false;
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return false;
    auto* scene = mgr->getSceneMgr();
    if (!scene) return false;
    const std::string sn = name.toStdString();
    if (scene->hasAnimation(sn)) return false;

    scene->createAnimation(sn, static_cast<Ogre::Real>(length));
    scene->createAnimationState(sn);

    SentryReporter::addBreadcrumb("scene.anim.node",
        QStringLiteral("create clip '%1' (%2s)").arg(name).arg(length, 0, 'f', 3));
    emit clipsChanged();
    return true;
}

bool NodeAnimationManager::deleteClip(const QString& name)
{
    assertMainThread();
    if (name.isEmpty()) return false;
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return false;
    auto* scene = mgr->getSceneMgr();
    if (!scene) return false;
    const std::string sn = name.toStdString();
    if (!scene->hasAnimation(sn)) return false;

    if (scene->hasAnimationState(sn))
        scene->destroyAnimationState(sn);
    scene->removeAnimation(sn);
    // Drop the clip's node→handle allocator entries — otherwise a
    // later createClip(name, ...) would reuse stale handles that
    // map to tracks Ogre no longer has.
    m_trackHandles.remove(name);

    // Clear active/edit state pointing at the just-deleted clip. Every delete
    // path (scene-node cleanup, undoable delete, undo-of-create) funnels here;
    // leaving m_editingClip == name kept isEditing(name) true, so a later clip
    // reusing the name could never be enabled (setClipEnabled refuses while
    // editing). Reset + signal so the UI drops the stale draft row. (#517 review)
    if (m_activeClip == name) { m_activeClip.clear(); emit activeClipChanged(); }
    if (m_editingClip == name) { m_editingClip.clear(); emit editingClipChanged(); }

    SentryReporter::addBreadcrumb("scene.anim.node",
        QStringLiteral("delete clip '%1'").arg(name));
    emit clipsChanged();
    return true;
}

bool NodeAnimationManager::addKeyframe(const QString& clipName,
                                       const QString& nodeName,
                                       double time,
                                       const Ogre::Vector3& translate,
                                       const Ogre::Quaternion& rotation,
                                       const Ogre::Vector3& scale)
{
    assertMainThread();
    if (clipName.isEmpty() || nodeName.isEmpty()) return false;
    if (time < 0.0) return false;

    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return false;
    auto* scene = mgr->getSceneMgr();
    if (!scene) return false;

    const std::string sclip = clipName.toStdString();
    if (!scene->hasAnimation(sclip)) return false;
    Ogre::Animation* anim = scene->getAnimation(sclip);
    if (!anim) return false;
    if (time > anim->getLength()) return false;

    Ogre::SceneNode* node = findSceneNode(nodeName);
    if (!node) return false;

    const unsigned short handle = trackHandleForNode(clipName, nodeName);
    Ogre::NodeAnimationTrack* track =
        anim->hasNodeTrack(handle) ? anim->getNodeTrack(handle)
                                   : anim->createNodeTrack(handle, node);
    if (!track) return false;

    // Idempotent overwrite: if the closest existing keyframe is
    // within `kKeyframeMergeEpsilon`, mutate it in place instead of
    // creating a near-duplicate. Without this, dragging a slider
    // back and forth would dribble extra keys all along the path.
    Ogre::TransformKeyFrame* kf = nullptr;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* candidate = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (candidate && std::abs(candidate->getTime() - time) < kKeyframeMergeEpsilon) {
            kf = candidate;
            break;
        }
    }
    if (!kf) kf = track->createNodeKeyFrame(static_cast<Ogre::Real>(time));
    if (!kf) return false;

    kf->setTranslate(translate);
    kf->setRotation(rotation);
    kf->setScale(scale);

    SentryReporter::addBreadcrumb("scene.anim.node",
        QStringLiteral("keyframe '%1':'%2'@%3").arg(clipName, nodeName).arg(time, 0, 'f', 3));
    emit keyframesChanged(clipName);
    return true;
}

void NodeAnimationManager::forgetTrackHandle(const QString& clipName,
                                             const QString& nodeName)
{
    assertMainThread();
    auto it = m_trackHandles.find(clipName);
    if (it == m_trackHandles.end()) return;
    it->remove(nodeName);
    if (it->isEmpty()) m_trackHandles.erase(it);
}

bool NodeAnimationManager::setClipEnabled(const QString& name, bool enabled)
{
    assertMainThread();
    if (name.isEmpty()) return false;
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return false;
    auto* scene = mgr->getSceneMgr();
    if (!scene) return false;
    const std::string sn = name.toStdString();
    if (!scene->hasAnimationState(sn)) return false;
    auto* state = scene->getAnimationState(sn);
    if (!state) return false;
    // Never enable a clip that's in an open edit session — the node must stay
    // free for the gizmo. (Disabling is always allowed.)
    if (enabled && m_editingClip == name) return false;
    state->setEnabled(enabled);
    if (enabled) state->setTimePosition(0.0f);
    return true;
}

QStringList NodeAnimationManager::listClips() const
{
    QStringList out;
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return out;
    auto* scene = mgr->getSceneMgr();
    if (!scene) return out;
    // SceneManager has `getNumAnimations()` + `getAnimation(index)` —
    // but it includes ALL animations (skeletal-attached clips, our
    // node clips, etc.). For the C1 manager surface we only need
    // node-clip names, but the filter is "has a NodeAnimationTrack
    // somewhere in the animation." Anything we created via
    // createClip qualifies; same-named anims created elsewhere are
    // either purely-skeletal (zero NodeAnimationTrack) or our own.
    for (unsigned short i = 0; i < scene->getNumAnimations(); ++i) {
        Ogre::Animation* a = scene->getAnimation(i);
        if (!a) continue;
        // Cheap filter: only animations whose first track is a
        // NodeAnimationTrack (and is non-empty) belong to us. Empty
        // freshly-created clips also qualify so the UI can list them
        // before any keyframe is added.
        const auto& nodeTracks = a->_getNodeTrackList();
        const auto& vertTracks = a->_getVertexTrackList();
        // Skip purely-vertex animations (they belong to the morph /
        // mesh-anim subsystems, not node-anim).
        if (nodeTracks.empty() && !vertTracks.empty()) continue;
        out << QString::fromStdString(a->getName());
    }
    return out;
}

QList<double> NodeAnimationManager::keyTimesForNode(const QString& clipName,
                                                    const QString& nodeName) const
{
    QList<double> out;
    if (clipName.isEmpty() || nodeName.isEmpty()) return out;
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return out;
    auto* scene = mgr->getSceneMgr();
    if (!scene) return out;
    const std::string sclip = clipName.toStdString();
    if (!scene->hasAnimation(sclip)) return out;
    Ogre::Animation* anim = scene->getAnimation(sclip);
    if (!anim) return out;
    // Read-only lookup — don't allocate a new handle for a node that
    // doesn't yet have one. If the {clip, node} pair was never
    // written, return empty.
    auto clipIt = m_trackHandles.constFind(clipName);
    if (clipIt == m_trackHandles.constEnd()) return out;
    auto handleIt = clipIt->constFind(nodeName);
    if (handleIt == clipIt->constEnd()) return out;
    const unsigned short handle = *handleIt;
    if (!anim->hasNodeTrack(handle)) return out;
    auto* track = anim->getNodeTrack(handle);
    if (!track) return out;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = track->getKeyFrame(i);
        if (kf) out.append(static_cast<double>(kf->getTime()));
    }
    return out;
}

void NodeAnimationManager::emitKeyframesChanged(const QString& clipName)
{
    emit keyframesChanged(clipName);
}

void NodeAnimationManager::emitClipsChanged()
{
    emit clipsChanged();
}

void NodeAnimationManager::setActiveClip(const QString& name)
{
    assertMainThread();
    if (m_activeClip == name) return;
    m_activeClip = name;
    emit activeClipChanged();
}

void NodeAnimationManager::beginEdit(const QString& name)
{
    assertMainThread();
    if (name.isEmpty()) return;
    // Force the clip's state DISABLED so the node is free for the gizmo while
    // authoring (an enabled state re-drives the node every frame).
    setClipEnabled(name, false);
    setActiveClip(name);
    if (m_editingClip != name) {
        m_editingClip = name;
        emit editingClipChanged();
    }
    // The set of listed clips changes (a draft is hidden from the main list).
    emit clipsChanged();
}

void NodeAnimationManager::endEdit()
{
    assertMainThread();
    if (m_editingClip.isEmpty()) return;
    m_editingClip.clear();
    emit editingClipChanged();
    // The committed clip now appears in the main animation list.
    emit clipsChanged();
}

namespace {
// The Ogre Animation for a clip, or null. Shared by the read helpers.
Ogre::Animation* animForClip(const QString& clipName)
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    auto* scene = mgr->getSceneMgr();
    if (!scene) return nullptr;
    const std::string sclip = clipName.toStdString();
    return scene->hasAnimation(sclip) ? scene->getAnimation(sclip) : nullptr;
}
} // namespace

QStringList NodeAnimationManager::animatedNodes(const QString& clipName) const
{
    QStringList out;
    Ogre::Animation* anim = animForClip(clipName);
    if (!anim) return out;
    // Iterate in handle order (the track map is keyed by handle) so
    // the row order is stable across rebuilds.
    const auto& tracks = anim->_getNodeTrackList();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        Ogre::NodeAnimationTrack* t = it->second;
        if (!t) continue;
        if (Ogre::Node* node = t->getAssociatedNode())
            out << QString::fromStdString(node->getName());
    }
    return out;
}

double NodeAnimationManager::clipLength(const QString& clipName) const
{
    Ogre::Animation* anim = animForClip(clipName);
    return anim ? static_cast<double>(anim->getLength()) : 0.0;
}

QVariantList NodeAnimationManager::nodeRows(const QString& clipName) const
{
    QVariantList rows;
    Ogre::Animation* anim = animForClip(clipName);
    if (!anim) return rows;
    const auto& tracks = anim->_getNodeTrackList();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        Ogre::NodeAnimationTrack* t = it->second;
        if (!t) continue;
        Ogre::Node* node = t->getAssociatedNode();
        if (!node) continue;

        QVariantList keyTimes;
        keyTimes.reserve(static_cast<int>(t->getNumKeyFrames()));
        for (unsigned short i = 0; i < t->getNumKeyFrames(); ++i)
            keyTimes.append(static_cast<double>(t->getKeyFrame(i)->getTime()));

        QVariantMap row;
        row[QStringLiteral("node")]     = QString::fromStdString(node->getName());
        row[QStringLiteral("keyTimes")] = keyTimes;
        // #520: the curve editor needs the active-channel map to know
        // which TRS curves to draw. Backward-compatible add — the dope
        // sheet's node band ignores it and still reads keyTimes.
        row[QStringLiteral("channels")] = collectActiveChannels(t);
        rows.append(row);
    }
    return rows;
}

QVariantMap NodeAnimationManager::nodeChannels(const QString& clipName,
                                               const QString& nodeName) const
{
    Ogre::Animation* anim = animForClip(clipName);
    if (!anim || nodeName.isEmpty()) return {};
    // Resolve the track by associated-node name (same walk animatedNodes /
    // the commands use — the m_trackHandles cache can be stale after undo).
    const auto& tracks = anim->_getNodeTrackList();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        Ogre::NodeAnimationTrack* t = it->second;
        if (!t || !t->getAssociatedNode()) continue;
        if (QString::fromStdString(t->getAssociatedNode()->getName()) == nodeName)
            return collectActiveChannels(t);
    }
    return {};
}

QVariantList NodeAnimationManager::nodeChannelValuesAt(const QString& clipName,
                                                       const QString& nodeName,
                                                       const QString& channel) const
{
    QVariantList out;
    if (!isKnownChannel(channel)) return out;
    Ogre::Animation* anim = animForClip(clipName);
    if (!anim || nodeName.isEmpty()) return out;
    const auto& tracks = anim->_getNodeTrackList();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        Ogre::NodeAnimationTrack* t = it->second;
        if (!t || !t->getAssociatedNode()) continue;
        if (QString::fromStdString(t->getAssociatedNode()->getName()) != nodeName)
            continue;
        out.reserve(static_cast<int>(t->getNumKeyFrames()));
        for (unsigned short i = 0; i < t->getNumKeyFrames(); ++i) {
            const auto* kf = static_cast<const Ogre::TransformKeyFrame*>(t->getKeyFrame(i));
            out.append(readChannel(kf, channel));
        }
        break;
    }
    return out;
}

bool NodeAnimationManager::setNodeKeyframeValue(const QString& clipName,
                                                const QString& nodeName,
                                                const QString& channel,
                                                double time, double value)
{
    assertMainThread();
    if (clipName.isEmpty() || nodeName.isEmpty() || !isKnownChannel(channel))
        return false;
    Ogre::Animation* anim = animForClip(clipName);
    if (!anim) return false;

    // Resolve the track + the exact keyframe at `time`. We snapshot the
    // FULL current TRS, overwrite the one requested channel on it, and
    // push a SetNodeKeyframeCommand — its overwrite path restores the
    // prior TRS on undo, so a single-channel curve edit is Ctrl+Z-able
    // exactly like the bone path (which uses SetKeyframeValueCommand).
    Ogre::TransformKeyFrame* target = nullptr;
    const auto& tracks = anim->_getNodeTrackList();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        Ogre::NodeAnimationTrack* t = it->second;
        if (!t || !t->getAssociatedNode()) continue;
        if (QString::fromStdString(t->getAssociatedNode()->getName()) != nodeName)
            continue;
        for (unsigned short i = 0; i < t->getNumKeyFrames(); ++i) {
            auto* kf = static_cast<Ogre::TransformKeyFrame*>(t->getKeyFrame(i));
            if (std::abs(kf->getTime() - static_cast<float>(time)) < kKeyframeMergeEpsilon) {
                target = kf;
                break;
            }
        }
        break;
    }
    if (!target) return false;   // no keyframe at `time` — don't push a no-op

    // Compose the new full TRS: start from the current values, apply the
    // one requested channel (rotation is renormalised, matching
    // writeChannel). We build the TRS in locals rather than mutating the
    // live keyframe here because committing the change is the command's
    // job — the command snapshots the prior TRS for undo, then writes.
    Ogre::Vector3    t = target->getTranslate();
    Ogre::Quaternion r = target->getRotation();
    Ogre::Vector3    s = target->getScale();
    const QString c = channel.toLower();
    const float fv = static_cast<float>(value);
    if      (c == "tx") t.x = fv;
    else if (c == "ty") t.y = fv;
    else if (c == "tz") t.z = fv;
    else if (isRotationChannel(c)) {
        if      (c == "rw") r.w = fv;
        else if (c == "rx") r.x = fv;
        else if (c == "ry") r.y = fv;
        else                r.z = fv;
        if (r.Norm() > 1e-8f) r.normalise();
        else                  r = Ogre::Quaternion::IDENTITY;
    }
    else if (c == "sx") s.x = fv;
    else if (c == "sy") s.y = fv;
    else if (c == "sz") s.z = fv;

    SentryReporter::addBreadcrumb("scene.anim.node.curve",
        QStringLiteral("set '%1':'%2'.%3@%4=%5")
            .arg(clipName, nodeName, c)
            .arg(time, 0, 'f', 3).arg(value, 0, 'f', 4));
    UndoManager::getSingleton()->push(new SetNodeKeyframeCommand(
        clipName, nodeName, time, t, r, s));

    // The command's redo() overwrote the keyframe's TRS in place via
    // addKeyframe (setTranslate/setRotation/setScale) — which does NOT
    // invalidate the track's interpolation caches. Flush them so the
    // next apply() (Play preview) reflects the edit instead of replaying
    // the pre-edit pose (the arm-space gotcha in CLAUDE.md). (#520)
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        Ogre::NodeAnimationTrack* t2 = it->second;
        if (t2 && t2->getAssociatedNode() &&
            QString::fromStdString(t2->getAssociatedNode()->getName()) == nodeName) {
            t2->_keyFrameDataChanged();
            break;
        }
    }
    return true;
}

bool NodeAnimationManager::setNodeKeyframeValuePreview(const QString& clipName,
                                                       const QString& nodeName,
                                                       const QString& channel,
                                                       double time, double value)
{
    assertMainThread();
    if (clipName.isEmpty() || nodeName.isEmpty() || !isKnownChannel(channel))
        return false;
    Ogre::Animation* anim = animForClip(clipName);
    if (!anim) return false;

    Ogre::NodeAnimationTrack* track = nullptr;
    Ogre::TransformKeyFrame* target = nullptr;
    const auto& tracks = anim->_getNodeTrackList();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        Ogre::NodeAnimationTrack* t = it->second;
        if (!t || !t->getAssociatedNode()) continue;
        if (QString::fromStdString(t->getAssociatedNode()->getName()) != nodeName)
            continue;
        track = t;
        for (unsigned short i = 0; i < t->getNumKeyFrames(); ++i) {
            auto* kf = static_cast<Ogre::TransformKeyFrame*>(t->getKeyFrame(i));
            if (std::abs(kf->getTime() - static_cast<float>(time)) < kKeyframeMergeEpsilon) {
                target = kf;
                break;
            }
        }
        break;
    }
    if (!track || !target) return false;

    // No undo push — this is the live-drag path; the caller commits the
    // final value through setNodeKeyframeValue on release.
    writeChannel(target, channel, value);
    // setRotation/setTranslate/setScale do NOT invalidate the track's
    // interpolation caches (the arm-space gotcha documented in CLAUDE.md),
    // so force a cache rebuild or the next apply() replays the pre-edit
    // pose — the drag would appear to lag one event. (#520)
    track->_keyFrameDataChanged();
    return true;
}

// ── Curve resample / Bake (#520) ────────────────────────────────────
// Node-clip equivalents of AnimationControlController::resampleCurveSegment
// / resampleAllSegmentsForBone. Ogre's NodeAnimationTrack interpolates
// linearly/spherically between keyframes, so a Bezier curve authored in
// the curve editor is purely VISUAL until it is densified into keyframes
// that trace the curve. These push a node-capable ResampleCurveCommand
// (isNodeClip=true) — the exact same densification/undo machinery the
// bone Bake uses.
//
// The CurveEditModel tangent side-table is keyed by
// (entityName, animName, boneName, channel). qml/AnimationCurveEditor.qml
// writes node-clip tangents under (selectedEntityName, selectedAnimation,
// node) which for a node clip is (nodeName, clipName, nodeName) — so we
// pass entity=node, anim=clip, bone=node to ResampleCurveCommand and it
// reads the SAME tangents the editor drew.
bool NodeAnimationManager::resampleNodeCurveSegment(const QString& clipName,
                                                    const QString& nodeName,
                                                    const QString& channel,
                                                    double t0, double t1,
                                                    double toleranceMul,
                                                    int fixedFps)
{
    assertMainThread();
    if (clipName.isEmpty() || nodeName.isEmpty() || !isKnownChannel(channel))
        return false;
    Ogre::Animation* anim = animForClip(clipName);
    if (!anim) return false;
    if (t1 <= t0) return false;

    // Resolve the track by associated-node name (the m_trackHandles
    // cache can be stale after undo — same walk the query helpers use).
    Ogre::NodeAnimationTrack* track = nullptr;
    const auto& tracks = anim->_getNodeTrackList();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        Ogre::NodeAnimationTrack* t = it->second;
        if (t && t->getAssociatedNode()
            && QString::fromStdString(t->getAssociatedNode()->getName()) == nodeName) {
            track = t;
            break;
        }
    }
    if (!track) return false;

    // Pre-check: both endpoints must sit near existing keyframes so the
    // command's interior-overwrite math is well defined (matches the
    // bone path's guard).
    bool foundT0 = false, foundT1 = false;
    constexpr float kEps = 0.001f;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        const float kt = track->getKeyFrame(i)->getTime();
        if (std::fabs(kt - static_cast<float>(t0)) <= kEps) foundT0 = true;
        if (std::fabs(kt - static_cast<float>(t1)) <= kEps) foundT1 = true;
    }
    if (!foundT0 || !foundT1) return false;

    UndoManager::getSingleton()->push(new ResampleCurveCommand( // NOSONAR — stack owns
        nodeName.toStdString(),          // entity key == node (curve tangent side-table)
        clipName.toStdString(),          // anim key == clip
        nodeName.toStdString(),          // bone key == node
        channel.toLower().toStdString(),
        static_cast<float>(t0),
        static_cast<float>(t1),
        toleranceMul,
        fixedFps,
        /*isNodeClip=*/true));
    SentryReporter::addBreadcrumb("scene.anim.node.curve",
        QStringLiteral("resample '%1':'%2'.%3 [%4,%5]")
            .arg(clipName, nodeName, channel.toLower())
            .arg(t0, 0, 'f', 3).arg(t1, 0, 'f', 3));
    emit keyframesChanged(clipName);
    return true;
}

int NodeAnimationManager::resampleAllNodeSegments(const QString& clipName,
                                                  const QString& nodeName,
                                                  const QString& channel,
                                                  int density)
{
    assertMainThread();
    if (clipName.isEmpty() || nodeName.isEmpty() || !isKnownChannel(channel))
        return 0;
    Ogre::Animation* anim = animForClip(clipName);
    if (!anim) return 0;

    Ogre::NodeAnimationTrack* track = nullptr;
    const auto& tracks = anim->_getNodeTrackList();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        Ogre::NodeAnimationTrack* t = it->second;
        if (t && t->getAssociatedNode()
            && QString::fromStdString(t->getAssociatedNode()->getName()) == nodeName) {
            track = t;
            break;
        }
    }
    if (!track || track->getNumKeyFrames() < 2) return 0;

    // Density level → (toleranceMul, baselineFps, fixedFps) — mirrors
    // AnimationControlController::resampleAllSegmentsForBone exactly so
    // node Bake behaves identically to bone Bake.
    double toleranceMul = 1.0;
    int    fixedFps     = 0;   // exact-rate modes
    int    baselineFps  = 0;   // adaptive pre-decimate target
    switch (density) {
        case 6:  fixedFps = 60; break;          // 60 FPS exact
        case 5:  fixedFps = 30; break;          // 30 FPS exact
        case 4:  fixedFps = 15; break;          // 15 FPS exact
        case 3:  fixedFps = 10; break;          // 10 FPS exact
        case 2:  toleranceMul = 1.0;  baselineFps = 30; break;  // Dense
        case 1:  toleranceMul = 4.0;  baselineFps = 15; break;  // Medium
        default: toleranceMul = 12.0; baselineFps = 5;  break;  // Sparse
    }

    std::vector<double> anchors;
    anchors.reserve(track->getNumKeyFrames());
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i)
        anchors.push_back(track->getKeyFrame(i)->getTime());

    auto* stack = UndoManager::getSingleton()->stack();
    stack->beginMacro(QObject::tr("Resample node curve"));

    int count = 0;
    if (fixedFps > 0 && anchors.size() >= 2) {
        // Fixed-FPS bake: treat the whole clip as one segment so the
        // per-pair "1/N source duration → 0 samples" issue disappears.
        // ResampleCurveCommand snapshots the full series before the
        // strip, so the curve evaluator still sees every original pose.
        if (resampleNodeCurveSegment(clipName, nodeName, channel,
                                     anchors.front(), anchors.back(),
                                     1.0, fixedFps)) {
            ++count;
        }
    } else {
        // Adaptive modes pre-decimate to a coarser baseline so repeated
        // bakes converge to a stable keyframe count (same rationale as
        // the bone path — without it an already-dense track no-ops).
        if (baselineFps > 0) {
            UndoManager::getSingleton()->push(new DecimateTrackCommand( // NOSONAR — stack owns
                nodeName.toStdString(), clipName.toStdString(),
                nodeName.toStdString(), baselineFps, /*isNodeClip=*/true));
            anchors.clear();
            anchors.reserve(track->getNumKeyFrames());
            for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i)
                anchors.push_back(track->getKeyFrame(i)->getTime());
        }
        for (size_t i = 1; i < anchors.size(); ++i) {
            if (resampleNodeCurveSegment(clipName, nodeName, channel,
                                         anchors[i-1], anchors[i],
                                         toleranceMul, fixedFps)) {
                ++count;
            }
        }
    }

    stack->endMacro();
    emit keyframesChanged(clipName);
    return count;
}

bool NodeAnimationManager::isClipEnabled(const QString& name) const
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return false;
    auto* scene = mgr->getSceneMgr();
    if (!scene) return false;
    const std::string sn = name.toStdString();
    if (!scene->hasAnimationState(sn)) return false;
    auto* state = scene->getAnimationState(sn);
    return state && state->getEnabled();
}

void NodeAnimationManager::scrubClip(const QString& /*clipName*/, double /*time*/)
{
    // Intentionally a no-op on the node.
    //
    // Node clips are authored by moving the SceneNode with the gizmo,
    // so the node must stay under the USER's control while paused. If
    // we enabled the AnimationState, SceneManager::_applySceneAnimations()
    // would reset + re-drive the node every render frame and the gizmo
    // could no longer move it (bug: "after I keyed the node it did not
    // allow me to move anymore"). Applying a one-shot pose via
    // Animation::apply() is also unsafe here: bare apply() does not reset
    // the node first, so repeated scrubs accumulate transforms.
    //
    // The clean authoring model (matches Blender/Maya): while PAUSED the
    // node is fully editable and shows whatever transform you set; press
    // Play to preview the animated motion. Scrubbing only moves the
    // timeline playhead (so "Key" lands at the right time) — it does not
    // pose the node. Kept as a stable API point in case a future slice
    // adds a proper reset-then-apply preview.
    assertMainThread();
}

bool NodeAnimationManager::createClipUndoable(const QString& name, double length)
{
    assertMainThread();
    if (name.isEmpty() || length <= 0.0) return false;
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || !mgr->getSceneMgr()) return false;
    // Reject an existing name up front so we never push a no-op.
    if (mgr->getSceneMgr()->hasAnimation(name.toStdString())) return false;
    UndoManager::getSingleton()->push(new CreateNodeAnimClipCommand(name, length));
    return true;
}

bool NodeAnimationManager::deleteClipUndoable(const QString& name)
{
    assertMainThread();
    if (name.isEmpty()) return false;
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || !mgr->getSceneMgr()) return false;
    if (!mgr->getSceneMgr()->hasAnimation(name.toStdString())) return false;
    UndoManager::getSingleton()->push(new DeleteNodeAnimClipCommand(name));
    return true;
}

bool NodeAnimationManager::keyNodeCurrentTransform(const QString& clipName,
                                                   const QString& nodeName,
                                                   double time)
{
    assertMainThread();
    if (clipName.isEmpty() || nodeName.isEmpty() || time < 0.0) return false;
    Ogre::Animation* anim = animForClip(clipName);
    if (!anim) return false;
    if (time > anim->getLength()) return false;
    Ogre::SceneNode* node = findSceneNode(nodeName);
    if (!node) return false;
    // Capture the node's CURRENT local transform. NodeAnimationTrack
    // keyframes store the node-space TRS Ogre resets to and interpolates
    // between, so we snapshot what the user set with the gizmo.
    UndoManager::getSingleton()->push(new SetNodeKeyframeCommand(
        clipName, nodeName, time,
        node->getPosition(), node->getOrientation(), node->getScale()));
    return true;
}

bool NodeAnimationManager::moveNodeKeyframe(const QString& clipName,
                                            const QString& nodeName,
                                            double oldTime,
                                            double newTime)
{
    assertMainThread();
    if (clipName.isEmpty() || nodeName.isEmpty()) return false;
    if (newTime < 0.0) return false;
    Ogre::Animation* anim = animForClip(clipName);
    if (!anim) return false;
    if (newTime > anim->getLength()) return false;
    if (std::abs(oldTime - newTime) < kKeyframeMergeEpsilon) return false;

    // Validate there IS a key at oldTime and NONE already at newTime,
    // so we never push a command that would no-op or clobber. Read
    // times straight off the Ogre track (not the m_trackHandles cache,
    // which an undo-rebuilt clip can leave stale for a node).
    Ogre::NodeAnimationTrack* track = nullptr;
    const auto& tracks = anim->_getNodeTrackList();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        auto* t = it->second;
        if (t && t->getAssociatedNode() &&
            QString::fromStdString(t->getAssociatedNode()->getName()) == nodeName) {
            track = t;
            break;
        }
    }
    if (!track) return false;
    bool haveOld = false, clashNew = false;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        const double t = static_cast<double>(track->getKeyFrame(i)->getTime());
        if (std::abs(t - oldTime) < kKeyframeMergeEpsilon) haveOld = true;
        if (std::abs(t - newTime) < kKeyframeMergeEpsilon) clashNew = true;
    }
    if (!haveOld || clashNew) return false;

    UndoManager::getSingleton()->push(
        new MoveNodeKeyframeCommand(clipName, nodeName, oldTime, newTime));
    return true;
}

bool NodeAnimationManager::deleteNodeKeyframe(const QString& clipName,
                                              const QString& nodeName,
                                              double time)
{
    assertMainThread();
    if (clipName.isEmpty() || nodeName.isEmpty()) return false;
    auto* cmd = new DeleteNodeKeyframeCommand(clipName, nodeName, time);
    if (!cmd->valid()) { delete cmd; return false; }
    UndoManager::getSingleton()->push(cmd);
    return true;
}

bool NodeAnimationManager::createClipForName(const QString& name, double length)
{
    return createClip(name, length);
}

bool NodeAnimationManager::deleteClipForName(const QString& name)
{
    return deleteClip(name);
}

bool NodeAnimationManager::addKeyframeFromQml(const QString& clipName,
                                              const QString& nodeName,
                                              double time,
                                              double tx, double ty, double tz,
                                              double qw, double qx, double qy, double qz,
                                              double sx, double sy, double sz)
{
    return addKeyframe(clipName, nodeName, time,
                       Ogre::Vector3(static_cast<Ogre::Real>(tx),
                                     static_cast<Ogre::Real>(ty),
                                     static_cast<Ogre::Real>(tz)),
                       Ogre::Quaternion(static_cast<Ogre::Real>(qw),
                                        static_cast<Ogre::Real>(qx),
                                        static_cast<Ogre::Real>(qy),
                                        static_cast<Ogre::Real>(qz)),
                       Ogre::Vector3(static_cast<Ogre::Real>(sx),
                                     static_cast<Ogre::Real>(sy),
                                     static_cast<Ogre::Real>(sz)));
}
