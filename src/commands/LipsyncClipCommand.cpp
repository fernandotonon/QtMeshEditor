#include "LipsyncClipCommand.h"

#include "../Manager.h"

#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreEntity.h>
#include <OgreKeyFrame.h>
#include <OgreMesh.h>

#include <map>

namespace {

Ogre::Entity* findEntity(const std::string& name)
{
    auto* mgr = Manager::getSingleton();
    if (!mgr) return nullptr;
    for (Ogre::Entity* e : mgr->getEntities())
        if (e && e->getName() == name) return e;
    return nullptr;
}

}  // namespace

/// The clip as it stood before the take. `existed` false means there was no
/// such animation, and undo() simply removes ours.
struct LipsyncClipCommand::Snapshot {
    bool existed = false;
    float length = 0.0f;
    std::map<unsigned short, std::vector<Key>> tracks;   // vertex handle -> keys
};

LipsyncClipCommand::LipsyncClipCommand(std::string entityName,
                                       QString clipName,
                                       std::vector<Key> keys,
                                       QUndoCommand* parent)
    : QUndoCommand(parent),
      m_entityName(std::move(entityName)),
      m_clipName(std::move(clipName)),
      m_keys(std::move(keys))
{
    setText(QObject::tr("Lipsync '%1'").arg(m_clipName));
}

LipsyncClipCommand::~LipsyncClipCommand() = default;

void LipsyncClipCommand::redo()
{
    Ogre::Entity* entity = findEntity(m_entityName);
    if (!entity) return;
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return;

    const std::string clip = m_clipName.toStdString();

    if (!m_snapshotTaken) {
        m_before = std::make_unique<Snapshot>();
        if (mesh->hasAnimation(clip)) {
            m_before->existed = true;
            Ogre::Animation* anim = mesh->getAnimation(clip);
            m_before->length = anim->getLength();
            for (const auto& [handle, track] : anim->_getVertexTrackList()) {
                auto& keys = m_before->tracks[handle];
                for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
                    auto* kf = static_cast<Ogre::VertexPoseKeyFrame*>(
                        track->getKeyFrame(i));
                    Key k;
                    k.time = kf->getTime();
                    for (const auto& ref : kf->getPoseReferences())
                        k.poseRefs.emplace_back(ref.poseIndex, ref.influence);
                    keys.push_back(std::move(k));
                }
            }
        }
        m_snapshotTaken = true;
    }

    // Replace rather than merge: reusing an existing clip leaves stale keys at
    // times this take does not cover, and the length only ever grows.
    if (mesh->hasAnimation(clip)) mesh->removeAnimation(clip);

    float length = 0.0f;
    for (const Key& k : m_keys) length = std::max(length, k.time);
    Ogre::Animation* anim = mesh->createAnimation(clip, length);

    // Every key writes ALL its pose refs into one keyframe per track: ARKit
    // targets on a submesh share a VAT_POSE track, and Ogre interpolates a
    // pose missing from the next keyframe toward zero.
    m_keysWritten = 0;
    const auto& poses = mesh->getPoseList();
    for (const Key& k : m_keys) {
        std::map<unsigned short, std::vector<std::pair<unsigned short, float>>> byTrack;
        for (const auto& [poseIndex, influence] : k.poseRefs) {
            if (poseIndex >= poses.size()) continue;
            byTrack[poses[poseIndex]->getTarget()].emplace_back(poseIndex, influence);
        }
        for (const auto& [handle, refs] : byTrack) {
            Ogre::VertexAnimationTrack* track =
                anim->hasVertexTrack(handle)
                    ? anim->getVertexTrack(handle)
                    : anim->createVertexTrack(handle, Ogre::VAT_POSE);
            auto* kf = track->createVertexPoseKeyFrame(k.time);
            for (const auto& [poseIndex, influence] : refs)
                kf->addPoseReference(poseIndex, influence);
            ++m_keysWritten;
        }
    }
    entity->refreshAvailableAnimationState();
}

void LipsyncClipCommand::undo()
{
    Ogre::Entity* entity = findEntity(m_entityName);
    if (!entity || !m_before) return;
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return;

    const std::string clip = m_clipName.toStdString();
    if (mesh->hasAnimation(clip)) mesh->removeAnimation(clip);

    if (m_before->existed) {
        Ogre::Animation* anim = mesh->createAnimation(clip, m_before->length);
        for (const auto& [handle, keys] : m_before->tracks) {
            auto* track = anim->createVertexTrack(handle, Ogre::VAT_POSE);
            for (const Key& k : keys) {
                auto* kf = track->createVertexPoseKeyFrame(k.time);
                for (const auto& [poseIndex, influence] : k.poseRefs)
                    kf->addPoseReference(poseIndex, influence);
            }
        }
    }
    entity->refreshAvailableAnimationState();
}
