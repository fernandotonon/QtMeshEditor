/*/////////////////////////////////////////////////////////////////////////////////
/// A QtMeshEditor file
///
/// Copyright (c) HogPog Team (www.hogpog.com.br)
///
/// The MIT License
///
/// Permission is hereby granted, free of charge, to any person obtaining a copy
/// of this software and associated documentation files (the "Software"), to deal
/// in the Software without restriction, including without limitation the rights
/// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
/// copies of the Software, and to permit persons to whom the Software is
/// furnished to do so, subject to the following conditions:
///
/// The above copyright notice and this permission notice shall be included in
/// all copies or substantial portions of the Software.
///
/// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
/// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
/// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
/// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
/// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
/// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
/// THE SOFTWARE.
////////////////////////////////////////////////////////////////////////////////*/

#ifndef PRIMITIVES_OBJECT_H
#define PRIMITIVES_OBJECT_H

#include <OgrePrerequisites.h>

class QString;

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
        AP_ROUNDEDBOX
    };

    PrimitiveObject(const QString& name);
    PrimitiveObject(const QString& name, PrimitiveType type);
    ~PrimitiveObject();

    Ogre::SceneNode* createPrimitive();
    void clone(const PrimitiveObject* primitiveToClone);

    static bool isPrimitive(const Ogre::SceneNode* node);
    static bool isPrimitive(const Ogre::Entity* entity);
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

    int                 getNumSegRing()     const;
    int                 getNumSegBase()     const;
    int                 getNumSegCircle()   const;
    int                 getNumIterations()  const;

    int                 getNumSegLoop()     const;
    int                 getNumSegSection()  const;

    int                 getNumSegHeight()   const;

    const Ogre::Real&   getUTile()          const;
    const Ogre::Real&   getVTile()          const;
    bool                hasUVSwitched()     const;

    void setSizeX           (const Ogre::Real& sizeX);
    void setSizeY           (const Ogre::Real& sizeY);
    void setSizeZ           (const Ogre::Real& sizeZ);
    void setRadius          (const Ogre::Real& radius);
    void setOuterRadius     (const Ogre::Real& radius);
    void setChamferRadius   (const Ogre::Real& radius);
    void setInnerRadius     (const Ogre::Real& radius);
    void setSectionRadius   (const Ogre::Real& radius);
    void setHeight          (const Ogre::Real& height);

    void setNumSegX         (int numSegX);
    void setNumSegY         (int numSegY);
    void setNumSegZ         (int numSegZ);

    void setNumSegRing      (int numSegRing);
    void setNumSegBase      (int numSegBase);
    void setNumSegCircle    (int numSegCircle);
    void setNumIterations   (int numIterations);

    void setNumSegLoop      (int numSegLoop);
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
    Ogre::SceneNode* mSceneNode;

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
