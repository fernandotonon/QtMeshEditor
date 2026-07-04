#include "commands/LightCommands.h"

#include "LightManager.h"
#include "LightRigLibrary.h"
#include "Manager.h"

#include <OgreSceneManager.h>

CreateLightCommand::CreateLightCommand(const LightSnapshot& snapshot, QUndoCommand* parent)
    : QUndoCommand(QObject::tr("Create Light"), parent)
    , m_snapshot(snapshot)
{
}

void CreateLightCommand::undo()
{
    LightManager::getSingleton()->deleteLight(m_snapshot.name);
}

void CreateLightCommand::redo()
{
    if (m_firstRedo)
    {
        m_firstRedo = false;
        return;
    }
    LightManager::getSingleton()->restoreSnapshot(m_snapshot);
}

DeleteLightsCommand::DeleteLightsCommand(QList<LightSnapshot> snapshots, QUndoCommand* parent)
    : QUndoCommand(QObject::tr("Delete Light"), parent)
    , m_snapshots(std::move(snapshots))
{
    if (m_snapshots.size() > 1)
        setText(QObject::tr("Delete Lights"));
}

void DeleteLightsCommand::undo()
{
    for (const LightSnapshot& snapshot : m_snapshots)
        LightManager::getSingleton()->restoreSnapshot(snapshot);
}

void DeleteLightsCommand::redo()
{
    if (m_firstRedo)
    {
        m_firstRedo = false;
        return;
    }
    for (const LightSnapshot& snapshot : m_snapshots)
        LightManager::getSingleton()->deleteLight(snapshot.name);
}

RenameLightCommand::RenameLightCommand(const QString& oldName,
                                       const QString& newName,
                                       QUndoCommand* parent)
    : QUndoCommand(QObject::tr("Rename Light"), parent)
    , m_oldName(oldName)
    , m_newName(newName)
{
}

void RenameLightCommand::undo()
{
    LightManager::getSingleton()->renameLight(m_newName, m_oldName);
}

void RenameLightCommand::redo()
{
    if (m_firstRedo)
    {
        m_firstRedo = false;
        return;
    }
    LightManager::getSingleton()->renameLight(m_oldName, m_newName);
}

DuplicateLightsCommand::DuplicateLightsCommand(QList<LightSnapshot> cloneSnapshots,
                                             QUndoCommand* parent)
    : QUndoCommand(QObject::tr("Duplicate Light"), parent)
    , m_cloneSnapshots(std::move(cloneSnapshots))
{
    if (m_cloneSnapshots.size() > 1)
        setText(QObject::tr("Duplicate Lights"));
}

void DuplicateLightsCommand::undo()
{
    for (const LightSnapshot& snapshot : m_cloneSnapshots)
        LightManager::getSingleton()->deleteLight(snapshot.name);
}

void DuplicateLightsCommand::redo()
{
    if (m_firstRedo)
    {
        m_firstRedo = false;
        return;
    }
    for (const LightSnapshot& snapshot : m_cloneSnapshots)
        LightManager::getSingleton()->restoreSnapshot(snapshot);
}

QString lightPropertyClassLabel(LightPropertyClass propertyClass)
{
    switch (propertyClass)
    {
    case LightPropertyClass::Type:
        return QStringLiteral("type");
    case LightPropertyClass::Enabled:
        return QStringLiteral("enabled");
    case LightPropertyClass::Colour:
        return QStringLiteral("colour");
    case LightPropertyClass::Intensity:
        return QStringLiteral("intensity");
    case LightPropertyClass::Range:
        return QStringLiteral("range");
    case LightPropertyClass::Attenuation:
        return QStringLiteral("attenuation");
    case LightPropertyClass::SpotCone:
        return QStringLiteral("cone");
    }
    return QStringLiteral("property");
}

void EditLightPropertyCommand::applySnapshots(const QList<LightSnapshot>& snapshots)
{
    auto* lights = LightManager::getSingleton();
    for (const LightSnapshot& snapshot : snapshots)
        lights->applyProperties(snapshot.name, snapshot);
}

EditLightPropertyCommand::EditLightPropertyCommand(LightPropertyClass propertyClass,
                                                   QList<LightSnapshot> before,
                                                   QList<LightSnapshot> after,
                                                   QUndoCommand* parent)
    : QUndoCommand(QObject::tr("Edit Light"), parent)
    , m_propertyClass(propertyClass)
    , m_before(std::move(before))
    , m_after(std::move(after))
{
}

void EditLightPropertyCommand::undo()
{
    applySnapshots(m_before);
}

