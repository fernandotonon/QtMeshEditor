#include "SkeletonBoneCommands.h"

#include "AnimationMerger.h"
#include "AutoRigController.h"
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

RemoveSkeletonCommand::RemoveSkeletonCommand(std::string entityName,
                                             QUndoCommand* parent)
    : QUndoCommand(parent)
    , m_entityName(std::move(entityName))
{
    setText(QStringLiteral("Remove skeleton"));
}

void RemoveSkeletonCommand::redo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;

    if (m_firstRedo) {
        m_before = SkeletonEditor::captureSnapshot(entity);
        // The imported-rest cache is cleared with the skeleton; capture the
        // original entry so undo restores it VERBATIM — repopulating lazily
        // from the restored skeleton would bake any pre-removal rest EDITS
        // in as the "import".
        m_restCache = SkeletonEditor::serializeImportedRestCache(entity);
        m_firstRedo = false;
    }

    const auto result = SkeletonEditor::removeSkeleton(entity);
    if (!result.ok) return;

    m_applied = true;
    SentryReporter::addBreadcrumb(QStringLiteral("scene.skel.remove_skeleton"),
        QString::fromStdString(m_entityName));
    SkeletonEditor::refreshAfterEdit(m_entityName);
    // The mesh just flipped to "riggable" — refresh the Inspector's
    // Auto-Rig / Skinning section gating.
    if (auto* rig = AutoRigController::instance())
        emit rig->selectionChanged();
}

void RemoveSkeletonCommand::undo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;
    QString err;
    if (SkeletonEditor::restoreSnapshot(entity, m_before, &err)) {
        SkeletonEditor::deserializeImportedRestCache(entity, m_restCache);
        SkeletonEditor::refreshAfterEdit(m_entityName);
    }
    m_applied = false;
    if (auto* rig = AutoRigController::instance())
        emit rig->selectionChanged();
}

TrimAnimationCommand::TrimAnimationCommand(std::string entityName,
                                           std::string animName,
                                           float t0, float t1,
                                           QUndoCommand* parent)
    : QUndoCommand(parent)
    , m_entityName(std::move(entityName))
    , m_animName(std::move(animName))
    , m_t0(t0)
    , m_t1(t1)
{
    setText(QStringLiteral("Trim animation"));
}

void TrimAnimationCommand::redo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity || !entity->getMesh() || !entity->getMesh()->hasSkeleton()) {
        m_error = QStringLiteral("Entity has no skeleton.");
        return;
    }
    // Master skeleton — the resource exports read; the entity's
    // SkeletonInstance shares its animation list.
    Ogre::Skeleton* master = entity->getMesh()->getSkeleton().get();
    if (!master || !master->hasAnimation(m_animName)) {
        m_error = QStringLiteral("Animation not found on the skeleton.");
        return;
    }

    if (m_firstRedo) {
        m_before = SkeletonEditor::captureSnapshot(entity);
        m_firstRedo = false;
    }

    const auto r = AnimationMerger::trimAnimation(master, m_animName,
                                                  m_t0, m_t1);
    if (!r.ok) {
        m_error = r.error;
        return;
    }
    m_keyframesRemoved = r.keyframesRemoved;
    m_newLength = r.newLength;
    m_applied = true;
    // The entity's AnimationState still carries the OLD length — rebuild the
    // state set so the timeline/slider sees the trimmed clip.
    entity->refreshAvailableAnimationState();
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.trim"),
        QStringLiteral("%1 [%2..%3] -%4 keys")
            .arg(QString::fromStdString(m_animName))
            .arg(m_t0).arg(m_t1).arg(m_keyframesRemoved));
    SkeletonEditor::refreshAfterEdit(m_entityName);
}

void TrimAnimationCommand::undo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;
    QString err;
    if (SkeletonEditor::restoreSnapshot(entity, m_before, &err))
        SkeletonEditor::refreshAfterEdit(m_entityName);
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

ReparentBoneCommand::ReparentBoneCommand(std::string entityName,
                                         QString boneName,
                                         QString newParentName,
                                         SkeletonEditor::ReparentOptions opts,
                                         QUndoCommand* parent)
    : QUndoCommand(parent)
    , m_entityName(std::move(entityName))
    , m_boneName(std::move(boneName))
    , m_newParentName(std::move(newParentName))
    , m_opts(opts)
{
    setText(m_newParentName.isEmpty()
                ? QStringLiteral("Detach bone")
                : QStringLiteral("Reparent bone"));
}

void ReparentBoneCommand::redo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;
    if (m_firstRedo) {
        m_before = SkeletonEditor::captureSnapshot(entity);
        m_firstRedo = false;
    }
    const auto result = SkeletonEditor::reparentBone(entity, m_boneName, m_newParentName, m_opts);
    if (!result.ok) return;
    m_applied = true;
    SentryReporter::addBreadcrumb(
        m_newParentName.isEmpty()
            ? QStringLiteral("scene.skel.hier.detach")
            : QStringLiteral("scene.skel.hier.reparent"),
        QStringLiteral("%1: %2 → %3")
            .arg(QString::fromStdString(m_entityName), m_boneName,
                 m_newParentName.isEmpty() ? QStringLiteral("(root)") : m_newParentName));
    SkeletonEditor::refreshAfterEdit(m_entityName, m_boneName);
}

