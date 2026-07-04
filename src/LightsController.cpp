#include "LightsController.h"

#include "LightManager.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TransformOperator.h"
#include "UndoManager.h"
#include "commands/LightCommands.h"
#include "mainwindow.h"
#include "SentryReporter.h"

#include <OgreCamera.h>
#include <OgreSceneNode.h>

#include <SpaceCamera.h>
#include <EditorViewport.h>
#include <OgreWidget.h>

LightsController* LightsController::s_singleton = nullptr;

LightsController* LightsController::instance()
{
    if (!s_singleton)
        s_singleton = new LightsController(); // NOSONAR — singleton
    return s_singleton;
}

LightsController* LightsController::qmlInstance(QQmlEngine*, QJSEngine*)
{
    return instance();
}

void LightsController::kill()
{
    delete s_singleton; // NOSONAR — singleton
    s_singleton = nullptr;
}

LightsController::LightsController(QObject* parent)
    : QObject(parent)
{
    if (auto* sel = SelectionSet::getSingletonPtr())
        connect(sel, &SelectionSet::selectionChanged, this, &LightsController::selectionChanged);
}

bool LightsController::hasLightSelection() const
{
    auto* sel = SelectionSet::getSingletonPtr();
    if (!sel || !sel->hasNodes())
        return false;

    for (Ogre::SceneNode* node : sel->getNodesSelectionList())
    {
        if (LightManager::sceneNodeIsUserLight(node))
            return true;
    }
    return false;
}

bool LightsController::computeViewportPlacement(Ogre::Vector3& position,
                                                Ogre::Vector3& direction) const
{
    SpaceCamera* spaceCam = nullptr;

    if (auto* xform = TransformOperator::getSingleton())
    {
        if (OgreWidget* active = xform->getActiveWidget())
            spaceCam = active->getSpaceCamera();
    }

    if (!spaceCam && Manager::getSingletonPtr())
    {
        if (MainWindow* mw = Manager::getSingleton()->getMainWindow())
        {
            const ViewportCameraSnapshot snap = mw->queryViewportCamera(false);
            if (snap.valid)
            {
                Ogre::Matrix4 invView = snap.viewMatrix.inverse();
                position = invView.getTrans();
                direction = invView * Ogre::Vector3::UNIT_Z;
                direction.normalise();
                position += direction * 3.0f;
                return true;
            }
        }
    }

    if (!spaceCam)
        return false;

    Ogre::Camera* camera = spaceCam->getCamera();
    if (!camera)
        return false;

    position = camera->getDerivedPosition();
    direction = camera->getDerivedDirection();
    position += direction * 3.0f;
    return true;
}

void LightsController::selectLightHandle(const QString& name)
{
    auto* mgr = Manager::getSingletonPtr();
    auto* sel = SelectionSet::getSingletonPtr();
    if (!mgr || !sel)
        return;

    try
    {
        Ogre::SceneNode* node = mgr->getSceneNode(name);
        if (node)
            sel->selectOne(node);
    }
    catch (...)
    {
    }
}

void LightsController::addLight(Ogre::Light::LightTypes type, bool atViewport)
{
    auto* lights = LightManager::getSingleton();
    LightHandle handle;

    if (atViewport)
    {
        Ogre::Vector3 position;
        Ogre::Vector3 direction;
        if (!computeViewportPlacement(position, direction))
            return;

        const bool setDirection =
            type == Ogre::Light::LT_DIRECTIONAL || type == Ogre::Light::LT_SPOTLIGHT;
        handle = lights->createLightAt(type,
                                       LightManager::defaultBaseNameForType(type),
                                       position,
                                       direction,
                                       setDirection);
    }
    else
    {
        handle = lights->createLight(type);
    }

    if (!handle.isValid())
        return;

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Create light: %1")
                                      .arg(LightManager::defaultBaseNameForType(type)));

    UndoManager::getSingleton()->push(new CreateLightCommand(LightSnapshot::fromHandle(handle)));
    selectLightHandle(handle.name);
}

void LightsController::addDirectionalLight()
{
    addLight(Ogre::Light::LT_DIRECTIONAL, false);
}

void LightsController::addPointLight()
{
    addLight(Ogre::Light::LT_POINT, false);
}

