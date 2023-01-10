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


#include <QString>

#include <OgreAny.h>
#include <OgreUserObjectBindings.h>

#include "Manager.h"
#include "PrimitiveObject.h"

#include "ProceduralBoxGenerator.h"
#include "ProceduralSphereGenerator.h"
#include "ProceduralPlaneGenerator.h"
#include "ProceduralCylinderGenerator.h"
#include "ProceduralConeGenerator.h"
#include "ProceduralTorusGenerator.h"
#include "ProceduralTubeGenerator.h"
#include "ProceduralIcoSphereGenerator.h"
#include "ProceduralCapsuleGenerator.h"
#include "ProceduralRoundedBoxGenerator.h"


// TODO add spring primitive

// TODO play with numTextureCoord (require to change the UI)

// TODO fix the bug in the num of segment for rounded box

// TODO switchUV doesn't seems to work

////////////////////////////////////////
// Constructor & Destructor

PrimitiveObject::PrimitiveObject(const QString& name ) :
    mType(AP_NONE), mName(name), mSceneNode(0),
    mSizeX(1.0f),mSizeY(1.0f),mSizeZ(1.0f),
    mRadius(1.0f),mRadius2(0.5f),mHeight(1.0f),
    mNumSegX(1),mNumSegY(1),mNumSegZ(1),
    mUTile(1.0f),mVTile(1.0f),mSwitchUV(false)

{
}

PrimitiveObject::PrimitiveObject(const QString& name, PrimitiveType type) :
    mType(type), mName(name), mSceneNode(0)
{
    setDefaultParams();
}

PrimitiveObject::~PrimitiveObject()
{
   //We don't destroy this scene node as it will be destroy by the Manager
}

void PrimitiveObject::setDefaultParams()
{
    switch(mType)
    {
        case AP_CUBE:
            mSizeX = 2.0f;  mSizeY = 2.0f;      mSizeZ = 2.0f;
            mRadius = 0.0f; mRadius2 = 0.0f;    mHeight = 0.0f;
            mNumSegX = 1;   mNumSegY = 1;       mNumSegZ = 1;
            mUTile = 1.0f;  mVTile = 1.0f;      mSwitchUV = false;
            break;
        case AP_SPHERE:
            mSizeX = 0.0f;  mSizeY = 0.0f;      mSizeZ = 0.0f;
            mRadius = 1.0f; mRadius2 = 0.0f;    mHeight = 0.0f;
            mNumSegX = 16;  mNumSegY = 16;      mNumSegZ = 0;
            mUTile = 1.0f;  mVTile = 1.0f;      mSwitchUV = false;
            break;
        case AP_PLANE:
            mSizeX = 2.0f;  mSizeY = 2.0f;      mSizeZ = 0.0f;
            mRadius = 0.0f; mRadius2 = 0.0f;    mHeight = 0.0f;
            mNumSegX = 3;   mNumSegY = 3;       mNumSegZ = 0;
            mUTile = 1.0f;  mVTile = 1.0f;      mSwitchUV = false;
            break;
        case AP_CYLINDER:
            mSizeX = 0.0f;  mSizeY = 0.0f;      mSizeZ = 0.0f;
            mRadius = 1.0f; mRadius2 = 0.0f;    mHeight = 3.0f;
            mNumSegX = 16;  mNumSegY = 0;       mNumSegZ = 1;
            mUTile = 1.0f;  mVTile = 1.0f;      mSwitchUV = false;
            break;
        case AP_CONE:
            mSizeX = 0.0f;  mSizeY = 0.0f;      mSizeZ = 0.0f;
            mRadius = 2.0f; mRadius2 = 0.0f;    mHeight = 3.0f;
            mNumSegX = 16;  mNumSegY = 0;       mNumSegZ = 1;
            mUTile = 1.0f;  mVTile = 1.0f;      mSwitchUV = false;
            break;
        case AP_TORUS:
            mSizeX = 0.0f;  mSizeY = 0.0f;      mSizeZ = 0.0f;
            mRadius = 3.0f; mRadius2 = 1.0f;    mHeight = 0.0f;
            mNumSegX = 16;  mNumSegY = 16;      mNumSegZ = 0;
            mUTile = 1.0f;  mVTile = 1.0f;      mSwitchUV = false;
            break;
        case AP_TUBE:
            mSizeX = 0.0f;  mSizeY = 0.0f;      mSizeZ = 0.0f;
            mRadius = 3.0f; mRadius2 = 2.0f;    mHeight = 3.0f;
            mNumSegX = 16;  mNumSegY = 0;       mNumSegZ = 1;
            mUTile = 1.0f;  mVTile = 1.0f;      mSwitchUV = false;
            break;
        case AP_CAPSULE:
            mSizeX = 0.0f;  mSizeY = 0.0f;      mSizeZ = 0.0f;
            mRadius = 1.0f; mRadius2 = 0.0f;    mHeight = 2.0f;
            mNumSegX = 8;   mNumSegY = 16;      mNumSegZ = 1;
            mUTile = 1.0f;  mVTile = 1.0f;      mSwitchUV = false;
            break;
        case AP_ICOSPHERE:
            mSizeX = 0.0f;  mSizeY = 0.0f;      mSizeZ = 0.0f;
            mRadius = 2.0f; mRadius2 = 0.0f;    mHeight = 0.0f;
            mNumSegX = 2;   mNumSegY = 0;       mNumSegZ = 0;
            mUTile = 1.0f;  mVTile = 1.0f;      mSwitchUV = false;
            break;
        case AP_ROUNDEDBOX:
            mSizeX = 2.0f;  mSizeY = 2.0f;      mSizeZ = 2.0f;
            mRadius = 1.0f; mRadius2 = 0.0f;    mHeight = 0.0f;
            mNumSegX = 1;   mNumSegY = 1;       mNumSegZ = 1;
            mUTile = 1.0f;  mVTile = 1.0f;      mSwitchUV = false;
            break;
        case AP_NONE:
        break;
    }
}


