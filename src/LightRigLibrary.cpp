#include "LightRigLibrary.h"

#include "AppSettingsKeys.h"
#include "HDR/HdrEnvironmentController.h"
#include "Manager.h"
#include "SentryReporter.h"

#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

#include <QSettings>

#include <cmath>

namespace
{

struct RigLightSpec
{
    QString baseName;
    Ogre::Light::LightTypes type = Ogre::Light::LT_DIRECTIONAL;
    Ogre::Vector3 position = Ogre::Vector3::ZERO;
    Ogre::Vector3 direction = Ogre::Vector3(0, -1, -1);
    Ogre::ColourValue diffuse = Ogre::ColourValue::White;
    Ogre::ColourValue specular = Ogre::ColourValue(0.8f, 0.8f, 0.8f);
    float powerScale = 1.0f;
    float attenuationRange = 10.0f;
    float attenuationConstant = 1.0f;
    float attenuationLinear = 0.0f;
    float attenuationQuadratic = 0.0f;
    float spotlightInnerAngleDeg = 30.0f;
    float spotlightOuterAngleDeg = 40.0f;
    float spotlightFalloff = 1.0f;
    bool castShadows = true;
};

struct RigSpec
{
    QString id;
    QString displayName;
    QString groupName;
    Ogre::ColourValue ambient = Ogre::ColourValue(0.3f, 0.3f, 0.3f);
    QString suggestedHdri;
    QList<RigLightSpec> lights;
};

RigLightSpec directional(const QString& name,
                         const Ogre::Vector3& direction,
                         const Ogre::ColourValue& diffuse,
                         float power,
                         bool shadows = true)
{
    RigLightSpec spec;
    spec.baseName = name;
    spec.type = Ogre::Light::LT_DIRECTIONAL;
    spec.direction = direction;
    spec.direction.normalise();
    spec.diffuse = diffuse;
    spec.powerScale = power;
    spec.castShadows = shadows;
    return spec;
}

RigLightSpec point(const QString& name,
                   const Ogre::Vector3& position,
                   const Ogre::ColourValue& diffuse,
                   float power,
                   float range)
{
    RigLightSpec spec;
    spec.baseName = name;
    spec.type = Ogre::Light::LT_POINT;
    spec.position = position;
    spec.diffuse = diffuse;
    spec.powerScale = power;
    spec.attenuationRange = range;
    return spec;
}

const QList<RigSpec>& rigCatalog()
{
    static const QList<RigSpec> kCatalog = {
        {QStringLiteral("single_key"),
         QStringLiteral("Single Key"),
         QStringLiteral("Single Key rig"),
         Ogre::ColourValue(0.3f, 0.3f, 0.3f),
         {},
         {directional(QStringLiteral("KeyLight"),
                      Ogre::Vector3(1, -1, 1),
                      Ogre::ColourValue(1.0f, 1.0f, 1.0f),
                      1.0f)}},
        {QStringLiteral("three_point_studio"),
         QStringLiteral("Three-point Studio"),
         QStringLiteral("Three-point rig"),
         Ogre::ColourValue(0.15f, 0.15f, 0.18f),
         QStringLiteral("studio_neutral.hdr"),
         {directional(QStringLiteral("KeyLight"),
                      Ogre::Vector3(1, -1.2f, 0.6f),
                      Ogre::ColourValue(1.0f, 0.97f, 0.92f),
                      1.2f),
          directional(QStringLiteral("FillLight"),
                      Ogre::Vector3(-1, -0.4f, 0.3f),
                      Ogre::ColourValue(0.75f, 0.82f, 1.0f),
                      0.45f),
          directional(QStringLiteral("BackLight"),
                      Ogre::Vector3(-0.2f, 0.8f, -1),
                      Ogre::ColourValue(1.0f, 0.9f, 0.75f),
                      0.65f)}},
        {QStringLiteral("outdoor_sunset"),
         QStringLiteral("Outdoor Sunset"),
         QStringLiteral("Sunset rig"),
         Ogre::ColourValue(0.25f, 0.28f, 0.35f),
         QStringLiteral("sunset_outdoor.hdr"),
         {directional(QStringLiteral("SunLight"),
                      Ogre::Vector3(0.8f, -0.35f, -0.6f),
                      Ogre::ColourValue(1.0f, 0.62f, 0.32f),
                      1.4f)}},
        {QStringLiteral("outdoor_overcast"),
         QStringLiteral("Outdoor Overcast"),
         QStringLiteral("Overcast rig"),
         Ogre::ColourValue(0.55f, 0.55f, 0.58f),
         QStringLiteral("overcast_outdoor.hdr"),
         {directional(QStringLiteral("SkyLight"),
                      Ogre::Vector3(0.2f, -1, 0.1f),
                      Ogre::ColourValue(0.92f, 0.94f, 0.98f),
                      0.55f)}},
        {QStringLiteral("indoor_window"),
         QStringLiteral("Indoor Window"),
         QStringLiteral("Indoor window rig"),
         Ogre::ColourValue(0.35f, 0.32f, 0.28f),
         QStringLiteral("indoor_window.hdr"),
         {directional(QStringLiteral("WindowLight"),
                      Ogre::Vector3(1, -0.15f, 0.2f),
                      Ogre::ColourValue(1.0f, 0.95f, 0.85f),
                      1.1f),
          directional(QStringLiteral("BounceLight"),
                      Ogre::Vector3(-0.7f, -0.5f, -0.4f),
                      Ogre::ColourValue(0.85f, 0.78f, 0.65f),
                      0.35f)}},
        {QStringLiteral("product_turntable"),
         QStringLiteral("Product Turntable"),
         QStringLiteral("Product rig"),
         Ogre::ColourValue(0.65f, 0.65f, 0.65f),
         {},
         {directional(QStringLiteral("TopLight"),
                      Ogre::Vector3(0, -1, 0),
                      Ogre::ColourValue(1.0f, 1.0f, 1.0f),
                      0.9f,
                      false),
          directional(QStringLiteral("LeftFill"),
                      Ogre::Vector3(-1, -0.2f, 0),
                      Ogre::ColourValue(0.95f, 0.95f, 0.95f),
                      0.45f,
                      false),
          directional(QStringLiteral("RightFill"),
                      Ogre::Vector3(1, -0.2f, 0),
                      Ogre::ColourValue(0.95f, 0.95f, 0.95f),
                      0.45f,
                      false)}},
    };
    return kCatalog;
}

const RigSpec* findRigSpec(const QString& rigId)
{
    for (const RigSpec& spec : rigCatalog())
    {
        if (spec.id == rigId)
            return &spec;
    }
    return nullptr;
}

LightSnapshot snapshotFromSpec(const RigLightSpec& spec, const QString& nodeName)
{
    LightSnapshot snapshot;
    snapshot.name = nodeName;
    snapshot.type = spec.type;
    snapshot.enabled = true;
    snapshot.diffuse = spec.diffuse;
    snapshot.specular = spec.specular;
    snapshot.powerScale = spec.powerScale;
    snapshot.attenuationRange = spec.attenuationRange;
    snapshot.attenuationConstant = spec.attenuationConstant;
    snapshot.attenuationLinear = spec.attenuationLinear;
    snapshot.attenuationQuadratic = spec.attenuationQuadratic;
    snapshot.spotlightInnerAngleDeg = spec.spotlightInnerAngleDeg;
    snapshot.spotlightOuterAngleDeg = spec.spotlightOuterAngleDeg;
    snapshot.spotlightFalloff = spec.spotlightFalloff;
    snapshot.position = spec.position;
    snapshot.orientation = Ogre::Quaternion::IDENTITY;
    snapshot.scale = Ogre::Vector3::UNIT_SCALE;
    if (spec.type == Ogre::Light::LT_DIRECTIONAL || spec.type == Ogre::Light::LT_SPOTLIGHT)
    {
        snapshot.usesDirection = true;
        snapshot.direction = spec.direction;
    }
    return snapshot;
}

void tagRigGroup(Ogre::SceneNode* node)
{
    if (!node)
        return;
    node->getUserObjectBindings().setUserAny(LightRigLibrary::kRigGroupTag, Ogre::Any(true));
}

} // namespace

