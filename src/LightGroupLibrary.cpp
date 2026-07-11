#include "LightGroupLibrary.h"

#include "Manager.h"
#include "SentryReporter.h"

#include <OgreSceneNode.h>

#include <QSet>

namespace
{

Ogre::SceneNode* rootNode()
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || !mgr->getSceneMgr())
        return nullptr;
    return mgr->getSceneMgr()->getRootSceneNode();
}

} // namespace

namespace LightGroupLibrary
{

bool sceneNodeIsLightGroup(Ogre::SceneNode* node)
{
    if (!node)
        return false;
    const auto& bindings = node->getUserObjectBindings();
    const auto any = bindings.getUserAny(kLightGroupTag);
    return any.has_value() && Ogre::any_cast<bool>(any);
}

void tagLightGroupNode(Ogre::SceneNode* node)
{
    if (!node)
        return;
    node->getUserObjectBindings().setUserAny(kLightGroupTag, Ogre::Any(true));
}

QStringList listLightGroupNames()
{
    QStringList names;
    Ogre::SceneNode* root = rootNode();
    if (!root)
        return names;

    for (const auto& child : root->getChildren())
    {
        auto* node = static_cast<Ogre::SceneNode*>(child);
        if (sceneNodeIsLightGroup(node))
            names.append(QString::fromStdString(node->getName()));
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

LightGroupCreateResult createGroup(const QString& groupName, const QStringList& lightNames)
{
    LightGroupCreateResult result;
    if (groupName.trimmed().isEmpty())
    {
        result.error = QStringLiteral("Group name is required");
        return result;
    }
    if (lightNames.isEmpty())
    {
        result.error = QStringLiteral("Select at least one light");
        return result;
    }

    auto* lights = LightManager::getSingletonPtr();
    if (!lights)
    {
        result.error = QStringLiteral("Light manager unavailable");
        return result;
    }

    auto* mgr = Manager::getSingletonPtr();
    if (!mgr)
    {
        result.error = QStringLiteral("Scene manager unavailable");
        return result;
    }

    Ogre::SceneNode* groupNode = lights->createRigGroupNode(groupName.trimmed());
    if (!groupNode)
    {
        result.error = QStringLiteral("Failed to create group node");
        return result;
    }
    tagLightGroupNode(groupNode);
    result.groupNodeName = QString::fromStdString(groupNode->getName());

    QSet<QString> seen;
    for (const QString& lightName : lightNames)
    {
        const QString trimmed = lightName.trimmed();
        if (trimmed.isEmpty() || seen.contains(trimmed))
            continue;
        seen.insert(trimmed);

        LightHandle* handle = lights->findLight(trimmed);
        if (!handle || !handle->sceneNode)
            continue;

        mgr->reparentNode(handle->sceneNode, groupNode);
        result.movedLightNames.append(trimmed);
    }

    if (result.movedLightNames.isEmpty())
    {
        Manager::getSingletonPtr()->destroySceneNode(result.groupNodeName);
        result.groupNodeName.clear();
        result.error = QStringLiteral("No valid lights to group");
        return result;
    }

    result.ok = true;
    SentryReporter::addBreadcrumb(QStringLiteral("scene.light.group_create"),
                                  QStringLiteral("Created light group %1 (%2 lights)")
                                      .arg(result.groupNodeName)
                                      .arg(result.movedLightNames.size()));
    return result;
}

bool dissolveGroup(const QString& groupNodeName)
{
    auto* mgr = Manager::getSingletonPtr();
    auto* lights = LightManager::getSingletonPtr();
    Ogre::SceneNode* root = rootNode();
    if (!mgr || !lights || !root || groupNodeName.isEmpty())
        return false;

    Ogre::SceneNode* groupNode = mgr->getSceneNode(groupNodeName);
    if (!groupNode || !sceneNodeIsLightGroup(groupNode))
        return false;

    const auto children = groupNode->getChildren();
    for (const auto& child : children)
    {
        auto* childNode = static_cast<Ogre::SceneNode*>(child);
        mgr->reparentNode(childNode, root);
    }

    mgr->destroySceneNode(groupNodeName);
    SentryReporter::addBreadcrumb(QStringLiteral("scene.light.group_dissolve"),
                                  QStringLiteral("Dissolved light group %1").arg(groupNodeName));
    return true;
}

bool setGroupEnabled(const QString& groupNodeName, bool enabled)
{
    auto* mgr = Manager::getSingletonPtr();
    auto* lights = LightManager::getSingletonPtr();
    if (!mgr || !lights || groupNodeName.isEmpty())
        return false;

    Ogre::SceneNode* groupNode = mgr->getSceneNode(groupNodeName);
    if (!groupNode || !sceneNodeIsLightGroup(groupNode))
        return false;

    for (const auto& child : groupNode->getChildren())
    {
        auto* childNode = static_cast<Ogre::SceneNode*>(child);
        if (!LightManager::sceneNodeIsUserLight(childNode))
            continue;
        if (LightHandle* handle = lights->findLightBySceneNode(childNode))
        {
            if (handle->light)
                handle->light->setVisible(enabled);
        }
    }

    SentryReporter::addBreadcrumb(QStringLiteral("scene.light.group_enable"),
                                  QStringLiteral("%1 group %2")
                                      .arg(enabled ? QStringLiteral("Enable")
                                                   : QStringLiteral("Disable"))
                                      .arg(groupNodeName));
    return true;
}

bool renameGroup(const QString& oldName, const QString& newName)
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || oldName.isEmpty() || newName.trimmed().isEmpty())
        return false;

    Ogre::SceneNode* groupNode = mgr->getSceneNode(oldName);
    if (!groupNode || !sceneNodeIsLightGroup(groupNode))
        return false;

    const QString trimmed = newName.trimmed();
    if (mgr->hasSceneNode(trimmed))
        return false;

    Ogre::SceneNode* renamedNode = mgr->addSceneNode(trimmed);
    if (!renamedNode)
        return false;
    tagLightGroupNode(renamedNode);

    const auto children = groupNode->getChildren();
    for (const auto& child : children)
        mgr->reparentNode(static_cast<Ogre::SceneNode*>(child), renamedNode);

    mgr->destroySceneNode(groupNode);
    return true;
}

} // namespace LightGroupLibrary
