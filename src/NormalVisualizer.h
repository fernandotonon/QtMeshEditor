#ifndef NORMALVISUALIZER_H
#define NORMALVISUALIZER_H

#include <Ogre.h>
#include <OgreSubEntity.h>
#include <QObject>
#include <QMap>
#include <QTimer>
#include <QSet>

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
    void updateAnimatedOverlays();

    Ogre::SceneManager* mSceneMgr;
    Ogre::MaterialPtr mMaterial;
    bool mVisible = false;
    QTimer mUpdateTimer;

    static constexpr float NORMAL_LENGTH = 0.1f;

    struct OverlayData {
        Ogre::ManualObject* manualObject;
        Ogre::SceneNode* node;
        bool hasSkeleton;
    };
    QMap<Ogre::Entity*, OverlayData> mOverlays;
    QSet<Ogre::Entity*> mSoftwareAnimRequested;
};

#endif // NORMALVISUALIZER_H
