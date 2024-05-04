#ifndef PRIMITIVES_OBJECT_H
#define PRIMITIVES_OBJECT_H

#include <OgrePrerequisites.h>
#include <QString>

namespace Procedural{
class BoxGenerator;
class SphereGenerator;
class PlaneGenerator;
class CylinderGenerator;
class ConeGenerator;
class TorusGenerator;
class TubeGenerator;
class CapsuleGenerator;
class IcoSphereGenerator;
class RoundedBoxGenerator;
}

class PrimitiveObject
{
public:
    enum PrimitiveType
    {
        AP_NONE,
        AP_CUBE,
        AP_SPHERE,
        AP_PLANE,
        AP_CYLINDER,
        AP_CONE,
        AP_TORUS,
        AP_TUBE,
        AP_CAPSULE,
        AP_ICOSPHERE,
        AP_ROUNDEDBOX,
        AP_SPRING
    };

    explicit PrimitiveObject(const QString& name);
    PrimitiveObject(const QString& name, PrimitiveType type);
    ~PrimitiveObject();

    Ogre::SceneNode* createPrimitive();

    static bool isPrimitive(const Ogre::SceneNode* node);
    static PrimitiveObject* getPrimitiveFromSceneNode(const Ogre::SceneNode* node);

    static Ogre::SceneNode* createCube(const QString& name);
    static Ogre::SceneNode* createSphere(const QString& name);
    static Ogre::SceneNode* createPlane(const QString& name);
    static Ogre::SceneNode* createCylinder(const QString& name);
    static Ogre::SceneNode* createCone(const QString& name);
    static Ogre::SceneNode* createTorus(const QString& name);
    static Ogre::SceneNode* createTube(const QString& name);
    static Ogre::SceneNode* createCapsule(const QString& name);
    static Ogre::SceneNode* createIcoSphere(const QString& name);
    static Ogre::SceneNode* createRoundedBox(const QString& name);
    static Ogre::SceneNode* createSpring(const QString& name);

    const QString&      getName()           const;
    Ogre::SceneNode*    getSceneNode()      const;
    PrimitiveType       getType()           const;

    const Ogre::Real&   getSizeX()          const;
    const Ogre::Real&   getSizeY()          const;
    const Ogre::Real&   getSizeZ()          const;

    const Ogre::Real&   getRadius()         const;
    const Ogre::Real&   getOuterRadius()    const;
    const Ogre::Real&   getChamferRadius()  const;
    const Ogre::Real&   getSectionRadius()  const;
    const Ogre::Real&   getInnerRadius()    const;
    const Ogre::Real&   getHeight()         const;

    int                 getNumSegX()        const;
    int                 getNumSegY()        const;
    int                 getNumSegZ()        const;

    int                 getNumSegBase()     const;
    int                 getNumSegCircle()   const;
    int                 getNumIterations()  const;

    const Ogre::Real&   getUTile()          const;
    const Ogre::Real&   getVTile()          const;
    bool                hasUVSwitched()     const;

    void setSizeX           (const Ogre::Real& sizeX);
    void setSizeY           (const Ogre::Real& sizeY);
    void setSizeZ           (const Ogre::Real& sizeZ);
    void setRadius          (const Ogre::Real& radius);
    void setOuterRadius     (const Ogre::Real& radius);
    void setInnerRadius     (const Ogre::Real& radius);
    void setSectionRadius   (const Ogre::Real& radius);
    void setHeight          (const Ogre::Real& height);

    void setNumSegX         (int numSegX);
    void setNumSegY         (int numSegY);
    void setNumSegZ         (int numSegZ);

    void setNumSegBase      (int numSegBase);
    void setNumSegCircle    (int numSegCircle);
    void setNumIterations   (int numIterations);

    void setNumSegSection   (int numSegSection);

    void setNumSegHeight    (int numSegHeight);

    void setUTile(const Ogre::Real& uTile);
    void setVTile(const Ogre::Real& vTile);
    void setUVSwitch(bool switched);


private:
    void setDefaultParams();
    Ogre::MeshPtr createMesh();

    void updatePrimitive();

private:
    PrimitiveType   mType;
    QString         mName;
    Ogre::SceneNode* mSceneNode = nullptr;

    Ogre::Real      mSizeX;
    Ogre::Real      mSizeY;
    Ogre::Real      mSizeZ;
    Ogre::Real      mRadius;
    Ogre::Real      mRadius2;
    Ogre::Real      mHeight;
    int             mNumSegX;
    int             mNumSegY;
    int             mNumSegZ;
    Ogre::Real      mUTile;
    Ogre::Real      mVTile;
    bool            mSwitchUV;

};

#endif // PRIMITIVES_OBJECT_H
