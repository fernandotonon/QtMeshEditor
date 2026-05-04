#ifndef SKELETONDEBUG_H_INCLUDED
#define SKELETONDEBUG_H_INCLUDED

#include <Ogre.h>
#include <OgreTagPoint.h>
#include <vector>
#include <map>
#include <string>
#include <QObject>
#include <QTimer>

class SkeletonDebug: public QObject
{
    Q_OBJECT
public:
    SkeletonDebug(Ogre::Entity *entity, Ogre::SceneManager *man, float boneSize = 0.1f, float scaleAxes = 0.1f);
    ~SkeletonDebug() override;

    void setAxesScale(Ogre::Real scale){mScaleAxes = scale;}
    Ogre::Real getAxesScale() const {return mScaleAxes;}

    void showAxes(bool show);
    void showNames(bool show);
    void showBones(bool show);
    bool axesShown() const {return mShowAxes;}
    bool namesShown() const {return mShowNames;}
    bool bonesShown() const {return mShowBones;}

    void update() const;
    short selectedBoneIndex() const { return mLastSelectedBone; }

    // Bone-name tag attached to every visual entity (bone meshes + axes
    // meshes) so a ray-pick in the viewport can map back to the bone.
    // Returns empty string when the movable isn't a SkeletonDebug visual.
    static Ogre::String boneNameForMovable(const Ogre::MovableObject* obj);

signals:
    void boneSelected(unsigned short boneIndex);

private:
    std::vector<Ogre::Entity*> mAxisEntities;
    std::vector<Ogre::Entity*> mBoneEntities;

    float mBoneSize;

    Ogre::Entity *mEntity;
    Ogre::MaterialPtr mAxisMatPtr;
    Ogre::MaterialPtr mBoneMatPtr;
    Ogre::MaterialPtr mBoneMatSelectedPtr;
    Ogre::MaterialPtr mBoneMatRootPtr;
    Ogre::MeshPtr mBoneMeshPtr;
    Ogre::MeshPtr mAxesMeshPtr;
    Ogre::SceneManager *mSceneMan;

    Ogre::Real mScaleAxes;

    bool mShowAxes = true;
    bool mShowBones = true;
    bool mShowNames = true;

    void createAxesMaterial();
    void createBoneMaterial();
    void createAxesMesh();
    void createBoneMesh();
    std::map<std::string, Ogre::Entity*, std::less<>> createBoneVisuals();
    void createChildBoneRepresentations(const Ogre::Bone* pBone, Ogre::Entity*& lastEnt);

    QTimer mTimer;
    short mLastSelectedBone = -1;
};

#endif // SKELETONDEBUG_H_INCLUDED
