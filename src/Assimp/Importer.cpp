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

#include "Importer.h"
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "AnimationProcessor.h"
#include "BoneProcessor.h"
#include "MeshProcessor.h"

#include <algorithm>

Ogre::MeshPtr AssimpToOgreImporter::loadModel(const std::string& path, bool convertToLeftHanded, unsigned int additionalFlags) {
    skeleton.reset();  // Clear any skeleton from a previous import
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

    unsigned int flags = aiProcess_CalcTangentSpace |
                         aiProcess_JoinIdenticalVertices |
                         aiProcess_Triangulate |
                         aiProcess_RemoveComponent |
                         aiProcess_GenSmoothNormals |
                         aiProcess_ValidateDataStructure |
                         aiProcess_OptimizeGraph |
                         aiProcess_LimitBoneWeights |
                         aiProcess_SortByPType |
                         aiProcess_ImproveCacheLocality |
                         aiProcess_FixInfacingNormals |
                         aiProcess_PopulateArmatureData | // necessary to load bone node information
                         aiProcess_OptimizeMeshes |
                         aiProcess_GlobalScale;
    if (convertToLeftHanded)
        flags |= aiProcess_ConvertToLeftHanded;
    flags |= additionalFlags;

    const aiScene* scene = importer.ReadFile(path, flags);

    // Read coordinate system from FBX metadata (1=Y-up, 2=Z-up).
    // Do this immediately after ReadFile while the scene is still valid.
    m_sceneUpAxis = 1; // default: Y-up
    if (scene && scene->mMetaData)
        scene->mMetaData->Get("UpAxis", m_sceneUpAxis);

    // A null scene or missing root node is always fatal.
    if(!scene || !scene->mRootNode) {
        Ogre::LogManager::getSingleton().logError("ERROR::ASSIMP::" + std::string(importer.GetErrorString()));
        return {};
    }
    // animationOnly: the scene has no geometry (e.g. Unreal Engine retarget FBX).
    // AI_SCENE_FLAGS_INCOMPLETE does NOT reliably indicate "no meshes" — it can also be set on
    // scenes that have valid geometry but missing materials or other partial data.  Use the actual
    // mesh count as the authoritative check.
    const bool animationOnly = (scene->mNumMeshes == 0);
    if(animationOnly && !scene->HasAnimations()) {
        Ogre::LogManager::getSingleton().logError("ERROR::ASSIMP:: Scene has no meshes and no animations.");
        return {};
    }

    modelName = scene->mName.C_Str();
    if(modelName.empty()) modelName = path.substr(path.find_last_of("/\\") + 1);

    // Remove stale resources from a previous import of the same file
    if (auto oldMesh = Ogre::MeshManager::getSingleton().getByName(modelName))
        Ogre::MeshManager::getSingleton().remove(oldMesh);
    if (auto oldSkel = Ogre::SkeletonManager::getSingleton().getByName(modelName + ".skeleton"))
        Ogre::SkeletonManager::getSingleton().remove(oldSkel);

    // Process materials
    materialProcessor.loadScene(scene);

    // Process the skeleton whenever the scene has bones (skinned mesh) or animations.
    // A mesh can be skinned without having any animations (e.g. a rigged bind-pose).
    bool hasBones = false;
    for(unsigned i = 0; i < scene->mNumMeshes && !hasBones; ++i)
        hasBones = scene->mMeshes[i]->mNumBones > 0;

    if(hasBones || scene->HasAnimations()) {
        skeleton = Ogre::SkeletonManager::getSingleton().create(modelName+".skeleton", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);
        BoneProcessor boneProcessor;
        boneProcessor.processBones(skeleton, scene);

        // Save the bind pose so that animation deltas are applied relative to
        // the correct base transforms.  Binary SkeletonSerializer calls this
        // automatically on load, but for in-memory skeletons we must do it
        // explicitly before creating animations.
        skeleton->setBindingPose();

        if(scene->HasAnimations()) {
            // Process animations
            AnimationProcessor animationProcessor(skeleton);
            animationProcessor.processAnimations(scene);
        }
    }

    // For animation-only files there is no geometry to process.
    if(animationOnly)
        return {};

    // Process the root node recursively (meshes)
    MeshProcessor meshProcessor(skeleton);
    meshProcessor.processNode(scene->mRootNode, scene);
    Ogre::MeshPtr ogreMesh = meshProcessor.createMesh(modelName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, materialProcessor);

    return ogreMesh;
}
