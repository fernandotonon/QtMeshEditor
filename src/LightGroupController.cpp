#include "LightGroupController.h"

#include "LightGroupLibrary.h"
#include "LightManager.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <OgreSceneNode.h>

LightGroupController* LightGroupController::s_singleton = nullptr;

LightGroupController* LightGroupController::instance()
{
    if (!s_singleton)
        s_singleton = new LightGroupController(); // NOSONAR — singleton
    return s_singleton;
}

LightGroupController* LightGroupController::qmlInstance(QQmlEngine*, QJSEngine*)
{
    return instance();
}

void LightGroupController::kill()
{
    delete s_singleton; // NOSONAR — singleton
    s_singleton = nullptr;
}

LightGroupController::LightGroupController(QObject* parent)
    : QObject(parent)
{
    if (auto* sel = SelectionSet::getSingletonPtr())
        connect(sel, &SelectionSet::selectionChanged, this, &LightGroupController::selectionChanged);
    if (auto* lights = LightManager::getSingletonPtr())
    {
        connect(lights, &LightManager::lightCreated, this, &LightGroupController::groupsChanged);
        connect(lights, &LightManager::lightDeleted, this, &LightGroupController::groupsChanged);
        connect(lights, &LightManager::lightChanged, this, &LightGroupController::groupsChanged);
    }
}

QStringList LightGroupController::selectedLightNames() const
{
    QStringList names;
    auto* sel = SelectionSet::getSingletonPtr();
    if (!sel)
        return names;

    for (Ogre::SceneNode* node : sel->getNodesSelectionList())
    {
        if (node && LightManager::sceneNodeIsUserLight(node))
            names.append(QString::fromStdString(node->getName()));
    }
    return names;
}

QString LightGroupController::selectedLightGroupNodeName() const
{
    auto* sel = SelectionSet::getSingletonPtr();
    auto* mgr = Manager::getSingletonPtr();
    if (!sel || !mgr)
        return {};

    for (Ogre::SceneNode* node : sel->getNodesSelectionList())
    {
        if (!node)
            continue;
        if (LightGroupLibrary::sceneNodeIsLightGroup(node))
            return QString::fromStdString(node->getName());
    }
    return {};
}

bool LightGroupController::canGroupSelection() const
{
    return selectedLightNames().size() >= 2;
}

bool LightGroupController::hasLightGroupSelection() const
{
    return !selectedLightGroupNodeName().isEmpty();
}

QStringList LightGroupController::lightGroupNames() const
{
    return LightGroupLibrary::listLightGroupNames();
}

QString LightGroupController::selectedGroupName() const
{
    return selectedLightGroupNodeName();
}

void LightGroupController::createGroupFromSelection(const QString& groupName)
{
    const LightGroupCreateResult result =
        LightGroupLibrary::createGroup(groupName, selectedLightNames());
    if (!result.ok)
        return;

    emit groupsChanged();
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Create light group: %1").arg(result.groupNodeName));
}

void LightGroupController::dissolveSelectedGroup()
{
    const QString groupName = selectedLightGroupNodeName();
    if (groupName.isEmpty())
        return;
    if (!LightGroupLibrary::dissolveGroup(groupName))
        return;

    emit groupsChanged();
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Dissolve light group: %1").arg(groupName));
}

void LightGroupController::setSelectedGroupEnabled(bool enabled)
{
    const QString groupName = selectedLightGroupNodeName();
    if (groupName.isEmpty())
        return;
    if (!LightGroupLibrary::setGroupEnabled(groupName, enabled))
        return;

    emit groupsChanged();
}
