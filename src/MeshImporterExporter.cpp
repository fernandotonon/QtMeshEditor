/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

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

#include "MeshImporterExporter.h"
#include <assimp/Importer.hpp>
#include <assimp/Exporter.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <QFileDialog>
#include <QImage>
#include <QMessageBox>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QRegularExpression>
#include <set>
#include <cmath>
#include <cstring>
#include <algorithm>

#include "OgreXML/OgreXMLMeshSerializer.h"
#include "OgreXML/OgreXMLSkeletonSerializer.h"
#include "OgreXML/pugixml.hpp"

#include "AnimationMerger.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "RTShaderHelper.h"
#include "Assimp/Importer.h"
#include "Assimp/MaterialProcessor.h"
#include "Assimp/MeshProcessor.h"
#include "Assimp/BoneProcessor.h"
#include "Assimp/AnimationProcessor.h"
#include "CLIPipeline.h"
#include "PS1/PS1PLY.h"
#include "PS1/PS1MAT.h"
#include "PS1/PS1RSD.h"
#include "PS1/PS1TMD.h"
#include "PS1/PS1TIM.h"
#include "EditableMesh.h"
#include "EditModeController.h"
#include <OgreMaterialManager.h>
#include <OgreDataStream.h>
#include <OgrePixelFormat.h>

#ifndef WIN32
    #include <unistd.h>
#endif

const QMap<QString, QString> MeshImporterExporter::exportFormats = {
    {"Ogre Mesh (*.mesh)", ".mesh"},
    {"Ogre Mesh v1.10+(*.mesh)", ".mesh"},
    {"Ogre Mesh v1.8+(*.mesh)", ".mesh"},
    {"Ogre Mesh v1.7+(*.mesh)", ".mesh"},
    {"Ogre Mesh v1.4+(*.mesh)", ".mesh"},
    {"Ogre Mesh v1.0+(*.mesh)", ".mesh"},
    {"Ogre XML (*.mesh.xml)", ".mesh.xml"},
    {"Collada (*.dae)", ".dae"},
    {"X (*.x)", ".x"},
    {"OBJ (*.obj)", ".obj"},
    {"OBJ without MTL (*.objnomtl)", ".obj"},
    {"STL (*.stl)", ".stl"},
    {"PLY (*.ply)", ".ply"},
    {"3DS (*.3ds)", ".3ds"},
    {"glTF 2.0 (*.gltf)", ".gltf"},
    {"glTF 2.0 Binary (*.glb)", ".glb"},
    {"Assimp Binary (*.assbin)", ".assbin"},
    {"FBX Binary (*.fbx)", ".fbx"},
    {"PlayStation TMD (*.tmd)", ".tmd"},
    {"PlayStation RSD (*.rsd)", ".rsd"}
};

void MeshImporterExporter::configureCamera(const Ogre::Entity *en)
{
    // Use the WORLD-space bbox so the camera distance accounts for any
    // scale applied to the parent SceneNode (e.g. the auto-scale fix
    // in importer() for sub-unit meshes). Without `derive=true` we'd
    // read mesh-local bbox sizes — fine for sensible-scale assets, but
    // for an auto-scaled mm-unit FBX the local bbox is still ~5 mm and
    // the camera would land at distance ~0, leaving the camera inside
    // the enlarged mesh and the near-clip plane. (Codex review on PR #456.)
    const Ogre::AxisAlignedBox worldBb = en->getWorldBoundingBox(/*derive=*/true);
    const auto worldSize = worldBb.getSize();
    Ogre::Real size = std::max({worldSize.x, worldSize.y, worldSize.z});
    auto cameras = Manager::getSingleton()->getSceneMgr()->getCameras();
    for (const auto &[_, camera] : cameras) {
        const Ogre::Radian fov = camera->getFOVy();
        if (fov.valueRadians() <= 1e-6f || size <= 0.f)
            continue;
        const Ogre::Real tanHalf = std::tan(fov.valueRadians() / 2.0f);
        if (!std::isfinite(tanHalf) || tanHalf <= 1e-8f)
            continue;
        Ogre::Real distance = size / (2 * tanHalf);
        if (!std::isfinite(distance) || distance <= 0.f)
            continue;
        Ogre::SceneNode* parent = camera->getParentSceneNode();
        if (parent)
            parent->setPosition(0, 0, -distance);
    }
}

void MeshImporterExporter::exportMaterial(const Ogre::Entity* e, const QFileInfo& file)
{
    Ogre::MaterialSerializer ms;
    std::set<std::string, std::less<>> queued;
    for (const auto &subEntity : e->getSubEntities())
    {
        auto mat = subEntity->getMaterial();
        if (queued.insert(mat->getName()).second)
        {
            ms.queueForExport(mat);
            exportTextures(mat, file);
        }
    }
    ms.exportQueued((file.path() + "/" + file.baseName() + ".material").toStdString());
}

QString MeshImporterExporter::exportTextureName(const QString& originalName)
{
    QFileInfo fi(originalName);
    QString ext = fi.suffix().toLower();
    // STBI codec supports writing: png, bmp, tga, hdr
    // For anything else (jpg, jpeg, dds, etc.), convert to png
    static const QStringList supported = {"png", "bmp", "tga", "hdr"};
    if (supported.contains(ext))
        return fi.fileName();
    return fi.completeBaseName() + ".png";
}

void MeshImporterExporter::exportTextures(const Ogre::MaterialPtr& material, const QFileInfo& file)
{
    for (const auto &technique : material->getTechniques())
    {
        for (const auto &pass : technique->getPasses())
        {
            for (const auto &tus : pass->getTextureUnitStates())
            {
                if (tus->getContentType() != Ogre::TextureUnitState::CONTENT_NAMED)
                    continue;

                Ogre::TexturePtr tex = tus->_getTexturePtr();
                if (tex->getTextureType() == Ogre::TEX_TYPE_2D)
                {
                    Ogre::Image img;
                    tex->convertToImage(img, true);
                    QString saveName = exportTextureName(QString::fromStdString(tex->getName()));
                    try {
                        img.save((file.path() + "/" + saveName).toStdString());
                    } catch (Ogre::Exception& ex) {
                        Ogre::LogManager::getSingleton().logError(
                            "Failed to save texture '" + saveName.toStdString() + "': " + ex.what());
                    }
                }
            }
        }
    }
}

// Convert Ogre matrix to Assimp matrix
static aiMatrix4x4 toAiMatrix(const Ogre::Matrix4& m)
{
    return aiMatrix4x4(
        m[0][0], m[0][1], m[0][2], m[0][3],
        m[1][0], m[1][1], m[1][2], m[1][3],
        m[2][0], m[2][1], m[2][2], m[2][3],
        m[3][0], m[3][1], m[3][2], m[3][3]
    );
}

// Build bone node hierarchy recursively, with optional name prefix for multi-entity scenes
static aiNode* buildBoneNode(Ogre::Bone* bone, aiNode* parent, const std::string& bonePrefix = "")
{
    auto* node = new aiNode(bonePrefix + std::string(bone->getName()));
    node->mParent = parent;

    Ogre::Matrix4 localTransform;
    localTransform.makeTransform(bone->getPosition(), bone->getScale(), bone->getOrientation());
    node->mTransformation = toAiMatrix(localTransform);

    const auto& children = bone->getChildren();
    if (!children.empty())
    {
        node->mNumChildren = static_cast<unsigned int>(children.size());
        node->mChildren = new aiNode*[node->mNumChildren];
        unsigned int ci = 0;
        for (auto* child : children)
        {
            auto* childBone = dynamic_cast<Ogre::Bone*>(child);
            if (childBone)
                node->mChildren[ci++] = buildBoneNode(childBone, node, bonePrefix);
        }
        node->mNumChildren = ci;
    }

    return node;
}

// Convert an Ogre material to an aiMaterial (colors + textures)
static aiMaterial* buildAiMaterialFromOgre(const Ogre::MaterialPtr& mat)
{
    auto* aiMat = new aiMaterial();
    aiString matName(mat->getName());
    aiMat->AddProperty(&matName, AI_MATKEY_NAME);

    auto* tech = mat->getTechnique(0);
    if (tech && tech->getNumPasses() > 0)
    {
        auto* pass = tech->getPass(0);
        auto d = pass->getDiffuse();
        aiColor4D diffuse(d.r, d.g, d.b, d.a);
        aiMat->AddProperty(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);

        auto s = pass->getSpecular();
        aiColor4D specular(s.r, s.g, s.b, s.a);
        aiMat->AddProperty(&specular, 1, AI_MATKEY_COLOR_SPECULAR);

        auto a = pass->getAmbient();
        aiColor4D ambient(a.r, a.g, a.b, a.a);
        aiMat->AddProperty(&ambient, 1, AI_MATKEY_COLOR_AMBIENT);

        auto e = pass->getSelfIllumination();
        aiColor4D emissive(e.r, e.g, e.b, e.a);
        aiMat->AddProperty(&emissive, 1, AI_MATKEY_COLOR_EMISSIVE);

        float shininess = pass->getShininess();
        aiMat->AddProperty(&shininess, 1, AI_MATKEY_SHININESS);

        // Map our canonical slot names to Assimp texture types so the
        // re-import path (MaterialProcessor) recognises them. Without
        // this the metallic / roughness / ao / emissive slots would all
        // export under aiTextureType_DIFFUSE, and on reimport the first
        // one wins as the "diffuse" texture and the rest are dropped.
        // We also keep "albedo" routed to DIFFUSE (legacy compatible)
        // AND mirror it under BASE_COLOR so PBR-aware reimport finds it.
        unsigned short diffuseIdx = 0;
        unsigned short normalIdx = 0;
        unsigned short baseColorIdx = 0;
        unsigned short metalIdx = 0;
        unsigned short roughIdx = 0;
        unsigned short aoIdx = 0;
        unsigned short emissiveIdx = 0;
        for (unsigned short ti = 0; ti < pass->getNumTextureUnitStates(); ++ti)
        {
            auto* tus = pass->getTextureUnitState(ti);
            if (tus->getContentType() != Ogre::TextureUnitState::CONTENT_NAMED)
                continue;
            QString safeName = MeshImporterExporter::exportTextureName(
                QString::fromStdString(tus->getTextureName()));
            aiString texPath(safeName.toStdString());
            const auto& tusName = tus->getName();

            if (tusName == "normal_map" || tusName == "NormalMap") {
                aiMat->AddProperty(&texPath, AI_MATKEY_TEXTURE(aiTextureType_NORMALS, normalIdx));
                ++normalIdx;
            } else if (tusName == "albedo") {
                // glTF base colour: write BASE_COLOR (PBR re-import) AND
                // DIFFUSE (legacy / Phong renderers). Most engines accept
                // both; Assimp routes BASE_COLOR back to aiTextureType_BASE_COLOR
                // on re-read, which our MaterialProcessor binds to the
                // "albedo" canonical slot.
                aiMat->AddProperty(&texPath, AI_MATKEY_TEXTURE(aiTextureType_BASE_COLOR, baseColorIdx));
                ++baseColorIdx;
                aiMat->AddProperty(&texPath, AI_MATKEY_TEXTURE(aiTextureType_DIFFUSE, diffuseIdx));
                ++diffuseIdx;
            } else if (tusName == "metallic") {
                aiMat->AddProperty(&texPath, AI_MATKEY_TEXTURE(aiTextureType_METALNESS, metalIdx));
                ++metalIdx;
            } else if (tusName == "roughness") {
                aiMat->AddProperty(&texPath, AI_MATKEY_TEXTURE(aiTextureType_DIFFUSE_ROUGHNESS, roughIdx));
                ++roughIdx;
            } else if (tusName == "ao") {
                aiMat->AddProperty(&texPath, AI_MATKEY_TEXTURE(aiTextureType_AMBIENT_OCCLUSION, aoIdx));
                ++aoIdx;
            } else if (tusName == "emissive") {
                aiMat->AddProperty(&texPath, AI_MATKEY_TEXTURE(aiTextureType_EMISSIVE, emissiveIdx));
                ++emissiveIdx;
            } else if (tusName == "diffuse_map" || tusName.empty()) {
                // Legacy Phong diffuse, or unnamed TUS — route as DIFFUSE.
                aiMat->AddProperty(&texPath, AI_MATKEY_TEXTURE(aiTextureType_DIFFUSE, diffuseIdx));
                ++diffuseIdx;
            } else {
                // Unknown slot name — route as UNKNOWN so it's preserved
                // round-trip without being mistaken for a diffuse.
                aiMat->AddProperty(&texPath, AI_MATKEY_TEXTURE(aiTextureType_UNKNOWN, ti));
            }
        }
    }
    return aiMat;
}

// Read vertex/index data from an Ogre submesh into a pre-allocated aiMesh
static void readSubmeshGeometry(
    aiMesh* aiM,
    const Ogre::VertexData* vData,
    const Ogre::SubMesh* subMesh,
    const Ogre::Entity* entity,
    unsigned int subIndex,
    const std::map<std::string, unsigned int, std::less<>>& matIndexMap)
{
    aiM->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    aiM->mNumVertices = static_cast<unsigned int>(vData->vertexCount);
    aiM->mVertices = new aiVector3D[aiM->mNumVertices];

    // Material index
    const auto* subEnt = entity->getSubEntity(subIndex);
    auto matIt = matIndexMap.find(subEnt->getMaterial()->getName());
    aiM->mMaterialIndex = (matIt != matIndexMap.end()) ? matIt->second : 0;

    // Read positions
    const auto* posElem = vData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (posElem)
    {
        auto vbuf = vData->vertexBufferBinding->getBuffer(posElem->getSource());
        auto* base = static_cast<const unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (unsigned int j = 0; j < aiM->mNumVertices; ++j)
        {
            const Ogre::Real* p;
            posElem->baseVertexPointerToElement(const_cast<unsigned char*>(base + j * vbuf->getVertexSize()), &p);
            aiM->mVertices[j] = aiVector3D(p[0], p[1], p[2]);
        }
        vbuf->unlock();
    }

    // Read normals
    const auto* normElem = vData->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL);
    if (normElem)
    {
        aiM->mNormals = new aiVector3D[aiM->mNumVertices];
        auto vbuf = vData->vertexBufferBinding->getBuffer(normElem->getSource());
        auto* base = static_cast<const unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (unsigned int j = 0; j < aiM->mNumVertices; ++j)
        {
            const Ogre::Real* p;
            normElem->baseVertexPointerToElement(const_cast<unsigned char*>(base + j * vbuf->getVertexSize()), &p);
            aiM->mNormals[j] = aiVector3D(p[0], p[1], p[2]);
        }
        vbuf->unlock();
    }

    // Read texture coordinates
    const auto* tcElem = vData->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES);
    if (tcElem)
    {
        aiM->mTextureCoords[0] = new aiVector3D[aiM->mNumVertices];
        aiM->mNumUVComponents[0] = 2;
        auto vbuf = vData->vertexBufferBinding->getBuffer(tcElem->getSource());
        auto* base = static_cast<const unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (unsigned int j = 0; j < aiM->mNumVertices; ++j)
        {
            const Ogre::Real* p;
            tcElem->baseVertexPointerToElement(const_cast<unsigned char*>(base + j * vbuf->getVertexSize()), &p);
            aiM->mTextureCoords[0][j] = aiVector3D(p[0], p[1], 0.0f);
        }
        vbuf->unlock();
    }

    // Read vertex colors (diffuse) into Assimp color set 0
    const auto* colElem = vData->vertexDeclaration->findElementBySemantic(Ogre::VES_DIFFUSE);
    if (colElem)
    {
        aiM->mColors[0] = new aiColor4D[aiM->mNumVertices];
        auto vbuf = vData->vertexBufferBinding->getBuffer(colElem->getSource());
        auto* base = static_cast<const unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (unsigned int j = 0; j < aiM->mNumVertices; ++j)
        {
            const Ogre::RGBA* p;
            colElem->baseVertexPointerToElement(const_cast<unsigned char*>(base + j * vbuf->getVertexSize()), &p);
            Ogre::ColourValue cv;
            // Ogre's VET_COLOUR backing varies by render system; respect the declared element type.
            if (colElem->getType() == Ogre::VET_COLOUR_ABGR)
                cv.setAsABGR(*p);
            else
                cv.setAsARGB(*p);
            aiM->mColors[0][j] = aiColor4D(cv.r, cv.g, cv.b, cv.a);
        }
        vbuf->unlock();
    }

    // Read indices.
    //
    // Prefer the cached n-gon face list (qtme.faces.<i> on the source
    // mesh — written at import time by chunk 3 and refreshed by every
    // commit; chunk 6 of #326). When present, the exporter emits one
    // aiFace per polygon with the original vertex count, so quads /
    // n-gons survive into formats that support them (FBX, glTF, OBJ,
    // Collada all do). When absent — legacy .mesh imports, or assets
    // that have never carried n-gon data — fall back to reading the
    // triangle index buffer directly.
    std::vector<std::vector<unsigned int>> ngonFaces;
    const bool hasNgon = readNgonFacesFromMesh(
        entity->getMesh().get(), subIndex, ngonFaces);
    if (hasNgon)
    {
        // Assimp's mPrimitiveTypes is a bitmask of all primitive kinds
        // present. A submesh with mixed quads + triangles must declare
        // both bits — Assimp's exporters check the bitmask to gate
        // format-specific emission (e.g. STL emits TRIANGLE only).
        // (CodeRabbit follow-up on PR #349.)
        aiM->mPrimitiveTypes = 0;
        aiM->mNumFaces = static_cast<unsigned int>(ngonFaces.size());
        aiM->mFaces = new aiFace[aiM->mNumFaces];
        for (unsigned int f = 0; f < aiM->mNumFaces; ++f)
        {
            const auto& poly = ngonFaces[f];
            aiM->mPrimitiveTypes |= (poly.size() == 3)
                ? aiPrimitiveType_TRIANGLE
                : aiPrimitiveType_POLYGON;
            aiM->mFaces[f].mNumIndices = static_cast<unsigned int>(poly.size());
            aiM->mFaces[f].mIndices = new unsigned int[poly.size()];
            for (size_t v = 0; v < poly.size(); ++v) {
                aiM->mFaces[f].mIndices[v] = poly[v];
            }
        }
    }
    else
    {
        const Ogre::IndexData* iData = subMesh->indexData;
        if (iData && iData->indexCount > 0)
        {
            aiM->mNumFaces = static_cast<unsigned int>(iData->indexCount / 3);
            aiM->mFaces = new aiFace[aiM->mNumFaces];
            auto ibuf = iData->indexBuffer;
            auto* ibase = static_cast<const unsigned char*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
            bool use32 = ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT;

            const unsigned int indexStart = static_cast<unsigned int>(iData->indexStart);
            for (unsigned int f = 0; f < aiM->mNumFaces; ++f)
            {
                aiM->mFaces[f].mNumIndices = 3;
                aiM->mFaces[f].mIndices = new unsigned int[3];
                for (unsigned int v = 0; v < 3; ++v)
                {
                    unsigned int idx = use32
                        ? reinterpret_cast<const uint32_t*>(ibase)[indexStart + f * 3 + v]
                        : reinterpret_cast<const uint16_t*>(ibase)[indexStart + f * 3 + v];
                    aiM->mFaces[f].mIndices[v] = idx;
                }
            }
            ibuf->unlock();
        }
    }
}

