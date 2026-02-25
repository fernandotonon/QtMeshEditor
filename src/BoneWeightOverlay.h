#ifndef BONEWEIGHTOVERLAY_H
#define BONEWEIGHTOVERLAY_H

#include <Ogre.h>
#include <OgreSubEntity.h>
#include <QObject>
#include <QTimer>
#include <vector>

class BoneWeightOverlay : public QObject
{
    Q_OBJECT
public:
    BoneWeightOverlay(Ogre::Entity* entity, Ogre::SceneManager* sceneMgr);
    ~BoneWeightOverlay() override;

    void setSelectedBone(unsigned short boneIndex);
    void setVisible(bool visible);
    bool isVisible() const { return mVisible; }

    static Ogre::ColourValue weightToColor(float weight);

private:
    void buildOverlay();
    void createMaterial();
    void destroyOverlay();
    void pollBoneSelection();
    void updateOverlayPositions();

    Ogre::Entity* mEntity;
    Ogre::SceneManager* mSceneMgr;
    Ogre::ManualObject* mOverlay = nullptr;
    Ogre::MaterialPtr mMaterial;
    unsigned short mBoneIndex = 0;
    bool mVisible = false;
    bool mSoftwareAnimRequested = false;
    // Per-section cached data for efficient frame updates
    std::vector<std::vector<Ogre::ColourValue>> mSectionColours;
    std::vector<std::vector<uint32_t>> mSectionIndices;
    QTimer mUpdateTimer;
};

#endif // BONEWEIGHTOVERLAY_H
