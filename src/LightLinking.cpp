#include "LightLinking.h"
#include "LightManager.h"
#include "Manager.h"

#include <OgreEntity.h>
#include <OgreLight.h>
#include <OgreSceneNode.h>

#include <QHash>
#include <QSet>

namespace
{

struct ActiveRule
{
    LightLinking::Mode mode = LightLinking::Mode::None;
    QStringList entityNames;
    uint32_t channelBit = 0;
};

QHash<QString, ActiveRule> g_rulesByLight;

uint32_t channelBitFromIndex(int index)
{
    if (index < 1 || index > LightLinking::kMaxLinkChannels)
        return 0;
    return 1u << static_cast<uint32_t>(index);
}

int indexFromChannelBit(uint32_t bit)
{
    if (bit == 0)
        return 0;
    for (int i = 1; i <= LightLinking::kMaxLinkChannels; ++i)
    {
        if (bit == channelBitFromIndex(i))
            return i;
    }
    return 0;
}

uint32_t allocateChannelBit(const QString& lightName)
{
    QSet<int> used;
    for (auto it = g_rulesByLight.constBegin(); it != g_rulesByLight.constEnd(); ++it)
    {
        if (it.key() == lightName)
            continue;
        const int idx = indexFromChannelBit(it->channelBit);
        if (idx > 0)
            used.insert(idx);
    }

    for (int i = 1; i <= LightLinking::kMaxLinkChannels; ++i)
    {
        if (!used.contains(i))
            return channelBitFromIndex(i);
    }
    return 0;
}

QList<Ogre::Entity*> allEntities()
{
    QList<Ogre::Entity*> out;
    if (!Manager::getSingletonPtr())
        return out;

    for (Ogre::MovableObject* obj : Manager::getSingleton()->getEntities())
    {
        if (obj && obj->getMovableType() == QStringLiteral("Entity"))
            out.append(static_cast<Ogre::Entity*>(obj));
    }
    return out;
}

Ogre::Entity* entityByNodeName(const QString& nodeName)
{
    if (!Manager::getSingletonPtr() || nodeName.isEmpty())
        return nullptr;

    Ogre::SceneNode* node = Manager::getSingleton()->getSceneNode(nodeName);
    if (!node)
        return nullptr;

    for (unsigned short i = 0; i < node->numAttachedObjects(); ++i)
    {
        Ogre::MovableObject* obj = node->getAttachedObject(i);
        if (obj && obj->getMovableType() == QStringLiteral("Entity"))
            return static_cast<Ogre::Entity*>(obj);
    }
    return nullptr;
}

void clearChannelFromAllEntities(uint32_t channelBit)
{
    if (channelBit == 0)
        return;
    for (Ogre::Entity* entity : allEntities())
        entity->setLightMask(entity->getLightMask() & ~channelBit);
}

void applyIncludeRule(uint32_t channelBit, const QStringList& includedNames)
{
    clearChannelFromAllEntities(channelBit);

    QSet<QString> included;
    for (const QString& name : includedNames)
        included.insert(name);

    for (Ogre::Entity* entity : allEntities())
    {
        const QString nodeName =
            QString::fromStdString(entity->getParentSceneNode()->getName());
        if (included.contains(nodeName))
            entity->setLightMask(entity->getLightMask() | channelBit);
    }
}

void applyExcludeRule(uint32_t channelBit, const QStringList& excludedNames)
{
    QSet<QString> excluded;
    for (const QString& name : excludedNames)
        excluded.insert(name);

    for (Ogre::Entity* entity : allEntities())
    {
        const QString nodeName =
            QString::fromStdString(entity->getParentSceneNode()->getName());
        if (excluded.contains(nodeName))
        {
            // Mask must not overlap the light's (~channelBit) mask — only the bit itself.
            entity->setLightMask(channelBit);
        }
        else if (entity->getLightMask() == channelBit)
        {
            entity->setLightMask(LightLinking::kDefaultMask);
        }
    }
}

bool channelBitInUseByOther(const QString& lightName, uint32_t bit)
{
    if (bit == 0)
        return false;
    for (auto it = g_rulesByLight.constBegin(); it != g_rulesByLight.constEnd(); ++it)
    {
        if (it.key() == lightName)
            continue;
        if (it->channelBit == bit)
            return true;
    }
    return false;
}

void applyRule(const QString& lightName, const ActiveRule& rule, Ogre::Light* light = nullptr)
{
    if (!light)
    {
        if (auto* lights = LightManager::getSingletonPtr())
            if (const LightHandle* handle = lights->findLight(lightName))
                light = handle->light;
    }
    if (!light)
        return;

    if (rule.mode == LightLinking::Mode::None || rule.channelBit == 0)
    {
        light->setLightMask(LightLinking::kDefaultMask);
        return;
    }

    if (rule.mode == LightLinking::Mode::Include)
    {
        light->setLightMask(rule.channelBit);
        applyIncludeRule(rule.channelBit, rule.entityNames);
    }
    else if (rule.mode == LightLinking::Mode::Exclude)
    {
        light->setLightMask(LightLinking::kDefaultMask & ~rule.channelBit);
        applyExcludeRule(rule.channelBit, rule.entityNames);
    }
}

void rebuildAllRules()
{
    const QStringList lightNames = g_rulesByLight.keys();
    for (const QString& name : lightNames)
        applyRule(name, g_rulesByLight.value(name));
}

} // namespace