// Assign bone weights from Ogre bone assignments to an aiMesh
static void assignBoneWeights(
    aiMesh* aiM,
    const Ogre::SubMesh* subMesh,
    const Ogre::MeshPtr& mesh,
    Ogre::Skeleton* skeleton,
    const std::map<unsigned short, std::string>& boneHandleToName)
{
    const auto& boneAssignments = subMesh->useSharedVertices
        ? mesh->getBoneAssignments() : subMesh->getBoneAssignments();
    std::map<unsigned short, std::vector<aiVertexWeight>> boneWeightsMap;
    for (const auto& [vertIdx, vba] : boneAssignments)
    {
        aiVertexWeight w;
        w.mVertexId = vba.vertexIndex;
        w.mWeight = vba.weight;
        boneWeightsMap[vba.boneIndex].push_back(w);
    }

    if (!boneWeightsMap.empty())
    {
        aiM->mNumBones = static_cast<unsigned int>(boneWeightsMap.size());
        aiM->mBones = new aiBone*[aiM->mNumBones];
        unsigned int bi = 0;
        for (const auto& [handle, weights] : boneWeightsMap)
        {
            auto* aiBoneObj = new aiBone();
            auto nameIt = boneHandleToName.find(handle);
            if (nameIt != boneHandleToName.end())
                aiBoneObj->mName = aiString(nameIt->second);

            auto* bone = skeleton->getBone(handle);
            Ogre::Matrix4 globalTransform = bone->_getFullTransform();
            aiBoneObj->mOffsetMatrix = toAiMatrix(globalTransform.inverse());

            aiBoneObj->mNumWeights = static_cast<unsigned int>(weights.size());
            aiBoneObj->mWeights = new aiVertexWeight[aiBoneObj->mNumWeights];
            for (unsigned int wi = 0; wi < aiBoneObj->mNumWeights; ++wi)
                aiBoneObj->mWeights[wi] = weights[wi];

            aiM->mBones[bi++] = aiBoneObj;
        }
    }
}

// Compact an aiMesh: remove unreferenced vertices, remap face indices and bone
// weights. Required when exporting LOD-reduced geometry (full vertex buffer but
// only a fraction of triangles), which otherwise causes expensive post-processing
// (aiProcess_JoinIdenticalVertices, aiProcess_OptimizeMeshes) on import.
static void compactAiMesh(aiMesh* aiM)
{
    if (!aiM || aiM->mNumVertices == 0 || aiM->mNumFaces == 0) return;

    std::vector<bool> used(aiM->mNumVertices, false);
    for (unsigned int f = 0; f < aiM->mNumFaces; ++f)
        for (unsigned int v = 0; v < aiM->mFaces[f].mNumIndices; ++v)
            if (aiM->mFaces[f].mIndices[v] < aiM->mNumVertices)
                used[aiM->mFaces[f].mIndices[v]] = true;

    std::vector<unsigned int> remap(aiM->mNumVertices, UINT_MAX);
    unsigned int newCount = 0;
    for (unsigned int i = 0; i < aiM->mNumVertices; ++i)
        if (used[i]) remap[i] = newCount++;

    if (newCount == aiM->mNumVertices) return; // nothing to compact

    for (unsigned int i = 0; i < aiM->mNumVertices; ++i) {
        if (!used[i]) continue;
        unsigned int ni = remap[i];
        aiM->mVertices[ni] = aiM->mVertices[i];
        if (aiM->mNormals)          aiM->mNormals[ni]          = aiM->mNormals[i];
        if (aiM->mTangents)         aiM->mTangents[ni]         = aiM->mTangents[i];
        if (aiM->mBitangents)       aiM->mBitangents[ni]       = aiM->mBitangents[i];
        for (int c = 0; c < AI_MAX_NUMBER_OF_COLOR_SETS; ++c)
            if (aiM->mColors[c]) aiM->mColors[c][ni] = aiM->mColors[c][i];
        for (int t = 0; t < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++t)
            if (aiM->mTextureCoords[t]) aiM->mTextureCoords[t][ni] = aiM->mTextureCoords[t][i];
    }
    aiM->mNumVertices = newCount;

    for (unsigned int f = 0; f < aiM->mNumFaces; ++f)
        for (unsigned int v = 0; v < aiM->mFaces[f].mNumIndices; ++v)
            aiM->mFaces[f].mIndices[v] = remap[aiM->mFaces[f].mIndices[v]];

    for (unsigned int bi = 0; bi < aiM->mNumBones; ++bi) {
        aiBone* bone = aiM->mBones[bi];
        unsigned int kept = 0;
        for (unsigned int wi = 0; wi < bone->mNumWeights; ++wi) {
            unsigned int vid = bone->mWeights[wi].mVertexId;
            if (vid < remap.size() && remap[vid] != UINT_MAX) {
                bone->mWeights[kept] = bone->mWeights[wi];
                bone->mWeights[kept].mVertexId = remap[vid];
                ++kept;
            }
        }
        bone->mNumWeights = kept;
    }
}

// Convert an Ogre skeleton animation to an aiAnimation
static aiAnimation* buildAiAnimation(Ogre::Animation* ogreAnim, const std::string& bonePrefix = "")
{
    auto* anim = new aiAnimation();
    anim->mName = aiString(bonePrefix + ogreAnim->getName());
    anim->mTicksPerSecond = 1.0;
    anim->mDuration = ogreAnim->getLength();

    std::vector<aiNodeAnim*> channels;
    for (const auto& [handle, track] : ogreAnim->_getNodeTrackList())
    {
        auto* bone = dynamic_cast<Ogre::Bone*>(track->getAssociatedNode());
        if (!bone) continue;

        auto* nodeAnim = new aiNodeAnim();
        nodeAnim->mNodeName = aiString(bonePrefix + std::string(bone->getName()));

        auto numKeyFrames = track->getNumKeyFrames();
        nodeAnim->mNumPositionKeys = numKeyFrames;
        nodeAnim->mNumRotationKeys = numKeyFrames;
        nodeAnim->mNumScalingKeys = numKeyFrames;
        nodeAnim->mPositionKeys = new aiVectorKey[numKeyFrames];
        nodeAnim->mRotationKeys = new aiQuatKey[numKeyFrames];
        nodeAnim->mScalingKeys = new aiVectorKey[numKeyFrames];

        Ogre::Vector3 bindPos = bone->getPosition();
        Ogre::Quaternion bindRot = bone->getOrientation();

        for (unsigned short ki = 0; ki < numKeyFrames; ++ki)
        {
            auto* kf = track->getNodeKeyFrame(ki);
            double time = kf->getTime();

            Ogre::Vector3 pos = bindPos + kf->getTranslate();
            nodeAnim->mPositionKeys[ki].mTime = time;
            nodeAnim->mPositionKeys[ki].mValue = aiVector3D(pos.x, pos.y, pos.z);

            Ogre::Quaternion rot = bindRot * kf->getRotation();
            rot.normalise();
            nodeAnim->mRotationKeys[ki].mTime = time;
            nodeAnim->mRotationKeys[ki].mValue = aiQuaternion(rot.w, rot.x, rot.y, rot.z);

            Ogre::Vector3 scl = kf->getScale();
            nodeAnim->mScalingKeys[ki].mTime = time;
            nodeAnim->mScalingKeys[ki].mValue = aiVector3D(scl.x, scl.y, scl.z);
        }
        channels.push_back(nodeAnim);
    }

    anim->mNumChannels = static_cast<unsigned int>(channels.size());
    anim->mChannels = new aiNodeAnim*[anim->mNumChannels];
    for (unsigned int ci = 0; ci < anim->mNumChannels; ++ci)
        anim->mChannels[ci] = channels[ci];

    return anim;
}

// Build an aiScene directly from an Ogre Entity, bypassing the XML round-trip
static aiScene* buildAiScene(const Ogre::Entity* entity)
{
    auto* scene = new aiScene();
    scene->mRootNode = new aiNode();
    scene->mRootNode->mName = aiString(entity->getName());

    const Ogre::MeshPtr mesh = entity->getMesh();
    const unsigned int numSub = mesh->getNumSubMeshes();
    const bool hasSkeleton = entity->hasSkeleton();
    // Use the master skeleton (from the Mesh) rather than the SkeletonInstance
    // (from the Entity) so bone positions are always the original bind pose,
    // unaffected by animation playback.
    Ogre::Skeleton* skeleton = hasSkeleton ? mesh->getSkeleton().get() : nullptr;

    // --- Materials with texture references ---
    std::vector<Ogre::MaterialPtr> materials;
    std::set<std::string, std::less<>> seen;
    for (const auto* sub : entity->getSubEntities())
    {
        auto mat = sub->getMaterial();
        if (seen.insert(mat->getName()).second)
            materials.push_back(mat);
    }

    scene->mNumMaterials = static_cast<unsigned int>(materials.size());
    scene->mMaterials = new aiMaterial*[scene->mNumMaterials];
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i)
        scene->mMaterials[i] = buildAiMaterialFromOgre(materials[i]);

    // Build material name→index map
    std::map<std::string, unsigned int, std::less<>> matIndexMap;
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i)
        matIndexMap[materials[i]->getName()] = i;

    // --- Bone hierarchy ---
    // Build a map of bone handle → bone name for vertex weight lookup
    std::map<unsigned short, std::string> boneHandleToName;
    if (hasSkeleton)
    {
        // Build bone node tree as children of root
        auto numBones = skeleton->getNumBones();
        std::vector<aiNode*> rootBoneNodes;
        for (unsigned short bi = 0; bi < numBones; ++bi)
        {
            auto* bone = skeleton->getBone(bi);
            boneHandleToName[bone->getHandle()] = bone->getName();
            // Only process root bones (no parent)
            if (!bone->getParent())
                rootBoneNodes.push_back(buildBoneNode(bone, scene->mRootNode));
        }

        // Attach bone nodes and mesh node to root
        // Root has: bone tree roots + one mesh node
        auto* meshNode = new aiNode(std::string(entity->getName()) + "_mesh");
        meshNode->mParent = scene->mRootNode;
        meshNode->mNumMeshes = numSub;
        meshNode->mMeshes = new unsigned int[numSub];
        for (unsigned int si = 0; si < numSub; ++si)
            meshNode->mMeshes[si] = si;

        scene->mRootNode->mNumChildren = static_cast<unsigned int>(rootBoneNodes.size()) + 1;
        scene->mRootNode->mChildren = new aiNode*[scene->mRootNode->mNumChildren];
        for (unsigned int i = 0; i < rootBoneNodes.size(); ++i)
            scene->mRootNode->mChildren[i] = rootBoneNodes[i];
        scene->mRootNode->mChildren[rootBoneNodes.size()] = meshNode;
    }
    else
    {
        // No skeleton — meshes directly on root node
        scene->mRootNode->mNumMeshes = numSub;
        scene->mRootNode->mMeshes = new unsigned int[numSub];
        for (unsigned int si = 0; si < numSub; ++si)
            scene->mRootNode->mMeshes[si] = si;
    }

    // --- Meshes with bone weights ---
    scene->mNumMeshes = numSub;
    scene->mMeshes = numSub > 0 ? new aiMesh*[numSub] : nullptr;

    for (unsigned int si = 0; si < numSub; ++si)
    {
        const Ogre::SubMesh* subMesh = mesh->getSubMesh(si);
        const Ogre::VertexData* vData = subMesh->useSharedVertices
            ? mesh->sharedVertexData : subMesh->vertexData;
        if (!vData) { scene->mMeshes[si] = new aiMesh(); continue; }

        auto* aiM = new aiMesh();
        scene->mMeshes[si] = aiM;
        readSubmeshGeometry(aiM, vData, subMesh, entity, si, matIndexMap);

        if (hasSkeleton)
            assignBoneWeights(aiM, subMesh, mesh, skeleton, boneHandleToName);

        compactAiMesh(aiM);
    }

    // --- Animations ---
    if (hasSkeleton && skeleton->getNumAnimations() > 0)
    {
        scene->mNumAnimations = skeleton->getNumAnimations();
        scene->mAnimations = new aiAnimation*[scene->mNumAnimations];
        for (unsigned short ai = 0; ai < skeleton->getNumAnimations(); ++ai)
            scene->mAnimations[ai] = buildAiAnimation(skeleton->getAnimation(ai));
    }

    return scene;
}

