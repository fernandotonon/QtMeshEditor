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
#include <assimp/material.h>
#include "AnimationProcessor.h"
#include "BoneProcessor.h"
#include "MeshProcessor.h"
#include <algorithm>
#include <string_view>
#include <cstdlib>

#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace {

bool isProtectedOgreMaterialName(const std::string& name)
{
    if (name.empty())
        return true;
    if (name == "BaseWhite" || name == "BaseWhiteNoLighting" || name == "GUI_Material")
        return true;
    return name.rfind("Ogre/", 0) == 0;
}

// #933: world ORIENTATION of the reference (first skinned) mesh node — per
// Assimp's FBX semantics the bone offsets are relative to the mesh's global
// frame, so its rotation maps the imported content back to the file's
// intended Y-up world. Exact when the rig's skin bind matches the node pose
// (the normal export case); rigs whose bind diverges from the node tree
// (rare) keep a residual tilt. Identity when there is no skinned mesh.
Ogre::Quaternion detectNodeBakeRotation(const aiScene* scene)
{
    if (!scene || !scene->mRootNode)
        return Ogre::Quaternion::IDENTITY;
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        if (scene->mMeshes[i]->mNumBones == 0)
            continue;
        const aiNode* meshNode = BoneProcessor::findMeshNode(scene->mRootNode, i);
        if (!meshNode)
            return Ogre::Quaternion::IDENTITY;
        const Ogre::Matrix4 world = BoneProcessor::nodeWorldTransform(meshNode);
        // A mirrored (negative-determinant) node frame has no meaningful
        // rotation decomposition — skip the bake rather than apply garbage.
        if (world.linear().determinant() < 0.0f)
            return Ogre::Quaternion::IDENTITY;
        Ogre::Vector3 pos;
        Ogre::Vector3 scl;
        Ogre::Quaternion rot;
        Ogre::Affine3(world).decomposition(pos, scl, rot);
        return rot;   // first skinned mesh decides the orientation
    }
    return Ogre::Quaternion::IDENTITY;
}

} // namespace

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
    // HISTORICAL NAME, NEW MEANING (#977): this used to apply the full
    // aiProcess_ConvertToLeftHanded (axis mirror + winding flip + V flip),
    // which presented every imported asset MIRRORED in the editor — glTF,
    // FBX and OBJ are right-handed like Ogre, so the mirror was never a
    // conversion, just a defect the whole pipeline compensated around
    // (exports, VAT shaders, retarget side conventions). Only the V flip is
    // a real convention difference, so that is all this applies now: the
    // viewport, the source file and every export share one space.
    if (convertToLeftHanded)
        flags |= aiProcess_FlipUVs;
    flags |= additionalFlags;

    auto pathEndsWithInsensitive = [](const std::string& p, std::string_view suf) -> bool {
        const size_t n = suf.size();
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
    m_nodeBakeRotation = Ogre::Quaternion::IDENTITY;
    if (scene && scene->mMetaData)
        scene->mMetaData->Get("UpAxis", m_sceneUpAxis);
    // glTF / glb are Y-up BY SPECIFICATION. Assimp's glTF importer can stamp a
    // bogus UpAxis=2 (Z-up) into the scene metadata, which made us bake a
    // spurious +90°X rotation and IMPORT THE MODEL UPSIDE-DOWN on re-imported
    // glbs (e.g. a Mixamo character round-tripped to glb). Trust the format
    // spec, not the metadata, for glTF.
    if (pathEndsWithInsensitive(path, ".gltf") || pathEndsWithInsensitive(path, ".glb"))
        m_sceneUpAxis = 1;

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
            lightFlags |= aiProcess_FlipUVs;   // V flip only — see above
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

    materialProcessor.setSourceDirectory(QFileInfo(QString::fromStdString(path)).absolutePath());
    for (unsigned int mi = 0; mi < scene->mNumMaterials; ++mi) {
        aiString matNameAi;
        if (scene->mMaterials[mi]->Get(AI_MATKEY_NAME, matNameAi) != AI_SUCCESS
            || matNameAi.length == 0) {
            continue;
        }
        const std::string matName = matNameAi.C_Str();
        if (isProtectedOgreMaterialName(matName))
            continue;
        if (auto existing = Ogre::MaterialManager::getSingleton().getByName(matName))
            Ogre::MaterialManager::getSingleton().remove(existing);
    }

    // Process materials
    materialProcessor.loadScene(scene);

    // Process the skeleton whenever the scene has bones (skinned mesh) or a
    // SKELETAL animation (one with node channels). A mesh can be skinned without
    // animations (rigged bind-pose). Crucially, a MORPH-only animation
    // (aiAnimation with mMorphMeshChannels but zero mNumChannels) must NOT
    // trigger skeleton creation: doing so builds an empty skeleton, and
    // setBindingPose() → deriveRootBone() then asserts on the empty bone list,
    // producing a mesh that fails to load/render. So require at least one node
    // channel, not merely HasAnimations().
    bool hasBones = false;
    for(unsigned i = 0; i < scene->mNumMeshes && !hasBones; ++i)
        hasBones = scene->mMeshes[i]->mNumBones > 0;

    bool hasSkeletalAnim = false;
    for(unsigned i = 0; i < scene->mNumAnimations && !hasSkeletalAnim; ++i)
        hasSkeletalAnim = scene->mAnimations[i]->mNumChannels > 0;

    const bool isZup = (m_sceneUpAxis == 2);

    if(hasBones || hasSkeletalAnim) {
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
        } else if (!animationOnly) {
            // #933: Blender-style FBX stamps Y-up METADATA but carries the
            // standing orientation on the NODE chain (armature/mesh nodes get
            // e.g. −90°X). Bones and vertices live in mesh-node space, so the
            // bind pose renders LYING DOWN (marketplace thumbnails read
            // top-down). Bake the skinned mesh node's world ORIENTATION the
            // same way as the Z-up path (root bones here; vertices in
            // MeshProcessor below). Identity for Mixamo-style rigs, so the
            // common case is untouched.
            m_nodeBakeRotation = detectNodeBakeRotation(scene);
            if (!m_nodeBakeRotation.equals(Ogre::Quaternion::IDENTITY,
                                           Ogre::Radian(1e-3f))) {
                BoneProcessor::bakeRootRotation(skeleton, m_nodeBakeRotation);
                skeleton->setBindingPose();
            } else {
                m_nodeBakeRotation = Ogre::Quaternion::IDENTITY;
            }
        }
    }

    // For animation-only files there is no geometry to process.
    if(animationOnly)
        return {};

    // Process the root node recursively (meshes)
    MeshProcessor meshProcessor(skeleton, isZup, m_nodeBakeRotation);

    // ARKit blendshape name sidecar (`<file>.arkit.json`, schema
    // qtmesh-arkit-blendshapes-v1): Assimp's glTF2 exporter drops
    // `targetNames`, so shapes in a re-imported glb arrive nameless and would
    // degrade to "Shape_N". Restore the authored ARKit names from the sidecar
    // the face-rig exporters write next to the mesh.
    {
        QFile sidecar(QString::fromStdString(path) + ".arkit.json");
        if (sidecar.exists() && sidecar.open(QIODevice::ReadOnly)) {
            const QJsonObject root = QJsonDocument::fromJson(sidecar.readAll()).object();
            if (root.value("schema").toString().startsWith("qtmesh-arkit-blendshapes")) {
                std::vector<std::string> names;
                for (const auto& v : root.value("names").toArray())
                    names.push_back(v.toString().toStdString());
                if (!names.empty())
                    meshProcessor.setMorphNameHints(std::move(names));
            }
        }
    }

    meshProcessor.processNode(scene->mRootNode, scene);
    Ogre::MeshPtr ogreMesh = meshProcessor.createMesh(modelName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, materialProcessor);

    // Consume authored morph-WEIGHT animation channels (aiAnimation::
    // mMorphMeshChannels) into mesh-level VAT_POSE weight clips. Must run AFTER
    // createMesh — the morph poses these clips reference only exist once the
    // mesh + poses have been built by MeshProcessor. No-op when the scene has
    // no morph channels or the mesh has no poses.
    if (ogreMesh && scene->HasAnimations())
        AnimationProcessor::processMorphWeightAnimations(ogreMesh, scene);

    return ogreMesh;
}
