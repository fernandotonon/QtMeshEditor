/*/////////////////////////////////////////////////////////////////////////////////
/// A QtMeshEditor file
///
/// Copyright (c)
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

#include "Importer.h"
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "AnimationProcessor.h"
#include "BoneProcessor.h"
#include "MeshProcessor.h"

#include <algorithm>

Ogre::MeshPtr AssimpToOgreImporter::loadModel(const std::string& path) {
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

    const aiScene* scene = importer.ReadFile(path,
                                             aiProcess_CalcTangentSpace |
                                                 aiProcess_JoinIdenticalVertices |
                                                 aiProcess_Triangulate |
                                                 aiProcess_RemoveComponent |
                                                 aiProcess_GenSmoothNormals |
                                                 aiProcess_ValidateDataStructure |
                                                 aiProcess_OptimizeGraph |
                                                 aiProcess_LimitBoneWeights |
                                                 aiProcess_FindInvalidData |
                                                 aiProcess_SortByPType |
                                                 aiProcess_ImproveCacheLocality |
                                                 aiProcess_FixInfacingNormals |
                                                 aiProcess_PopulateArmatureData | // necessary to load bone node information
                                                 aiProcess_ConvertToLeftHanded |
                                                 aiProcess_OptimizeMeshes |
                                                 aiProcess_GlobalScale
                                             );

    if(!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        Ogre::LogManager::getSingleton().logError("ERROR::ASSIMP::" + std::string(importer.GetErrorString()));
        return {};
    }

    modelName = scene->mName.C_Str();
    if(modelName.empty()) modelName = path.substr(path.find_last_of("/\\") + 1);

    // Process materials
    materialProcessor.loadScene(scene);

    // Process the skeleton
    skeleton = Ogre::SkeletonManager::getSingleton().create(modelName+".skeleton", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);
    BoneProcessor boneProcessor;
    boneProcessor.processBones(skeleton, scene);

    // Process animations
    AnimationProcessor animationProcessor(skeleton);
    animationProcessor.processAnimations(scene);

    // Process the root node recursively (meshes)
    MeshProcessor meshProcessor(skeleton);
    meshProcessor.processNode(scene->mRootNode, scene);
    Ogre::MeshPtr ogreMesh = meshProcessor.createMesh(modelName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, materialProcessor);

    return ogreMesh;
}