// ─── Custom Ogre XML mesh importer ───────────────────────────────
// Reads .mesh.xml using pugixml and builds the Ogre mesh manually.
// Ogre's XMLMeshSerializer::readGeometry() fails with a GL buffer
// lock error in this context, so we build buffers via
// HardwareBufferManager directly (the same way MeshProcessor does).
static Ogre::MeshPtr importOgreXmlMesh(const QString& filePath, const std::string& meshName)
{
    pugi::xml_document doc;
    if (!doc.load_file(filePath.toStdString().c_str()))
        return {};

    auto root = doc.child("mesh");
    if (!root)
        return {};

    // Remove stale resources from a previous import
    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual(meshName, "General");

    // Skeleton: try <skeletonlink> first, then auto-detect .skeleton.xml next to the mesh
    QFileInfo fi(filePath);
    QString skelPath;
    std::string skelName;

    auto skelLink = root.child("skeletonlink");
    if (skelLink)
    {
        skelName = skelLink.attribute("name").value();
        skelPath = fi.path() + "/" + QString::fromStdString(skelName);
    }

    // Auto-detect: look for baseName.skeleton.xml next to the mesh file
    if (skelPath.isEmpty() || !QFileInfo::exists(skelPath))
    {
        QString autoSkelPath = fi.path() + "/" + fi.baseName() + ".skeleton.xml";
        if (QFileInfo::exists(autoSkelPath))
        {
            skelPath = autoSkelPath;
            skelName = (fi.baseName() + ".skeleton.xml").toStdString();
        }
    }

    Ogre::SkeletonPtr skelPtr;
    if (!skelName.empty() && QFileInfo::exists(skelPath))
    {
        // Remove stale skeleton
        if (auto oldSkel = Ogre::SkeletonManager::getSingleton().getByName(skelName))
            Ogre::SkeletonManager::getSingleton().remove(oldSkel);

        try {
            // Create as non-manual so SkeletonInstance can reference it normally
            skelPtr = Ogre::SkeletonManager::getSingleton().create(skelName, "General");
            Ogre::XMLSkeletonSerializer xmlSS;
            xmlSS.importSkeleton(skelPath.toStdString().c_str(), skelPtr.get());
            // setBindingPose() saves initial bone transforms and computes the
            // derived-inverse matrices needed by _getOffsetTransform() for
            // animation.  The binary SkeletonSerializer calls this automatically
            // after import but the XML serializer does not.
            skelPtr->setBindingPose();
            // Prevent any subsequent SkeletonManager::load() call from trying to
            // re-load (and clearing) our already-populated skeleton data.
            skelPtr->setBackgroundLoaded(true);
        } catch (Ogre::Exception& e) {
            skelName.clear(); // don't link a failed skeleton
            skelPtr.reset();
            SentryReporter::addBreadcrumb("import",
                QString("XML skeleton load failed: %1").arg(e.getFullDescription().c_str()), "error");
        }
    }

    // Helper: parse a <geometry>/<vertexbuffer> and build an Ogre VertexData
    // with hardware buffers.  Returns the VertexData and updates bounds.
    struct GeomData {
        std::vector<Ogre::Vector3> positions;
        std::vector<Ogre::Vector3> normals;
        std::vector<Ogre::Vector2> texCoords;
    };

    auto parseGeometry = [](pugi::xml_node geomNode) -> GeomData {
        GeomData data;
        for (auto& vbElem : geomNode.children("vertexbuffer"))
        {
            bool hasPos = Ogre::StringConverter::parseBool(vbElem.attribute("positions").value());
            bool hasNorm = Ogre::StringConverter::parseBool(vbElem.attribute("normals").value());
            int numTC = Ogre::StringConverter::parseInt(vbElem.attribute("texture_coords").value());

            for (auto& vtx : vbElem.children("vertex"))
            {
                if (hasPos) {
                    auto p = vtx.child("position");
                    data.positions.emplace_back(
                        Ogre::StringConverter::parseReal(p.attribute("x").value()),
                        Ogre::StringConverter::parseReal(p.attribute("y").value()),
                        Ogre::StringConverter::parseReal(p.attribute("z").value()));
                }
                if (hasNorm) {
                    auto n = vtx.child("normal");
                    data.normals.emplace_back(
                        Ogre::StringConverter::parseReal(n.attribute("x").value()),
                        Ogre::StringConverter::parseReal(n.attribute("y").value()),
                        Ogre::StringConverter::parseReal(n.attribute("z").value()));
                }
                if (numTC > 0) {
                    auto t = vtx.child("texcoord");
                    data.texCoords.emplace_back(
                        Ogre::StringConverter::parseReal(t.attribute("u").value()),
                        Ogre::StringConverter::parseReal(t.attribute("v").value()));
                }
            }
        }
        return data;
    };

    Ogre::Vector3 minCoords(Ogre::Vector3::ZERO), maxCoords(Ogre::Vector3::ZERO);
    bool firstVertex = true;

    // Helper: build hardware vertex/index buffers from parsed geometry data
    auto buildVertexData = [&](const GeomData& geom) -> Ogre::VertexData*
    {
        auto* vertexData = new Ogre::VertexData();
        auto* decl = vertexData->vertexDeclaration;
        size_t offset = decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION).getSize();
        if (!geom.normals.empty())
            offset += decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL).getSize();
        if (!geom.texCoords.empty())
            decl->addElement(0, offset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

        vertexData->vertexCount = geom.positions.size();

        auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            decl->getVertexSize(0), vertexData->vertexCount,
            Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float* pVertex = static_cast<float*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));

        for (size_t i = 0; i < geom.positions.size(); ++i)
        {
            *pVertex++ = geom.positions[i].x;
            *pVertex++ = geom.positions[i].y;
            *pVertex++ = geom.positions[i].z;
            if (!geom.normals.empty()) {
                *pVertex++ = geom.normals[i].x;
                *pVertex++ = geom.normals[i].y;
                *pVertex++ = geom.normals[i].z;
            }
            if (!geom.texCoords.empty()) {
                *pVertex++ = geom.texCoords[i].x;
                *pVertex++ = geom.texCoords[i].y;
            }
            const auto& pos = geom.positions[i];
            if (firstVertex) { minCoords = maxCoords = pos; firstVertex = false; }
            else { minCoords.makeFloor(pos); maxCoords.makeCeil(pos); }
        }
        vbuf->unlock();
        vertexData->vertexBufferBinding->setBinding(0, vbuf);
        return vertexData;
    };

    auto buildIndexBuffer = [](Ogre::SubMesh* subMesh, const std::vector<unsigned int>& indices,
                               size_t vertexCount)
    {
        if (indices.empty()) return;
        bool use32bit = vertexCount > 65535;
        auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
            use32bit ? Ogre::HardwareIndexBuffer::IT_32BIT : Ogre::HardwareIndexBuffer::IT_16BIT,
            indices.size(), Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        if (use32bit) {
            auto* p = static_cast<unsigned int*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
            for (auto idx : indices) *p++ = idx;
        } else {
            auto* p = static_cast<unsigned short*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
            for (auto idx : indices) *p++ = static_cast<unsigned short>(idx);
        }
        ibuf->unlock();
        subMesh->indexData->indexCount = indices.size();
        subMesh->indexData->indexBuffer = ibuf;
    };

    // Build shared vertex data on the mesh (if the XML has <sharedgeometry>)
    auto sharedGeomNode = root.child("sharedgeometry");
    if (sharedGeomNode)
    {
        auto geom = parseGeometry(sharedGeomNode);
        if (!geom.positions.empty())
            mesh->sharedVertexData = buildVertexData(geom);
    }

    // Parse submeshes — preserve shared-vertex flag
    for (auto& smElem : root.child("submeshes").children("submesh"))
    {
        Ogre::SubMesh* subMesh = mesh->createSubMesh();

        // Material
        const char* mat = smElem.attribute("material").as_string(nullptr);
        if (mat && mat[0]) {
            auto existing = Ogre::MaterialManager::getSingleton().getByName(mat, Ogre::RGN_DEFAULT);
            if (!existing) existing = Ogre::MaterialManager::getSingleton().getByName(mat);
            subMesh->setMaterial(existing ? existing
                : Ogre::MaterialManager::getSingleton().create(mat, Ogre::RGN_DEFAULT));
        }

        // Parse faces
        std::vector<unsigned int> indices;
        auto faces = smElem.child("faces");
        if (faces) {
            for (auto& f : faces.children("face")) {
                indices.push_back(Ogre::StringConverter::parseUnsignedInt(f.attribute("v1").value()));
                indices.push_back(Ogre::StringConverter::parseUnsignedInt(f.attribute("v2").value()));
                indices.push_back(Ogre::StringConverter::parseUnsignedInt(f.attribute("v3").value()));
            }
        }

        bool useShared = Ogre::StringConverter::parseBool(smElem.attribute("usesharedvertices").value());
        if (useShared && mesh->sharedVertexData)
        {
            // Reference the mesh-level shared vertex data
            subMesh->useSharedVertices = true;
            buildIndexBuffer(subMesh, indices, mesh->sharedVertexData->vertexCount);
        }
        else
        {
            // Per-submesh geometry
            auto geom = parseGeometry(smElem.child("geometry"));
            if (geom.positions.empty()) continue;
            subMesh->useSharedVertices = false;
            subMesh->vertexData = buildVertexData(geom);
            buildIndexBuffer(subMesh, indices, geom.positions.size());

            // Per-submesh bone assignments
            auto boneAssigns = smElem.child("boneassignments");
            if (boneAssigns) {
                for (auto& ba : boneAssigns.children("vertexboneassignment")) {
                    Ogre::VertexBoneAssignment vba;
                    vba.vertexIndex = Ogre::StringConverter::parseUnsignedInt(ba.attribute("vertexindex").value());
                    vba.boneIndex = Ogre::StringConverter::parseUnsignedInt(ba.attribute("boneindex").value());
                    vba.weight = Ogre::StringConverter::parseReal(ba.attribute("weight").value());
                    subMesh->addBoneAssignment(vba);
                }
            }
        }
    }

    // Mesh-level (shared) bone assignments — stored on the mesh, not submeshes
    auto sharedBoneAssigns = root.child("boneassignments");
    if (sharedBoneAssigns) {
        for (auto& ba : sharedBoneAssigns.children("vertexboneassignment")) {
            Ogre::VertexBoneAssignment vba;
            vba.vertexIndex = Ogre::StringConverter::parseUnsignedInt(ba.attribute("vertexindex").value());
            vba.boneIndex = Ogre::StringConverter::parseUnsignedInt(ba.attribute("boneindex").value());
            vba.weight = Ogre::StringConverter::parseReal(ba.attribute("weight").value());
            mesh->addBoneAssignment(vba);
        }
    }

    // Link skeleton
    if (skelPtr) {
        mesh->_notifySkeleton(skelPtr);
    }

    // Set bounds and finalize
    mesh->_setBounds(Ogre::AxisAlignedBox(minCoords, maxCoords));
    mesh->_setBoundingSphereRadius((maxCoords - minCoords).length() / 2.0f);
    mesh->load();

    return mesh;
}

namespace {

bool meshHasTextureCoordinates(const Ogre::Mesh* mesh)
{
    if (!mesh)
        return false;
    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
        const Ogre::SubMesh* sm = mesh->getSubMesh(i);
        const Ogre::VertexData* vd = sm->useSharedVertices ? mesh->sharedVertexData : sm->vertexData;
        if (vd && vd->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES))
            return true;
    }
    return false;
}

} // namespace

// Apply RTSS normal map shaders to any materials that have a normal-map texture
// unit. Called after loading .mesh/.xml files where MaterialProcessor doesn't run.
// Also reachable as MeshImporterExporter::applyNormalMapsToEntity for callers
// that need to refresh bump-map RTSS state after a topology change wiped it
// (chunk 4b: Edit Mode subdivide / extrude / etc.).
void MeshImporterExporter::applyNormalMapsToEntity(const Ogre::Entity* en)
{
    if (!en) return;
    auto& log = Ogre::LogManager::getSingleton();

    // Build tangent vectors on the mesh if they're missing — required by RTSS normal mapping.
    if (auto mesh = en->getMesh()) {
        bool hasTangents = false;
        const auto* vd = mesh->getVertexDataByTrackHandle(0);
        if (!vd && mesh->sharedVertexData)
            vd = mesh->sharedVertexData;
        if (vd && vd->vertexDeclaration->findElementBySemantic(Ogre::VES_TANGENT))
            hasTangents = true;
        if (!hasTangents && meshHasTextureCoordinates(mesh.get())) {
            try {
                // storeParityInW=true → VET_FLOAT4 tangents with handedness in w,
                // matching what MeshProcessor exports and what RTSS expects.
                mesh->buildTangentVectors(Ogre::VES_TANGENT, 0, 0, false, false, true);
                log.logMessage("applyNormalMapsToEntity: built tangents for '" +
                               mesh->getName() + "'");
            } catch (const Ogre::Exception& e) {
                log.logMessage("applyNormalMapsToEntity: could not build tangents for '" +
                               mesh->getName() + "': " + e.getDescription());
            }
        }
    }

    for (const auto* subEnt : en->getSubEntities()) {
        auto mat = subEnt->getMaterial();
        if (!mat) continue;
        // Ensure the material is fully loaded so TUS names are populated.
        // `load()` can throw on broken/unresolvable resources; this hook
        // runs on every topology mutation AND undo/redo, so a single
        // bad material shouldn't abort the edit op. Skip the offending
        // sub-entity and let the rest of the entity refresh.
        if (!mat->isLoaded()) {
            try {
                mat->load();
            } catch (const Ogre::Exception& e) {
                log.logMessage("applyNormalMapsToEntity: skipping mat '"
                    + mat->getName() + "' — load failed: " + e.getDescription());
                continue;
            }
        }
        if (mat->getNumTechniques() == 0) continue;
        auto* pass = mat->getTechnique(0)->getPass(0);
        if (!pass) continue;
        for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
            auto* tus = pass->getTextureUnitState(i);
            const auto& tusName = tus->getName();
            if (tusName == "normal_map" || tusName == "NormalMap") {
                std::string texName = tus->getTextureName();
                log.logMessage("applyNormalMapsToEntity: found normal map TUS '" +
                               tusName + "' tex='" + texName + "' on mat='" + mat->getName() + "'",
                               Ogre::LML_TRIVIAL);
                if (texName.empty()) break;
                // Ensure the texture is loaded before RTSS inspects it.
                auto tex = Ogre::TextureManager::getSingleton().getByName(texName);
                if (!tex || !tex->isLoaded()) {
                    try {
                        Ogre::TextureManager::getSingleton().load(texName, mat->getGroup());
                    } catch (...) {
                        try {
                            Ogre::TextureManager::getSingleton().load(
                                texName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
                        } catch (...) {}
                    }
                }
                RTShaderHelper::applyNormalMap(mat, texName);
                break;
            }
        }
    }
}

static void ensureResourceGroup(const QString &path)
{
    const QString absPath = QFileInfo(path).absoluteFilePath();
    auto group = absPath.toStdString();
    auto &rgm = Ogre::ResourceGroupManager::getSingleton();

    // If the directory was already registered earlier in this process (common in tests and CLI),
    // Ogre's FileSystem archive may have been initialised before new files were written there.
    // Re-registering the same location forces Ogre to re-scan the directory so freshly exported
    // `.mesh` / `.material` sidecars are discoverable by name.
    if (const bool haveLocation = rgm.resourceLocationExists(group, group);
        haveLocation && rgm.isResourceGroupInitialised(group))
    {
        try {
            rgm.removeResourceLocation(group, group);
        } catch (const Ogre::Exception& e) {
            Ogre::LogManager::getSingleton().logMessage(
                "Note: removeResourceLocation: " + e.getFullDescription());
        }
    }

    // Ensure the location exists, then (re)initialise to refresh the file listing.
    try {
        if (!rgm.resourceLocationExists(group, group))
            rgm.addResourceLocation(group, "FileSystem", group);
        rgm.initialiseResourceGroup(group);
    } catch (const Ogre::Exception& e) {
        Ogre::LogManager::getSingleton().logMessage(
            "Warning during resource group init: " + e.getFullDescription());
    }
}

/** @return true if at least one declared material exists in the manager for this group. */
static bool loadMaterialsDeclaredInOgreMaterialScript(const QByteArray& script, const Ogre::String& group)
{
    bool anyFound = false;
    const QList<QByteArray> lines = script.split('\n');
    for (QByteArray raw : lines)
    {
        QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith("//"))
            continue;
        if (line.size() < 10)
            continue;
        if (line.left(8).compare(QByteArrayLiteral("material"), Qt::CaseInsensitive) != 0)
            continue;
        const char boundary = line[8];
        if (boundary != ' ' && boundary != '\t')
            continue; // e.g. "materials" must not match

        QByteArray rest = line.mid(9).trimmed();
        if (rest.isEmpty())
            continue;

        int end = 0;
        while (end < rest.size())
        {
            const char c = rest[end];
            if (c == ' ' || c == '\t' || c == ':' || c == '{')
                break;
            ++end;
        }
        if (end <= 0)
            continue;

        const QByteArray matName = rest.left(end).trimmed();
        if (matName.isEmpty())
            continue;
        Ogre::MaterialPtr m = Ogre::MaterialManager::getSingleton().getByName(
            Ogre::String(matName.constData(), matName.size()), group);
        if (m)
        {
            anyFound = true;
            m->load();
        }
    }
    return anyFound;
}

/** @return first declared material name in script (ASCII), or empty. */
static QString firstMaterialNameInOgreMaterialScript(const QByteArray& script)
{
    const QList<QByteArray> lines = script.split('\n');
    for (QByteArray raw : lines) {
        QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith("//"))
            continue;
        if (line.size() < 10)
            continue;
        if (line.left(8).compare(QByteArrayLiteral("material"), Qt::CaseInsensitive) != 0)
            continue;
        const char boundary = line[8];
        if (boundary != ' ' && boundary != '\t')
            continue;
        QByteArray rest = line.mid(9).trimmed();
        if (rest.isEmpty())
            continue;
        int end = 0;
        while (end < rest.size()) {
            const char c = rest[end];
            if (c == ' ' || c == '\t' || c == ':' || c == '{')
                break;
            ++end;
        }
        if (end <= 0)
            continue;
        const QByteArray matName = rest.left(end).trimmed();
        if (!matName.isEmpty())
            return QString::fromUtf8(matName);
    }
    return {};
}

/// Load `texPath` into Ogre as a manual texture under `resourceName`, replacing any existing
/// entry. Handles common 2D formats (PNG/JPG/BMP/TGA/...) by leaning on Qt's QImage decoder
/// when the file extension is not one Ogre's built-in codecs already register. Returns true
/// on success.
static bool loadExternalTextureForRsd(const QString& texPath,
                                      const Ogre::String& resourceName,
                                      QString* outError = nullptr)
{
    if (texPath.isEmpty() || !QFileInfo::exists(texPath)) {
        if (outError) *outError = QStringLiteral("Texture file not found: %1").arg(texPath);
        return false;
    }
    Ogre::Image img;

    // Try Ogre's native codecs first via the file extension (cheap, no QImage decode).
    const QString ext = QFileInfo(texPath).suffix().toLower();
    bool loaded = false;
    if (!ext.isEmpty()) {
        try {
            QFile f(texPath);
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray bytes = f.readAll();
                Ogre::DataStreamPtr ds(new Ogre::MemoryDataStream(
                    const_cast<char*>(bytes.constData()),
                    static_cast<size_t>(bytes.size()),
                    /*freeOnClose*/ false,
                    /*readOnly*/ true));
                img.load(ds, ext.toStdString());
                loaded = (img.getWidth() > 0 && img.getHeight() > 0);
            }
        } catch (const Ogre::Exception&) {
            loaded = false;
        }
    }

    if (!loaded) {
        // Fall back to QImage so e.g. .jpg works even when no codec plugin is registered.
        QImage qi(texPath);
        if (qi.isNull()) {
            if (outError) *outError = QStringLiteral("Could not decode image: %1").arg(texPath);
            return false;
        }
        QImage rgba = qi.convertToFormat(QImage::Format_RGBA8888);
        const size_t bytes = static_cast<size_t>(rgba.sizeInBytes());
        // Ogre takes ownership of `data` when autoDelete=true (loadDynamicImage with autoDelete).
        Ogre::uchar* data = OGRE_ALLOC_T(Ogre::uchar, bytes, Ogre::MEMCATEGORY_GENERAL);
        std::memcpy(data, rgba.constBits(), bytes);
        try {
            img.loadDynamicImage(data,
                                 static_cast<uint32_t>(rgba.width()),
                                 static_cast<uint32_t>(rgba.height()),
                                 1, // depth
                                 Ogre::PF_BYTE_RGBA,
                                 true /* autoDelete: Ogre owns `data` */);
        } catch (const Ogre::Exception& e) {
            OGRE_FREE(data, Ogre::MEMCATEGORY_GENERAL);
            if (outError) *outError = QString::fromStdString(e.getFullDescription());
            return false;
        }
    }

    auto& tm = Ogre::TextureManager::getSingleton();
    if (tm.resourceExists(resourceName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME))
        tm.remove(resourceName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    try {
        tm.loadImage(resourceName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, img);
    } catch (const Ogre::Exception& e) {
        if (outError) *outError = QString::fromStdString(e.getFullDescription());
        return false;
    }
    return true;
}

