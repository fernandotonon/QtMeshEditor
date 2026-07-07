#pragma once

#include <OgreLight.h>

#include <QObject>
#include <QQmlEngine>
#include <QString>

class LightsController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasLightSelection READ hasLightSelection NOTIFY selectionChanged)

public:
    static LightsController* instance();
    static LightsController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool hasLightSelection() const;

    Q_INVOKABLE void addDirectionalLight();
    Q_INVOKABLE void addPointLight();
    Q_INVOKABLE void addSpotLight();
    Q_INVOKABLE void addDirectionalLightAtViewport();
    Q_INVOKABLE void addPointLightAtViewport();
    Q_INVOKABLE void addSpotLightAtViewport();
    Q_INVOKABLE void duplicateSelectedLights();
    Q_INVOKABLE void deleteSelectedLights();
    Q_INVOKABLE void deleteLightByName(const QString& name);
    Q_INVOKABLE void duplicateLightByName(const QString& name);
    Q_INVOKABLE void renameLight(const QString& oldName, const QString& newName);
    Q_INVOKABLE bool isLightNode(const QString& nodeName) const;
    Q_INVOKABLE bool isLightNodeName(const QString& nodeName) const { return isLightNode(nodeName); }

    void addLight(Ogre::Light::LightTypes type, bool atViewport);

signals:
    void selectionChanged();

private:
    explicit LightsController(QObject* parent = nullptr);

    bool computeViewportPlacement(Ogre::Vector3& position, Ogre::Vector3& direction) const;
    void selectLightHandle(const QString& name);

    static LightsController* s_singleton;
};