void ReparentBoneCommand::undo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;
    QString err;
    if (SkeletonEditor::restoreSnapshot(entity, m_before, &err))
        SkeletonEditor::refreshAfterEdit(m_entityName, m_boneName);
    m_applied = false;
}

SplitBoneCommand::SplitBoneCommand(std::string entityName,
                                   QString boneName,
                                   float t,
                                   QUndoCommand* parent)
    : QUndoCommand(parent)
    , m_entityName(std::move(entityName))
    , m_boneName(std::move(boneName))
    , m_t(t)
{
    setText(QStringLiteral("Split bone"));
}

void SplitBoneCommand::redo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;
    if (m_firstRedo) {
        m_before = SkeletonEditor::captureSnapshot(entity);
        m_firstRedo = false;
    }
    const auto result = SkeletonEditor::splitBone(entity, m_boneName, m_t);
    if (!result.ok) return;
    m_splitBoneName = result.boneName;
    m_applied = true;
    SentryReporter::addBreadcrumb(QStringLiteral("scene.skel.hier.split"),
        QStringLiteral("%1: %2 @%3 → %4")
            .arg(QString::fromStdString(m_entityName), m_boneName)
            .arg(m_t, 0, 'f', 2)
            .arg(m_splitBoneName));
    SkeletonEditor::refreshAfterEdit(m_entityName, m_splitBoneName);
}

void SplitBoneCommand::undo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;
    QString err;
    if (SkeletonEditor::restoreSnapshot(entity, m_before, &err))
        SkeletonEditor::refreshAfterEdit(m_entityName, m_boneName);
    m_applied = false;
}

ConnectBoneCommand::ConnectBoneCommand(std::string entityName,
                                       QString boneName,
                                       bool connected,
                                       QUndoCommand* parent)
    : QUndoCommand(parent)
    , m_entityName(std::move(entityName))
    , m_boneName(std::move(boneName))
    , m_connected(connected)
{
    setText(connected ? QStringLiteral("Connect bone") : QStringLiteral("Disconnect bone"));
}

void ConnectBoneCommand::redo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;
    if (m_firstRedo) {
        m_before = SkeletonEditor::captureSnapshot(entity);
        m_firstRedo = false;
    }
    const auto result = SkeletonEditor::setBoneConnected(entity, m_boneName, m_connected);
    if (!result.ok) return;
    m_applied = true;
    SentryReporter::addBreadcrumb(QStringLiteral("scene.skel.hier.connect"),
        QStringLiteral("%1: %2 %3")
            .arg(QString::fromStdString(m_entityName), m_boneName,
                 m_connected ? QStringLiteral("connect") : QStringLiteral("disconnect")));
    SkeletonEditor::refreshAfterEdit(m_entityName, m_boneName);
}

void ConnectBoneCommand::undo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;
    QString err;
    if (SkeletonEditor::restoreSnapshot(entity, m_before, &err))
        SkeletonEditor::refreshAfterEdit(m_entityName, m_boneName);
    m_applied = false;
}

AttachBoneToEntityCommand::AttachBoneToEntityCommand(std::string srcEntityName,
                                                     QStringList boneNames,
                                                     std::string dstEntityName,
                                                     SkeletonEditor::AttachOptions opts,
                                                     QUndoCommand* parent)
    : QUndoCommand(parent)
    , m_srcEntityName(std::move(srcEntityName))
    , m_boneNames(std::move(boneNames))
    , m_dstEntityName(std::move(dstEntityName))
    , m_opts(opts)
{
    setText(QStringLiteral("Attach bone to entity"));
}

void AttachBoneToEntityCommand::redo()
{
    Ogre::Entity* src = resolveEntityByName(m_srcEntityName);
    Ogre::Entity* dst = resolveEntityByName(m_dstEntityName);
    if (!src || !dst) return;
    if (m_firstRedo) {
        if (dst->getMesh() && dst->getMesh()->hasSkeleton() && dst->getMesh()->getSkeleton())
            m_dstBefore = SkeletonEditor::captureSnapshot(dst);
        m_firstRedo = false;
    }

    const auto result = SkeletonEditor::attachBonesToEntity(src, m_boneNames, dst, m_opts);
    if (!result.ok) return;
    m_attachedBoneName = result.boneName;
    m_applied = true;
    SentryReporter::addBreadcrumb(QStringLiteral("scene.skel.hier.attach"),
        QStringLiteral("%1 → %2: %3")
            .arg(QString::fromStdString(m_srcEntityName),
                 QString::fromStdString(m_dstEntityName),
                 m_attachedBoneName));
    SkeletonEditor::refreshAfterEdit(m_dstEntityName, m_attachedBoneName);
}