static void applyTextureMaterialToEntity(Ogre::Entity* entity,
                                              const QString& materialName,
                                              const QString& textureResourceNameOrEmpty)
{
    if (!entity)
        return;
    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().getByName(
        materialName.toStdString(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!mat) {
        mat = Ogre::MaterialManager::getSingleton().create(
            materialName.toStdString(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    }
    mat->removeAllTechniques();
    Ogre::Technique* tech = mat->createTechnique();
    Ogre::Pass* pass = tech->createPass();
    pass->setLightingEnabled(true);
    pass->setAmbient(1.0f, 1.0f, 1.0f);
    pass->setDiffuse(1.0f, 1.0f, 1.0f, 1.0f);
    pass->setEmissive(0.0f, 0.0f, 0.0f);
    pass->removeAllTextureUnitStates();
    if (!textureResourceNameOrEmpty.isEmpty())
        pass->createTextureUnitState(textureResourceNameOrEmpty.toStdString());
    mat->load();

    for (unsigned int si = 0; si < entity->getNumSubEntities(); ++si) {
        if (auto* se = const_cast<Ogre::SubEntity*>(entity->getSubEntity(si)))
            se->setMaterial(mat);
    }
}

static void applySolidColorMaterialToEntity(Ogre::Entity* entity,
                                                 const QString& materialName,
                                                 const QColor& rgb)
{
    if (!entity)
        return;
    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().getByName(
        materialName.toStdString(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!mat) {
        mat = Ogre::MaterialManager::getSingleton().create(
            materialName.toStdString(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    }
    mat->removeAllTechniques();
    Ogre::Technique* tech = mat->createTechnique();
    Ogre::Pass* pass = tech->createPass();
    pass->setLightingEnabled(true);
    const Ogre::Real r = Ogre::Real(rgb.redF());
    const Ogre::Real g = Ogre::Real(rgb.greenF());
    const Ogre::Real b = Ogre::Real(rgb.blueF());
    pass->setAmbient(r, g, b);
    pass->setDiffuse(r, g, b, 1.0f);
    pass->setEmissive(0.0f, 0.0f, 0.0f);
    pass->removeAllTextureUnitStates();
    mat->load();

    for (unsigned int si = 0; si < entity->getNumSubEntities(); ++si) {
        if (auto* se = const_cast<Ogre::SubEntity*>(entity->getSubEntity(si)))
            se->setMaterial(mat);
    }
}

/** Parse `<mesh-complete-basename>.material` next to the `.mesh` file (uses completeBaseName so `a.v2.mesh` → `a.v2.material`). */
static void tryLoadSidecarMaterialScript(const QFileInfo& meshFile)
{
    // `.mesh` stores material names, but not the script definitions. If the
    // corresponding `.material` isn't loaded, Ogre falls back to BaseWhite.
    const QString sidecar =
        meshFile.path() + "/" + meshFile.completeBaseName() + ".material";
    QFile f(sidecar);
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return;

    QByteArray scriptBytes = f.readAll();
    if (scriptBytes.isEmpty())
        return;

    const Ogre::String group = meshFile.path().toStdString();
    try {
        auto& rgm = Ogre::ResourceGroupManager::getSingleton();
        if (rgm.isResourceGroupInitialised(group))
            rgm.loadResourceGroup(group);
        else
            rgm.initialiseResourceGroup(group);

        auto sweepUnloadedInGroup = [&group]() {
            Ogre::ResourceManager::ResourceMapIterator mit =
                Ogre::MaterialManager::getSingleton().getResourceIterator();
            while (mit.hasMoreElements())
            {
                Ogre::ResourcePtr res = mit.peekNextValue();
                if (res->getGroup() == group
                    && res->getLoadingState() == Ogre::Resource::LOADSTATE_UNLOADED)
                    res->load();
                mit.moveNext();
            }
        };

        bool haveDeclared = loadMaterialsDeclaredInOgreMaterialScript(scriptBytes, group);
        sweepUnloadedInGroup();

        // If the folder was initialised before the .material existed, or the filesystem
        // parser skipped it, inject the script once from memory (avoid when already declared
        // — duplicate parse throws and would skip loadResourceGroup if caught too broadly).
        if (!haveDeclared)
        {
            void* scriptMem = scriptBytes.data();
            Ogre::DataStreamPtr ds(new Ogre::MemoryDataStream(
                scriptMem,
                static_cast<size_t>(scriptBytes.size()),
                false,
                true));
            Ogre::MaterialManager::getSingleton().parseScript(ds, group);
            if (rgm.isResourceGroupInitialised(group))
                rgm.loadResourceGroup(group);
            loadMaterialsDeclaredInOgreMaterialScript(scriptBytes, group);
            sweepUnloadedInGroup();
        }

        SentryReporter::addBreadcrumb("file.import",
            QStringLiteral("Loaded sidecar material: %1").arg(sidecar));
    } catch (const Ogre::Exception& e) {
        Ogre::LogManager::getSingleton().logMessage(
            "Warning: sidecar material load: " + e.getFullDescription());
    }
}

void MeshImporterExporter::importer(const QStringList &_uriList, unsigned int additionalFlags,
                                     QList<Ogre::SkeletonPtr>* outAnimOnlySkeletons,
                                     int* outUpAxis)
{
    try{
        foreach(const QString &fileName,_uriList)
        {
            if(!fileName.size()) continue;

            const QFileInfo file(QFileInfo(fileName).absoluteFilePath());
            ensureResourceGroup(file.absolutePath());

            Ogre::SceneNode *sn;
            const Ogre::Entity *en;

            if(!file.suffix().compare("mesh",Qt::CaseInsensitive))
            {
                tryLoadSidecarMaterialScript(file);
                const Ogre::String meshResName = file.fileName().toStdString();
                const Ogre::String meshGroup = file.absolutePath().toStdString();
                // Drop any cached mesh so a replaced file on disk is re-read (tests/CLI reuse paths).
                if (auto existing = Ogre::MeshManager::getSingleton().getByName(meshResName, meshGroup))
                    Ogre::MeshManager::getSingleton().remove(existing);
                Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().load(
                    meshResName, meshGroup);
                if (!mesh)
                    continue;

                sn = Manager::getSingleton()->addSceneNode(QString(file.baseName()));
                en = Manager::getSingleton()->createEntity(sn, mesh);
                if (en->getMesh() && en->getMesh()->getSkeleton())
                    AnimationMerger::registerSkeletonUpAxis(en->getMesh()->getSkeleton()->getName(), 1);
                applyNormalMapsToEntity(en);
            }
            else if(!file.suffix().compare("xml",Qt::CaseInsensitive))
            {
                auto meshName = file.baseName();
                Ogre::MeshPtr mesh = importOgreXmlMesh(file.filePath(), meshName.toStdString());
                if (!mesh) return;

                sn = Manager::getSingleton()->addSceneNode(meshName);
                en = Manager::getSingleton()->createEntity(sn, mesh);
                if (en->getMesh() && en->getMesh()->getSkeleton())
                    AnimationMerger::registerSkeletonUpAxis(en->getMesh()->getSkeleton()->getName(), 1);
                applyNormalMapsToEntity(en);
            }
            else if (!file.suffix().compare(QStringLiteral("tmd"), Qt::CaseInsensitive))
            {
                const std::string meshName = (file.baseName() + QStringLiteral("_tmd")).toStdString();
                Ogre::MeshPtr mesh = PS1TMD::importTmd(file.filePath(), meshName);
                if (!mesh) {
                    QMessageBox::warning(
                        nullptr,
                        QStringLiteral("PlayStation TMD"),
                        QStringLiteral("Could not import %1 — invalid file or unsupported primitive types.")
                            .arg(file.fileName()));
                    continue;
                }

                SentryReporter::addBreadcrumb(
                    QStringLiteral("file.import"),
                    QStringLiteral("Imported PlayStation TMD: %1").arg(file.fileName()));

                sn = Manager::getSingleton()->addSceneNode(file.baseName());
                en = Manager::getSingleton()->createEntity(sn, mesh);
                applyNormalMapsToEntity(en);
            }
            else if (!file.suffix().compare(QStringLiteral("rsd"), Qt::CaseInsensitive))
            {
                PS1RSD::RsdDescriptor rsd;
                QString err;
                if (!PS1RSD::parseRsdFile(file.filePath(), rsd, &err)) {
                    QMessageBox::warning(
                        nullptr,
                        QStringLiteral("PlayStation RSD"),
                        QStringLiteral("Could not parse %1: %2").arg(file.fileName(), err));
                    continue;
                }

                // Preload referenced textures. RSD/TEX[] may point at PS1 TIMs (Psy-Q toolchains)
                // OR at modern raster formats like JPG/PNG (Blender-RSD exporter pipeline) — we
                // try TIM first, then fall back to QImage-decoded loaders.
                const QString rsdDir = file.absolutePath();
                const auto resolve = [&rsdDir](const QString& rel) -> QString {
                    if (rel.isEmpty()) return {};
                    const QFileInfo fi(rel);
                    return fi.isAbsolute() ? fi.absoluteFilePath() : QDir(rsdDir).filePath(rel);
                };

                struct RsdTextureSlot {
                    QString resourceName;   ///< Ogre resource name (or empty when not loaded).
                    int width = 0;
                    int height = 0;
                };
                std::vector<RsdTextureSlot> rsdTexSlots(rsd.textures.size());
                QString firstTimResource;
                for (int ti = 0; ti < rsd.textures.size(); ++ti) {
                    const QString texRel = rsd.textures[ti];
                    const QString texPath = resolve(texRel);
                    if (texPath.isEmpty() || !QFileInfo::exists(texPath))
                        continue;

                    Ogre::Image img;
                    bool loaded = false;
                    const QString suffix = QFileInfo(texPath).suffix().toLower();
                    if (suffix == QStringLiteral("tim")) {
                        QString timErr;
                        loaded = PS1TIM::loadTimToOgreImage(texPath, img, &timErr);
                    }

                    const QString resName = QFileInfo(texPath).completeBaseName()
                        + QStringLiteral(".")
                        + (suffix.isEmpty() ? QStringLiteral("tex") : suffix);
                    const Ogre::String ogreName = resName.toStdString();

                    if (loaded) {
                        if (Ogre::TextureManager::getSingleton().resourceExists(
                                ogreName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME))
                            Ogre::TextureManager::getSingleton().remove(
                                ogreName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
                        Ogre::TexturePtr tex = Ogre::TextureManager::getSingleton().loadImage(
                            ogreName,
                            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
                            img);
                        rsdTexSlots[ti].resourceName = resName;
                        rsdTexSlots[ti].width = static_cast<int>(img.getWidth());
                        rsdTexSlots[ti].height = static_cast<int>(img.getHeight());
                    } else {
                        // Non-TIM raster format (PNG/JPG/BMP/TGA…)
                        QString extErr;
                        if (loadExternalTextureForRsd(texPath, ogreName, &extErr)) {
                            rsdTexSlots[ti].resourceName = resName;
                            // Reuse the texture we just loaded — no need to redecode the
                            // file with QImage just to measure dimensions.
                            if (auto tex = Ogre::TextureManager::getSingleton().getByName(
                                    ogreName,
                                    Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)) {
                                rsdTexSlots[ti].width = static_cast<int>(tex->getWidth());
                                rsdTexSlots[ti].height = static_cast<int>(tex->getHeight());
                            }
                        } else {
                            Ogre::LogManager::getSingleton().logMessage(
                                "Warning: RSD texture load failed for "
                                + texPath.toStdString() + ": " + extErr.toStdString());
                        }
                    }
                    if (firstTimResource.isEmpty() && !rsdTexSlots[ti].resourceName.isEmpty())
                        firstTimResource = rsdTexSlots[ti].resourceName;
                }

                // If a MAT sidecar exists, try to interpret it.
                // - If it looks like an Ogre material script, load it into the RSD directory group.
                // - Otherwise, treat it as a PS1/Psy-Q descriptor (typed entries with UVs and colours).
                const QString matPath = resolve(rsd.matPath);
                QString rsdMaterialFromScript;
                QVector<PS1MAT::MatEntry> rsdMatEntries; ///< Full Psy-Q MAT entries (UVs + colours + texIdx).
                QVector<QColor> rsdFaceColors;          ///< Per-face flat colour fallback (back-compat).
                if (!matPath.isEmpty() && QFileInfo::exists(matPath)) {
                    QFile matFile(matPath);
                    if (matFile.open(QIODevice::ReadOnly)) {
                        const QByteArray matBytes = matFile.readAll();
                        const QString firstMatName = firstMaterialNameInOgreMaterialScript(matBytes);
                        if (!firstMatName.isEmpty()) {
                            const Ogre::String group = rsdDir.toStdString();
                            try {
                                void* scriptMem = const_cast<char*>(matBytes.constData());
                                Ogre::DataStreamPtr ds(new Ogre::MemoryDataStream(
                                    scriptMem,
                                    static_cast<size_t>(matBytes.size()),
                                    false,
                                    true));
                                Ogre::MaterialManager::getSingleton().parseScript(ds, group);
                                Ogre::ResourceGroupManager::getSingleton().initialiseResourceGroup(group);
                                Ogre::ResourceGroupManager::getSingleton().loadResourceGroup(group);
                                rsdMaterialFromScript = firstMatName;
                            } catch (const Ogre::Exception& e) {
                                Ogre::LogManager::getSingleton().logMessage(
                                    "Warning: .mat material script load: " + e.getFullDescription());
                            }
                        }
                    }

                    // Psy-Q MAT (not an Ogre script): keep the full per-face descriptor so we can
                    // route UVs + texture indices into the textured PLY import path.
                    if (rsdMaterialFromScript.isEmpty()) {
                        QString matErr;
                        if (PS1MAT::parseMatFile(matPath, rsdMatEntries, &matErr) && !rsdMatEntries.isEmpty()) {
                            rsdFaceColors.reserve(rsdMatEntries.size());
                            for (const auto& me : rsdMatEntries)
                                rsdFaceColors.push_back(me.rgb);
                        }
                    }
                }

                // Resolve geometry target: prefer PLY= entry.
                const QString geomRel = rsd.plyPath;
                const QString geomPath = resolve(geomRel);
                if (geomPath.isEmpty() || !QFileInfo::exists(geomPath)) {
                    QMessageBox::warning(
                        nullptr,
                        QStringLiteral("PlayStation RSD"),
                        QStringLiteral("RSD does not reference an existing geometry file (PLY=...)."));
                    continue;
                }

                // Decide whether to use the textured import path. We do so when MAT entries were
                // parsed AND at least one of them is textured (T/H/D) — otherwise we fall back to
                // the simpler vertex-color path for performance and to keep submesh count low.
                bool useTexturedMatPath = false;
                if (!rsdMatEntries.isEmpty()) {
                    for (const auto& me : rsdMatEntries) {
                        if (me.textured && me.textureIndex >= 0
                            && me.textureIndex < static_cast<int>(rsdTexSlots.size())
                            && !rsdTexSlots[me.textureIndex].resourceName.isEmpty()) {
                            useTexturedMatPath = true;
                            break;
                        }
                    }
                }

                Ogre::MeshPtr mesh;
                const QFileInfo geomFi(geomPath);
                if (!geomFi.suffix().compare(QStringLiteral("tmd"), Qt::CaseInsensitive)) {
                    const std::string meshName = (file.baseName() + QStringLiteral("_rsd_tmd")).toStdString();
                    mesh = PS1TMD::importTmd(geomPath, meshName);
                } else if (!geomFi.suffix().compare(QStringLiteral("ply"), Qt::CaseInsensitive)
                           && PS1PLY::isPsyqPlyFile(geomPath)) {
                    const std::string meshName = (file.baseName() + QStringLiteral("_rsd_ply")).toStdString();
                    if (useTexturedMatPath) {
                        // Convert the Psy-Q MAT entries into per-face PLY material bindings: each MAT
                        // entry maps 1:1 to a PLY face (in declaration order). UVs are normalised by
                        // the bound texture's width/height; colours retain the PS1 corner ordering.
                        QVector<PS1PLY::FaceMaterial> faceMats(rsdMatEntries.size());
                        for (int fi = 0; fi < rsdMatEntries.size(); ++fi) {
                            const PS1MAT::MatEntry& me = rsdMatEntries[fi];
                            PS1PLY::FaceMaterial fm;
                            fm.textured = me.textured;
                            fm.textureIndex = me.textureIndex;
                            int texW = 256, texH = 256;
                            if (me.textured
                                && me.textureIndex >= 0
                                && me.textureIndex < static_cast<int>(rsdTexSlots.size())) {
                                if (rsdTexSlots[me.textureIndex].width > 0)
                                    texW = rsdTexSlots[me.textureIndex].width;
                                if (rsdTexSlots[me.textureIndex].height > 0)
                                    texH = rsdTexSlots[me.textureIndex].height;
                            }
                            for (int k = 0; k < me.uvs.size() && k < 4; ++k) {
                                fm.u[k] = float(me.uvs[k].u) / float(texW);
                                fm.v[k] = float(me.uvs[k].v) / float(texH);
                            }
                            if (!me.vertColors.isEmpty()) {
                                fm.vertColors = me.vertColors;
                                fm.color = me.vertColors.first();
                            } else if (me.rgb.isValid()) {
                                fm.color = me.rgb;
                            }
                            faceMats[fi] = fm;
                        }
                        mesh = PS1PLY::importPsyqPlyWithFaceMaterials(geomPath, meshName, faceMats);
                        // Fall back to the simpler vertex-colour path if the textured importer
                        // rejected the file (e.g. face-count mismatch).
                        if (!mesh)
                            mesh = rsdFaceColors.isEmpty()
                                ? PS1PLY::importPsyqPly(geomPath, meshName)
                                : PS1PLY::importPsyqPlyWithFaceColors(geomPath, meshName, rsdFaceColors);
                    } else {
                        mesh = rsdFaceColors.isEmpty()
                            ? PS1PLY::importPsyqPly(geomPath, meshName)
                            : PS1PLY::importPsyqPlyWithFaceColors(geomPath, meshName, rsdFaceColors);
                    }
                } else {
                    AssimpToOgreImporter importer;
                    bool convertLH = (geomFi.suffix().compare("x", Qt::CaseInsensitive) != 0);
                    const std::string sourcePath = geomPath.toStdString();
                    mesh = importer.loadModel(sourcePath, convertLH, additionalFlags);
                    if (outUpAxis) *outUpAxis = importer.getSceneUpAxis();
                }

                if (!mesh) {
                    QMessageBox::warning(
                        nullptr,
                        QStringLiteral("PlayStation RSD"),
                        QStringLiteral("Could not import geometry referenced by %1").arg(file.fileName()));
                    continue;
                }

                SentryReporter::addBreadcrumb(
                    QStringLiteral("file.import"),
                    QStringLiteral("Imported PlayStation RSD: %1").arg(file.fileName()));

                sn = Manager::getSingleton()->addSceneNode(file.baseName());
                en = Manager::getSingleton()->createEntity(sn, mesh);
                applyNormalMapsToEntity(en);

                // Apply MAT override if we loaded a script-defined material.
                if (!rsdMaterialFromScript.isEmpty()) {
                    for (unsigned int si = 0; si < en->getNumSubEntities(); ++si) {
                        if (auto* se = const_cast<Ogre::SubEntity*>(en->getSubEntity(si)))
                            se->setMaterialName(rsdMaterialFromScript.toStdString(), rsdDir.toStdString());
                    }
                } else if (useTexturedMatPath) {
                    // Textured PLY path emits one submesh per texture group with material names
                    // shaped `PLY/<meshName>_texN` or `PLY/<meshName>_solid`. Bind the matching
                    // RSD texture slot to each textured submesh's first texture unit; untextured
                    // submeshes keep their vertex-colour material.
                    for (unsigned int si = 0; si < en->getNumSubEntities(); ++si) {
                        Ogre::SubEntity* se = const_cast<Ogre::SubEntity*>(en->getSubEntity(si));
                        Ogre::MaterialPtr mat = se->getMaterial();
                        if (mat.isNull() || mat->getNumTechniques() == 0
                            || mat->getTechnique(0)->getNumPasses() == 0)
                            continue;
                        Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
                        if (!pass)
                            continue;

                        const QString mname = QString::fromStdString(mat->getName());
                        static const QRegularExpression kTexSlotRe(
                            QStringLiteral("_tex(\\d+)$"));
                        const auto m = kTexSlotRe.match(mname);
                        if (!m.hasMatch())
                            continue; // untextured submesh — leave as is.
                        bool ok = false;
                        const int slot = m.captured(1).toInt(&ok);
                        if (!ok || slot < 0 || slot >= static_cast<int>(rsdTexSlots.size())
                            || rsdTexSlots[slot].resourceName.isEmpty())
                            continue;

                        // Replace any existing texture unit states so re-imports refresh cleanly.
                        pass->removeAllTextureUnitStates();
                        pass->createTextureUnitState(rsdTexSlots[slot].resourceName.toStdString());
                        pass->setLightingEnabled(true);
                        pass->setAmbient(1.0f, 1.0f, 1.0f);
                        pass->setDiffuse(1.0f, 1.0f, 1.0f, 1.0f);
                        pass->setVertexColourTracking(Ogre::TVC_NONE);
                        mat->compile();
                    }
                }

                // Single-texture legacy fallback: if no per-face MAT routing happened but a TIM
                // is available, bind it on materials lacking explicit texture units. Skipped when
                // the textured MAT path already produced per-submesh materials.
                if (!useTexturedMatPath && !firstTimResource.isEmpty()) {
                    for (unsigned int si = 0; si < en->getNumSubEntities(); ++si) {
                        Ogre::SubEntity* se = const_cast<Ogre::SubEntity*>(en->getSubEntity(si));
                        Ogre::MaterialPtr mat = se->getMaterial();
                        if (mat.isNull() || mat->getNumTechniques() == 0)
                            continue;
                        Ogre::Technique* tech = mat->getTechnique(0);
                        if (!tech || tech->getNumPasses() == 0)
                            continue;
                        Ogre::Pass* pass = tech->getPass(0);
                        if (!pass)
                            continue;
                        if (pass->getNumTextureUnitStates() == 0) {
                            pass->createTextureUnitState(firstTimResource.toStdString());
                        }
                    }
                }

                // Final fallback: if MAT exists but we couldn't parse it (no script, no per-face colours) and the mesh
                // is still effectively untextured, force a simple unlit textured material so the asset isn't blank.
                if (!useTexturedMatPath && !firstTimResource.isEmpty()
                    && rsdMaterialFromScript.isEmpty() && rsdFaceColors.isEmpty()
                    && !matPath.isEmpty() && QFileInfo::exists(matPath)) {
                    applyTextureMaterialToEntity(const_cast<Ogre::Entity*>(en),
                                                      QStringLiteral("PS1/RSD/") + file.baseName(),
                                                      firstTimResource);
                }
            }
            else if (!file.suffix().compare(QStringLiteral("ply"), Qt::CaseInsensitive)
                     && PS1PLY::isPsyqPlyFile(file.filePath()))
            {
                const std::string meshName = (file.baseName() + QStringLiteral("_psyq_ply")).toStdString();
                Ogre::MeshPtr mesh = PS1PLY::importPsyqPly(file.filePath(), meshName);
                if (!mesh) {
                    QMessageBox::warning(
                        nullptr,
                        QStringLiteral("PlayStation PLY"),
                        QStringLiteral("Could not import %1 — invalid Psy-Q PLY (expected @PLY header and "
                                       "face lines from the Psy-Q / RSD toolchain, not Stanford PLY).")
                            .arg(file.fileName()));
                    continue;
                }

                SentryReporter::addBreadcrumb(
                    QStringLiteral("file.import"),
                    QStringLiteral("Imported Psy-Q PLY: %1").arg(file.fileName()));

                sn = Manager::getSingleton()->addSceneNode(file.baseName());
                en = Manager::getSingleton()->createEntity(sn, mesh);
                applyNormalMapsToEntity(en);
            }
            else
            {
                AssimpToOgreImporter importer;
                // DirectX .x is natively left-handed — skip ConvertToLeftHanded
                // to avoid double-flipping geometry and UVs.
                bool convertLH = (file.suffix().compare("x", Qt::CaseInsensitive) != 0);
                const std::string sourcePath = file.filePath().toStdString();
                Ogre::MeshPtr mesh = importer.loadModel(sourcePath, convertLH, additionalFlags);
                // Read coordinate system from metadata immediately — valid for both mesh and animation-only files.
                if (outUpAxis) *outUpAxis = importer.getSceneUpAxis();
                if (mesh) {
                    // Cache the source file path so EditModeController can
                    // re-import the asset through the n-gon-aware
                    // EditableMesh::loadFromAssimpFile path. Quad-bearing
                    // assets keep their polygon structure when entering
                    // Edit Mode; without this cache only the triangulated
                    // Ogre buffer is available and quads are lost.
                    // (Quad migration #326, chunk 3.)
                    mesh->getUserObjectBindings().setUserAny(
                        "qtme.source_path", Ogre::Any(sourcePath));
                    // ALSO cache the convert-to-left-handed flag so the
                    // n-gon re-import uses the SAME coordinate-system
                    // transform AssimpToOgreImporter::loadModel applied
                    // when building the rendered Ogre mesh. Without this
                    // the Edit-Mode vertex overlay would appear mirrored
                    // (X flipped) relative to the on-screen geometry on
                    // every non-.x asset. (Chunk 4.)
                    mesh->getUserObjectBindings().setUserAny(
                        "qtme.source_convert_lh", Ogre::Any(convertLH));
                    // Cache the source up-axis (1 = Y-up, 2 = Z-up) so
                    // EditModeController can apply MeshProcessor's
                    // +90°-around-X bake when re-importing the asset
                    // through the n-gon-aware path. Without this the
                    // editable representation lives in pre-bake space
                    // while the rendered buffers are post-bake — the
                    // overlays appear rotated 90° on FBX/glTF Z-up
                    // assets, and a commit would write the rotated
                    // positions back. (Quad migration follow-up.)
                    mesh->getUserObjectBindings().setUserAny(
                        "qtme.source_up_axis",
                        Ogre::Any(importer.getSceneUpAxis()));

                    // Cache the per-submesh n-gon faces directly on the
                    // Ogre::Mesh so exporters can recover the source
                    // polygon structure without re-reading the file —
                    // even after the user edits the mesh and the
                    // qtme.source_path binding gets cleared. (Quad
                    // migration #326, chunk 6.)
                    EditableMesh ngonProbe;
                    if (ngonProbe.loadFromAssimpFile(
                            sourcePath, convertLH,
                            importer.getSceneUpAxis() == 2,
                            /*skeletonForBoneHandles*/ nullptr,
                            additionalFlags)) {
                        writeNgonFacesToMesh(mesh.get(), ngonProbe.subMeshes());
                    }
                }
                if (!mesh) {
                    // Animation-only file: skeleton/animations were loaded, but there is no mesh.
                    // Collect into the caller-provided list; callers that want UI notifications
                    // should pass outAnimOnlySkeletons and handle presentation themselves.
                    Ogre::SkeletonPtr skel = importer.getLoadedSkeleton();
                    if (skel) {
                        // Animation-only skeletons are NOT baked — register native up-axis
                        // so AnimationMerger can apply the correct Z-up → Y-up conversion
                        // when merging into a baked (Y-up) mesh skeleton.
                        AnimationMerger::registerSkeletonUpAxis(skel->getName(), importer.getSceneUpAxis());
                        if (outAnimOnlySkeletons)
                            outAnimOnlySkeletons->append(skel);
                    } else if (!file.suffix().compare(QStringLiteral("ply"), Qt::CaseInsensitive)) {
                        // Was not routed through PS1PLY (e.g. missing @PLY header) and Assimp failed.
                        QMessageBox::warning(
                            nullptr,
                            QStringLiteral("Import PLY"),
                            QStringLiteral("Could not import %1. PlayStation Psy-Q PLY uses an @PLY header; "
                                           "Stanford PLY must start with \"ply\".")
                                .arg(file.fileName()));
                    }
                    continue;
                }

                auto meshName = file.baseName();
                sn = Manager::getSingleton()->addSceneNode(QString(meshName));
                en = Manager::getSingleton()->createEntity(sn, mesh);

                // Register the original coordinate system. Baking is purely a display
                // fix (vertices + root-bone rest poses rotated); it does not change the
                // bone-orientation relationships that AnimationMerger relies on for the
                // per-bone boneCorrection, so the native upAxis is still correct here.
                if (en->getMesh() && en->getMesh()->getSkeleton())
                    AnimationMerger::registerSkeletonUpAxis(
                        en->getMesh()->getSkeleton()->getName(), importer.getSceneUpAxis());

                // Same as .mesh/.xml path: ensure tangents exist and RTSS normal maps are
                // applied after the Entity exists (import-time material setup can run before
                // mesh data is finalized; Assimp sometimes omits tangents on awkward assets).
                applyNormalMapsToEntity(en);
            }

            sn->setPosition(0,0,0);

            // Auto-scale sub-unit meshes so they aren't clipped by the
            // camera near plane. FBX/glTF files exported with millimetre
            // or centimetre source units (Blender default 0.001 unit
            // scale, real-world-scale photogrammetry, etc.) come in with
            // bounding-box extents <0.01 — the entity loads but sits
            // entirely inside the default near-clip distance and never
            // renders. Scale the parent SceneNode so the largest
            // dimension lands at ~1 unit. Threshold of 0.01 avoids
            // touching sensible-scale assets (anything from a few cm up).
            if (en && en->getMesh()) {
                const auto& bbSize = en->getBoundingBox().getSize();
                const Ogre::Real maxExtent = std::max({bbSize.x, bbSize.y, bbSize.z});
                if (maxExtent > 0.0f && maxExtent < 0.01f) {
                    const Ogre::Real factor = 1.0f / maxExtent;
                    sn->setScale(factor, factor, factor);
                    Ogre::LogManager::getSingleton().logMessage(
                        "MeshImporterExporter: auto-scaled '" + en->getName() +
                        "' by " + std::to_string(factor) +
                        " (source max-extent " + std::to_string(maxExtent) +
                        " was inside the near-clip plane)");
                }
            }

            configureCamera(en);
        }
    }
    catch (Ogre::Exception& e)
    {
        Ogre::LogManager::getSingleton().logMessage(e.getFullDescription());
    } catch (const std::exception& e) {
        Ogre::LogManager::getSingleton().logMessage(
            Ogre::String("MeshImporterExporter::importer: ") + e.what());
    }
}

QString MeshImporterExporter::formatFileURI(const QString &_uri, const QString &_format)
{
    if(_uri.isEmpty()) return "";
    auto ext = exportFormats[_format];
    // Fall back to treating the format string itself as the extension (short aliases
    // like "gltf", "glb", "fbx" that are in assimpFormatIds but not exportFormats).
    if (ext.isEmpty() && !_format.isEmpty() && !_format.contains(' ') && !_format.contains('('))
        ext = "." + _format;
    if(_uri.right(ext.size())==ext)
        return _uri;
    return _uri+ext;
}

QString MeshImporterExporter::exportFileDialogFilter()
{
    QString filter;
    for(auto format = exportFormats.keyBegin(); format!=exportFormats.keyEnd(); ++format)
        filter+=*format+";;";
    filter.chop(2);
    return filter;
}

QString MeshImporterExporter::importFileDialogFilterFromExtensionList(
    const QString& spaceSeparatedDotExtensions)
{
    const QStringList parts = spaceSeparatedDotExtensions.split(' ', Qt::SkipEmptyParts);
    QStringList globs;
    globs.reserve(parts.size());
    for (QString ext : parts) {
        ext = ext.trimmed();
        if (ext.startsWith('.'))
            globs.append(QLatin1Char('*') + ext);
    }
    const QString allSupported = globs.join(QLatin1Char(' '));
    return QStringLiteral(
               "All supported (%1);;"
               "PlayStation RSD / TMD / Psy-Q PLY (*.rsd *.tmd *.ply);;"
               "All files (*.*)")
        .arg(allSupported);
}

QString MeshImporterExporter::importFileDialogFilter()
{
    return importFileDialogFilterFromExtensionList(Manager::getSingleton()->getValidFileExtention());
}

QString MeshImporterExporter::exporter(const Ogre::SceneNode *_sn)
{
    if(!_sn)
    {
        QMessageBox::warning(nullptr,"No object","Which object are you trying to export?",QMessageBox::Ok);
        return QString();
    }

    QString filter = "Ogre Mesh (*.mesh)";
    QString fileName = QFileDialog::getSaveFileName(nullptr, QObject::tr("Export Mesh"),
                                                     _sn->getName().data(),
                                                     exportFileDialogFilter(),&filter,
                                                    QFileDialog::DontUseNativeDialog);
    if(fileName.isEmpty()) return QString();

    QString uri = formatFileURI(fileName, filter);

    exporter(_sn, uri, filter);
    return uri;
}

int MeshImporterExporter::exporter(const Ogre::SceneNode *_sn, const QString &_uri, const QString &_format,
                                    bool stripAnimations)
{
    if(!_sn) return -1;

    if(_uri.isEmpty()) return -1;

    QFileInfo file;
    file.setFile(_uri);

    if(!Manager::getSingleton()->getSceneMgr()->hasEntity(_sn->getName())) return -1;
    const Ogre::Entity *e = Manager::getSingleton()->getSceneMgr()->getEntity(_sn->getName());
    if(!e) return -1;

    // Vertex paint defers GPU upload; export reads Ogre buffers — sync first.
    EditModeController::instance()->flushPendingVertexPaintForEntity(
        const_cast<Ogre::Entity*>(e));

    if(_format=="Ogre XML (*.mesh.xml)")
    {
        Ogre::XMLMeshSerializer xmlMS;

        if(e->hasSkeleton())
        {
            // Use the master skeleton (from the Mesh) rather than the
            // SkeletonInstance (from the Entity).  The master skeleton always
            // holds the original bind pose and animation deltas, unaffected
            // by animation playback.
            Ogre::XMLSkeletonSerializer xmlSS;
            xmlSS.exportSkeleton(e->getMesh()->getSkeleton().get(),(_uri.left(_uri.length()-8)+"skeleton.xml").toStdString().data());
        }

        // Export mesh XML with the current skeleton name intact.
        // In Ogre 14.x, setSkeletonName() calls SkeletonManager::load()
        // which fails for .skeleton.xml files (XML format, not binary),
        // clearing hasSkeleton() and losing all bone assignments.
        // Instead, export with the original skeleton, then fix up the
        // skeleton link name in the XML to point to the .skeleton.xml file.
        xmlMS.exportMesh(e->getMesh().get(),_uri.toStdString().data());

        // Fix up the skeleton link name in the exported XML
        if(e->hasSkeleton())
        {
            auto newSkelName = (file.baseName()+".skeleton.xml").toStdString();
            pugi::xml_document doc;
            if (doc.load_file(_uri.toStdString().c_str()))
            {
                auto skelLink = doc.child("mesh").child("skeletonlink");
                if (skelLink)
                    skelLink.attribute("name").set_value(newSkelName.c_str());
                doc.save_file(_uri.toStdString().c_str());
            }
        }

        exportMaterial(e, file);
    }
    else if(_format.contains("mesh"))
    {
        Ogre::MeshSerializer m;

        unsigned int version = 0;
        std::map<QString, unsigned int> versionMap = {
            {"Ogre Mesh (*.mesh)", 0},
            {"Ogre Mesh v1.10+(*.mesh)", 1},
            {"Ogre Mesh v1.8+(*.mesh)", 2},
            {"Ogre Mesh v1.7+(*.mesh)", 3},
            {"Ogre Mesh v1.4+(*.mesh)", 4},
            {"Ogre Mesh v1.0+(*.mesh)", 5}
        };

        version = versionMap[_format];

        if(e->hasSkeleton())
        {
            // Use the master skeleton — always has original bind pose
            Ogre::SkeletonSerializer ss;
            ss.exportSkeleton(e->getMesh()->getSkeleton().get(),QString(file.path()+"/"+e->getMesh().get()->getSkeletonName().c_str()).toStdString().data());
        }

        m.exportMesh(e->getMesh().get(),_uri.toStdString().data(),(Ogre::MeshVersion)version);

        exportMaterial(e, file);
    } else if (_format == "FBX Binary (*.fbx)") {
        bool ok = FBXExporter::exportFBX(e, _uri);
        // FBXExporter embeds textures (Video.Content) so avoid emitting sidecar
        // .material and extracted image files next to the FBX.
        if (!ok)
            return -1;
    } else if (_format == QStringLiteral("PlayStation TMD (*.tmd)")) {
        if (!PS1TMD::exportEntity(e, _uri))
            return -1;
        SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
            QStringLiteral("Exported PlayStation TMD: %1").arg(_uri));
    } else if (_format == QStringLiteral("PlayStation RSD (*.rsd)")) {
        const QFileInfo outFi(_uri);
        const QString base = outFi.absolutePath() + QDir::separator() + outFi.completeBaseName();
        const QString plyPath = base + QStringLiteral(".ply");
        const QString matPath = base + QStringLiteral(".mat");

        QVector<QColor> faceColors;
        QVector<PS1PLY::ExportFaceTexture> faceTexInfos;
        QString err;
        if (!PS1PLY::exportPsyqPlyFromEntity(e, plyPath, &faceColors, &faceTexInfos, &err)) {
            Ogre::LogManager::getSingleton().logError("Failed to write Psy-Q PLY: " + err.toStdString());
            return -1;
        }

        // Build the RSD texture table from textured submeshes' first texture unit. Each
        // unique texture name gets one RSD slot; we copy the source image next to the
        // .rsd output so the descriptor stays self-contained.
        struct OutTex {
            QString resourceName; ///< Ogre resource name (e.g. "Wood.jpg" or "tex.tim").
            QString outFile;       ///< File name written next to the .rsd.
            int width = 0;
            int height = 0;
        };
        std::vector<OutTex> rsdOutTextures;
        std::unordered_map<int, int> submeshToTexSlot; ///< submeshIndex -> rsd slot.
        std::unordered_map<std::string, int> resourceToSlot;

        for (unsigned int si = 0; si < e->getNumSubEntities(); ++si) {
            const Ogre::SubEntity* se = e->getSubEntity(si);
            if (!se)
                continue;
            Ogre::MaterialPtr mat = se->getMaterial();
            if (mat.isNull() || mat->getNumTechniques() == 0
                || mat->getTechnique(0)->getNumPasses() == 0)
                continue;
            Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
            if (!pass || pass->getNumTextureUnitStates() == 0)
                continue;
            const Ogre::TextureUnitState* tus = pass->getTextureUnitState(0);
            if (!tus || tus->getContentType() != Ogre::TextureUnitState::CONTENT_NAMED)
                continue;
            const std::string texName = tus->getTextureName();
            if (texName.empty())
                continue;

            auto rit = resourceToSlot.find(texName);
            int slot = -1;
            if (rit != resourceToSlot.end()) {
                slot = rit->second;
            } else {
                OutTex ot;
                ot.resourceName = QString::fromStdString(texName);
                ot.outFile = ot.resourceName;
                auto tex = Ogre::TextureManager::getSingleton().getByName(texName);
                if (tex) {
                    ot.width = static_cast<int>(tex->getWidth());
                    ot.height = static_cast<int>(tex->getHeight());
                }
                slot = static_cast<int>(rsdOutTextures.size());
                rsdOutTextures.push_back(ot);
                resourceToSlot.emplace(texName, slot);
            }
            submeshToTexSlot[static_cast<int>(si)] = slot;
        }

        // Synthesise MAT entries: one per output PLY face. Mix textured (T) and untextured (C)
        // types based on whether the source submesh provided UVs + a texture. Texture indices
        // map through `submeshToTexSlot` so a single texture used by multiple submeshes still
        // collapses to a single RSD slot.
        const bool haveColors  = !faceColors.isEmpty()
                               && faceColors.size() == static_cast<int>(faceTexInfos.size());
        const bool haveTexInfo = !faceTexInfos.isEmpty();

        QVector<PS1MAT::MatEntry> entries;
        if (haveTexInfo || haveColors) {
            const int nFaces = haveTexInfo ? faceTexInfos.size() : faceColors.size();
            entries.reserve(nFaces);
            for (int fi = 0; fi < nFaces; ++fi) {
                PS1MAT::MatEntry me;
                me.shadingChar = 'F';

                const PS1PLY::ExportFaceTexture& eft = haveTexInfo
                    ? faceTexInfos[fi]
                    : PS1PLY::ExportFaceTexture{};
                const QColor faceColor = haveColors ? faceColors[fi] : QColor(255, 255, 255);

                const int slotIt = (eft.textured && submeshToTexSlot.count(eft.submeshIndex))
                    ? submeshToTexSlot[eft.submeshIndex]
                    : -1;
                if (eft.textured && slotIt >= 0) {
                    me.typeChar = 'T';
                    me.textured = true;
                    me.textureIndex = slotIt;
                    const OutTex& ot = rsdOutTextures[slotIt];
                    const int texW = ot.width > 0 ? ot.width : 256;
                    const int texH = ot.height > 0 ? ot.height : 256;
                    const int corners = (eft.cornerCount == 4) ? 4 : 3;
                    me.uvs.resize(corners);
                    for (int k = 0; k < corners; ++k) {
                        me.uvs[k].u = static_cast<int>(std::lround(double(eft.u[k]) * double(texW)));
                        me.uvs[k].v = static_cast<int>(std::lround(double(eft.v[k]) * double(texH)));
                    }
                    me.rgb = QColor(255, 255, 255);
                } else {
                    me.typeChar = 'C';
                    me.vertColors.push_back(faceColor);
                    me.rgb = faceColor;
                }
                entries.push_back(me);
            }
        }

        if (!entries.isEmpty()) {
            if (!PS1MAT::writeMatFile(matPath, entries, &err)) {
                Ogre::LogManager::getSingleton().logError("Failed to write MAT: " + err.toStdString());
                return -1;
            }
        }

        // Copy referenced textures next to the .rsd so the descriptor stays self-contained.
        // We use Ogre's in-memory image (Image::loadDynamicImage from the bound texture) and
        // write a PNG when the original encoding is unknown.
        for (auto& ot : rsdOutTextures) {
            const QString outPath = outFi.absolutePath() + QDir::separator() + ot.outFile;
            if (QFileInfo::exists(outPath))
                continue; // Skip if a file with that name already lives in the output dir.
            try {
                auto tex = Ogre::TextureManager::getSingleton().getByName(ot.resourceName.toStdString());
                if (!tex)
                    continue;
                Ogre::Image img;
                tex->convertToImage(img, true);
                if (img.getWidth() == 0 || img.getHeight() == 0)
                    continue;
                // Force RGBA8 layout — Ogre textures may live in PF_A8R8G8B8 / BGRA / DXT /
                // float formats and feeding any of those into QImage::Format_RGBA8888 would
                // either misorder channels or, for compressed/non-byte formats, walk past
                // the buffer end during the save.
                std::vector<uint8_t> rgba;
                if (img.getFormat() != Ogre::PF_BYTE_RGBA) {
                    const size_t pixels = static_cast<size_t>(img.getWidth()) * img.getHeight();
                    rgba.resize(pixels * 4);
                    Ogre::PixelBox src(img.getWidth(), img.getHeight(), 1,
                                       img.getFormat(),
                                       const_cast<uint8_t*>(img.getData()));
                    Ogre::PixelBox dst(img.getWidth(), img.getHeight(), 1,
                                       Ogre::PF_BYTE_RGBA, rgba.data());
                    Ogre::PixelUtil::bulkPixelConversion(src, dst);
                }
                const uint8_t* rgbaData = rgba.empty() ? img.getData() : rgba.data();
                const QImage qi(rgbaData,
                                static_cast<int>(img.getWidth()),
                                static_cast<int>(img.getHeight()),
                                static_cast<int>(img.getWidth()) * 4,
                                QImage::Format_RGBA8888);
                // Always save a PNG copy with the same basename (lossless, widely supported).
                const QString png = outFi.absolutePath() + QDir::separator()
                    + QFileInfo(ot.outFile).completeBaseName() + QStringLiteral(".png");
                qi.copy().save(png, "PNG");
                ot.outFile = QFileInfo(png).fileName();
            } catch (const std::exception&) {
                // ignored — texture remains referenced by its original resource name.
            }
        }

        PS1RSD::RsdDescriptor rsd;
        rsd.headerId = QStringLiteral("@RSD940102");
        rsd.plyPath = QFileInfo(plyPath).fileName();
        if (!entries.isEmpty())
            rsd.matPath = QFileInfo(matPath).fileName();
        if (!rsdOutTextures.empty()) {
            rsd.ntex = static_cast<int>(rsdOutTextures.size());
            rsd.textures.reserve(static_cast<int>(rsdOutTextures.size()));
            for (const auto& ot : rsdOutTextures)
                rsd.textures.push_back(ot.outFile);
        } else {
            // Legacy fallback: if a same-basename .tim already sits next to the output, reference it.
            const QString timPath = base + QStringLiteral(".tim");
            if (QFileInfo::exists(timPath)) {
                rsd.ntex = 1;
                rsd.textures = { QFileInfo(timPath).fileName() };
            }
        }

        if (!PS1RSD::writeRsdFile(_uri, rsd, &err)) {
            Ogre::LogManager::getSingleton().logError("Failed to write RSD: " + err.toStdString());
            return -1;
        }

        SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
            QStringLiteral("Exported PlayStation RSD: %1").arg(_uri));
    } else {
        // Export using Assimp — build aiScene directly from Ogre mesh data
        try {
            aiScene* scene = buildAiScene(e);
            if (!scene || !scene->mNumMeshes)
            {
                Ogre::LogManager::getSingleton().logError("Failed to build aiScene from entity");
                delete scene;
                return -1;
            }

            // LOD exports strip all skeleton data. Game engines apply the original
            // mesh's skeleton to every LOD level; the LOD files only need geometry.
            // Keeping bones without proper per-vertex blend elements triggers
            // Ogre's softwareVertexBlend assertion crash on re-import.
            if (stripAnimations)
            {
                for (unsigned int ai = 0; ai < scene->mNumAnimations; ++ai)
                    delete scene->mAnimations[ai];
                delete[] scene->mAnimations;
                scene->mAnimations = nullptr;
                scene->mNumAnimations = 0;

                for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi)
                {
                    auto* m = scene->mMeshes[mi];
                    for (unsigned int bi = 0; bi < m->mNumBones; ++bi)
                        delete m->mBones[bi];
                    delete[] m->mBones;
                    m->mBones = nullptr;
                    m->mNumBones = 0;
                }
            }

            // Map format display name to Assimp export format ID
            static const QMap<QString, QString> assimpFormatIds = {
                {"Collada (*.dae)", "collada"},
                {"X (*.x)", "x"},
                {"OBJ (*.obj)", "obj"},
                {"OBJ without MTL (*.objnomtl)", "objnomtl"},
                {"STL (*.stl)", "stl"},
                {"PLY (*.ply)", "ply"},
                {"3DS (*.3ds)", "3ds"},
                {"glTF 2.0 (*.gltf)", "gltf2"},
                {"glTF 2.0 Binary (*.glb)", "glb2"},
                {"gltf", "gltf2"},  // short alias used by LOD exporter
                {"glb", "glb2"},    // short alias used by LOD exporter
                {"Assimp Binary (*.assbin)", "assbin"},
            };

            QString formatId = assimpFormatIds.value(_format, file.suffix());

            // Formats that do NOT support skeletal data — strip bones & animations
            static const QSet<QString> noSkeletonFormats = {
                "3ds", "obj", "objnomtl", "stl", "ply"
            };
            if (noSkeletonFormats.contains(formatId))
            {
                for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi)
                {
                    auto* m = scene->mMeshes[mi];
                    for (unsigned int bi = 0; bi < m->mNumBones; ++bi)
                        delete m->mBones[bi];
                    delete[] m->mBones;
                    m->mBones = nullptr;
                    m->mNumBones = 0;
                }
                for (unsigned int ai = 0; ai < scene->mNumAnimations; ++ai)
                    delete scene->mAnimations[ai];
                delete[] scene->mAnimations;
                scene->mAnimations = nullptr;
                scene->mNumAnimations = 0;
            }

            Assimp::Exporter exporter;
            // DirectX .x is natively left-handed — Assimp's exporter
            // handles the RH→LH conversion internally, so we must NOT
            // apply ConvertToLeftHanded or the geometry gets double-flipped.
            unsigned int exportFlags = (formatId == "x")
                ? 0 : aiProcess_ConvertToLeftHanded;
            aiReturn result = exporter.Export(scene, formatId.toStdString().c_str(),
                                             file.filePath().toStdString().c_str(),
                                             exportFlags);
            if (result != AI_SUCCESS)
            {
                auto msg = QString("Assimp export to %1 failed (code %2): %3")
                    .arg(formatId).arg(result).arg(exporter.GetErrorString());
                Ogre::LogManager::getSingleton().logError(msg.toStdString());
                SentryReporter::captureMessage(msg, "error");
            }
            else
            {
                // Export texture files alongside the model.
                // For FBX we aim for a single-file export (embedded textures), so skip sidecars.
                if (_format != "FBX Binary (*.fbx)")
                    exportMaterial(e, file);
            }

            delete scene;
        } catch (std::exception& ex) {
            auto msg = QString("Assimp export failed: %1").arg(ex.what());
            Ogre::LogManager::getSingleton().logError(msg.toStdString());
            SentryReporter::captureMessage(msg, "error");
        } catch (...) {
            Ogre::LogManager::getSingleton().logError("Assimp export failed with unknown exception");
            SentryReporter::captureMessage("Assimp export failed with unknown exception", "error");
        }
    }

    return 0;
}

// ─── Export current pose as static mesh ──────────────────────────────
int MeshImporterExporter::exportCurrentPose(Ogre::Entity* entity, const QString& outputPath,
                                             const QString& format)
{
    if (!entity) return -1;
    if (outputPath.isEmpty()) return -1;

    EditModeController::instance()->flushPendingVertexPaintForEntity(entity);

    SentryReporter::addBreadcrumb("file.export", QString("Export pose: %1").arg(outputPath));

    const bool hasSkel = entity->hasSkeleton();
    if (!hasSkel) {
        // No skeleton means the mesh is already static — just export as-is
        auto* node = entity->getParentSceneNode();
        if (!node) return -1;
        QString fmt = format.isEmpty() ? CLIPipeline::formatForExtension(outputPath) : format;
        return exporter(node, outputPath, fmt, true);
    }

    // Request software skinning so we can read CPU-side deformed vertices.
    // Use a guard to ensure the request is always released, even on exception.
    entity->addSoftwareAnimationRequest(false);
    struct SoftAnimGuard {
        Ogre::Entity* ent;
        ~SoftAnimGuard() { if (ent) ent->removeSoftwareAnimationRequest(false); }
    } softAnimGuard{entity};

    entity->_updateAnimation();

    const Ogre::MeshPtr mesh = entity->getMesh();
    const unsigned int numSub = mesh->getNumSubMeshes();

    // Build an aiScene with deformed vertex positions, NO skeleton, NO animations
    auto* scene = new aiScene();
    scene->mRootNode = new aiNode();
    scene->mRootNode->mName = aiString(entity->getName());
    scene->mRootNode->mNumMeshes = numSub;
    scene->mRootNode->mMeshes = new unsigned int[numSub];
    for (unsigned int si = 0; si < numSub; ++si)
        scene->mRootNode->mMeshes[si] = si;

    // Materials
    std::vector<Ogre::MaterialPtr> materials;
    std::set<std::string, std::less<>> seen;
    for (const auto* sub : entity->getSubEntities())
    {
        auto mat = sub->getMaterial();
        if (seen.insert(mat->getName()).second)
            materials.push_back(mat);
    }
    scene->mNumMaterials = static_cast<unsigned int>(materials.size());
    scene->mMaterials = new aiMaterial*[scene->mNumMaterials];
    std::map<std::string, unsigned int, std::less<>> matIndexMap;
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
        scene->mMaterials[i] = buildAiMaterialFromOgre(materials[i]);
        matIndexMap[materials[i]->getName()] = i;
    }

    // Meshes — read deformed positions from software-skinned buffers
    scene->mNumMeshes = numSub;
    scene->mMeshes = numSub > 0 ? new aiMesh*[numSub] : nullptr;

    for (unsigned int si = 0; si < numSub; ++si)
    {
        const Ogre::SubMesh* subMesh = mesh->getSubMesh(si);
        Ogre::SubEntity* subEnt = entity->getSubEntity(si);

        // Get the software-skinned vertex data (deformed positions/normals)
        Ogre::VertexData* animData = subMesh->useSharedVertices
            ? entity->_getSkelAnimVertexData()
            : subEnt->_getSkelAnimVertexData();

        // Fall back to bind-pose vertex data if skinned data unavailable
        const Ogre::VertexData* bindData = subMesh->useSharedVertices
            ? mesh->sharedVertexData : subMesh->vertexData;

        if (!animData && !bindData) {
            scene->mMeshes[si] = new aiMesh();
            continue;
        }

        auto* aiM = new aiMesh();
        scene->mMeshes[si] = aiM;
        aiM->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;

        // Use animData for positions and normals, bindData for UVs and indices
        const Ogre::VertexData* posSource = animData ? animData : bindData;
        aiM->mNumVertices = static_cast<unsigned int>(posSource->vertexCount);
        aiM->mVertices = new aiVector3D[aiM->mNumVertices];

        // Material index
        auto matIt = matIndexMap.find(subEnt->getMaterial()->getName());
        aiM->mMaterialIndex = (matIt != matIndexMap.end()) ? matIt->second : 0;

        // Read deformed positions
        const auto* posElem = posSource->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        if (posElem)
        {
            auto vbuf = posSource->vertexBufferBinding->getBuffer(posElem->getSource());
            auto* base = static_cast<const unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
            for (unsigned int j = 0; j < aiM->mNumVertices; ++j)
            {
                const Ogre::Real* p;
                posElem->baseVertexPointerToElement(const_cast<unsigned char*>(base + j * vbuf->getVertexSize()), &p);
                aiM->mVertices[j] = aiVector3D(p[0], p[1], p[2]);
            }
            vbuf->unlock();
        }

        // Read deformed normals (from animData if available, otherwise bindData)
        const auto* normElem = posSource->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL);
        if (normElem)
        {
            aiM->mNormals = new aiVector3D[aiM->mNumVertices];
            auto vbuf = posSource->vertexBufferBinding->getBuffer(normElem->getSource());
            auto* base = static_cast<const unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
            for (unsigned int j = 0; j < aiM->mNumVertices; ++j)
            {
                const Ogre::Real* p;
                normElem->baseVertexPointerToElement(const_cast<unsigned char*>(base + j * vbuf->getVertexSize()), &p);
                aiM->mNormals[j] = aiVector3D(p[0], p[1], p[2]);
            }
            vbuf->unlock();
        }

        // Read UVs from bind-pose data (skinning doesn't affect UVs)
        if (bindData)
        {
            const auto* tcElem = bindData->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES);
            if (tcElem)
            {
                aiM->mTextureCoords[0] = new aiVector3D[aiM->mNumVertices];
                aiM->mNumUVComponents[0] = 2;
                auto vbuf = bindData->vertexBufferBinding->getBuffer(tcElem->getSource());
                auto* base = static_cast<const unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                for (unsigned int j = 0; j < aiM->mNumVertices; ++j)
                {
                    const Ogre::Real* p;
                    tcElem->baseVertexPointerToElement(const_cast<unsigned char*>(base + j * vbuf->getVertexSize()), &p);
                    aiM->mTextureCoords[0][j] = aiVector3D(p[0], p[1], 0.0f);
                }
                vbuf->unlock();
            }
        }

        // Vertex colors live on bind-pose buffers (unchanged by skinning), like UVs.
        if (bindData)
        {
            const auto* colElem = bindData->vertexDeclaration->findElementBySemantic(Ogre::VES_DIFFUSE);
            if (colElem)
            {
                const unsigned int n = std::min(
                    aiM->mNumVertices,
                    static_cast<unsigned int>(bindData->vertexCount));
                aiM->mColors[0] = new aiColor4D[aiM->mNumVertices];
                for (unsigned int j = 0; j < aiM->mNumVertices; ++j)
                    aiM->mColors[0][j] = aiColor4D(1, 1, 1, 1);
                auto vbuf = bindData->vertexBufferBinding->getBuffer(colElem->getSource());
                auto* base = static_cast<const unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                for (unsigned int j = 0; j < n; ++j)
                {
                    const Ogre::RGBA* p;
                    colElem->baseVertexPointerToElement(const_cast<unsigned char*>(base + j * vbuf->getVertexSize()), &p);
                    Ogre::ColourValue cv;
                    if (colElem->getType() == Ogre::VET_COLOUR_ABGR)
                        cv.setAsABGR(*p);
                    else
                        cv.setAsARGB(*p);
                    aiM->mColors[0][j] = aiColor4D(cv.r, cv.g, cv.b, cv.a);
                }
                vbuf->unlock();
            }
        }

        // Read indices from the original submesh
        const Ogre::IndexData* iData = subMesh->indexData;
        if (iData && iData->indexCount > 0)
        {
            aiM->mNumFaces = static_cast<unsigned int>(iData->indexCount / 3);
            aiM->mFaces = new aiFace[aiM->mNumFaces];
            auto ibuf = iData->indexBuffer;
            auto* ibase = static_cast<const unsigned char*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
            bool use32 = ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT;

            const unsigned int indexStart = static_cast<unsigned int>(iData->indexStart);
            for (unsigned int f = 0; f < aiM->mNumFaces; ++f)
            {
                aiM->mFaces[f].mNumIndices = 3;
                aiM->mFaces[f].mIndices = new unsigned int[3];
                for (unsigned int v = 0; v < 3; ++v)
                {
                    unsigned int idx = use32
                        ? reinterpret_cast<const uint32_t*>(ibase)[indexStart + f * 3 + v]
                        : reinterpret_cast<const uint16_t*>(ibase)[indexStart + f * 3 + v];
                    aiM->mFaces[f].mIndices[v] = idx;
                }
            }
            ibuf->unlock();
        }

        compactAiMesh(aiM);
    }

    // No animations, no bones — this is a static mesh
    // (Software animation request is released by softAnimGuard destructor)

    // Export the scene using the same Assimp path as regular export
    QFileInfo file(outputPath);
    QString fmt = format.isEmpty() ? CLIPipeline::formatForExtension(outputPath) : format;

    int result = -1;

    if (fmt == "FBX Binary (*.fbx)") {
        // For FBX, we need to use Assimp since FBXExporter writes skeleton data
        // from the original entity. Fall through to the Assimp path.
    }

    // Use Assimp exporter for all formats (the scene has no skeleton)
    {
        static const QMap<QString, QString> assimpFormatIds = {
            {"Collada (*.dae)", "collada"},
            {"X (*.x)", "x"},
            {"OBJ (*.obj)", "obj"},
            {"OBJ without MTL (*.objnomtl)", "objnomtl"},
            {"STL (*.stl)", "stl"},
            {"PLY (*.ply)", "ply"},
            {"3DS (*.3ds)", "3ds"},
            {"glTF 2.0 (*.gltf)", "gltf2"},
            {"glTF 2.0 Binary (*.glb)", "glb2"},
            {"gltf", "gltf2"},
            {"glb", "glb2"},
            {"Assimp Binary (*.assbin)", "assbin"},
            {"FBX Binary (*.fbx)", "fbx"},
        };

        QString formatId = assimpFormatIds.value(fmt, file.suffix());

        try {
            Assimp::Exporter exporter;
            // glTF/GLB and DirectX (.x) are right-handed like Ogre — skip handedness conversion.
            // Only apply ConvertToLeftHanded for formats that expect left-handed coords (e.g. FBX).
            bool rightHanded = (formatId == "x" || formatId == "gltf2" || formatId == "glb2");
            unsigned int exportFlags = rightHanded ? 0 : aiProcess_ConvertToLeftHanded;
            aiReturn aiResult = exporter.Export(scene, formatId.toStdString().c_str(),
                                                file.filePath().toStdString().c_str(),
                                                exportFlags);
            if (aiResult == AI_SUCCESS) {
                result = 0;
                // For FBX, prefer a single-file export. Assimp FBX export may still
                // reference textures by name, but our current expectation for FBX is
                // "no sidecar dumps" (materials/textures are embedded or handled by importer).
                if (fmt != "FBX Binary (*.fbx)")
                    exportMaterial(entity, file);
            } else {
                auto msg = QString("Assimp export (pose) to %1 failed: %2")
                    .arg(formatId).arg(exporter.GetErrorString());
                Ogre::LogManager::getSingleton().logError(msg.toStdString());
                SentryReporter::captureMessage(msg, "error");
            }
        } catch (std::exception& ex) {
            auto msg = QString("Export pose failed: %1").arg(ex.what());
            Ogre::LogManager::getSingleton().logError(msg.toStdString());
            SentryReporter::captureMessage(msg, "error");
        }
    }

    delete scene;
    return result;
}

