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
#include <cstring>

Ogre::MeshPtr AssimpToOgreImporter::loadModel(const std::string& path, bool convertToLeftHanded, unsigned int additionalFlags) {
    skeleton.reset();  // Clear any skeleton from a previous import
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

    unsigned int flags = aiProcess_CalcTangentSpace |
                         aiProcess_JoinIdenticalVertices |
                         aiProcess_Triangulate |
                         aiProcess_RemoveComponent |
                         aiProcess_GenSmoothNormals |
                         aiProcess_ValidateDataStructure |
                         // aiProcess_OptimizeGraph intentionally omitted: it collapses the
                         // node hierarchy that aiProcess_PopulateArmatureData requires to
                         // link aiBone objects to their aiNode, causing hangs on re-imported
                         // skeletal meshes (e.g. exported LOD gltf2 files).
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

    auto pathEndsWithInsensitive = [](const std::string& p, const char* suf) -> bool {
        const size_t n = std::strlen(suf);
        if (p.size() < n)
            return false;
        for (size_t i = 0; i < n; ++i) {
            char a = p[p.size() - n + i];
            char b = suf[i];
            if (a >= 'A' && a <= 'Z')
                a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = static_cast<char>(b - 'A' + 'a');
            if (a != b)
                return false;
        }
        return true;
    };

    const aiScene* scene = importer.ReadFile(path, flags);
    // Do this immediately after ReadFile while the scene is still valid.
    m_sceneUpAxis = 1; // default: Y-up
    if (scene && scene->mMetaData)
        scene->mMetaData->Get("UpAxis", m_sceneUpAxis);

    // Some FBX animation takes fail the full post-process stack (null scene or no root)
    // but load with a lighter flag set. Retry once before giving up.
    if ((!scene || !scene->mRootNode) &&
        (pathEndsWithInsensitive(path, ".fbx") || pathEndsWithInsensitive(path, ".fbxa"))) {
        unsigned int lightFlags = aiProcess_Triangulate |
                                  aiProcess_ValidateDataStructure |
                                  aiProcess_LimitBoneWeights |
                                  aiProcess_PopulateArmatureData |
                                  aiProcess_GlobalScale;
        if (convertToLeftHanded)
            lightFlags |= aiProcess_ConvertToLeftHanded;
        lightFlags |= additionalFlags;
        importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        scene = importer.ReadFile(path, lightFlags);
        m_sceneUpAxis = 1;
        if (scene && scene->mMetaData)
            scene->mMetaData->Get("UpAxis", m_sceneUpAxis);
    }

    // A null scene or missing root node is always fatal.
    if(!scene || !scene->mRootNode) {
        const char* errStr = importer.GetErrorString();
        const std::string errMsg = (errStr && *errStr) ? std::string(errStr)
                                                       : std::string("ReadFile failed (no scene / no root node)");
        Ogre::LogManager::getSingleton().logError("ERROR::ASSIMP::" + errMsg);
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

    // Always prefer the path-based filename — FBX metadata names (scene->mName) are
    // often generic ("unreal_take", "RootNode", "Scene", etc.) and don't reflect the
    // actual file the user imported.
    {
        std::string filename = path.substr(path.find_last_of("/\\") + 1);
        auto dot = filename.find_last_of('.');
        std::string pathName = (dot != std::string::npos) ? filename.substr(0, dot) : filename;
        if (!pathName.empty())
            modelName = pathName;
        else {
            modelName = scene->mName.C_Str();
            if (modelName.empty())
                modelName = "model";
        }
    }

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

    const bool isZup = (m_sceneUpAxis == 2);

    if(hasBones || scene->HasAnimations()) {
        skeleton = Ogre::SkeletonManager::getSingleton().create(modelName+".skeleton", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);
        BoneProcessor boneProcessor;
        // Create bones at their native FBX-space positions (no Z-up bake yet).
        boneProcessor.processBones(skeleton, scene);

        // Save the bind pose BEFORE processing animations so that animation
        // deltas are computed relative to the original (unbaked) T-pose.
        // This avoids a basis mismatch for Z-up files that have embedded animations:
        // if we baked first, AnimationProcessor would measure deltas against the
        // already-baked bone orientations while the keyframe data is still in Z-up.
        skeleton->setBindingPose();

        if(scene->HasAnimations()) {
            AnimationProcessor animationProcessor(skeleton);
            animationProcessor.processAnimations(scene);
        }

        // Now bake Z-up → Y-up into root bone rest poses — mesh skeletons only.
        // Animation-only skeletons stay in native Z-up so AnimationMerger's
        // boneCorrection can apply the single correct conversion on merge.
        if (isZup && !animationOnly) {
            BoneProcessor::bakeZupToYup(skeleton);
            // Re-snapshot the binding pose after baking so Ogre's reset() returns
            // to the correct Y-up rest pose.
            skeleton->setBindingPose();
        }
    }

    // For animation-only files there is no geometry to process.
    if(animationOnly)
        return {};

    // Process the root node recursively (meshes)
    MeshProcessor meshProcessor(skeleton, isZup);
    meshProcessor.processNode(scene->mRootNode, scene);
    Ogre::MeshPtr ogreMesh = meshProcessor.createMesh(modelName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, materialProcessor);

    return ogreMesh;
}