void EditLightPropertyCommand::redo()
{
    if (m_firstRedo)
    {
        m_firstRedo = false;
        return;
    }
    applySnapshots(m_after);
}

int EditLightPropertyCommand::id() const
{
    return static_cast<int>(m_propertyClass);
}

namespace
{

QStringList sortedLightNames(const QList<LightSnapshot>& snapshots)
{
    QStringList names;
    names.reserve(snapshots.size());
    for (const LightSnapshot& snapshot : snapshots)
        names.append(snapshot.name);
    names.sort();
    return names;
}

} // namespace

bool EditLightPropertyCommand::mergeWith(const QUndoCommand* other)
{
    if (other->id() != id())
        return false;

    const auto* cmd = static_cast<const EditLightPropertyCommand*>(other);
    if (cmd->m_propertyClass != m_propertyClass)
        return false;
    if (sortedLightNames(cmd->m_after) != sortedLightNames(m_after))
        return false;

    m_after = cmd->m_after;
    return true;
}

ApplyLightRigCommand::ApplyLightRigCommand(const LightRigApplyResult& result, QUndoCommand* parent)
    : QUndoCommand(QObject::tr("Apply Light Rig"), parent)
    , m_rigId(result.rigId)
    , m_rigGroupNodeName(result.rigGroupNodeName)
    , m_addedLights(result.addedLights)
    , m_removedLights(result.removedLights)
    , m_ambientBefore(result.ambientBefore)
    , m_ambientAfter(result.ambientAfter)
    , m_replaceExisting(result.replaceExisting)
{
    if (!result.rigId.isEmpty())
        setText(QObject::tr("Apply %1").arg(LightRigLibrary::displayNameForId(result.rigId)));
}

void ApplyLightRigCommand::undo()
{
    auto* lights = LightManager::getSingleton();
    for (const LightSnapshot& snapshot : m_addedLights)
        lights->deleteLight(snapshot.name);
    LightRigLibrary::destroyRigGroupNode(m_rigGroupNodeName);

    for (const LightSnapshot& snapshot : m_removedLights)
        lights->restoreSnapshot(snapshot);

    if (auto* mgr = Manager::getSingletonPtr())
    {
        if (Ogre::SceneManager* sceneMgr = mgr->getSceneMgr())
            sceneMgr->setAmbientLight(m_ambientBefore);
    }
}

void ApplyLightRigCommand::redo()
{
    if (m_firstRedo)
    {
        m_firstRedo = false;
        return;
    }

    auto* lights = LightManager::getSingleton();
    auto* mgr = Manager::getSingletonPtr();
    if (!lights || !mgr || !mgr->getSceneMgr())
        return;

    LightRigLibrary::destroyAllRigGroups();
    if (m_replaceExisting)
        lights->deleteAllUserLights();
    else
    {
        for (const LightSnapshot& snapshot : m_removedLights)
            lights->deleteLight(snapshot.name);
    }

    Ogre::SceneNode* rigGroup = LightRigLibrary::createRigGroupForRig(m_rigId);
    if (!rigGroup)
        return;

    for (const LightSnapshot& snapshot : m_addedLights)
        lights->restoreSnapshotUnderParent(rigGroup, snapshot);

    mgr->getSceneMgr()->setAmbientLight(m_ambientAfter);
}

void SetSceneAmbientCommand::applyAmbient(const Ogre::ColourValue& ambient)
{
    if (auto* mgr = Manager::getSingletonPtr())
    {
        if (Ogre::SceneManager* sceneMgr = mgr->getSceneMgr())
            sceneMgr->setAmbientLight(ambient);
    }
}

SetSceneAmbientCommand::SetSceneAmbientCommand(const Ogre::ColourValue& before,
                                               const Ogre::ColourValue& after,
                                               QUndoCommand* parent)
    : QUndoCommand(QObject::tr("Set Ambient Light"), parent)
    , m_before(before)
    , m_after(after)
{
}

void SetSceneAmbientCommand::undo()
{
    applyAmbient(m_before);
}

void SetSceneAmbientCommand::redo()
{
    if (m_firstRedo)
    {
        m_firstRedo = false;
        return;
    }
    applyAmbient(m_after);
}

int SetSceneAmbientCommand::id() const
{
    return 9001;
}

bool SetSceneAmbientCommand::mergeWith(const QUndoCommand* other)
{
    if (other->id() != id())
        return false;

    const auto* cmd = static_cast<const SetSceneAmbientCommand*>(other);
    m_after = cmd->m_after;
    return true;
}