////////////////////////////////////////
/// Static Member
///

bool PrimitiveObject::isPrimitive(const Ogre::SceneNode* node)
{
  try
    {
        PrimitiveObject* prim =0;
        prim = Ogre::any_cast<PrimitiveObject*>(node->getUserObjectBindings().getUserAny());
        if (prim)
            return true;  //TODO check this &node
        else
            return false;
    }
    catch(...)
    {
       //We've got something else in userAny
        return false;
    }
}

bool PrimitiveObject::isPrimitive(const Ogre::Entity* entity)
{
   return isPrimitive(entity->getParentSceneNode());
}

PrimitiveObject* PrimitiveObject::getPrimitiveFromSceneNode(const Ogre::SceneNode* node)
{
    return Ogre::any_cast<PrimitiveObject*>(node->getUserObjectBindings().getUserAny());
}

Ogre::SceneNode*  PrimitiveObject::createCube(const QString& name)
{
    PrimitiveObject* newPrimitive = new PrimitiveObject(name, AP_CUBE);
    return newPrimitive->createPrimitive();
}

Ogre::SceneNode* PrimitiveObject::createSphere(const QString& name)
{
    PrimitiveObject* newPrimitive = new PrimitiveObject(name, AP_SPHERE);
    return newPrimitive->createPrimitive();
}

Ogre::SceneNode* PrimitiveObject::createPlane(const QString& name)
{
    PrimitiveObject* newPrimitive = new PrimitiveObject(name, AP_PLANE);
    return newPrimitive->createPrimitive();
}

Ogre::SceneNode* PrimitiveObject::createCylinder(const QString& name)
{
    PrimitiveObject* newPrimitive = new PrimitiveObject(name, AP_CYLINDER);
    return newPrimitive->createPrimitive();
}
Ogre::SceneNode* PrimitiveObject::createCone(const QString& name)
{
    PrimitiveObject* newPrimitive = new PrimitiveObject(name, AP_CONE);
    return newPrimitive->createPrimitive();
}
Ogre::SceneNode* PrimitiveObject::createTorus(const QString& name)
{
    PrimitiveObject* newPrimitive = new PrimitiveObject(name, AP_TORUS);
    return newPrimitive->createPrimitive();
}
Ogre::SceneNode* PrimitiveObject::createTube(const QString& name)
{
    PrimitiveObject* newPrimitive = new PrimitiveObject(name, AP_TUBE);
    return newPrimitive->createPrimitive();
}
Ogre::SceneNode* PrimitiveObject::createCapsule(const QString& name)
{
    PrimitiveObject* newPrimitive = new PrimitiveObject(name, AP_CAPSULE);
    return newPrimitive->createPrimitive();
}
Ogre::SceneNode* PrimitiveObject::createIcoSphere(const QString& name)
{
    PrimitiveObject* newPrimitive = new PrimitiveObject(name, AP_ICOSPHERE);
    return newPrimitive->createPrimitive();
}
Ogre::SceneNode* PrimitiveObject::createRoundedBox(const QString& name)
{
    PrimitiveObject* newPrimitive = new PrimitiveObject(name, AP_ROUNDEDBOX);
    return newPrimitive->createPrimitive();
}