// ─── Scene-level export: all scene nodes → single glTF ──────────────
static aiScene* buildSceneAiScene()
{
    auto* manager = Manager::getSingleton();
    const auto& sceneNodes = manager->getSceneNodes();

    auto* scene = new aiScene();
    scene->mRootNode = new aiNode("Scene");

    if (sceneNodes.isEmpty())
    {
        // Valid empty scene
        scene->mNumMeshes = 0;
        scene->mNumMaterials = 1;
        scene->mMaterials = new aiMaterial*[1];
        scene->mMaterials[0] = new aiMaterial();
        return scene;
    }

    // --- Collect all entities from scene nodes ---
    struct NodeEntityPair {
        Ogre::SceneNode* sceneNode;
        Ogre::Entity* entity;
    };
    std::vector<NodeEntityPair> nodeEntities;
    for (auto* sn : sceneNodes)
    {
        if (!manager->getSceneMgr()->hasEntity(sn->getName()))
            continue;
        auto* entity = manager->getSceneMgr()->getEntity(sn->getName());
        if (entity)
            nodeEntities.push_back({sn, entity});
    }

    if (nodeEntities.empty())
    {
        scene->mNumMeshes = 0;
        scene->mNumMaterials = 1;
        scene->mMaterials = new aiMaterial*[1];
        scene->mMaterials[0] = new aiMaterial();
        return scene;
    }

    // --- Deduplicate materials across all entities ---
    std::vector<Ogre::MaterialPtr> materials;
    std::map<std::string, unsigned int, std::less<>> matIndexMap;
    for (const auto& [sn, entity] : nodeEntities)
    {
        for (const auto* sub : entity->getSubEntities())
        {
            auto mat = sub->getMaterial();
            if (matIndexMap.find(mat->getName()) == matIndexMap.end())
            {
                matIndexMap[mat->getName()] = static_cast<unsigned int>(materials.size());
                materials.push_back(mat);
            }
        }
    }

    scene->mNumMaterials = static_cast<unsigned int>(materials.size());
    scene->mMaterials = new aiMaterial*[scene->mNumMaterials];
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i)
        scene->mMaterials[i] = buildAiMaterialFromOgre(materials[i]);

    // --- Count total meshes and build child nodes ---
    unsigned int totalMeshes = 0;
    for (const auto& [sn, entity] : nodeEntities)
        totalMeshes += entity->getMesh()->getNumSubMeshes();

    scene->mNumMeshes = totalMeshes;
    scene->mMeshes = totalMeshes > 0 ? new aiMesh*[totalMeshes] : nullptr;

    // Root node children: one per scene node
    scene->mRootNode->mNumChildren = static_cast<unsigned int>(nodeEntities.size());
    scene->mRootNode->mChildren = new aiNode*[scene->mRootNode->mNumChildren];

    unsigned int globalMeshIdx = 0;
    std::set<Ogre::Skeleton*> processedSkeletons;
    std::vector<aiAnimation*> allAnimations;

    for (unsigned int ni = 0; ni < nodeEntities.size(); ++ni)
    {
        auto* sn = nodeEntities[ni].sceneNode;
        auto* entity = nodeEntities[ni].entity;
        EditModeController::instance()->flushPendingVertexPaintForEntity(entity);
        const Ogre::MeshPtr mesh = entity->getMesh();
        const unsigned int numSub = mesh->getNumSubMeshes();
        const bool hasSkeleton = entity->hasSkeleton();
        Ogre::Skeleton* skeleton = hasSkeleton ? mesh->getSkeleton().get() : nullptr;

        // Build bone handle→name map for this entity
        // Prefix bone names with entity name to ensure uniqueness across entities
        std::map<unsigned short, std::string> boneHandleToName;
        std::string bonePrefix = (nodeEntities.size() > 1 && hasSkeleton)
            ? std::string(sn->getName()) + "_" : "";

        // Create the scene node's aiNode
        aiNode* entityNode;
        if (hasSkeleton)
        {
            entityNode = new aiNode(std::string(sn->getName()));
            entityNode->mParent = scene->mRootNode;

            auto numBones = skeleton->getNumBones();
            std::vector<aiNode*> rootBoneNodes;
            for (unsigned short bi = 0; bi < numBones; ++bi)
            {
                auto* bone = skeleton->getBone(bi);
                boneHandleToName[bone->getHandle()] = bonePrefix + std::string(bone->getName());
                if (!bone->getParent())
                    rootBoneNodes.push_back(buildBoneNode(bone, entityNode, bonePrefix));
            }

            auto* meshNode = new aiNode(std::string(sn->getName()) + "_mesh");
            meshNode->mParent = entityNode;
            meshNode->mNumMeshes = numSub;
            meshNode->mMeshes = new unsigned int[numSub];
            for (unsigned int si = 0; si < numSub; ++si)
                meshNode->mMeshes[si] = globalMeshIdx + si;

            entityNode->mNumChildren = static_cast<unsigned int>(rootBoneNodes.size()) + 1;
            entityNode->mChildren = new aiNode*[entityNode->mNumChildren];
            for (unsigned int i = 0; i < rootBoneNodes.size(); ++i)
                entityNode->mChildren[i] = rootBoneNodes[i];
            entityNode->mChildren[rootBoneNodes.size()] = meshNode;
        }
        else
        {
            entityNode = new aiNode(std::string(sn->getName()));
            entityNode->mParent = scene->mRootNode;
            entityNode->mNumMeshes = numSub;
            entityNode->mMeshes = new unsigned int[numSub];
            for (unsigned int si = 0; si < numSub; ++si)
                entityNode->mMeshes[si] = globalMeshIdx + si;
        }

        Ogre::Matrix4 nodeTransform;
        nodeTransform.makeTransform(sn->getPosition(), sn->getScale(), sn->getOrientation());
        entityNode->mTransformation = toAiMatrix(nodeTransform);

        scene->mRootNode->mChildren[ni] = entityNode;

        // --- Build meshes for this entity ---
        for (unsigned int si = 0; si < numSub; ++si)
        {
            const Ogre::SubMesh* subMesh = mesh->getSubMesh(si);
            const Ogre::VertexData* vData = subMesh->useSharedVertices
                ? mesh->sharedVertexData : subMesh->vertexData;
            if (!vData) { scene->mMeshes[globalMeshIdx + si] = new aiMesh(); continue; }

            auto* aiM = new aiMesh();
            scene->mMeshes[globalMeshIdx + si] = aiM;
            readSubmeshGeometry(aiM, vData, subMesh, entity, si, matIndexMap);

            if (hasSkeleton)
                assignBoneWeights(aiM, subMesh, mesh, skeleton, boneHandleToName);

            compactAiMesh(aiM);
        }

        // --- Animations ---
        // When bone names are prefixed, each entity needs its own animations
        // Only dedup when there's no prefix (single entity)
        if (hasSkeleton && skeleton->getNumAnimations() > 0
            && (bonePrefix.empty() ? processedSkeletons.insert(skeleton).second : true))
        {
            for (unsigned short ai = 0; ai < skeleton->getNumAnimations(); ++ai)
                allAnimations.push_back(buildAiAnimation(skeleton->getAnimation(ai), bonePrefix));
        }

        globalMeshIdx += numSub;
    }

    // Assign animations to scene
    if (!allAnimations.empty())
    {
        scene->mNumAnimations = static_cast<unsigned int>(allAnimations.size());
        scene->mAnimations = new aiAnimation*[scene->mNumAnimations];
        for (unsigned int i = 0; i < scene->mNumAnimations; ++i)
            scene->mAnimations[i] = allAnimations[i];
    }

    return scene;
}

