#ifdef ENABLE_MOCAP

#include "RecordMocapClipCommand.h"

#include "../Manager.h"
#include "../NodeAnimationManager.h"

#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreKeyFrame.h>
#include <OgreMesh.h>
#include <OgreSceneManager.h>
#include <OgreSkeletonInstance.h>

#include <QObject>

#include <map>
#include <utility>

namespace {

Ogre::Entity* findEntity(const std::string& name)
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || !mgr->getSceneMgr())
        return nullptr;
    auto* scene = mgr->getSceneMgr();
    return scene->hasEntity(name) ? scene->getEntity(name) : nullptr;
}

// serialized VAT_POSE clip: per track handle, keyframes with pose references
struct VertexClipSnapshot {
    float length = 0.f;
    struct Key {
        float time;
        std::vector<std::pair<unsigned short, float>> poseRefs;
    };
    std::map<unsigned short, std::vector<Key>> tracks;
};

// serialized bone rotation clip: per bone handle, (time, rotation) keys
struct BoneClipSnapshot {
    float length = 0.f;
    struct Key {
        float time;
        Ogre::Quaternion rotation;
        Ogre::Vector3 translate;
        Ogre::Vector3 scale;
    };
    std::map<unsigned short, std::vector<Key>> tracks;
};

}  // namespace

struct RecordMocapClipCommand::Snapshots {
    bool hadWeightClip = false;
    VertexClipSnapshot weightClip;
    bool hadHeadBoneClip = false;
    BoneClipSnapshot headBoneClip;
};

RecordMocapClipCommand::RecordMocapClipCommand(
    std::string entityName, std::vector<FaceSample> samples,
    FaceCapMapper::Mapping mapping, MocapRecorder::FaceRecordOptions options,
    QUndoCommand* parent)
    : QUndoCommand(parent),
      m_entityName(std::move(entityName)),
      m_samples(std::move(samples)),
      m_mapping(std::move(mapping)),
      m_options(options)
{
    setText(QObject::tr("Record performance capture '%1'")
                .arg(options.clipName));
}

RecordMocapClipCommand::~RecordMocapClipCommand() = default;

void RecordMocapClipCommand::redo()
{
    Ogre::Entity* entity = findEntity(m_entityName);
    if (!entity)
        return;

    if (!m_snapshotTaken) {
        m_before = std::make_unique<Snapshots>();
        Ogre::MeshPtr mesh = entity->getMesh();
        const std::string clip = m_options.clipName.toStdString();
        if (mesh && mesh->hasAnimation(clip)) {
            m_before->hadWeightClip = true;
            Ogre::Animation* anim = mesh->getAnimation(clip);
            m_before->weightClip.length = anim->getLength();
            for (const auto& [handle, track] : anim->_getVertexTrackList()) {
                auto& keys = m_before->weightClip.tracks[handle];
                for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
                    auto* kf = static_cast<Ogre::VertexPoseKeyFrame*>(
                        track->getKeyFrame(i));
                    VertexClipSnapshot::Key key;
                    key.time = kf->getTime();
                    for (const auto& ref : kf->getPoseReferences())
                        key.poseRefs.push_back({ref.poseIndex, ref.influence});
                    keys.push_back(std::move(key));
                }
            }
        }
        const std::string headClip =
            (m_options.clipName + QStringLiteral("_Head")).toStdString();
        if (entity->hasSkeleton()
            && entity->getMesh()->getSkeleton()->hasAnimation(headClip)) {
            m_before->hadHeadBoneClip = true;
            Ogre::Animation* anim =
                entity->getMesh()->getSkeleton()->getAnimation(headClip);
            m_before->headBoneClip.length = anim->getLength();
            for (const auto& [handle, track] : anim->_getNodeTrackList()) {
                auto& keys = m_before->headBoneClip.tracks[handle];
                for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
                    auto* kf = static_cast<Ogre::TransformKeyFrame*>(
                        track->getKeyFrame(i));
                    keys.push_back({kf->getTime(), kf->getRotation(),
                                    kf->getTranslate(), kf->getScale()});
                }
            }
        }
        m_snapshotTaken = true;
    }

    m_report = MocapRecorder::recordFace(entity, m_samples, m_mapping, m_options);
}

