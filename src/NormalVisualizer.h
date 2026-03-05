#ifndef NORMALVISUALIZER_H
#define NORMALVISUALIZER_H

#include <Ogre.h>
#include <QObject>
#include <QMap>

class NormalVisualizer : public QObject
{
    Q_OBJECT
public:
    NormalVisualizer(Ogre::SceneManager* sceneMgr, QObject* parent = nullptr);
    ~NormalVisualizer() override;

    bool isVisible() const { return mVisible; }

public slots:
    void setVisible(bool visible);

private slots:
    void onEntityCreated(Ogre::Entity* const& entity);
    void onSceneNodeDestroyed(Ogre::SceneNode* const& node);

private:
    void buildOverlayForEntity(Ogre::Entity* entity);
    void destroyOverlayForEntity(Ogre::Entity* entity);
    void destroyAllOverlays();
    void createMaterial();

    Ogre::SceneManager* mSceneMgr;
    Ogre::MaterialPtr mMaterial;
    bool mVisible = false;
    struct OverlayData {
        Ogre::ManualObject* manualObject;
        Ogre::SceneNode* node;
    };
    QMap<Ogre::Entity*, OverlayData> mOverlays;
};

#endif // NORMALVISUALIZER_H
