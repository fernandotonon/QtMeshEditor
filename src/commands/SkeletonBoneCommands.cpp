#include "SkeletonBoneCommands.h"

#include "Manager.h"
#include "PropertiesPanelController.h"
#include "SelectionSet.h"
#include "SkeletonEditor.h"
#include "SentryReporter.h"
#include "UndoManager.h"

#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSkeleton.h>

namespace {

Ogre::Entity* resolveEntityByName(const std::string& name)
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    for (Ogre::Entity* ent : mgr->getEntities()) {
        if (!ent || ent->getMovableType() != "Entity") continue;
        if (ent->getName() == name) return ent;
    }
    return nullptr;
}

} // namespace

CreateBoneCommand::CreateBoneCommand(std::string entityName,
                                     SkeletonEditor::CreateOptions opts,
                                     QUndoCommand* parent)
    : QUndoCommand(parent)
    , m_entityName(std::move(entityName))
    , m_opts(std::move(opts))
{
    setText(QStringLiteral("Create bone"));
}

void CreateBoneCommand::redo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;

    SkeletonEditor::CreateOptions opts = m_opts;
    if (!m_firstRedo)
        opts.forcedName = m_createdBoneName;

    const auto result = SkeletonEditor::createBone(entity, opts);
    if (!result.ok) return;

    if (m_firstRedo) {
        m_createdBoneName = result.boneName;
        m_firstRedo = false;
        SentryReporter::addBreadcrumb(QStringLiteral("scene.skel.bone.create"),
            QStringLiteral("%1 → %2").arg(QString::fromStdString(m_entityName), m_createdBoneName));
        if (auto* editor = SkeletonEditor::getSingletonPtr())
            emit editor->boneCreated(QString::fromStdString(m_entityName), m_createdBoneName);
    }

    m_applied = true;
    SkeletonEditor::refreshAfterEdit(m_entityName, m_createdBoneName);
}

void CreateBoneCommand::undo()
{
    if (m_createdBoneName.isEmpty()) return;
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;

    SkeletonEditor::RemoveOptions opts;
    opts.removeChildren = false;
    opts.transferWeightsToParent = false;
    SkeletonEditor::removeBone(entity, m_createdBoneName, opts);
    SkeletonEditor::refreshAfterEdit(m_entityName, m_opts.parentBoneName);
    m_applied = false;
}

RemoveBoneCommand::RemoveBoneCommand(std::string entityName,
                                     QString boneName,
                                     SkeletonEditor::RemoveOptions opts,
                                     QUndoCommand* parent)
    : QUndoCommand(parent)
    , m_entityName(std::move(entityName))
    , m_boneName(std::move(boneName))
    , m_opts(std::move(opts))
{
    setText(QStringLiteral("Remove bone"));
}

void RemoveBoneCommand::redo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;

    if (m_firstRedo) {
        m_before = SkeletonEditor::captureSnapshot(entity);
        m_firstRedo = false;
    }

    const auto result = SkeletonEditor::removeBone(entity, m_boneName, m_opts);
    if (!result.ok) return;

    m_applied = true;
    SentryReporter::addBreadcrumb(QStringLiteral("scene.skel.bone.remove"),
        QStringLiteral("%1 → %2").arg(QString::fromStdString(m_entityName), m_boneName));
    if (auto* editor = SkeletonEditor::getSingletonPtr())
        emit editor->boneRemoved(QString::fromStdString(m_entityName), m_boneName);
    SkeletonEditor::refreshAfterEdit(m_entityName);
}

void RemoveBoneCommand::undo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;
    QString err;
    if (SkeletonEditor::restoreSnapshot(entity, m_before, &err))
        SkeletonEditor::refreshAfterEdit(m_entityName, m_boneName);
    m_applied = false;
}

RenameBoneCommand::RenameBoneCommand(std::string entityName,
                                     QString oldName,
                                     QString newName,
                                     QUndoCommand* parent)
    : QUndoCommand(parent)
    , m_entityName(std::move(entityName))
    , m_oldName(std::move(oldName))
    , m_newName(std::move(newName))
{
    setText(QStringLiteral("Rename bone"));
}

