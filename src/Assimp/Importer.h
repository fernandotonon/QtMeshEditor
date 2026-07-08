/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) HogPog Team (www.hogpog.com.br)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

#pragma once

#include <Ogre.h>
#include <assimp/Importer.hpp>

#include "MaterialProcessor.h"

class AssimpToOgreImporter {
public:
    AssimpToOgreImporter() : importer() {}

    Ogre::MeshPtr loadModel(const std::string& path, bool convertToLeftHanded = true, unsigned int additionalFlags = 0);

    // Non-null only when loadModel() processed an animation-only file (no mesh geometry).
    Ogre::SkeletonPtr getLoadedSkeleton() const { return skeleton; }

    const aiScene* getImportedScene() const { return importer.GetScene(); }

    // Returns the UpAxis from FBX metadata of the last loaded scene.
    // 1 = Y-up (Mixamo, default), 2 = Z-up (Unreal Engine).
    // Always returns 1 for non-FBX formats or when metadata is absent.
    int getSceneUpAxis() const { return m_sceneUpAxis; }

private:
    Assimp::Importer importer;
    Ogre::SkeletonPtr skeleton;
    std::string modelName;
    int m_sceneUpAxis = 1;

    MaterialProcessor materialProcessor;
};
