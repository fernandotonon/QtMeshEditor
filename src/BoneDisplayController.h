/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef BONEDISPLAYCONTROLLER_H
#define BONEDISPLAYCONTROLLER_H

#include "BoneDisplay.h"

#include <QHash>
#include <QObject>

class QJSEngine;
class QQmlEngine;
namespace Ogre { class Entity; class SceneNode; }

class BoneDisplayController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList shapeIds READ shapeIds CONSTANT)
    Q_PROPERTY(bool hasBone READ hasBone NOTIFY displayChanged)
    Q_PROPERTY(int shapeIndex READ shapeIndex NOTIFY displayChanged)
    Q_PROPERTY(QString customMesh READ customMesh NOTIFY displayChanged)
    Q_PROPERTY(QStringList proxyChoices READ proxyChoices NOTIFY displayChanged)
    Q_PROPERTY(int customMeshIndex READ customMeshIndex NOTIFY displayChanged)
    Q_PROPERTY(double scaleX READ scaleX NOTIFY displayChanged)
    Q_PROPERTY(double scaleY READ scaleY NOTIFY displayChanged)
    Q_PROPERTY(double scaleZ READ scaleZ NOTIFY displayChanged)
    Q_PROPERTY(double offsetX READ offsetX NOTIFY displayChanged)
    Q_PROPERTY(double offsetY READ offsetY NOTIFY displayChanged)
    Q_PROPERTY(double offsetZ READ offsetZ NOTIFY displayChanged)
    Q_PROPERTY(double rotationX READ rotationX NOTIFY displayChanged)
    Q_PROPERTY(double rotationY READ rotationY NOTIFY displayChanged)
    Q_PROPERTY(double rotationZ READ rotationZ NOTIFY displayChanged)
    Q_PROPERTY(bool boneVisible READ boneVisible NOTIFY displayChanged)
    Q_PROPERTY(bool selectionLocked READ selectionLocked NOTIFY displayChanged)
    Q_PROPERTY(bool lockTranslation READ lockTranslation NOTIFY displayChanged)
    Q_PROPERTY(bool lockRotation READ lockRotation NOTIFY displayChanged)
    Q_PROPERTY(bool lockScale READ lockScale NOTIFY displayChanged)
    Q_PROPERTY(bool useGroupColor READ useGroupColor NOTIFY displayChanged)
    Q_PROPERTY(QString colorHex READ colorHex NOTIFY displayChanged)
    Q_PROPERTY(QStringList groupNames READ groupNames NOTIFY displayChanged)
    Q_PROPERTY(QStringList groupColors READ groupColors NOTIFY displayChanged)
    Q_PROPERTY(QStringList boneGroups READ boneGroups NOTIFY displayChanged)
    Q_PROPERTY(QString isolatedGroup READ isolatedGroup NOTIFY displayChanged)
    Q_PROPERTY(QString boneLayerBits READ boneLayerBits NOTIFY displayChanged)
    Q_PROPERTY(QString visibleLayerBits READ visibleLayerBits NOTIFY displayChanged)
    Q_PROPERTY(QString groupError READ groupError NOTIFY displayChanged)

public:
    static BoneDisplayController* instance();
    static BoneDisplayController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);

    BoneDisplay::Document documentFor(Ogre::Entity* entity) const;
    bool isBoneShown(Ogre::Entity* entity, const QString& bone) const;
    bool canSelectBone(Ogre::Entity* entity, const QString& bone) const;
    bool isChannelLocked(Ogre::Entity* entity, const QString& bone, BoneDisplay::Channel channel) const;

    bool saveEntity(Ogre::Entity* entity, const QString& meshPath);
    bool loadEntity(Ogre::Entity* entity, const QString& meshPath);
    bool saveScene(const QHash<QString, Ogre::Entity*>& nodesToEntities, const QString& scenePath);
    bool loadScene(const QHash<QString, Ogre::Entity*>& nodesToEntities, const QString& scenePath);
    void rekeyBone(Ogre::Entity* entity, const QString& from, const QString& to);

    QStringList shapeIds() const { return BoneDisplay::shapeIds(); }
    bool hasBone() const;
    int shapeIndex() const;
    QString customMesh() const;
    QStringList proxyChoices() const;
    int customMeshIndex() const;
    double scaleX() const;
    double scaleY() const;
    double scaleZ() const;
    double offsetX() const;
    double offsetY() const;
    double offsetZ() const;
    double rotationX() const;
    double rotationY() const;
    double rotationZ() const;
    bool boneVisible() const;
    bool selectionLocked() const;
    bool lockTranslation() const;
    bool lockRotation() const;
    bool lockScale() const;
    bool useGroupColor() const;
    QString colorHex() const;
    QStringList groupNames() const;
    QStringList groupColors() const;
    QStringList boneGroups() const;
    QString isolatedGroup() const;
    QString boneLayerBits() const;
    QString visibleLayerBits() const;
    QString groupError() const { return m_groupError; }

    Q_INVOKABLE void setShapeIndex(int index);
    Q_INVOKABLE void setCustomMeshIndex(int index);
    Q_INVOKABLE void setScaleAxis(int axis, double value);
    Q_INVOKABLE void setOffsetAxis(int axis, double value);
    Q_INVOKABLE void setRotationAxis(int axis, double value);
    Q_INVOKABLE void setColorHex(const QString& hex);
    Q_INVOKABLE void setUseGroupColor(bool on);
    Q_INVOKABLE void setBoneVisible(bool visible);
    Q_INVOKABLE void setSelectionLocked(bool locked);
    Q_INVOKABLE void setLockTranslation(bool locked);
    Q_INVOKABLE void setLockRotation(bool locked);
    Q_INVOKABLE void setLockScale(bool locked);
    Q_INVOKABLE void setBoneOnLayer(int index, bool on);
    Q_INVOKABLE void setLayerVisible(int index, bool on);
    Q_INVOKABLE QString addGroup(const QString& name);
    Q_INVOKABLE void removeGroup(const QString& name);
    Q_INVOKABLE void setGroupMember(const QString& name, bool member);
    Q_INVOKABLE void setGroupColorHex(const QString& name, const QString& hex);
    Q_INVOKABLE void isolateGroup(const QString& name);

signals:
    void displayChanged();

private:
    explicit BoneDisplayController(QObject* parent = nullptr);
    void bindSignals();
    void forgetNode(Ogre::SceneNode* node);
    Ogre::Entity* selectedEntity() const;
    QString selectedBoneName() const;
    BoneDisplay::Bone selectedBone() const;
    BoneDisplay::Document* mutableSelected(bool* haveBone);
    void commitSelected(bool rebuildShapes);
    void note(const QString& category, const QString& message) const;
    static QString bits(std::uint32_t mask);

    QHash<Ogre::Entity*, BoneDisplay::Document> m_docs;
    QString m_groupError;
    bool m_mgrBound = false;
    bool m_animBound = false;
};

#endif