void RenameBoneCommand::redo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;
    const auto result = SkeletonEditor::renameBone(entity, m_oldName, m_newName);
    if (!result.ok) return;
    m_applied = true;
    SentryReporter::addBreadcrumb(QStringLiteral("scene.skel.bone.rename"),
        QStringLiteral("%1: %2 → %3").arg(QString::fromStdString(m_entityName), m_oldName, m_newName));
    if (auto* editor = SkeletonEditor::getSingletonPtr())
        emit editor->boneRenamed(QString::fromStdString(m_entityName), m_oldName, m_newName);
    SkeletonEditor::refreshAfterEdit(m_entityName, m_newName);
}

void RenameBoneCommand::undo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;
    SkeletonEditor::renameBone(entity, m_newName, m_oldName);
    SkeletonEditor::refreshAfterEdit(m_entityName, m_oldName);
    m_applied = false;
}

DuplicateBoneCommand::DuplicateBoneCommand(std::string entityName,
                                           QString sourceBoneName,
                                           QUndoCommand* parent)
    : QUndoCommand(parent)
    , m_entityName(std::move(entityName))
    , m_sourceBoneName(std::move(sourceBoneName))
{
    setText(QStringLiteral("Duplicate bone"));
}

void DuplicateBoneCommand::redo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;

    SkeletonEditor::Result result;
    if (m_firstRedo) {
        result = SkeletonEditor::duplicateBone(entity, m_sourceBoneName);
        if (!result.ok) return;
        m_duplicatedBoneName = result.boneName;
        m_firstRedo = false;
        SentryReporter::addBreadcrumb(QStringLiteral("scene.skel.bone.duplicate"),
            QStringLiteral("%1: %2 → %3")
                .arg(QString::fromStdString(m_entityName), m_sourceBoneName, m_duplicatedBoneName));
        if (auto* editor = SkeletonEditor::getSingletonPtr())
            emit editor->boneDuplicated(QString::fromStdString(m_entityName), m_duplicatedBoneName);
    } else {
        SkeletonEditor::CreateOptions opts;
        opts.forcedName = m_duplicatedBoneName;
        opts.baseName = m_duplicatedBoneName;
        if (Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton()) {
            if (skel->hasBone(m_sourceBoneName.toStdString())) {
                Ogre::Bone* src = skel->getBone(m_sourceBoneName.toStdString());
                opts.parentBoneName = src->getParent()
                    ? QString::fromStdString(src->getParent()->getName())
                    : QString();
                result = SkeletonEditor::createBone(entity, opts);
                if (result.ok) {
                    Ogre::Bone* dup = skel->getBone(m_duplicatedBoneName.toStdString());
                    dup->setPosition(src->getPosition());
                    dup->setOrientation(src->getOrientation());
                    dup->setScale(src->getScale());
                    dup->setInitialState();
                    entity->_initialise(true);
                }
            }
        }
        if (!result.ok) return;
    }

    m_applied = true;
    SkeletonEditor::refreshAfterEdit(m_entityName, m_duplicatedBoneName);
}

void DuplicateBoneCommand::undo()
{
    if (m_duplicatedBoneName.isEmpty()) return;
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;
    SkeletonEditor::RemoveOptions opts;
    SkeletonEditor::removeBone(entity, m_duplicatedBoneName, opts);
    SkeletonEditor::refreshAfterEdit(m_entityName, m_sourceBoneName);
    m_applied = false;
}

ToggleSkeletonDebugCommand::ToggleSkeletonDebugCommand(QString entityName,
                                                       bool show,
                                                       QUndoCommand* parent)
    : QUndoCommand(parent)
    , m_entityName(std::move(entityName))
    , m_show(show)
{
    setText(show ? QStringLiteral("Show skeleton") : QStringLiteral("Hide skeleton"));
}

void ToggleSkeletonDebugCommand::apply(bool show)
{
    auto* ppc = PropertiesPanelController::instance();
    if (!ppc)
        return;
    ppc->applySkeletonDebug(m_entityName, show);
}

void ToggleSkeletonDebugCommand::redo()
{
    apply(m_show);
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Skeleton debug %1: %2")
            .arg(m_show ? QStringLiteral("show") : QStringLiteral("hide"), m_entityName));
}

void ToggleSkeletonDebugCommand::undo()
{
    apply(!m_show);
}