int MeshImporterExporter::sceneExporter(const QString &_uri, const ProgressCallback& progress)
{
    if (_uri.isEmpty()) return -1;

    QFileInfo file(_uri);

    auto reportProgress = [&](int pct, const QString& status) {
        if (progress) progress(pct, status);
    };

    try {
        // Export textures for all entities
        auto* manager = Manager::getSingleton();
        const auto& sceneNodes = manager->getSceneNodes();

        // Count entities for progress tracking
        std::vector<std::pair<Ogre::SceneNode*, Ogre::Entity*>> entities;
        for (auto* sn : sceneNodes)
        {
            if (!manager->getSceneMgr()->hasEntity(sn->getName()))
                continue;
            auto* entity = manager->getSceneMgr()->getEntity(sn->getName());
            if (entity)
                entities.emplace_back(sn, entity);
        }

        int totalEntities = static_cast<int>(entities.size());
        for (int i = 0; i < totalEntities; ++i)
        {
            int pct = totalEntities > 0 ? (i * 30 / totalEntities) : 0;
            reportProgress(pct, QString("Exporting textures (%1/%2)...").arg(i + 1).arg(totalEntities));
            exportMaterial(entities[i].second, file);
        }

        reportProgress(30, QStringLiteral("Building scene data..."));
        aiScene* scene = buildSceneAiScene();
        if (!scene)
        {
            Ogre::LogManager::getSingleton().logError("Failed to build scene aiScene");
            return -1;
        }

        reportProgress(60, QStringLiteral("Writing file..."));

        // Determine format from extension
        // Strip .scene prefix if present (e.g., "model.scene.glb" → use "glb2")
        QString suffix = file.suffix().toLower();
        QString formatId = (suffix == "glb") ? "glb2" : "gltf2";

        Assimp::Exporter exporter;

        // Both Ogre and glTF are right-handed — no ConvertToLeftHanded
        aiReturn result = exporter.Export(scene, formatId.toStdString().c_str(),
                                         file.filePath().toStdString().c_str(), 0);
        if (result != AI_SUCCESS)
        {
            auto msg = QString("Scene export failed (code %1): %2")
                .arg(result).arg(exporter.GetErrorString());
            qWarning() << msg;
            Ogre::LogManager::getSingleton().logError(msg.toStdString());
            SentryReporter::captureMessage(msg, "error");
            delete scene;
            return -1;
        }

        delete scene;
        reportProgress(100, QStringLiteral("Done."));
    } catch (std::exception& ex) {
        auto msg = QString("Scene export failed: %1").arg(ex.what());
        Ogre::LogManager::getSingleton().logError(msg.toStdString());
        SentryReporter::captureMessage(msg, "error");
        return -1;
    } catch (...) {
        Ogre::LogManager::getSingleton().logError("Scene export failed with unknown exception");
        return -1;
    }

    return 0;
}