void RecordMocapClipCommand::undo()
{
    Ogre::Entity* entity = findEntity(m_entityName);
    if (!entity || !m_before)
        return;

    Ogre::MeshPtr mesh = entity->getMesh();
    const std::string clip = m_options.clipName.toStdString();
    if (mesh && mesh->hasAnimation(clip))
        mesh->removeAnimation(clip);
    if (mesh && m_before->hadWeightClip) {
        Ogre::Animation* anim =
            mesh->createAnimation(clip, m_before->weightClip.length);
        for (const auto& [handle, keys] : m_before->weightClip.tracks) {
            auto* track = anim->createVertexTrack(handle, Ogre::VAT_POSE);
            for (const auto& key : keys) {
                auto* kf = track->createVertexPoseKeyFrame(key.time);
                for (const auto& [poseIndex, influence] : key.poseRefs)
                    kf->addPoseReference(poseIndex, influence);
            }
        }
    }

    const std::string headClip =
        (m_options.clipName + QStringLiteral("_Head")).toStdString();
    if (entity->hasSkeleton()) {
        Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
        if (skel->hasAnimation(headClip))
            skel->removeAnimation(headClip);
        if (m_before->hadHeadBoneClip) {
            Ogre::Animation* anim =
                skel->createAnimation(headClip, m_before->headBoneClip.length);
            for (const auto& [handle, keys] : m_before->headBoneClip.tracks) {
                auto* track =
                    anim->createNodeTrack(handle, skel->getBone(handle));
                for (const auto& key : keys) {
                    auto* kf = track->createNodeKeyFrame(key.time);
                    kf->setRotation(key.rotation);
                    kf->setTranslate(key.translate);
                    kf->setScale(key.scale);
                }
            }
        }
    }
    // node-TRS head clip (static meshes): scene-level, owned by
    // NodeAnimationManager — a recorded node clip is simply deleted. This is
    // safe because recordFace REJECTS a name collision with a pre-existing
    // node clip (report.headError set, headTarget != "node") rather than
    // overwriting it, so a "node" headTarget always means we created it fresh
    // this record — there is nothing of the user's to restore.
    if (m_report.headTarget == QLatin1String("node")) {
        if (auto* nam = NodeAnimationManager::instance())
            nam->deleteClip(m_options.clipName + QStringLiteral("_Head"));
    }

    entity->refreshAvailableAnimationState();
}

// ---------------------------------------------------------------------------
// RecordBodyClipCommand (Slice E #874)
// ---------------------------------------------------------------------------

struct RecordBodyClipCommand::Snapshot {
    bool hadClip = false;
    BoneClipSnapshot clip;
};

RecordBodyClipCommand::RecordBodyClipCommand(
    std::string entityName,
    std::vector<std::vector<std::array<float, 4>>> clipQuats, int fps,
    MocapRecorder::BodyRecordOptions options, QUndoCommand* parent)
    : QUndoCommand(parent),
      m_entityName(std::move(entityName)),
      m_clipQuats(std::move(clipQuats)),
      m_fps(fps),
      m_options(std::move(options))
{
    setText(QObject::tr("Record body capture '%1'").arg(m_options.clipName));
}

RecordBodyClipCommand::~RecordBodyClipCommand() = default;

void RecordBodyClipCommand::redo()
{
    Ogre::Entity* entity = findEntity(m_entityName);
    if (!entity)
        return;

    if (!m_snapshotTaken) {
        m_before = std::make_unique<Snapshot>();
        const std::string clip = m_options.clipName.toStdString();
        if (entity->hasSkeleton()
            && entity->getMesh()->getSkeleton()->hasAnimation(clip)) {
            m_before->hadClip = true;
            Ogre::Animation* anim =
                entity->getMesh()->getSkeleton()->getAnimation(clip);
            m_before->clip.length = anim->getLength();
            for (const auto& [handle, track] : anim->_getNodeTrackList()) {
                auto& keys = m_before->clip.tracks[handle];
                for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
                    auto* kf = static_cast<Ogre::TransformKeyFrame*>(
                        track->getKeyFrame(i));
                    keys.push_back({kf->getTime(), kf->getRotation(),
                                    kf->getTranslate(), kf->getScale()});
                }
            }
        }
        m_snapshotTaken = true;
    }

    m_report = MocapRecorder::recordBody(entity, m_clipQuats, m_fps, m_options);
}

void RecordBodyClipCommand::undo()
{
    Ogre::Entity* entity = findEntity(m_entityName);
    if (!entity || !m_before || !entity->hasSkeleton())
        return;

    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
    const std::string clip = m_options.clipName.toStdString();
    if (skel->hasAnimation(clip))
        skel->removeAnimation(clip);
    if (m_before->hadClip) {
        Ogre::Animation* anim =
            skel->createAnimation(clip, m_before->clip.length);
        for (const auto& [handle, keys] : m_before->clip.tracks) {
            auto* track = anim->createNodeTrack(handle, skel->getBone(handle));
            for (const auto& key : keys) {
                auto* kf = track->createNodeKeyFrame(key.time);
                kf->setRotation(key.rotation);
                kf->setTranslate(key.translate);
                kf->setScale(key.scale);
            }
        }
    }
    entity->refreshAvailableAnimationState();
}

#endif  // ENABLE_MOCAP
