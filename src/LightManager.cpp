#include "LightManager.h"

#include "Manager.h"
#include "SentryReporter.h"

#include <OgreLight.h>
#include <OgreMath.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

LightManager* LightManager::s_singleton = nullptr;

LightSnapshot LightSnapshot::fromHandle(const LightHandle& handle)
{
    LightSnapshot snapshot;
    if (!handle.isValid())
        return snapshot;

    snapshot.name = handle.name;
    snapshot.type = handle.light->getType();
    snapshot.enabled = handle.light->isVisible();
    snapshot.diffuse = handle.light->getDiffuseColour();
    snapshot.specular = handle.light->getSpecularColour();
    snapshot.powerScale = handle.light->getPowerScale();
    snapshot.attenuationRange = handle.light->getAttenuationRange();
    snapshot.attenuationConstant = handle.light->getAttenuationConstant();
    snapshot.attenuationLinear = handle.light->getAttenuationLinear();
    snapshot.attenuationQuadratic = handle.light->getAttenuationQuadric();
    snapshot.spotlightInnerAngleDeg =
        handle.light->getSpotlightInnerAngle().valueDegrees();
    snapshot.spotlightOuterAngleDeg =
        handle.light->getSpotlightOuterAngle().valueDegrees();
    snapshot.spotlightFalloff = handle.light->getSpotlightFalloff();
    snapshot.position = handle.sceneNode->getPosition();
    snapshot.orientation = handle.sceneNode->getOrientation();
    snapshot.scale = handle.sceneNode->getScale();
    if (snapshot.type == Ogre::Light::LT_DIRECTIONAL
        || snapshot.type == Ogre::Light::LT_SPOTLIGHT)
    {
        snapshot.usesDirection = true;
        snapshot.direction =
            handle.sceneNode->getOrientation() * Ogre::Vector3::NEGATIVE_UNIT_Z;
    }
    return snapshot;
}

bool LightSnapshot::operator==(const LightSnapshot& other) const
{
    return name == other.name && type == other.type && enabled == other.enabled
           && diffuse == other.diffuse && specular == other.specular
           && powerScale == other.powerScale
           && attenuationRange == other.attenuationRange
           && attenuationConstant == other.attenuationConstant
           && attenuationLinear == other.attenuationLinear
           && attenuationQuadratic == other.attenuationQuadratic
           && spotlightInnerAngleDeg == other.spotlightInnerAngleDeg
           && spotlightOuterAngleDeg == other.spotlightOuterAngleDeg
           && spotlightFalloff == other.spotlightFalloff
           && position == other.position && orientation == other.orientation
           && scale == other.scale && usesDirection == other.usesDirection
           && direction == other.direction;
}

LightManager* LightManager::getSingleton()
{
    if (!s_singleton)
        s_singleton = new LightManager(); // NOSONAR — singleton
    return s_singleton;
}

LightManager* LightManager::getSingletonPtr()
{
    return s_singleton;
}

void LightManager::kill()
{
    delete s_singleton; // NOSONAR — singleton
    s_singleton = nullptr;
}

LightManager::LightManager(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<LightHandle>("LightHandle");
    qRegisterMetaType<LightSnapshot>("LightSnapshot");
    tryConnectToManager();
}

LightManager::~LightManager()
{
    m_lights.clear();
}

void LightManager::tryConnectToManager()
{
    if (m_connectedToManager)
        return;

    auto* mgr = Manager::getSingletonPtr();
    if (!mgr)
        return;

    connect(mgr, &Manager::sceneNodeDestroyed, this, &LightManager::onSceneNodeDestroyed);
    m_connectedToManager = true;
}

bool LightManager::isUserLight(const Ogre::Light* light)
{
    if (!light)
        return false;

    const auto& bindings = light->getUserObjectBindings();
    const auto any = bindings.getUserAny(kUserLightTag);
    return any.has_value() && Ogre::any_cast<bool>(any);
}

bool LightManager::sceneNodeIsUserLight(Ogre::SceneNode* node)
{
    if (!node)
        return false;

    for (unsigned short i = 0; i < node->numAttachedObjects(); ++i)
    {
        Ogre::MovableObject* obj = node->getAttachedObject(i);
        if (!obj || obj->getMovableType() != QLatin1String("Light"))
            continue;
        if (isUserLight(static_cast<Ogre::Light*>(obj)))
            return true;
    }
    return false;
}

QString LightManager::defaultBaseNameForType(Ogre::Light::LightTypes type)
{
    switch (type)
    {
    case Ogre::Light::LT_DIRECTIONAL:
        return QStringLiteral("DirectionalLight");
    case Ogre::Light::LT_SPOTLIGHT:
        return QStringLiteral("SpotLight");
    case Ogre::Light::LT_POINT:
    default:
        return QStringLiteral("PointLight");
    }
}

