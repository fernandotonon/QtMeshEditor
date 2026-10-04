#include "RetargetAnimationCommand.h"

#include "SkeletonResolver.h"
#include "../Manager.h"
#include "../SentryReporter.h"

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreAnimationTrack.h>
#include <OgreEntity.h>
#include <OgreKeyFrame.h>
#include <OgreSkeletonInstance.h>

namespace {

Ogre::Entity* entityByName(const std::string& name)
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    for (Ogre::MovableObject* obj : mgr->getEntities())
        if (obj && obj->getMovableType() == "Entity" && obj->getName() == name)
            return static_cast<Ogre::Entity*>(obj);
    return nullptr;
}

}  // namespace

RetargetAnimationCommand::RetargetAnimationCommand(std::string entityName,
                                                   std::string animName,
                                                   bool alreadyApplied,
                                                   QUndoCommand* parent)
    : QUndoCommand(parent)
    , m_entityName(std::move(entityName))
    , m_animName(std::move(animName))
    , m_skipFirstRedo(alreadyApplied)
{
    setText(QStringLiteral("Retarget animation '%1'").arg(QString::fromStdString(m_animName)));
    Ogre::SkeletonInstance* skel = SkeletonResolver::resolve(m_entityName);
    if (!skel || !skel->hasAnimation(m_animName)) return;
    Ogre::Animation* anim = skel->getAnimation(m_animName);
    m_length = anim->getLength();
    for (const auto& kv : anim->_getNodeTrackList()) {
        Ogre::NodeAnimationTrack* tr = kv.second;
        Track t;
        t.handle = kv.first;
        for (unsigned short i = 0; i < tr->getNumKeyFrames(); ++i) {
            const Ogre::TransformKeyFrame* kf = tr->getNodeKeyFrame(i);
            t.keys.push_back({kf->getTime(), kf->getRotation(), kf->getTranslate(), kf->getScale()});
        }
        m_tracks.push_back(std::move(t));
    }
    m_valid = true;
}

void RetargetAnimationCommand::undo()
{
    Ogre::Entity* entity = entityByName(m_entityName);
    Ogre::SkeletonInstance* skel = SkeletonResolver::resolve(m_entityName);
    if (!entity || !skel || !skel->hasAnimation(m_animName)) return;
    if (auto* states = entity->getAllAnimationStates())
        if (states->hasAnimationState(m_animName))
            states->getAnimationState(m_animName)->setEnabled(false);
    skel->removeAnimation(m_animName);
    // refreshAvailableAnimationState only ADDS states, never drops stale
    // ones — without this the undone clip lingers as a ghost state (shown
    // in the Inspector, throwing when resolved back to the skeleton).
    if (auto* states = entity->getAllAnimationStates())
        if (states->hasAnimationState(m_animName)) states->removeAnimationState(m_animName);
    entity->refreshAvailableAnimationState();
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.retarget.cmd"),
        QStringLiteral("undo '%1'").arg(QString::fromStdString(m_animName)));
}

void RetargetAnimationCommand::redo()
{
    if (m_skipFirstRedo) { m_skipFirstRedo = false; return; }
    if (!m_valid) return;
    Ogre::Entity* entity = entityByName(m_entityName);
    Ogre::SkeletonInstance* skel = SkeletonResolver::resolve(m_entityName);
    if (!entity || !skel || skel->hasAnimation(m_animName)) return;
    Ogre::Animation* anim = skel->createAnimation(m_animName, m_length);
    anim->setInterpolationMode(Ogre::Animation::IM_LINEAR);
    for (const Track& t : m_tracks) {
        if (t.handle >= skel->getNumBones()) continue;
        Ogre::NodeAnimationTrack* tr = anim->createNodeTrack(t.handle, skel->getBone(t.handle));
        for (const Key& k : t.keys) {
            Ogre::TransformKeyFrame* kf = tr->createNodeKeyFrame(k.time);
            kf->setRotation(k.rot);
            kf->setTranslate(k.pos);
            kf->setScale(k.scale);
        }
    }
    entity->refreshAvailableAnimationState();
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.retarget.cmd"),
        QStringLiteral("redo '%1'").arg(QString::fromStdString(m_animName)));
}
