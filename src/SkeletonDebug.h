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

#ifndef SKELETONDEBUG_H_INCLUDED
#define SKELETONDEBUG_H_INCLUDED

#include <Ogre.h>
#include <OgreTagPoint.h>
#include <vector>
//#include "ObjectTextDisplay.h"

class SkeletonDebug
{
public:
    SkeletonDebug(Ogre::Entity *entity, Ogre::SceneManager *man,/* Ogre::Camera *cam,*/ float boneSize = 0.1f, float scaleAxes =0.1f);
    ~SkeletonDebug();

    void setAxesScale(Ogre::Real scale){mScaleAxes = scale;}
    Ogre::Real getAxesScale(){return mScaleAxes;}

    void showAxes(bool show);
    void showNames(bool show);
    void showBones(bool show);
    bool axesShown(){return mShowAxes;}
    bool namesShown(){return mShowNames;}
    bool bonesShown(){return mShowBones;}

    void update();

private:
    std::vector<Ogre::Entity*> mAxisEntities;
    std::vector<Ogre::Entity*> mBoneEntities;
    //std::vector<ObjectTextDisplay*> mTextOverlays;

    float mBoneSize;

    Ogre::Entity *mEntity;
    Ogre::MaterialPtr mAxisMatPtr;
    Ogre::MaterialPtr mBoneMatPtr;
    Ogre::MeshPtr mBoneMeshPtr;
    Ogre::MeshPtr mAxesMeshPtr;
    Ogre::SceneManager *mSceneMan;
    //Ogre::Camera *mCamera;

    Ogre::Real mScaleAxes;

    bool mShowAxes;
    bool mShowBones;
    bool mShowNames;

    void createAxesMaterial();
    void createBoneMaterial();
    void createAxesMesh();
    void createBoneMesh();
};

#endif // SKELETONDEBUG_H_INCLUDED