////////////////////////////////////////
/// Accessors
///
const QString& PrimitiveObject::getName()               const
{   return mName;   }

Ogre::SceneNode* PrimitiveObject::getSceneNode()        const
{   return mSceneNode;  }

PrimitiveObject::PrimitiveType PrimitiveObject::getType() const
{   return mType;   }

const Ogre::Real& PrimitiveObject::getSizeX()           const
{   return mSizeX;  }

const Ogre::Real& PrimitiveObject::getSizeY()           const
{   return mSizeY;  }

const Ogre::Real& PrimitiveObject::getSizeZ()           const
{   return mSizeZ;  }

const Ogre::Real& PrimitiveObject::getRadius()          const
{   return mRadius; }

const Ogre::Real& PrimitiveObject::getOuterRadius()     const
{   return mRadius; }

const Ogre::Real& PrimitiveObject::getChamferRadius()   const
{   return mRadius; }

const Ogre::Real& PrimitiveObject::getSectionRadius()   const
{   return mRadius2; }

const Ogre::Real& PrimitiveObject::getInnerRadius()     const
{   return mRadius2; }

const Ogre::Real& PrimitiveObject::getHeight()          const
{   return mHeight; }

int PrimitiveObject::getNumSegX()        const
{   return mNumSegX;    }

int PrimitiveObject::getNumSegRing()     const
{   return mNumSegX;    }

int PrimitiveObject::getNumSegBase()     const
{   return mNumSegX;    }

int PrimitiveObject::getNumSegCircle()   const
{   return mNumSegX;    }

int PrimitiveObject::getNumIterations()  const
{   return mNumSegX;    }

int PrimitiveObject::getNumSegY()        const
{   return mNumSegY;    }

int PrimitiveObject::getNumSegLoop()     const
{   return mNumSegY;    }

int PrimitiveObject::getNumSegSection()  const
{   return mNumSegY;    }

int PrimitiveObject::getNumSegZ()        const
{   return mNumSegZ;    }

int PrimitiveObject::getNumSegHeight()   const
{   return mNumSegZ;    }

const Ogre::Real& PrimitiveObject::getUTile()          const
{   return mUTile;  }

const Ogre::Real& PrimitiveObject::getVTile()          const
{   return mVTile;  }

bool PrimitiveObject::hasUVSwitched()     const
{   return mSwitchUV;   }

////////////////////////////////////////
/// Mutators
///
void PrimitiveObject::setSizeX(const Ogre::Real& sizeX)
{
   if(sizeX>0.0)
    mSizeX = sizeX;

    updatePrimitive();
}

void PrimitiveObject::setSizeY(const Ogre::Real& sizeY)
{
   if(sizeY>0.0)
        mSizeY = sizeY;

    updatePrimitive();
}

void PrimitiveObject::setSizeZ(const Ogre::Real& sizeZ)
{ 
    if(sizeZ>0.0)
        mSizeZ = sizeZ;

    updatePrimitive();
}

void PrimitiveObject::setRadius(const Ogre::Real& radius)
{
    if(radius>0.0)
        mRadius = radius;

    updatePrimitive();
}

void PrimitiveObject::setOuterRadius(const Ogre::Real& radius)
{
    if(radius>0.0)
        mRadius = radius;

    updatePrimitive();
}

void PrimitiveObject::setChamferRadius(const Ogre::Real& radius)
{
    if(radius>0.0)
        mRadius = radius;

    updatePrimitive();
}

void PrimitiveObject::setInnerRadius(const Ogre::Real& radius)
{
   if((radius>0.0)&&(radius<mRadius))
        mRadius2 = radius;

    updatePrimitive();
}

void PrimitiveObject::setSectionRadius(const Ogre::Real& radius)
{

    if((radius>0.0)&&(radius<mRadius))
        mRadius2 = radius;

    updatePrimitive();
}

void PrimitiveObject::setHeight(const Ogre::Real& height)
{

    if(height>0.0)
        mHeight = height;

    updatePrimitive();
}

void PrimitiveObject::setNumSegX(int numSegX)
{
   if(numSegX>0)
        mNumSegX = numSegX;

    updatePrimitive();

}

void PrimitiveObject::setNumSegY(int numSegY)
{
   if(numSegY>0)
    mNumSegY = numSegY;

    updatePrimitive();
}