void AttachBoneToEntityCommand::undo()
{
    Ogre::Entity* dst = resolveEntityByName(m_dstEntityName);
    if (!dst) return;
    if (m_dstBefore.bones.empty()) {
        // Destination had no skeleton before attach — strip skeleton binding.
        if (dst->getMesh()) {
            dst->getMesh()->_notifySkeleton(Ogre::SkeletonPtr());
            dst->_initialise(true);
        }
        SkeletonEditor::refreshAfterEdit(m_dstEntityName);
    } else {
        QString err;
        if (SkeletonEditor::restoreSnapshot(dst, m_dstBefore, &err))
            SkeletonEditor::refreshAfterEdit(m_dstEntityName);
    }
    m_applied = false;
}

SetRestPoseCommand::SetRestPoseCommand(std::string entityName,
                                       Op op,
                                       QStringList boneNames,
                                       QUndoCommand* parent)
    : QUndoCommand(parent)
    , m_entityName(std::move(entityName))
    , m_op(op)
    , m_boneNames(std::move(boneNames))
{
    switch (m_op) {
    case Op::CaptureAll:
        setText(QStringLiteral("Capture rest pose"));
        break;
    case Op::SnapSelected:
        setText(QStringLiteral("Snap bones to rest pose"));
        break;
    case Op::Reset:
        setText(QStringLiteral("Reset rest pose"));
        break;
    }
}

SetRestPoseCommand::SetRestPoseCommand(std::string entityName,
                                       std::vector<ExplicitPose> explicitPoses,
                                       QUndoCommand* parent)
    : QUndoCommand(parent)
    , m_entityName(std::move(entityName))
    , m_op(Op::SnapSelected)
    , m_explicitPoses(std::move(explicitPoses))
{
    setText(QStringLiteral("Edit bone rest pose"));
    for (const auto& p : m_explicitPoses)
        m_boneNames.append(p.boneName);
}

void SetRestPoseCommand::redo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity) return;

    if (m_firstRedo) {
        m_before = SkeletonEditor::captureSnapshot(entity);
        SkeletonEditor::Result result;
        if (!m_explicitPoses.empty()) {
            result.ok = true;
            for (const auto& pose : m_explicitPoses) {
                const auto one = SkeletonEditor::commitBoneRestPose(
                    entity, pose.boneName, pose.position, pose.orientation, pose.scale);
                if (!one.ok) {
                    result = one;
                    break;
                }
            }
        } else {
            switch (m_op) {
            case Op::CaptureAll:
                result = SkeletonEditor::captureRestPose(entity);
                break;
            case Op::SnapSelected:
                result = SkeletonEditor::captureRestPose(entity, m_boneNames);
                break;
            case Op::Reset:
                result = SkeletonEditor::resetRestPose(entity);
                break;
            }
        }
        if (!result.ok) return;
        m_after = SkeletonEditor::captureSnapshot(entity);
        m_firstRedo = false;
    } else {
        QString err;
        if (!SkeletonEditor::restoreSnapshot(entity, m_after, &err))
            return;
    }

    m_applied = true;
    QString crumb;
    if (!m_explicitPoses.empty()) {
        crumb = QStringLiteral("scene.skel.rest_pose.edit");
    } else {
        switch (m_op) {
        case Op::CaptureAll:
            crumb = QStringLiteral("scene.skel.rest_pose.capture");
            break;
        case Op::SnapSelected:
            crumb = QStringLiteral("scene.skel.rest_pose.snap");
            break;
        case Op::Reset:
            crumb = QStringLiteral("scene.skel.rest_pose.reset");
            break;
        }
    }
    SentryReporter::addBreadcrumb(crumb,
        QStringLiteral("%1 (%2 bone(s))")
            .arg(QString::fromStdString(m_entityName))
            .arg(m_boneNames.isEmpty() ? QStringLiteral("all")
                                       : QString::number(m_boneNames.size())));
    SkeletonEditor::refreshAfterEdit(m_entityName);
    if (auto* editor = SkeletonEditor::getSingletonPtr())
        emit editor->restPoseChanged();
}

void SetRestPoseCommand::undo()
{
    Ogre::Entity* entity = resolveEntityByName(m_entityName);
    if (!entity || m_before.bones.empty()) return;
    QString err;
    if (!SkeletonEditor::restoreSnapshot(entity, m_before, &err))
        return;
    m_applied = false;
    SentryReporter::addBreadcrumb(QStringLiteral("scene.skel.rest_pose.undo"),
        QString::fromStdString(m_entityName));
    SkeletonEditor::refreshAfterEdit(m_entityName);
    if (auto* editor = SkeletonEditor::getSingletonPtr())
        emit editor->restPoseChanged();
}