void LightManager::tagAsUserLight(Ogre::Light* light)
{
    if (!light)
        return;
    light->getUserObjectBindings().setUserAny(kUserLightTag, Ogre::Any(true));
}

QString LightManager::uniqueLightBaseName(const QString& base) const
{
    QString candidate = base.isEmpty() ? QStringLiteral("Light") : base;
    if (!Manager::getSingletonPtr())
        return candidate;

    auto* mgr = Manager::getSingletonPtr();
    unsigned int suffix = 0;
    while (true)
    {
        const QString name = suffix == 0 ? candidate : candidate + QString::number(suffix);
        if (!mgr->hasSceneNode(name) && findLight(name) == nullptr)
            return name;
        ++suffix;
    }
}

void LightManager::applySnapshotToHandle(const LightSnapshot& snapshot, LightHandle& handle) const
{
    if (!handle.isValid())
        return;

    handle.light->setType(snapshot.type);
    handle.light->setVisible(snapshot.enabled);
    handle.light->setDiffuseColour(snapshot.diffuse);
    handle.light->setSpecularColour(snapshot.specular);
    handle.light->setPowerScale(snapshot.powerScale);
    handle.light->setAttenuation(snapshot.attenuationRange,
                                 snapshot.attenuationConstant,
                                 snapshot.attenuationLinear,
                                 snapshot.attenuationQuadratic);
    if (snapshot.type == Ogre::Light::LT_SPOTLIGHT)
    {
        handle.light->setSpotlightRange(
            Ogre::Degree(snapshot.spotlightInnerAngleDeg),
            Ogre::Degree(snapshot.spotlightOuterAngleDeg),
            snapshot.spotlightFalloff);
    }
    handle.sceneNode->setPosition(snapshot.position);
    handle.sceneNode->setOrientation(snapshot.orientation);
    handle.sceneNode->setScale(snapshot.scale);
    if (snapshot.usesDirection)
        handle.sceneNode->setDirection(snapshot.direction);
}

LightHandle LightManager::createLightInternal(Ogre::Light::LightTypes type, const QString& baseName)
{
    const int stackIndex = m_lights.size();
    const Ogre::Vector3 stackedOffset(
        static_cast<Ogre::Real>(stackIndex) * 0.25f,
        static_cast<Ogre::Real>(stackIndex) * 0.1f,
        static_cast<Ogre::Real>(stackIndex) * 0.25f);
    const bool setDirection =
        type == Ogre::Light::LT_DIRECTIONAL || type == Ogre::Light::LT_SPOTLIGHT;
    return createLightAt(type,
                         baseName,
                         stackedOffset,
                         Ogre::Vector3::UNIT_Z,
                         setDirection);
}

LightHandle LightManager::createLightAt(Ogre::Light::LightTypes type,
                                        const QString& baseName,
                                        const Ogre::Vector3& position,
                                        const Ogre::Vector3& direction,
                                        bool setDirection)
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || !mgr->getSceneMgr())
        return {};

    Ogre::SceneNode* parent = mgr->getSceneMgr()->getRootSceneNode();
    return createLightUnderParent(parent, type, baseName, position, direction, setDirection);
}

LightHandle LightManager::createLightUnderParent(Ogre::SceneNode* parent,
                                                 Ogre::Light::LightTypes type,
                                                 const QString& baseName,
                                                 const Ogre::Vector3& position,
                                                 const Ogre::Vector3& direction,
                                                 bool setDirection)
{
    LightHandle handle;
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || !mgr->getSceneMgr() || !parent)
        return handle;

    const QString name = uniqueLightBaseName(baseName);
    Ogre::SceneNode* node = parent->createChildSceneNode(name.toStdString());
    if (!node)
        return handle;

    Ogre::SceneManager* sceneMgr = mgr->getSceneMgr();
    Ogre::Light* light = sceneMgr->createLight(name.toStdString());
    light->setType(type);
    light->setDiffuseColour(Ogre::ColourValue::White);
    light->setSpecularColour(Ogre::ColourValue(0.8f, 0.8f, 0.8f));
    light->setPowerScale(1.0f);
    if (type == Ogre::Light::LT_POINT || type == Ogre::Light::LT_SPOTLIGHT)
        light->setAttenuation(10.0f, 1.0f, 0.0f, 0.0f);
    if (type == Ogre::Light::LT_SPOTLIGHT)
        light->setSpotlightRange(Ogre::Degree(30.0f), Ogre::Degree(40.0f), 1.0f);
    node->attachObject(light);
    tagAsUserLight(light);
    node->setPosition(position);
    if (setDirection)
        node->setDirection(direction);

    handle.name = QString::fromStdString(node->getName());
    handle.light = light;
    handle.sceneNode = node;
    m_lights.append(handle);

    emit lightCreated(handle);
    return handle;
}