void PrimitiveObject::setNumSegZ(int numSegZ)
{
    if(numSegZ>0)
        mNumSegZ = numSegZ;

    updatePrimitive();
}

void PrimitiveObject::setNumSegRing(int numSegRing)
{
    if(numSegRing>0)
        mNumSegX = numSegRing;

    updatePrimitive();
}

void PrimitiveObject::setNumSegBase(int numSegBase)
{
    if(numSegBase>0)
        mNumSegX = numSegBase;

    updatePrimitive();
}
void PrimitiveObject::setNumSegCircle(int numSegCircle)
{
    if(numSegCircle>0)
        mNumSegX = numSegCircle;

    updatePrimitive();
}

void PrimitiveObject::setNumIterations(int numIterations)
{
    if(numIterations>0)
        mNumSegX = numIterations;

    updatePrimitive();
}

void PrimitiveObject::setNumSegLoop(int numSegLoop)
{

    if(numSegLoop>0)
        mNumSegY = numSegLoop;

    updatePrimitive();
}
void PrimitiveObject::setNumSegSection(int numSegSection)
{
    if(numSegSection>0)
        mNumSegY = numSegSection;

    updatePrimitive();
}

void PrimitiveObject::setNumSegHeight(int numSegHeight)
{
    if(numSegHeight>0)
        mNumSegZ = numSegHeight;

    updatePrimitive();
}

void PrimitiveObject::setUTile(const Ogre::Real& uTile)
{
    if(uTile>0.0)
        mUTile = uTile;

    updatePrimitive();
}

void PrimitiveObject::setVTile(const Ogre::Real& vTile)
{
    if(vTile>0.0)
        mVTile = vTile;

    updatePrimitive();
}

void PrimitiveObject::setUVSwitch(bool switched)
{
    mSwitchUV = switched;

    updatePrimitive();
}

void PrimitiveObject::clone(const PrimitiveObject* primitiveToClone)
{
    mSizeX  = primitiveToClone->mSizeX;
    mSizeY  = primitiveToClone->mSizeY;
    mSizeZ  = primitiveToClone->mSizeZ;

    mRadius = primitiveToClone->mRadius;
    mRadius2= primitiveToClone->mRadius2;
    mHeight = primitiveToClone->mHeight;

    mNumSegX= primitiveToClone->mNumSegX;
    mNumSegY= primitiveToClone->mNumSegY;
    mNumSegZ= primitiveToClone->mNumSegZ;

    mUTile  = primitiveToClone->mUTile;
    mVTile  = primitiveToClone->mVTile;
    mSwitchUV= primitiveToClone->mSwitchUV;
}

////////////////////////////////////////
/// Private Methods
///

void PrimitiveObject::updatePrimitive()
{
    if(!mSceneNode)
        return;

    //delete old entity
    Ogre::Entity* oldEntity = static_cast<Ogre::Entity*>(mSceneNode->getAttachedObject(0));
    //TODO check if no issue with sub entity (we are only at primitive stage so I assume that there is no issue)
    Ogre::MaterialPtr entMaterial = oldEntity->getSubEntity(0)->getMaterial();

    //if(oldEntity->getMesh().get()->isManuallyLoaded())
        Manager::getSingleton()->getSceneMgr()->destroyManualObject(mName.toStdString());
    Ogre::MeshManager::getSingleton().remove(mName.toStdString());

    //We don't use the Manager
    Manager::getSingleton()->getSceneMgr()->destroyEntity(oldEntity);

    //Create a new one with the given params
    // TODO Check if this line is required
    mSceneNode->getUserObjectBindings().setUserAny(Ogre::Any(this));

    Ogre::MeshPtr mp = createMesh();
    if(mp)
    {
        Ogre::Entity* ent;
        ent = Manager::getSingleton()->createEntity(mSceneNode,mp);

        ent->setMaterial(entMaterial);
    }

}