void LightsController::addSpotLight()
{
    addLight(Ogre::Light::LT_SPOTLIGHT, false);
}

void LightsController::addDirectionalLightAtViewport()
{
    addLight(Ogre::Light::LT_DIRECTIONAL, true);
}

void LightsController::addPointLightAtViewport()
{
    addLight(Ogre::Light::LT_POINT, true);
}

void LightsController::addSpotLightAtViewport()
{
    addLight(Ogre::Light::LT_SPOTLIGHT, true);
}

void LightsController::duplicateSelectedLights()
{
    auto* sel = SelectionSet::getSingletonPtr();
    auto* lights = LightManager::getSingleton();
    if (!sel || !lights)
        return;

    QList<LightSnapshot> cloneSnapshots;
    QStringList cloneNames;

    for (Ogre::SceneNode* node : sel->getNodesSelectionList())
    {
        if (!LightManager::sceneNodeIsUserLight(node))
            continue;

        const QString sourceName = QString::fromStdString(node->getName());
        LightHandle clone = lights->duplicateLight(sourceName);
        if (!clone.isValid())
            continue;

        cloneSnapshots.append(LightSnapshot::fromHandle(clone));
        cloneNames.append(clone.name);
    }

    if (cloneSnapshots.isEmpty())
        return;

    UndoManager::getSingleton()->push(new DuplicateLightsCommand(cloneSnapshots));

    auto* mgr = Manager::getSingletonPtr();
    sel->clearList();
    for (const QString& name : cloneNames)
    {
        if (!mgr)
            break;
        Ogre::SceneNode* node = mgr->getSceneNode(name);
        if (node)
            sel->append(node);
    }
}

void LightsController::deleteSelectedLights()
{
    auto* sel = SelectionSet::getSingletonPtr();
    auto* lights = LightManager::getSingleton();
    if (!sel || !lights)
        return;

    QList<LightSnapshot> snapshots;
    QStringList names;

    for (Ogre::SceneNode* node : sel->getNodesSelectionList())
    {
        if (!LightManager::sceneNodeIsUserLight(node))
            continue;

        const QString name = QString::fromStdString(node->getName());
        if (const LightHandle* handle = lights->findLight(name))
            snapshots.append(LightSnapshot::fromHandle(*handle));
        names.append(name);
    }

    if (snapshots.isEmpty())
        return;

    UndoManager::getSingleton()->push(new DeleteLightsCommand(snapshots));

    for (const QString& name : names)
    {
        lights->deleteLight(name);
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Delete light: %1").arg(name));
    }

    sel->clearList();
}

void LightsController::deleteLightByName(const QString& name)
{
    auto* sel = SelectionSet::getSingletonPtr();
    if (!sel)
        return;

    if (Ogre::SceneNode* node = Manager::getSingleton()->getSceneNode(name))
    {
        if (!sel->contains(node))
            sel->selectOne(node);
    }

    deleteSelectedLights();
}

void LightsController::duplicateLightByName(const QString& name)
{
    auto* sel = SelectionSet::getSingletonPtr();
    if (!sel)
        return;

    if (Ogre::SceneNode* node = Manager::getSingleton()->getSceneNode(name))
        sel->selectOne(node);

    duplicateSelectedLights();
}

void LightsController::renameLight(const QString& oldName, const QString& newName)
{
    const QString trimmed = newName.trimmed();
    if (oldName.isEmpty() || trimmed.isEmpty() || oldName == trimmed)
        return;

    if (!LightManager::getSingleton()->findLight(oldName))
        return;

    if (!LightManager::getSingleton()->renameLight(oldName, trimmed))
        return;

    UndoManager::getSingleton()->push(new RenameLightCommand(oldName, trimmed));
    selectLightHandle(trimmed);
}

bool LightsController::isLightNode(const QString& nodeName) const
{
    if (nodeName.isEmpty() || !Manager::getSingletonPtr())
        return false;

    try
    {
        Ogre::SceneNode* node = Manager::getSingleton()->getSceneNode(nodeName);
        return LightManager::sceneNodeIsUserLight(node);
    }
    catch (...)
    {
        return false;
    }
}
