#pragma once

#include "LightManager.h"

#include <OgreBillboardSet.h>
#include <OgreManualObject.h>

#include <QMap>
#include <QObject>
#include <QString>

class LightVisualizer : public QObject
{
    Q_OBJECT

public:
    explicit LightVisualizer(Ogre::SceneManager* sceneMgr, QObject* parent = nullptr);
    ~LightVisualizer() override;

    bool iconsVisible() const { return mIconsVisible; }
    bool selectedGizmosOnly() const { return mSelectedGizmosOnly; }

    static QString lightNameForMovable(const Ogre::MovableObject* obj);

public slots:
    void setIconsVisible(bool visible);
    void setSelectedGizmosOnly(bool selectedOnly);

private slots:
    void onLightCreated(const LightHandle& handle);
    void onLightChanged(const QString& name);
    void onLightDeleted(const QString& name);
    void onSelectionChanged();

private:
    struct OverlayData
    {
        Ogre::BillboardSet* icon = nullptr;
        Ogre::ManualObject* gizmo = nullptr;
        Ogre::SceneNode* overlayNode = nullptr;
        QString lightName;
    };

    void ensureResources();
    void rebuildAll();
    void buildOverlay(const LightHandle& handle);
    void destroyOverlay(const QString& name);
    void refreshOverlay(const QString& name);
    void updateOverlayVisibility(const QString& name);
    void rebuildGizmoGeometry(OverlayData& data, const LightHandle& handle, bool selected);
    bool isLightSelected(const QString& name) const;
    static void tagLightMovable(Ogre::MovableObject* obj, const QString& lightName);

    Ogre::SceneManager* mSceneMgr = nullptr;
    QMap<QString, OverlayData> mOverlays;
    bool mIconsVisible = true;
    bool mSelectedGizmosOnly = false;
    bool mResourcesReady = false;
    Ogre::MaterialPtr mGizmoMaterial;
    Ogre::TexturePtr mIconDirectionalTex;
    Ogre::TexturePtr mIconPointTex;
    Ogre::TexturePtr mIconSpotTex;
};
