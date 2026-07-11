#include "AreaLight.h"

#include "Manager.h"

#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

#include <QtMath>

#include <vector>

namespace
{

struct SamplePoint
{
    Ogre::Vector3 localPos;
};

std::vector<SamplePoint> sampleRectangle(float width, float height, int count)
{
    std::vector<SamplePoint> points;
    const int cols = qMax(1, static_cast<int>(std::ceil(std::sqrt(count))));
    const int rows = qMax(1, (count + cols - 1) / cols);
    for (int r = 0; r < rows; ++r)
    {
        for (int c = 0; c < cols; ++c)
        {
            if (static_cast<int>(points.size()) >= count)
                break;
            const float u = (cols == 1) ? 0.5f : static_cast<float>(c) / static_cast<float>(cols - 1);
            const float v = (rows == 1) ? 0.5f : static_cast<float>(r) / static_cast<float>(rows - 1);
            SamplePoint p;
            p.localPos = Ogre::Vector3((u - 0.5f) * width, (v - 0.5f) * height, 0.0f);
            points.push_back(p);
        }
    }
    return points;
}

std::vector<SamplePoint> sampleDisk(float radius, int count)
{
    std::vector<SamplePoint> points;
    const int n = qMax(1, count);
    for (int i = 0; i < n; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        const float angle = t * 2.0f * Ogre::Math::PI;
        const float ring = (i % 2 == 0) ? 1.0f : 0.55f;
        SamplePoint p;
        p.localPos = Ogre::Vector3(std::cos(angle) * radius * ring,
                                     std::sin(angle) * radius * ring,
                                     0.0f);
        points.push_back(p);
    }
    return points;
}

std::vector<SamplePoint> sampleLine(float length, int count)
{
    std::vector<SamplePoint> points;
    const int n = qMax(2, count);
    for (int i = 0; i < n; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        SamplePoint p;
        p.localPos = Ogre::Vector3((t - 0.5f) * length, 0.0f, 0.0f);
        points.push_back(p);
    }
    return points;
}

} // namespace

namespace AreaLight
{

bool isProxyLight(const Ogre::Light* light)
{
    if (!light)
        return false;
    const auto any = light->getUserObjectBindings().getUserAny(kProxyLightTag);
    return any.has_value() && Ogre::any_cast<bool>(any);
}

QString ownerLightName(const Ogre::Light* light)
{
    if (!light)
        return {};
    const auto any = light->getUserObjectBindings().getUserAny(kProxyOwnerKey);
    if (!any.has_value())
        return {};
    try
    {
        return QString::fromStdString(Ogre::any_cast<std::string>(any));
    }
    catch (...)
    {
        return {};
    }
}

void removeProxies(const QString& ownerLightName)
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || ownerLightName.isEmpty())
        return;

    Ogre::SceneManager* sceneMgr = mgr->getSceneMgr();
    if (!sceneMgr)
        return;

    const LightHandle* owner = LightManager::getSingletonPtr()
                                   ? LightManager::getSingleton()->findLight(ownerLightName)
                                   : nullptr;
    if (!owner || !owner->sceneNode)
        return;

    std::vector<Ogre::SceneNode*> toDestroy;
    for (const auto& child : owner->sceneNode->getChildren())
    {
        auto* childNode = static_cast<Ogre::SceneNode*>(child);
        for (unsigned short i = 0; i < childNode->numAttachedObjects(); ++i)
        {
            Ogre::MovableObject* obj = childNode->getAttachedObject(i);
            if (!obj || obj->getMovableType() != QStringLiteral("Light"))
                continue;
            auto* light = static_cast<Ogre::Light*>(obj);
            if (isProxyLight(light) && AreaLight::ownerLightName(light) == ownerLightName)
                toDestroy.push_back(childNode);
        }
    }

    for (Ogre::SceneNode* node : toDestroy)
    {
        node->removeAndDestroyAllChildren();
        node->getCreator()->destroySceneNode(node);
    }
}

void syncProxies(const LightHandle& owner, const LightSnapshot& snapshot)
{
#ifndef ENABLE_AREA_LIGHTS
    Q_UNUSED(owner);
    Q_UNUSED(snapshot);
    return;
#else
    if (!owner.isValid())
        return;

    removeProxies(owner.name);

    const QString shape = snapshot.areaShape.trimmed().toLower();
    if (shape.isEmpty())
    {
        owner.light->setVisible(snapshot.enabled);
        return;
    }

    std::vector<SamplePoint> samples;
    const int count = qBound(2, snapshot.areaSampleCount, 16);
    if (shape == QStringLiteral("rectangle"))
        samples = sampleRectangle(snapshot.areaWidth, snapshot.areaHeight, count);
    else if (shape == QStringLiteral("disk"))
        samples = sampleDisk(qMax(0.05f, snapshot.areaWidth * 0.5f), count);
    else if (shape == QStringLiteral("line"))
        samples = sampleLine(snapshot.areaWidth, count);
    else
        return;

    owner.light->setVisible(false);

    auto* lights = LightManager::getSingletonPtr();
    auto* mgr = Manager::getSingletonPtr();
    if (!lights || !mgr || !mgr->getSceneMgr())
        return;

    Ogre::SceneManager* sceneMgr = mgr->getSceneMgr();
    const float weight = 1.0f / static_cast<float>(samples.size());
    int index = 0;
    for (const SamplePoint& sample : samples)
    {
        const QString proxyName = owner.name + QStringLiteral("_area%1").arg(index++);
        Ogre::SceneNode* node = owner.sceneNode->createChildSceneNode(proxyName.toStdString());
        Ogre::Light* proxyLight = sceneMgr->createLight(proxyName.toStdString());
        proxyLight->setType(Ogre::Light::LT_POINT);
        proxyLight->setDiffuseColour(owner.light->getDiffuseColour());
        proxyLight->setSpecularColour(owner.light->getSpecularColour());
        proxyLight->setPowerScale(owner.light->getPowerScale() * weight);
        proxyLight->setAttenuation(owner.light->getAttenuationRange(),
                                   owner.light->getAttenuationConstant(),
                                   owner.light->getAttenuationLinear(),
                                   owner.light->getAttenuationQuadric());
        proxyLight->setVisible(snapshot.enabled);
        proxyLight->setCastShadows(false);
        proxyLight->getUserObjectBindings().setUserAny(kProxyLightTag, Ogre::Any(true));
        proxyLight->getUserObjectBindings().setUserAny(kProxyOwnerKey,
                                                          Ogre::Any(owner.name.toStdString()));
        node->attachObject(proxyLight);
        node->setPosition(sample.localPos);
    }
#endif
}

} // namespace AreaLight