Ogre::SceneNode* LightManager::createRigGroupNode(const QString& baseName)
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || !mgr->getSceneMgr())
        return nullptr;

    QString candidate = baseName.isEmpty() ? QStringLiteral("Light rig") : baseName;
    unsigned int suffix = 0;
    while (true)
    {
        const QString name = suffix == 0 ? candidate : candidate + QString::number(suffix);
        if (!mgr->hasSceneNode(name))
        {
            Ogre::SceneNode* node = mgr->addSceneNode(name);
            return node;
        }
        ++suffix;
    }
}

QList<LightSnapshot> LightManager::captureAllSnapshots() const
{
    QList<LightSnapshot> snapshots;
    snapshots.reserve(m_lights.size());
    for (const LightHandle& handle : m_lights)
        snapshots.append(LightSnapshot::fromHandle(handle));
    return snapshots;
}

void LightManager::deleteAllUserLights()
{
    while (!m_lights.isEmpty())
        deleteLight(m_lights.first().name);
}

void LightManager::clearAllLights()
{
    while (!m_lights.isEmpty())
    {
        const QString name = m_lights.last().name;
        deleteLight(name);
    }
}

LightHandle LightManager::createDefaultKeyLight()
{
    clearAllLights();

    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || !mgr->getSceneMgr())
        return {};

    mgr->getSceneMgr()->setAmbientLight(Ogre::ColourValue(0.3f, 0.3f, 0.3f));

    LightHandle handle = createLightInternal(Ogre::Light::LT_DIRECTIONAL,
                                             QStringLiteral("KeyLight"));
    if (!handle.isValid())
        return handle;

    handle.light->setDiffuseColour(1.0f, 1.0f, 1.0f);
    handle.light->setSpecularColour(0.8f, 0.8f, 0.8f);
    handle.sceneNode->setPosition(Ogre::Vector3::ZERO);
    handle.sceneNode->setDirection(Ogre::Vector3(1, -1, 1));

    SentryReporter::addBreadcrumb(QStringLiteral("scene.light.create"),
                                  QStringLiteral("default key light"));

    emit lightChanged(handle.name);
    return handle;
}

LightHandle LightManager::createLight(Ogre::Light::LightTypes type, const QString& name)
{
    const QString baseName =
        name.isEmpty() ? defaultBaseNameForType(type) : name;
    LightHandle handle = createLightInternal(type, baseName);
    return handle;
}

LightHandle LightManager::duplicateLight(const QString& sourceName)
{
    const LightHandle* source = findLight(sourceName);
    if (!source || !source->isValid())
        return {};

    LightSnapshot snapshot = LightSnapshot::fromHandle(*source);
    snapshot.name = uniqueLightBaseName(sourceName + QStringLiteral("_copy"));
    snapshot.position += Ogre::Vector3(0.25f, 0.1f, 0.25f);

    LightHandle clone = restoreSnapshot(snapshot);
    if (clone.isValid())
    {
        SentryReporter::addBreadcrumb(QStringLiteral("scene.light.duplicate"),
                                      sourceName);
    }
    return clone;
}

LightHandle LightManager::restoreSnapshot(const LightSnapshot& snapshot)
{
    if (snapshot.name.isEmpty())
        return {};

    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || !mgr->getSceneMgr())
        return {};

    return restoreSnapshotUnderParent(mgr->getSceneMgr()->getRootSceneNode(), snapshot);
}

LightHandle LightManager::restoreSnapshotUnderParent(Ogre::SceneNode* parent,
                                                     const LightSnapshot& snapshot)
{
    if (snapshot.name.isEmpty() || !parent)
        return {};

    if (findLight(snapshot.name))
        return {};

    const QString baseName =
        snapshot.name.isEmpty() ? defaultBaseNameForType(snapshot.type) : snapshot.name;

    LightHandle handle;
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || !mgr->getSceneMgr())
        return handle;

    QString nodeName = baseName;
    if (mgr->hasSceneNode(nodeName) || findLight(nodeName))
        nodeName = uniqueLightBaseName(baseName);

    Ogre::SceneNode* node = parent->createChildSceneNode(nodeName.toStdString());
    if (!node)
        return handle;

    Ogre::SceneManager* sceneMgr = mgr->getSceneMgr();
    Ogre::Light* light = sceneMgr->createLight(nodeName.toStdString());
    node->attachObject(light);
    tagAsUserLight(light);

    handle.name = QString::fromStdString(node->getName());
    handle.light = light;
    handle.sceneNode = node;
    applySnapshotToHandle(snapshot, handle);
    handle.name = QString::fromStdString(node->getName());

    m_lights.append(handle);
    emit lightCreated(handle);
    return handle;
}

