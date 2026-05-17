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

#include <QCoreApplication>
#include <QHash>
#include <QThread>

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
}

NodeAnimationManager::~NodeAnimationManager() = default;

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