namespace LightRigLibrary
{

QStringList rigIds()
{
    QStringList ids;
    for (const RigSpec& spec : rigCatalog())
        ids.append(spec.id);
    return ids;
}

QString displayNameForId(const QString& id)
{
    if (const RigSpec* spec = findRigSpec(id))
        return spec->displayName;
    return id;
}

int indexOfRig(const QString& id)
{
    const QStringList ids = rigIds();
    return ids.indexOf(id);
}

QString defaultRigId()
{
    return QStringLiteral("three_point_studio");
}

void setDefaultRigId(const QString& id)
{
    if (indexOfRig(id) < 0)
        return;
    QSettings settings;
    settings.setValue(AppSettingsKeys::lightingDefaultRig(), id);
}

QString readDefaultRigId()
{
    QSettings settings;
    const QString stored =
        settings.value(AppSettingsKeys::lightingDefaultRig(), defaultRigId()).toString();
    if (indexOfRig(stored) >= 0)
        return stored;
    return defaultRigId();
}

bool sceneNodeIsRigGroup(Ogre::SceneNode* node)
{
    if (!node)
        return false;

    const auto& bindings = node->getUserObjectBindings();
    const auto any = bindings.getUserAny(kRigGroupTag);
    return any.has_value() && Ogre::any_cast<bool>(any);
}

void destroyRigGroupNode(const QString& nodeName)
{
    if (nodeName.isEmpty())
        return;

    auto* mgr = Manager::getSingletonPtr();
    if (!mgr)
        return;

    mgr->destroySceneNode(nodeName);
}

void destroyAllRigGroups()
{
    auto* mgr = Manager::getSingletonPtr();
    auto* lights = LightManager::getSingletonPtr();
    if (!mgr || !mgr->getSceneMgr())
        return;

    if (lights)
    {
        QStringList rigLightNames;
        for (const LightHandle& handle : lights->lights())
        {
            if (!handle.isValid() || !handle.sceneNode)
                continue;
            auto* parentNode = static_cast<Ogre::SceneNode*>(handle.sceneNode->getParent());
            if (parentNode && sceneNodeIsRigGroup(parentNode))
                rigLightNames.append(handle.name);
        }
        for (const QString& name : rigLightNames)
            lights->deleteLight(name);
    }

    Ogre::SceneNode* root = mgr->getSceneMgr()->getRootSceneNode();
    QStringList rigNames;
    for (const auto& child : root->getChildren())
    {
        auto* node = static_cast<Ogre::SceneNode*>(child);
        if (sceneNodeIsRigGroup(node))
            rigNames.append(QString::fromStdString(node->getName()));
    }

    for (const QString& name : rigNames)
        mgr->destroySceneNode(name);
}

QList<LightSnapshot> captureLightsInRigGroups()
{
    QList<LightSnapshot> snapshots;
    auto* lights = LightManager::getSingletonPtr();
    if (!lights)
        return snapshots;

    for (const LightHandle& handle : lights->lights())
    {
        if (!handle.isValid() || !handle.sceneNode)
            continue;
        Ogre::Node* parent = handle.sceneNode->getParent();
        auto* parentNode = static_cast<Ogre::SceneNode*>(parent);
        if (parentNode && sceneNodeIsRigGroup(parentNode))
            snapshots.append(LightSnapshot::fromHandle(handle));
    }
    return snapshots;
}

LightRigApplyResult apply(const QString& rigId, bool replaceExisting)
{
    LightRigApplyResult result;
    result.rigId = rigId;

    const RigSpec* spec = findRigSpec(rigId);
    if (!spec)
    {
        result.error = QStringLiteral("Unknown light rig: %1").arg(rigId);
        return result;
    }

    auto* mgr = Manager::getSingletonPtr();
    auto* lights = LightManager::getSingleton();
    if (!mgr || !mgr->getSceneMgr() || !lights)
    {
        result.error = QStringLiteral("Scene is not ready");
        return result;
    }

    result.ambientBefore = mgr->getSceneMgr()->getAmbientLight();

    // Preset rigs always replace any previous rig installation so repeated
    // Apply clicks do not stack lights. replaceExisting additionally clears
    // individually-added user lights.
    result.removedLights = captureLightsInRigGroups();
    destroyAllRigGroups();

    if (replaceExisting)
    {
        const QList<LightSnapshot> remaining = lights->captureAllSnapshots();
        result.removedLights.append(remaining);
        lights->deleteAllUserLights();
    }

    Ogre::SceneNode* rigGroup = lights->createRigGroupNode(spec->groupName);
    if (!rigGroup)
    {
        result.error = QStringLiteral("Failed to create rig group node");
        return result;
    }
    tagRigGroup(rigGroup);
    result.rigGroupNodeName = QString::fromStdString(rigGroup->getName());

    for (const RigLightSpec& lightSpec : spec->lights)
    {
        const bool setDirection =
            lightSpec.type == Ogre::Light::LT_DIRECTIONAL
            || lightSpec.type == Ogre::Light::LT_SPOTLIGHT;
        LightHandle handle = lights->createLightUnderParent(rigGroup,
                                                            lightSpec.type,
                                                            lightSpec.baseName,
                                                            lightSpec.position,
                                                            lightSpec.direction,
                                                            setDirection);
        if (!handle.isValid())
            continue;

        LightSnapshot snapshot = snapshotFromSpec(lightSpec, handle.name);
        lights->applyProperties(handle.name, snapshot);
        if (handle.light)
            handle.light->setCastShadows(lightSpec.castShadows);

        result.addedLights.append(LightSnapshot::fromHandle(handle));
    }

    mgr->getSceneMgr()->setAmbientLight(spec->ambient);
    result.ambientAfter = spec->ambient;
    result.suggestedHdri = spec->suggestedHdri;
    result.replaceExisting = replaceExisting;
    result.ok = !result.addedLights.isEmpty();

    if (!result.ok)
        result.error = QStringLiteral("Rig produced no lights");

    SentryReporter::addBreadcrumb(QStringLiteral("scene.light.apply_rig"), rigId);

    return result;
}

void applyDefaultSceneLighting()
{
    apply(readDefaultRigId(), true);
}

} // namespace LightRigLibrary