// ─── Scene-level import: glTF → multiple scene nodes ────────────────
bool MeshImporterExporter::sceneImporter(const QString &_uri)
{
    if (_uri.isEmpty()) return false;

    QFileInfo file(_uri);
    if (!file.exists()) return false;

    ensureResourceGroup(file.path());

    // Parse the file BEFORE clearing the scene so we don't destroy
    // the user's work if the file is invalid.
    Assimp::Importer assimpImporter;
    assimpImporter.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

    unsigned int flags = aiProcess_CalcTangentSpace |
                         aiProcess_JoinIdenticalVertices |
                         aiProcess_Triangulate |
                         aiProcess_RemoveComponent |
                         aiProcess_GenSmoothNormals |
                         aiProcess_ValidateDataStructure |
                         aiProcess_LimitBoneWeights |
                         aiProcess_SortByPType |
                         aiProcess_ImproveCacheLocality |
                         aiProcess_FixInfacingNormals |
                         aiProcess_PopulateArmatureData |
                         aiProcess_OptimizeMeshes |
                         aiProcess_GlobalScale;

    const aiScene* scene = assimpImporter.ReadFile(file.filePath().toStdString(), flags);
    if (!scene || !scene->mRootNode)
    {
        Ogre::LogManager::getSingleton().logError(
            "Scene import failed: " + std::string(assimpImporter.GetErrorString()));
        return false;
    }

    // File is valid — now clear existing scene
    SelectionSet::getSingleton()->clearList();
    auto* manager = Manager::getSingleton();
    emit manager->sceneClearing();  // let listeners clean up before nodes are destroyed
    auto sceneNodesCopy = manager->getSceneNodes();
    for (auto* sn : sceneNodesCopy)
        manager->destroySceneNode(sn);

    try {

        // Process materials
        MaterialProcessor materialProcessor;
        materialProcessor.loadScene(scene);

        // Build set of bone names to distinguish bones from scene nodes
        std::set<std::string> boneNames;
        for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi)
        {
            const aiMesh* mesh = scene->mMeshes[mi];
            for (unsigned int bi = 0; bi < mesh->mNumBones; ++bi)
                boneNames.insert(mesh->mBones[bi]->mName.C_Str());
        }

        // Helper: decompose aiMatrix4x4 into position, orientation, scale
        auto decomposeTransform = [](const aiMatrix4x4& m,
                                     Ogre::Vector3& pos, Ogre::Quaternion& orient, Ogre::Vector3& scale)
        {
            aiVector3D aiPos, aiScale;
            aiQuaternion aiRot;
            m.Decompose(aiScale, aiRot, aiPos);
            pos = Ogre::Vector3(aiPos.x, aiPos.y, aiPos.z);
            orient = Ogre::Quaternion(aiRot.w, aiRot.x, aiRot.y, aiRot.z);
            scale = Ogre::Vector3(aiScale.x, aiScale.y, aiScale.z);
        };

        // Collect all mesh-bearing nodes with their world transforms
        struct NodeEntry {
            const aiNode* node;
            aiMatrix4x4 worldTransform;
        };
        std::vector<NodeEntry> meshNodes;

        std::function<void(const aiNode*, const aiMatrix4x4&)> collectNodes;
        collectNodes = [&](const aiNode* node, const aiMatrix4x4& parentTransform)
        {
            aiMatrix4x4 worldTransform = parentTransform * node->mTransformation;
            bool isBone = boneNames.count(node->mName.C_Str()) > 0 && node->mNumMeshes == 0;

            if (node->mNumMeshes > 0 && !isBone)
                meshNodes.push_back({node, worldTransform});

            if (!isBone)
            {
                for (unsigned int ci = 0; ci < node->mNumChildren; ++ci)
                    collectNodes(node->mChildren[ci], worldTransform);
            }
        };
        collectNodes(scene->mRootNode, aiMatrix4x4());

        // Create one Ogre entity per mesh-bearing node
        for (const auto& entry : meshNodes)
        {
            const aiNode* node = entry.node;

            QString nodeName = QString::fromUtf8(node->mName.C_Str());

            // Detect the synthetic "<parent>/<parent>_mesh" pattern produced
            // by our scene exporter for skeletal entities. Only strip the suffix
            // and enable prefix-based bone filtering when this exact pattern is present.
            bool isSyntheticMeshNode = false;
            if (node->mParent && node->mParent != scene->mRootNode)
            {
                QString parentName = QString::fromUtf8(node->mParent->mName.C_Str());
                if (!parentName.isEmpty() && nodeName == parentName + "_mesh")
                {
                    isSyntheticMeshNode = true;
                    nodeName = parentName;
                }
            }

            if (nodeName.isEmpty())
                nodeName = QString("SceneNode_%1").arg(manager->getSceneNodes().size());

            // Make unique name
            QString baseName = nodeName;
            int counter = 1;
            while (manager->hasSceneNode(nodeName) || manager->isForbiddenNodeName(nodeName))
                nodeName = QString("%1_%2").arg(baseName).arg(counter++);

            std::string meshName = (nodeName + "_mesh").toStdString();
            if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
                Ogre::MeshManager::getSingleton().remove(old);

            // Entity prefix for bone/animation filtering.
            // Only applied when we detected our synthetic export pattern AND
            // there are multiple entities (shared skin scenario).
            std::string entityPrefix;
            if (isSyntheticMeshNode && meshNodes.size() > 1)
            {
                entityPrefix = nodeName.toStdString() + "_";
            }

            // Check if any of this node's meshes have bones
            bool hasBones = false;
            for (unsigned int mi = 0; mi < node->mNumMeshes && !hasBones; ++mi)
            {
                if (scene->mMeshes[node->mMeshes[mi]]->mNumBones > 0)
                    hasBones = true;
            }

            // Create per-entity skeleton scoped to only this node's meshes
            Ogre::SkeletonPtr skeleton;
            if (hasBones)
            {
                std::string skelName = meshName + ".skeleton";
                if (auto old = Ogre::SkeletonManager::getSingleton().getByName(skelName))
                    Ogre::SkeletonManager::getSingleton().remove(old);

                skeleton = Ogre::SkeletonManager::getSingleton().create(
                    skelName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);

                std::vector<aiMesh*> nodeMeshPtrs;
                for (unsigned int mi = 0; mi < node->mNumMeshes; ++mi)
                    nodeMeshPtrs.push_back(scene->mMeshes[node->mMeshes[mi]]);

                // Build skeleton directly from mesh bone data.
                // When a shared glTF skin merges bones from multiple entities,
                // use the entity prefix to include only this entity's bones.

                // Step 1: Create all bones and compute global transforms
                std::map<std::string, Ogre::Matrix4> boneGlobalTransforms;
                for (auto* m : nodeMeshPtrs)
                {
                    for (unsigned int bi = 0; bi < m->mNumBones; ++bi)
                    {
                        aiBone* bone = m->mBones[bi];
                        std::string name = bone->mName.C_Str();

                        // Filter: only include bones matching this entity's prefix
                        if (!entityPrefix.empty()
                            && name.rfind(entityPrefix, 0) != 0)
                            continue;

                        if (skeleton->hasBone(name))
                            continue;

                        skeleton->createBone(name);

                        // Global transform = inverse of offset matrix
                        aiMatrix4x4 off = bone->mOffsetMatrix;
                        aiMatrix4x4 global = off;
                        global.Inverse();
                        Ogre::Matrix4 ogreGlobal(
                            global.a1, global.a2, global.a3, global.a4,
                            global.b1, global.b2, global.b3, global.b4,
                            global.c1, global.c2, global.c3, global.c4,
                            global.d1, global.d2, global.d3, global.d4);
                        boneGlobalTransforms[name] = ogreGlobal;
                    }
                }

                // Step 2: Set parent-child relationships using mNode hierarchy
                // (only between bones that both exist in this skeleton)
                for (auto* m : nodeMeshPtrs)
                {
                    for (unsigned int bi = 0; bi < m->mNumBones; ++bi)
                    {
                        aiBone* bone = m->mBones[bi];
                        if (!bone->mNode || !bone->mNode->mParent)
                            continue;

                        // Walk up the node tree to find a parent that's in this skeleton
                        aiNode* parentNode = bone->mNode->mParent;
                        while (parentNode)
                        {
                            std::string parentName = parentNode->mName.C_Str();
                            if (skeleton->hasBone(parentName))
                            {
                                Ogre::Bone* parentBone = skeleton->getBone(parentName);
                                Ogre::Bone* childBone = skeleton->getBone(bone->mName.C_Str());
                                if (!childBone->getParent())
                                    parentBone->addChild(childBone);
                                break;
                            }
                            parentNode = parentNode->mParent;
                        }
                    }
                }

                // Step 3: Apply transforms (convert global to local)
                for (auto& [name, globalTf] : boneGlobalTransforms)
                {
                    Ogre::Bone* bone = skeleton->getBone(name);
                    Ogre::Matrix4 localTf = globalTf;
                    if (bone->getParent())
                    {
                        auto parentIt = boneGlobalTransforms.find(bone->getParent()->getName());
                        if (parentIt != boneGlobalTransforms.end())
                            localTf = parentIt->second.inverse() * globalTf;
                    }
                    Ogre::Affine3 affine(localTf);
                    Ogre::Vector3 pos, scale;
                    Ogre::Quaternion orient;
                    affine.decomposition(pos, scale, orient);
                    bone->setPosition(pos);
                    bone->setOrientation(orient);
                    bone->setScale(scale);
                }

                skeleton->setBindingPose();

                // Filter animations: use entity prefix if available,
                // otherwise fall back to bone name matching
                if (scene->HasAnimations())
                {
                    std::vector<aiAnimation*> relevantAnims;
                    for (unsigned int ai = 0; ai < scene->mNumAnimations; ++ai)
                    {
                        aiAnimation* anim = scene->mAnimations[ai];
                        if (!entityPrefix.empty())
                        {
                            // Match by animation name prefix (e.g., "Hip Hop Dancing_mixamo.com")
                            std::string animName = anim->mName.C_Str();
                            if (animName.rfind(entityPrefix, 0) == 0)
                                relevantAnims.push_back(anim);
                        }
                        else
                        {
                            // No prefix: match by bone names in channels
                            for (unsigned int ci = 0; ci < anim->mNumChannels; ++ci)
                            {
                                if (skeleton->hasBone(anim->mChannels[ci]->mNodeName.C_Str()))
                                {
                                    relevantAnims.push_back(anim);
                                    break;
                                }
                            }
                        }
                    }

                    if (!relevantAnims.empty())
                    {
                        // Non-owning temp scene for AnimationProcessor
                        aiScene tempScene;
                        tempScene.mAnimations = relevantAnims.data();
                        tempScene.mNumAnimations = static_cast<unsigned int>(relevantAnims.size());
                        AnimationProcessor animationProcessor(skeleton);
                        animationProcessor.processAnimations(&tempScene);
                        tempScene.mAnimations = nullptr;
                        tempScene.mNumAnimations = 0;
                    }

                    // Remove empty animations (channels for other entities' bones
                    // got skipped, leaving zero tracks)
                    std::vector<Ogre::String> emptyAnims;
                    for (unsigned short ai = 0; ai < skeleton->getNumAnimations(); ++ai)
                    {
                        auto* anim = skeleton->getAnimation(ai);
                        if (anim->getNumNodeTracks() == 0)
                            emptyAnims.push_back(anim->getName());
                    }
                    for (const auto& name : emptyAnims)
                        skeleton->removeAnimation(name);
                }
            }

            // Use MeshProcessor: temporarily suppress children to process
            // only this node's meshes (not descendants).
            unsigned int savedNumChildren = node->mNumChildren;
            aiNode** savedChildren = node->mChildren;
            const_cast<aiNode*>(node)->mNumChildren = 0;
            const_cast<aiNode*>(node)->mChildren = nullptr;

            // When entity prefix filtering is active, strip foreign bones
            // from meshes so MeshProcessor doesn't try to look them up
            // in this entity's skeleton (they would cause getBone() to throw).
            struct SavedBones {
                aiMesh* mesh;
                aiBone** origBones;
                unsigned int origNumBones;
                std::vector<aiBone*> filteredBones;
            };
            std::vector<SavedBones> savedBones;

            if (!entityPrefix.empty() && skeleton)
            {
                for (unsigned int mi = 0; mi < node->mNumMeshes; ++mi)
                {
                    aiMesh* mesh = scene->mMeshes[node->mMeshes[mi]];
                    if (mesh->mNumBones == 0) continue;

                    SavedBones sb;
                    sb.mesh = mesh;
                    sb.origBones = mesh->mBones;
                    sb.origNumBones = mesh->mNumBones;
                    for (unsigned int bi = 0; bi < mesh->mNumBones; ++bi)
                    {
                        std::string bname = mesh->mBones[bi]->mName.C_Str();
                        if (bname.rfind(entityPrefix, 0) == 0)
                            sb.filteredBones.push_back(mesh->mBones[bi]);
                    }
                    mesh->mBones = sb.filteredBones.data();
                    mesh->mNumBones = static_cast<unsigned int>(sb.filteredBones.size());
                    savedBones.push_back(std::move(sb));
                }
            }

            MeshProcessor meshProcessor(skeleton);
            meshProcessor.processNode(const_cast<aiNode*>(node), const_cast<aiScene*>(scene));

            // Restore original bone arrays
            for (auto& sb : savedBones)
            {
                sb.mesh->mBones = sb.origBones;
                sb.mesh->mNumBones = sb.origNumBones;
            }

            const_cast<aiNode*>(node)->mNumChildren = savedNumChildren;
            const_cast<aiNode*>(node)->mChildren = savedChildren;

            Ogre::MeshPtr ogreMesh = meshProcessor.createMesh(
                meshName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
                materialProcessor);

            // Create scene node with decomposed world transform
            Ogre::SceneNode* sn = manager->addSceneNode(nodeName);

            Ogre::Vector3 pos;
            Ogre::Quaternion orient;
            Ogre::Vector3 scale;
            decomposeTransform(entry.worldTransform, pos, orient, scale);
            sn->setPosition(pos);
            sn->setOrientation(orient);
            sn->setScale(scale);

            manager->createEntity(sn, ogreMesh);
        }

        return true;
    } catch (Ogre::Exception& e) {
        Ogre::LogManager::getSingleton().logError("Scene import failed: " + e.getFullDescription());
        SentryReporter::captureMessage(
            QString("Scene import failed: %1").arg(e.getFullDescription().c_str()), "error");
        return false;
    } catch (std::exception& ex) {
        auto msg = QString("Scene import failed: %1").arg(ex.what());
        Ogre::LogManager::getSingleton().logError(msg.toStdString());
        SentryReporter::captureMessage(msg, "error");
        return false;
    }
}