namespace LightLinking
{

QString modeToString(Mode mode)
{
    switch (mode)
    {
    case Mode::Include:
        return QStringLiteral("include");
    case Mode::Exclude:
        return QStringLiteral("exclude");
    case Mode::None:
    default:
        return QStringLiteral("none");
    }
}

Mode modeFromString(const QString& text)
{
    const QString lower = text.trimmed().toLower();
    if (lower == QStringLiteral("include"))
        return Mode::Include;
    if (lower == QStringLiteral("exclude"))
        return Mode::Exclude;
    return Mode::None;
}

void applyFromSnapshot(const LightSnapshot& snapshot, Ogre::Light* light)
{
    if (snapshot.name.isEmpty())
        return;

    ActiveRule rule;
    rule.mode = snapshot.linkMode;
    rule.entityNames = snapshot.linkedEntityNames;
    rule.channelBit = snapshot.linkChannelBit;

    if (rule.mode == Mode::None)
    {
        if (rule.channelBit != 0)
            clearChannelFromAllEntities(rule.channelBit);
        g_rulesByLight.remove(snapshot.name);
        applyRule(snapshot.name, {}, light);
        return;
    }

    if (rule.channelBit == 0 || channelBitInUseByOther(snapshot.name, rule.channelBit))
        rule.channelBit = allocateChannelBit(snapshot.name);
    if (rule.channelBit == 0)
        return; // all 31 channels in use

    g_rulesByLight.insert(snapshot.name, rule);

    applyRule(snapshot.name, rule, light);

    if (!light)
    {
        if (LightHandle* handle = LightManager::getSingleton()->findLight(snapshot.name))
            light = handle->light;
    }
    if (light)
    {
        auto& bindings = light->getUserObjectBindings();
        bindings.setUserAny(QStringLiteral("light_link_mode").toStdString(),
                            Ogre::Any(modeToString(rule.mode).toStdString()));
        bindings.setUserAny(QStringLiteral("light_link_entities").toStdString(),
                            Ogre::Any(rule.entityNames.join(QLatin1Char('\n')).toStdString()));
        bindings.setUserAny(QStringLiteral("light_link_channel").toStdString(),
                            Ogre::Any(rule.channelBit));
    }
}

void onLightDeleted(const LightSnapshot& snapshot)
{
    uint32_t bit = g_rulesByLight.value(snapshot.name).channelBit;
    if (bit == 0)
        bit = snapshot.linkChannelBit;
    g_rulesByLight.remove(snapshot.name);
    if (bit != 0)
        clearChannelFromAllEntities(bit);
    rebuildAllRules();
}

void onEntityCreated(Ogre::Entity* entity)
{
    if (!entity)
        return;

    const QString nodeName =
        QString::fromStdString(entity->getParentSceneNode()->getName());

    for (auto it = g_rulesByLight.constBegin(); it != g_rulesByLight.constEnd(); ++it)
    {
        const ActiveRule& rule = it.value();
        if (rule.channelBit == 0)
            continue;

        if (rule.mode == Mode::Include)
        {
            if (rule.entityNames.contains(nodeName))
                entity->setLightMask(entity->getLightMask() | rule.channelBit);
            else
                entity->setLightMask(entity->getLightMask() & ~rule.channelBit);
        }
        else if (rule.mode == Mode::Exclude)
        {
            if (rule.entityNames.contains(nodeName))
                entity->setLightMask(rule.channelBit);
        }
    }
}

QStringList allEntityNodeNames()
{
    QStringList names;
    for (Ogre::Entity* entity : allEntities())
    {
        if (!entity->getParentSceneNode())
            continue;
        names.append(QString::fromStdString(entity->getParentSceneNode()->getName()));
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

void clearAllRules()
{
    g_rulesByLight.clear();
}

} // namespace LightLinking