Ogre::MeshPtr PrimitiveObject::createMesh()
{
    Ogre::MeshPtr mp;
    Ogre::String name = mName.toStdString();
    switch(mType)
    {
        case AP_CUBE:
            mp = Procedural::BoxGenerator()
                    .setSizeX(mSizeX).setSizeY(mSizeY).setSizeZ(mSizeZ)
                    .setNumSegX(mNumSegX).setNumSegY(mNumSegY).setNumSegZ(mNumSegZ)
                    .setUTile(mUTile).setVTile(mVTile).setSwitchUV(mSwitchUV)
                    .realizeMesh(name.data());
            break;
        case AP_SPHERE:
            mp = Procedural::SphereGenerator().setRadius(mRadius)
                    .setNumRings(mNumSegX).setNumSegments(mNumSegY)
                    .setUTile(mUTile).setVTile(mVTile).setSwitchUV(mSwitchUV)
                    .realizeMesh(name.data());
            break;
        case AP_PLANE:
            mp = Procedural::PlaneGenerator().setSizeX(mSizeX).setSizeY(mSizeY)
                    .setNumSegX(mNumSegX).setNumSegY(mNumSegY)
                    .setUTile(mUTile).setVTile(mVTile).setSwitchUV(mSwitchUV)
                    .realizeMesh(name.data());
            break;
        case AP_CYLINDER:
            mp = Procedural::CylinderGenerator().setHeight(mHeight).setRadius(mRadius)
                    .setNumSegBase(mNumSegX).setNumSegHeight(mNumSegZ)
                    .setUTile(mUTile).setVTile(mVTile).setSwitchUV(mSwitchUV)
                    .realizeMesh(name.data());
            break;
        case AP_CONE:
            mp = Procedural::ConeGenerator().setRadius(mRadius).setHeight(mHeight)
                    .setNumSegBase(mNumSegX).setNumSegHeight(mNumSegZ)
                    .setUTile(mUTile).setVTile(mVTile).setSwitchUV(mSwitchUV)
                    .realizeMesh(name.data());
            break;
        case AP_TORUS:
            mp = Procedural::TorusGenerator().setRadius(mRadius).setSectionRadius(mRadius2)
                    .setNumSegCircle(mNumSegX).setNumSegSection(mNumSegY)
                    .setUTile(mUTile).setVTile(mVTile).setSwitchUV(mSwitchUV)
                    .realizeMesh(name.data());
            break;
        case AP_TUBE:
            mp = Procedural::TubeGenerator().setOuterRadius(mRadius).setInnerRadius(mRadius2).setHeight(mHeight)
                    .setNumSegBase(mNumSegX).setNumSegHeight(mNumSegZ)
                    .setUTile(mUTile).setVTile(mVTile).setSwitchUV(mSwitchUV)
                    .realizeMesh(name.data());
            break;
        case AP_CAPSULE:
            mp = Procedural::CapsuleGenerator().setHeight(mHeight).setRadius(mRadius)
                    .setNumRings(mNumSegX).setNumSegments(mNumSegY).setNumSegHeight(mNumSegZ)
                    .setUTile(mUTile).setVTile(mVTile).setSwitchUV(mSwitchUV)
                    .realizeMesh(name.data());
            break;
        case AP_ICOSPHERE:
            mp = Procedural::IcoSphereGenerator().setRadius(mRadius)
                    .setNumIterations(mNumSegX)
                    .setUTile(mUTile).setVTile(mVTile).setSwitchUV(mSwitchUV)
                    .realizeMesh(name.data());
            break;
        case AP_ROUNDEDBOX:
            mp = Procedural::RoundedBoxGenerator().setSizeX(mSizeX).setSizeY(mSizeY).setSizeZ(mSizeZ).setChamferSize(mRadius)
                    .setNumSegX(mNumSegX).setNumSegY(mNumSegY).setNumSegZ(mNumSegZ)
                    .setUTile(mUTile).setVTile(mVTile).setSwitchUV(mSwitchUV)
                    .realizeMesh(name.data());
            break;
        default:
            mp.reset();
            break;
    }
    return mp;
}

Ogre::SceneNode* PrimitiveObject::createPrimitive()
{
    // TODO check if no memory leakage using setUserAny
    // http://www.ogre3d.org/forums/viewtopic.php?f=2&t=81594&sid=70ac0036f529bfc87dc2c464b03e33fe
    // This will trigger the selection change signal
    mSceneNode = Manager::getSingleton()->addSceneNode(mName.toStdString().data(), Ogre::Any(static_cast<PrimitiveObject*>(this)));

    mName = mSceneNode->getName().data();

    Ogre::MeshPtr mp;
    mp.reset();
    mp = createMesh();

    if(mp)
    {
        Ogre::Entity* ent = Manager::getSingleton()->createEntity(mSceneNode,mp);

        ent->setMaterialName("BaseWhite");
        mSceneNode->setPosition(0,0,0);
        return mSceneNode;
    }
    else
    {
        Manager::getSingleton()->destroySceneNode(mSceneNode);
        return 0;
    }

}




