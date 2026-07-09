#pragma once

#include <OgreLight.h>
#include <OgreQuaternion.h>
#include <OgreVector3.h>

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include "LightLinking.h"

namespace Ogre
{
class Light;
class SceneNode;
}

struct LightHandle
{
    QString name;
    Ogre::Light* light = nullptr;
    Ogre::SceneNode* sceneNode = nullptr;

    bool isValid() const { return light != nullptr && sceneNode != nullptr; }
};

struct LightSnapshot
{
    QString name;
    Ogre::Light::LightTypes type = Ogre::Light::LT_POINT;
    bool enabled = true;
    Ogre::ColourValue diffuse = Ogre::ColourValue::White;
    Ogre::ColourValue specular = Ogre::ColourValue(0.8f, 0.8f, 0.8f);
    float powerScale = 1.0f;
    float attenuationRange = 1000.0f;
    float attenuationConstant = 1.0f;
    float attenuationLinear = 0.0f;
    float attenuationQuadratic = 0.0f;
    float spotlightInnerAngleDeg = 30.0f;
    float spotlightOuterAngleDeg = 40.0f;
    float spotlightFalloff = 1.0f;
    Ogre::Vector3 position = Ogre::Vector3::ZERO;
    Ogre::Quaternion orientation = Ogre::Quaternion::IDENTITY;
    Ogre::Vector3 scale = Ogre::Vector3::UNIT_SCALE;
    bool usesDirection = false;
    Ogre::Vector3 direction = Ogre::Vector3::NEGATIVE_UNIT_Z;
    bool castShadows = false;
    float shadowDepthBias = 0.00005f;
    float shadowSlopeBias = 1.0f;

    /// Slice I (#491): entity include/exclude via Ogre light masks (bits 1..31).
    LightLinking::Mode linkMode = LightLinking::Mode::None;
    QStringList linkedEntityNames;
    uint32_t linkChannelBit = 0;

    static LightSnapshot fromHandle(const LightHandle& handle);
    bool operator==(const LightSnapshot& other) const;
};

Q_DECLARE_METATYPE(LightHandle)
Q_DECLARE_METATYPE(LightSnapshot)

/// Slice A (#483): user scene lights — owns Ogre lights under named scene nodes.
class LightManager : public QObject
{
    Q_OBJECT

public:
    static constexpr const char* kUserLightTag = "user_light";

    static LightManager* getSingleton();
    static LightManager* getSingletonPtr();
    static void kill();

    /// Remove every tracked user light from the scene.
    void clearAllLights();

    /// Default directional key light for empty scenes (visual parity with the
    /// legacy hard-coded light in Manager::CreateEmptyScene).
    LightHandle createDefaultKeyLight();

    LightHandle createLight(Ogre::Light::LightTypes type, const QString& name = QString());
    LightHandle createLightAt(Ogre::Light::LightTypes type,
                              const QString& baseName,
                              const Ogre::Vector3& position,
                              const Ogre::Vector3& direction,
                              bool setDirection);
    LightHandle duplicateLight(const QString& sourceName);
    LightHandle restoreSnapshot(const LightSnapshot& snapshot);
    LightHandle restoreSnapshotUnderParent(Ogre::SceneNode* parent, const LightSnapshot& snapshot);
    LightHandle createLightUnderParent(Ogre::SceneNode* parent,
                                       Ogre::Light::LightTypes type,
                                       const QString& baseName,
                                       const Ogre::Vector3& position,
                                       const Ogre::Vector3& direction,
                                       bool setDirection);
    Ogre::SceneNode* createRigGroupNode(const QString& baseName);
    QList<LightSnapshot> captureAllSnapshots() const;
    void deleteAllUserLights();
    bool deleteLight(const QString& name);
    /// Tear down a tracked user-light scene node (icons/gizmos first). Returns
    /// true when the node was handled and must not be destroyed again by Manager.
    bool deleteLightBySceneNode(Ogre::SceneNode* node);
    bool renameLight(const QString& oldName, const QString& newName);
    bool applyProperties(const QString& name, const LightSnapshot& snapshot);

    QList<LightHandle> lights() const;
    LightHandle* findLight(const QString& name);
    const LightHandle* findLight(const QString& name) const;
    LightHandle* findLightBySceneNode(Ogre::SceneNode* node);
    const LightHandle* findLightBySceneNode(Ogre::SceneNode* node) const;

    static bool isUserLight(const Ogre::Light* light);
    static bool sceneNodeIsUserLight(Ogre::SceneNode* node);
    static QString defaultBaseNameForType(Ogre::Light::LightTypes type);

    /// Connect to Manager::sceneNodeDestroyed when a Manager exists.
    void tryConnectToManager();

signals:
    void lightCreated(const LightHandle& handle);
    void lightChanged(const QString& name);
    void lightDeleted(const QString& name);

private:
    explicit LightManager(QObject* parent = nullptr);
    ~LightManager() override;

    void tagAsUserLight(Ogre::Light* light);
    void applySnapshotToHandle(const LightSnapshot& snapshot, LightHandle& handle) const;
    QString uniqueLightBaseName(const QString& base) const;
    LightHandle createLightInternal(Ogre::Light::LightTypes type, const QString& baseName);

    void onSceneNodeDestroyed(Ogre::SceneNode* node);

    static LightManager* s_singleton;

    QList<LightHandle> m_lights;
    int m_nextStackIndex = 0;
    bool m_connectedToManager = false;
    bool m_suppressSceneNodeDestroyed = false;
};
