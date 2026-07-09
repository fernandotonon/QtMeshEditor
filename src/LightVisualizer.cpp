#include "LightVisualizer.h"

#include "GlobalDefinitions.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <OgreImage.h>
#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>
#include <OgreSceneManager.h>
#include <OgreTextureManager.h>

#include <QImage>
#include <QPainter>
#include <QtMath>

namespace
{
constexpr const char* kLightNameTag = "lightVisualizerLightName";
constexpr float kIconSize = 0.4f;
constexpr int kSphereSegments = 16;
constexpr int kConeSegments = 20;

Ogre::ColourValue tintColour(const Ogre::ColourValue& diffuse, float alpha, bool selected)
{
    const float a = selected ? 1.0f : alpha;
    return Ogre::ColourValue(diffuse.r, diffuse.g, diffuse.b, a);
}

void addLine(Ogre::ManualObject* mo, const Ogre::Vector3& a, const Ogre::Vector3& b, const Ogre::ColourValue& c)
{
    mo->position(a);
    mo->colour(c);
    mo->position(b);
    mo->colour(c);
}

void addWireCircle(Ogre::ManualObject* mo,
                   const Ogre::Vector3& center,
                   const Ogre::Vector3& axisU,
                   const Ogre::Vector3& axisV,
                   float radius,
                   const Ogre::ColourValue& colour,
                   int segments)
{
    if (segments < 3)
        return;

    Ogre::Vector3 prev = center + axisU * radius;
    for (int i = 1; i <= segments; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(segments) * Ogre::Math::TWO_PI;
        const Ogre::Vector3 point = center + axisU * (std::cos(t) * radius) + axisV * (std::sin(t) * radius);
        addLine(mo, prev, point, colour);
        prev = point;
    }
}

void addWireSphere(Ogre::ManualObject* mo, float radius, const Ogre::ColourValue& colour, int segments)
{
    addWireCircle(mo, Ogre::Vector3::ZERO, Ogre::Vector3::UNIT_X, Ogre::Vector3::UNIT_Y, radius, colour, segments);
    addWireCircle(mo, Ogre::Vector3::ZERO, Ogre::Vector3::UNIT_X, Ogre::Vector3::UNIT_Z, radius, colour, segments);
    addWireCircle(mo, Ogre::Vector3::ZERO, Ogre::Vector3::UNIT_Y, Ogre::Vector3::UNIT_Z, radius, colour, segments);
}

Ogre::Vector3 orthoBasisU(const Ogre::Vector3& forward)
{
    Ogre::Vector3 up = Ogre::Vector3::UNIT_Y;
    if (std::fabs(forward.dotProduct(up)) > 0.95f)
        up = Ogre::Vector3::UNIT_X;
    Ogre::Vector3 u = up.crossProduct(forward);
    u.normalise();
    return u;
}

void addDirectionalGizmo(Ogre::ManualObject* mo, const Ogre::ColourValue& colour)
{
    const Ogre::Vector3 dir = Ogre::Vector3::NEGATIVE_UNIT_Z;
    const Ogre::Vector3 tail = Ogre::Vector3::ZERO;
    const Ogre::Vector3 head = dir * 1.4f;
    addLine(mo, tail, head, colour);

    const Ogre::Vector3 u = orthoBasisU(dir);
    const Ogre::Vector3 v = dir.crossProduct(u);
  for (int i = 0; i < 8; ++i)
    {
        const float t = static_cast<float>(i) / 8.0f * Ogre::Math::TWO_PI;
        const Ogre::Vector3 rayDir = (u * std::cos(t) + v * std::sin(t)).normalisedCopy();
        addLine(mo, Ogre::Vector3::ZERO, rayDir * 0.45f, colour);
    }

    const float headRadius = 0.12f;
    addWireCircle(mo, head, u, v, headRadius, colour, 10);
}

void addPointGizmo(Ogre::ManualObject* mo, float radius, const Ogre::ColourValue& colour)
{
    addWireSphere(mo, std::max(radius, 0.05f), colour, kSphereSegments);
}

void addSpotGizmo(Ogre::ManualObject* mo,
                  float range,
                  float innerAngleDeg,
                  float outerAngleDeg,
                  const Ogre::ColourValue& colour)
{
    const Ogre::Vector3 apex = Ogre::Vector3::ZERO;
    const Ogre::Vector3 forward = Ogre::Vector3::NEGATIVE_UNIT_Z;
    const Ogre::Vector3 u = orthoBasisU(forward);
    const Ogre::Vector3 v = forward.crossProduct(u);

    const float outerRadius = range * std::tan(Ogre::Degree(outerAngleDeg).valueRadians());
    const float innerRadius = range * std::tan(Ogre::Degree(innerAngleDeg).valueRadians());
    const Ogre::Vector3 outerCenter = forward * range;
    const Ogre::Vector3 innerCenter = forward * range;

    addWireCircle(mo, outerCenter, u, v, std::max(outerRadius, 0.02f), colour, kConeSegments);
    if (innerRadius > 0.01f && innerAngleDeg < outerAngleDeg)
        addWireCircle(mo, innerCenter, u, v, innerRadius, colour, kConeSegments);

    for (int i = 0; i < kConeSegments; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(kConeSegments) * Ogre::Math::TWO_PI;
        const Ogre::Vector3 offset = u * std::cos(t) + v * std::sin(t);
        addLine(mo, apex, outerCenter + offset * outerRadius, colour);
    }
}

QImage makeIconImage(const QString& kind)
{
    QImage image(64, 64, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (kind == QStringLiteral("directional"))
    {
        painter.setPen(QPen(QColor(255, 220, 90), 4));
        painter.setBrush(QColor(255, 220, 90, 220));
        painter.drawEllipse(20, 20, 24, 24);
        painter.setPen(QPen(QColor(255, 220, 90), 3));
        for (int i = 0; i < 8; ++i)
        {
            const qreal angle = i * 45.0 * M_PI / 180.0;
            const QPointF center(32, 32);
            const QPointF outer(center.x() + std::cos(angle) * 28, center.y() + std::sin(angle) * 28);
            const QPointF inner(center.x() + std::cos(angle) * 16, center.y() + std::sin(angle) * 16);
            painter.drawLine(inner, outer);
        }
    }
    else if (kind == QStringLiteral("point"))
    {
        painter.setPen(QPen(QColor(255, 245, 200), 3));
        painter.setBrush(QColor(255, 245, 200, 230));
        painter.drawEllipse(18, 18, 28, 28);
        painter.setPen(QPen(QColor(255, 255, 255, 180), 2));
        painter.drawEllipse(24, 24, 16, 16);
    }
    else
    {
        painter.setPen(QPen(QColor(255, 190, 90), 3));
        painter.setBrush(QColor(255, 190, 90, 200));
        QPolygonF cone;
        cone << QPointF(32, 10) << QPointF(52, 52) << QPointF(12, 52);
        painter.drawPolygon(cone);
    }

    return image;
}

Ogre::TexturePtr uploadIconTexture(Ogre::SceneManager* sceneMgr, const QString& name, const QImage& image)
{
    auto& texMgr = Ogre::TextureManager::getSingleton();
    const Ogre::String texName = ("LightVisualizer/Icon/" + name).toStdString();
    if (texMgr.resourceExists(texName))
        texMgr.remove(texName);

    Ogre::TexturePtr texture = texMgr.createManual(
        texName,
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
        Ogre::TEX_TYPE_2D,
        static_cast<Ogre::uint32>(image.width()),
        static_cast<Ogre::uint32>(image.height()),
        0,
        Ogre::PF_BYTE_RGBA,
        Ogre::TU_STATIC);

    QImage rgba = image.convertToFormat(QImage::Format_RGBA8888).mirrored(false, true);
    Ogre::Image ogreImage;
    ogreImage.loadDynamicImage(rgba.bits(),
                               static_cast<Ogre::uint32>(rgba.width()),
                               static_cast<Ogre::uint32>(rgba.height()),
                               Ogre::PF_BYTE_RGBA);
    texture->loadImage(ogreImage);

    const Ogre::String matName = texName + "/Mat";
    auto& matMgr = Ogre::MaterialManager::getSingleton();
    if (matMgr.resourceExists(matName))
        matMgr.remove(matName);

    Ogre::MaterialPtr mat = matMgr.create(
        matName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    pass->setLightingEnabled(false);
    pass->setDepthCheckEnabled(false);
    pass->setDepthWriteEnabled(false);
    pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
    pass->createTextureUnitState(texName);

    (void)sceneMgr;
    return texture;
}

QString iconKindForType(Ogre::Light::LightTypes type)
{
    switch (type)
    {
    case Ogre::Light::LT_DIRECTIONAL:
        return QStringLiteral("directional");
    case Ogre::Light::LT_SPOTLIGHT:
        return QStringLiteral("spot");
    default:
        return QStringLiteral("point");
    }
}

Ogre::MaterialPtr materialForIconType(Ogre::Light::LightTypes type)
{
    const QString kind = iconKindForType(type);
    const Ogre::String matName = ("LightVisualizer/Icon/" + kind + "/Mat").toStdString();
    return Ogre::MaterialManager::getSingleton().getByName(
        matName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
}
} // namespace

LightVisualizer::LightVisualizer(Ogre::SceneManager* sceneMgr, QObject* parent)
    : QObject(parent)
    , mSceneMgr(sceneMgr)
{
    if (auto* lights = LightManager::getSingletonPtr())
    {
        connect(lights, &LightManager::lightCreated, this, &LightVisualizer::onLightCreated);
        connect(lights, &LightManager::lightChanged, this, &LightVisualizer::onLightChanged);
        connect(lights, &LightManager::lightDeleted, this, &LightVisualizer::onLightDeleted);
    }

    if (auto* selection = SelectionSet::getSingletonPtr())
        connect(selection, &SelectionSet::selectionChanged, this, &LightVisualizer::onSelectionChanged);

    ensureResources();
    rebuildAll();
}

LightVisualizer::~LightVisualizer()
{
    try
    {
        const QStringList names = mOverlays.keys();
        for (const QString& name : names)
            destroyOverlay(name);
    }
    catch (...)
    {
    }
}

QString LightVisualizer::lightNameForMovable(const Ogre::MovableObject* obj)
{
    if (!obj)
        return {};
    const auto& any = obj->getUserObjectBindings().getUserAny(kLightNameTag);
    if (!any.has_value())
        return {};
    try
    {
        return QString::fromStdString(Ogre::any_cast<std::string>(any));
    }
    catch (const std::exception&)
    {
        return {};
    }
}

void LightVisualizer::tagLightMovable(Ogre::MovableObject* obj, const QString& lightName)
{
    if (!obj)
        return;
    obj->getUserObjectBindings().setUserAny(kLightNameTag, Ogre::Any(lightName.toStdString()));
    obj->setQueryFlags(LIGHT_QUERY_FLAGS);
    obj->setVisibilityFlags(GUI_VISIBILITY_FLAGS);
}

void LightVisualizer::ensureResources()
{
    if (mResourcesReady)
        return;

    mIconDirectionalTex = uploadIconTexture(mSceneMgr, QStringLiteral("directional"), makeIconImage(QStringLiteral("directional")));
    mIconPointTex = uploadIconTexture(mSceneMgr, QStringLiteral("point"), makeIconImage(QStringLiteral("point")));
    mIconSpotTex = uploadIconTexture(mSceneMgr, QStringLiteral("spot"), makeIconImage(QStringLiteral("spot")));

    const Ogre::String gizmoMatName = "LightVisualizer/GizmoMaterial";
    mGizmoMaterial = Ogre::MaterialManager::getSingleton().getByName(
        gizmoMatName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
    if (!mGizmoMaterial)
    {
        mGizmoMaterial = Ogre::MaterialManager::getSingleton().create(
            gizmoMatName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        Ogre::Pass* pass = mGizmoMaterial->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
        pass->setCullingMode(Ogre::CULL_NONE);
        pass->setDepthCheckEnabled(false);
        pass->setDepthWriteEnabled(false);
        pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
    }

    mResourcesReady = true;
}

void LightVisualizer::setIconsVisible(bool visible)
{
    SentryReporter::addBreadcrumb(QStringLiteral("scene.light.gizmo_toggle"),
                                  visible ? QStringLiteral("Show light icons")
                                          : QStringLiteral("Hide light icons"));
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  visible ? QStringLiteral("Show light icons")
                                          : QStringLiteral("Hide light icons"));
    if (mIconsVisible == visible)
        return;
    mIconsVisible = visible;
    if (visible)
        rebuildAll();
    else
    {
        const QStringList names = mOverlays.keys();
        for (const QString& name : names)
            destroyOverlay(name);
    }
}

void LightVisualizer::setSelectedGizmosOnly(bool selectedOnly)
{
    SentryReporter::addBreadcrumb(QStringLiteral("scene.light.gizmo_toggle"),
                                  selectedOnly ? QStringLiteral("Light gizmos: selected only")
                                               : QStringLiteral("Light gizmos: show all"));
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  selectedOnly ? QStringLiteral("Light gizmos: selected only")
                                               : QStringLiteral("Light gizmos: show all"));
    if (mSelectedGizmosOnly == selectedOnly)
        return;
    mSelectedGizmosOnly = selectedOnly;
    for (auto it = mOverlays.begin(); it != mOverlays.end(); ++it)
        updateOverlayVisibility(it.key());
}

void LightVisualizer::onLightCreated(const LightHandle& handle)
{
    if (!mIconsVisible || !handle.isValid())
        return;
    buildOverlay(handle);
}

void LightVisualizer::onLightChanged(const QString& name)
{
    refreshOverlay(name);
}

void LightVisualizer::onLightDeleted(const QString& name)
{
    destroyOverlay(name);
}

void LightVisualizer::onSelectionChanged()
{
    for (auto it = mOverlays.begin(); it != mOverlays.end(); ++it)
    {
        if (auto* lights = LightManager::getSingletonPtr())
        {
            if (const LightHandle* handle = lights->findLight(it.key()))
                rebuildGizmoGeometry(it.value(), *handle, isLightSelected(it.key()));
        }
        updateOverlayVisibility(it.key());
    }
}

void LightVisualizer::rebuildAll()
{
    if (!mIconsVisible)
        return;

    ensureResources();

    const QStringList existing = mOverlays.keys();
    for (const QString& name : existing)
        destroyOverlay(name);

    auto* lights = LightManager::getSingletonPtr();
    if (!lights)
        return;

    for (const LightHandle& handle : lights->lights())
        buildOverlay(handle);
}

void LightVisualizer::buildOverlay(const LightHandle& handle)
{
    if (!handle.isValid() || !mSceneMgr || !handle.sceneNode)
        return;

    destroyOverlay(handle.name);
    ensureResources();

    OverlayData data;
    data.lightName = handle.name;

    const Ogre::String overlayNodeName = ("LightVis_" + handle.name).toStdString();
    data.overlayNode = handle.sceneNode->createChildSceneNode(overlayNodeName);

    const Ogre::String iconName = ("LightVisIcon_" + handle.name).toStdString();
    data.icon = mSceneMgr->createBillboardSet(iconName, 1);
    data.icon->setDefaultDimensions(kIconSize, kIconSize);
    data.icon->setMaterial(materialForIconType(handle.light->getType()));
    data.icon->setRenderQueueGroup(ZORDER_OVERLAY);
    data.icon->createBillboard(Ogre::Vector3::ZERO);
    tagLightMovable(data.icon, handle.name);
    data.overlayNode->attachObject(data.icon);

    const Ogre::String gizmoName = ("LightVisGizmo_" + handle.name).toStdString();
    data.gizmo = mSceneMgr->createManualObject(gizmoName);
    data.gizmo->setRenderQueueGroup(ZORDER_OVERLAY);
    tagLightMovable(data.gizmo, handle.name);
    data.overlayNode->attachObject(data.gizmo);

    rebuildGizmoGeometry(data, handle, isLightSelected(handle.name));

    mOverlays.insert(handle.name, data);
    updateOverlayVisibility(handle.name);
}

void LightVisualizer::destroyOverlay(const QString& name)
{
    auto it = mOverlays.find(name);
    if (it == mOverlays.end() || !mSceneMgr)
        return;

    try
    {
        OverlayData data = it.value();
        if (data.overlayNode)
        {
            if (data.icon)
                data.overlayNode->detachObject(data.icon);
            if (data.gizmo)
                data.overlayNode->detachObject(data.gizmo);
            if (data.overlayNode->getParent())
                data.overlayNode->getParent()->removeChild(data.overlayNode);
            mSceneMgr->destroySceneNode(data.overlayNode);
        }
        if (data.icon)
            mSceneMgr->destroyBillboardSet(data.icon);
        if (data.gizmo)
            mSceneMgr->destroyManualObject(data.gizmo);

        mOverlays.erase(it);
    }
    catch (...)
    {
        mOverlays.erase(it);
    }
}

void LightVisualizer::refreshOverlay(const QString& name)
{
    auto it = mOverlays.find(name);
    if (it == mOverlays.end())
    {
        if (mIconsVisible)
        {
            if (const LightHandle* handle = LightManager::getSingletonPtr()->findLight(name))
                buildOverlay(*handle);
        }
        return;
    }

    if (auto* lights = LightManager::getSingletonPtr())
    {
        if (const LightHandle* handle = lights->findLight(name))
        {
            if (it->icon)
                it->icon->setMaterial(materialForIconType(handle->light->getType()));
            rebuildGizmoGeometry(*it, *handle, isLightSelected(name));
            updateOverlayVisibility(name);
        }
    }
}

bool LightVisualizer::isLightSelected(const QString& name) const
{
    auto* selection = SelectionSet::getSingletonPtr();
    if (!selection)
        return false;
    for (Ogre::SceneNode* node : selection->getNodesSelectionList())
    {
        if (node && QString::fromStdString(node->getName()) == name)
            return true;
    }
    return false;
}

void LightVisualizer::rebuildGizmoGeometry(OverlayData& data, const LightHandle& handle, bool selected)
{
    if (!data.gizmo || !handle.light)
        return;

    const float alpha = selected ? 1.0f : 0.5f;
    const Ogre::ColourValue colour = tintColour(handle.light->getDiffuseColour(), alpha, selected);

    data.gizmo->clear();
    data.gizmo->begin(mGizmoMaterial->getName(), Ogre::RenderOperation::OT_LINE_LIST);

    switch (handle.light->getType())
    {
    case Ogre::Light::LT_DIRECTIONAL:
        addDirectionalGizmo(data.gizmo, colour);
        break;
    case Ogre::Light::LT_SPOTLIGHT:
        addSpotGizmo(data.gizmo,
                     handle.light->getAttenuationRange(),
                     handle.light->getSpotlightInnerAngle().valueDegrees(),
                     handle.light->getSpotlightOuterAngle().valueDegrees(),
                     colour);
        break;
    default:
        addPointGizmo(data.gizmo, handle.light->getAttenuationRange(), colour);
        break;
    }

    data.gizmo->end();
}

void LightVisualizer::updateOverlayVisibility(const QString& name)
{
    auto it = mOverlays.find(name);
    if (it == mOverlays.end())
        return;

    const bool selected = isLightSelected(name);
    const bool showGizmo = mIconsVisible && (!mSelectedGizmosOnly || selected);

    if (it->icon)
        it->icon->setVisible(mIconsVisible);
    if (it->gizmo)
        it->gizmo->setVisible(showGizmo);
}