bool LightManager::deleteLight(const QString& name)
{
    for (int i = 0; i < m_lights.size(); ++i)
    {
        if (m_lights[i].name != name)
            continue;

        Ogre::SceneNode* node = m_lights[i].sceneNode;
        m_lights.removeAt(i);
        emit lightDeleted(name);

        if (node && Manager::getSingletonPtr())
        {
            m_suppressSceneNodeDestroyed = true;
            Manager::getSingleton()->destroySceneNode(node);
            m_suppressSceneNodeDestroyed = false;
        }

        return true;
    }
    return false;
}

bool LightManager::renameLight(const QString& oldName, const QString& newName)
{
    if (oldName.isEmpty() || newName.isEmpty() || oldName == newName)
        return false;

    LightHandle* handle = findLight(oldName);
    if (!handle || !handle->isValid())
        return false;

    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || mgr->hasSceneNode(newName) || findLight(newName) != nullptr)
        return false;

    const LightSnapshot snapshot = LightSnapshot::fromHandle(*handle);
    Ogre::SceneNode* oldNode = handle->sceneNode;

    for (int i = 0; i < m_lights.size(); ++i)
    {
        if (m_lights[i].name == oldName)
        {
            m_lights.removeAt(i);
            break;
        }
    }
    emit lightDeleted(oldName);

    if (mgr)
    {
        m_suppressSceneNodeDestroyed = true;
        mgr->destroySceneNode(oldNode);
        m_suppressSceneNodeDestroyed = false;
    }

    LightSnapshot renamedSnapshot = snapshot;
    renamedSnapshot.name = newName;
    LightHandle renamed = restoreSnapshot(renamedSnapshot);
    if (!renamed.isValid())
        return false;

    SentryReporter::addBreadcrumb(QStringLiteral("scene.light.rename"),
                                  oldName + QStringLiteral(" -> ") + newName);
    emit lightChanged(renamed.name);
    return true;
}

bool LightManager::applyProperties(const QString& name, const LightSnapshot& snapshot)
{
    LightHandle* handle = findLight(name);
    if (!handle || !handle->isValid())
        return false;

    LightSnapshot applied = snapshot;
    applied.name = handle->name;
    applied.position = handle->sceneNode->getPosition();
    applied.orientation = handle->sceneNode->getOrientation();
    applied.scale = handle->sceneNode->getScale();
    if (applied.type == Ogre::Light::LT_DIRECTIONAL
        || applied.type == Ogre::Light::LT_SPOTLIGHT)
    {
        applied.usesDirection = true;
        applied.direction =
            handle->sceneNode->getOrientation() * Ogre::Vector3::NEGATIVE_UNIT_Z;
    }
    else
    {
        applied.usesDirection = false;
    }

    applySnapshotToHandle(applied, *handle);
    emit lightChanged(handle->name);
    return true;
}

QList<LightHandle> LightManager::lights() const
{
    return m_lights;
}

LightHandle* LightManager::findLight(const QString& name)
{
    for (auto& handle : m_lights)
    {
        if (handle.name == name)
            return &handle;
    }
    return nullptr;
}

const LightHandle* LightManager::findLight(const QString& name) const
{
    for (const auto& handle : m_lights)
    {
        if (handle.name == name)
            return &handle;
    }
    return nullptr;
}

LightHandle* LightManager::findLightBySceneNode(Ogre::SceneNode* node)
{
    if (!node)
        return nullptr;

    const QString name = QString::fromStdString(node->getName());
    return findLight(name);
}

const LightHandle* LightManager::findLightBySceneNode(Ogre::SceneNode* node) const
{
    if (!node)
        return nullptr;

    const QString name = QString::fromStdString(node->getName());
    return findLight(name);
}

void LightManager::onSceneNodeDestroyed(Ogre::SceneNode* node)
{
    if (!node || m_suppressSceneNodeDestroyed)
        return;

    for (int i = 0; i < m_lights.size(); ++i)
    {
        if (m_lights[i].sceneNode != node)
            continue;

        const QString name = m_lights[i].name;
        m_lights.removeAt(i);
        emit lightDeleted(name);
        break;
    }
}
