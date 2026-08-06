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
#include "AlembicImporter.h"
#include "FeedbackReportHelper.h"
#include <assimp/Importer.hpp>
#include <assimp/Exporter.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <QCryptographicHash>
#include <QFileDialog>
#include <QImage>
#include <QMessageBox>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <unordered_set>
#include <unordered_map>
#include <set>
#include <array>
#include <functional>
#include <limits>
#include <cmath>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include "OgreXML/OgreXMLMeshSerializer.h"
#include "OgreXML/OgreXMLSkeletonSerializer.h"
#include "OgreXML/pugixml.hpp"

#include "AnimationMerger.h"
#include "MorphAnimationManager.h"
#include "LightManager.h"
#include "Manager.h"
#include "SceneLightsIO.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "FaceRig/FaceRigAttach.h"
#include "ExportOptimizer.h"
#include "RTShaderHelper.h"

// Debug trace helper for #681 export-crash repro. Writes to a
// dedicated file because stderr is consumed by MCP mode and the
// macOS .app bundle's launchd wrapper. Synchronous-flushed so we
// see the last line before SIGSEGV. Remove once fixed.
#include "Assimp/Importer.h"
#include "Assimp/MaterialProcessor.h"
#include "NodeAnimationManager.h"
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
#include <OgreRTShaderSystem.h>
#include <OgreDataStream.h>
#include <OgrePixelFormat.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>
#include <OgrePose.h>

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

namespace {

void showImportProblem(const QString& title,
                       const QString& text,
                       const QString& format,
                       const QString& errorMessage = QString())
{
    FeedbackReportHelper::showFailureWithReportOption(
        nullptr, title, text, FeedbackReportHelper::importFailurePrefill(format, errorMessage));
}

// Returns true when `tex` is safe to feed to `Ogre::Texture::convertToImage`.
// Guards against the failure modes that caused the #681 SIGSEGV:
//   1. `_getTexturePtr()` returns a null TexturePtr (TUS references a
//      texture that was never loaded — e.g. a .mesh sidecar with a
//      missing texture file).
//   2. `isLoaded()` is false (some imports defer GPU upload until first
//      render; convertToImage then reads a null buffer and crashes).
//      We trigger an explicit `load()` first and bail if it fails.
//   3. Zero-size textures from placeholder / generated TUS.
//   4. Non-2D textures — the exporter's image writer only handles 2D.
bool isTextureSerializable(const Ogre::TexturePtr& tex)
{
    if (!tex) return false;
    if (tex->getTextureType() != Ogre::TEX_TYPE_2D) return false;
    if (!tex->isLoaded()) {
        try { tex->load(); }
        catch (const Ogre::Exception&) { return false; }
        if (!tex->isLoaded()) return false;
    }
    return tex->getWidth() != 0 && tex->getHeight() != 0;
}

// Write a single texture as an image file next to `file`. Logs the
// error to Ogre's LogManager if Ogre or stdlib throws; never lets an
// exception escape the export pipeline (one bad texture must not
// abort the rest of the export).
void saveTextureAsImage(const Ogre::TexturePtr& tex, const QFileInfo& file)
{
    const QString saveName = MeshImporterExporter::exportTextureName(
        QString::fromStdString(tex->getName()));
    const QString outPath = file.path() + "/" + saveName;

    // PREFER copying the ORIGINAL texture file byte-for-byte when it's findable
    // on disk. `convertToImage` re-packs the GPU texture, which THROWS for GPU
    // formats Ogre can't pack back to a saveable image (e.g. the "pack to
    // PF_UNKNOWN" failure on some compressed/BGR textures) — losing the texture
    // entirely. A file copy is lossless and sidesteps the repack. Fall back to
    // convertToImage only when the source file can't be located.
    const QString texName = QString::fromStdString(tex->getName());
    QString srcPath;
    // 1) Ogre resource group's on-disk location for this named texture.
    try {
        const Ogre::String grp =
            Ogre::ResourceGroupManager::getSingleton().findGroupContainingResource(
                tex->getName());
        Ogre::FileInfoListPtr fil =
            Ogre::ResourceGroupManager::getSingleton().findResourceFileInfo(
                grp, tex->getName());
        if (fil && !fil->empty())
            srcPath = QString::fromStdString(
                fil->front().archive->getName() + "/" + fil->front().filename);
    } catch (const Ogre::Exception&) { /* not in a group — try raw path */ }
    // 2) The texture name may itself be an absolute/relative path.
    if (srcPath.isEmpty() && QFileInfo::exists(texName))
        srcPath = texName;

    if (!srcPath.isEmpty() && QFileInfo(srcPath).isFile()
        && QFileInfo(srcPath).canonicalFilePath() != QFileInfo(outPath).canonicalFilePath()) {
        QFile::remove(outPath);           // overwrite a stale copy
        if (QFile::copy(srcPath, outPath))
            return;                       // lossless copy succeeded
        // else fall through to the repack attempt
    }

    try {
        Ogre::Image img;
        tex->convertToImage(img, true);
        img.save(outPath.toStdString());
    } catch (const Ogre::Exception& ex) {
        Ogre::LogManager::getSingleton().logError(
            std::string("Failed to save texture '") + tex->getName() + "': " + ex.what()
            + " (and no source file to copy)");
    } catch (const std::exception& ex) {
        Ogre::LogManager::getSingleton().logError(
            std::string("Failed to save texture '") + tex->getName() + "': " + ex.what()
            + " (and no source file to copy)");
    }
}

// Yield every CONTENT_NAMED texture referenced by a material into the
// given functor. Flattens the technique → pass → TUS triple-loop so
// the call site stays at one level of nesting.
template <typename Fn>
void forEachNamedTexture(const Ogre::MaterialPtr& material, Fn&& fn)
{
    for (const auto& technique : material->getTechniques()) {
        for (const auto& pass : technique->getPasses()) {
            for (const auto& tus : pass->getTextureUnitStates()) {
                if (tus->getContentType() != Ogre::TextureUnitState::CONTENT_NAMED)
                    continue;
                fn(tus->_getTexturePtr());
            }
        }
    }
}

} // namespace

void MeshImporterExporter::exportTextures(const Ogre::MaterialPtr& material, const QFileInfo& file)
{
    forEachNamedTexture(material, [&](const Ogre::TexturePtr& tex) {
        if (isTextureSerializable(tex))
            saveTextureAsImage(tex, file);
    });
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
            } else if (tusName == "diffuse_map"
                       || tusName.empty()
                       || std::all_of(tusName.begin(), tusName.end(),
                                      [](unsigned char c) { return std::isdigit(c); })) {
                // Legacy Phong diffuse, anonymous TUS, or a TUS auto-named
                // by Ogre's .material script parser (texture_unit blocks
                // without an explicit name get assigned "0", "1", ... at
                // parse time). All three signal "this is a plain diffuse
                // texture with no canonical PBR slot" — route as DIFFUSE
                // so glTF / Phong consumers find a baseColorTexture.
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
    const std::map<std::string, unsigned int, std::less<>>& matIndexMap,
    bool preferNgonFaces = true)
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
    const bool hasNgon = preferNgonFaces && readNgonFacesFromMesh(
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

static std::string sceneNgonFacesMetaKey(unsigned int subIndex)
{
    return std::string("qtme.faces.") + std::to_string(subIndex);
}

static std::string sceneNgonFaceMetaKey(unsigned int subIndex, unsigned int faceIndex)
{
    return sceneNgonFacesMetaKey(subIndex) + ".face." + std::to_string(faceIndex);
}

static aiString encodeSceneNgonFaceMetadata(const std::vector<unsigned int>& face)
{
    std::string encoded;
    for (size_t vertexIndex = 0; vertexIndex < face.size(); ++vertexIndex)
    {
        if (!encoded.empty())
            encoded.push_back(',');
        encoded += std::to_string(face[vertexIndex]);
    }
    return aiString(encoded);
}

static bool decodeSceneNgonFaceMetadata(const aiString& encoded, std::vector<unsigned int>& outFace)
{
    outFace.clear();
    const std::string value = encoded.C_Str();
    if (value.empty())
        return false;

    size_t start = 0;
    while (start < value.size())
    {
        const size_t end = value.find(',', start);
        const std::string token = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (token.empty())
            return false;
        errno = 0;
        char* tail = nullptr;
        const unsigned long parsed = std::strtoul(token.c_str(), &tail, 10);
        if (!tail || *tail != '\0' || errno == ERANGE ||
            parsed > std::numeric_limits<unsigned int>::max())
            return false;
        outFace.push_back(static_cast<unsigned int>(parsed));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }

    return outFace.size() >= 3;
}

static bool remapNgonFaces(const std::vector<unsigned int>& remap,
                           std::vector<std::vector<unsigned int>>& faces)
{
    if (remap.empty())
        return true;

    for (auto& face : faces)
    {
        for (auto& idx : face)
        {
            if (idx >= remap.size() || remap[idx] == UINT_MAX)
                return false;
            idx = remap[idx];
        }
    }
    return true;
}

static bool decodeSceneNgonFacesMetadata(const aiMetadata* metadata,
                                         unsigned int subIndex,
                                         std::vector<std::vector<unsigned int>>& outFaces)
{
    outFaces.clear();
    if (!metadata)
        return false;

    const std::string prefix = sceneNgonFacesMetaKey(subIndex) + ".face.";
    std::map<unsigned int, std::vector<unsigned int>> orderedFaces;
    for (unsigned int i = 0; i < metadata->mNumProperties; ++i)
    {
        const std::string key = metadata->mKeys[i].C_Str();
        if (key.rfind(prefix, 0) != 0)
            continue;

        const std::string suffix = key.substr(prefix.size());
        if (suffix.empty())
            return false;
        errno = 0;
        char* tail = nullptr;
        const unsigned long faceIndex = std::strtoul(suffix.c_str(), &tail, 10);
        if (!tail || *tail != '\0' || errno == ERANGE ||
            faceIndex > std::numeric_limits<unsigned int>::max())
            return false;

        aiString encoded;
        if (!metadata->Get(i, encoded))
            return false;

        std::vector<unsigned int> face;
        if (!decodeSceneNgonFaceMetadata(encoded, face))
            return false;
        orderedFaces[static_cast<unsigned int>(faceIndex)] = std::move(face);
    }

    if (orderedFaces.empty())
        return false;

    outFaces.reserve(orderedFaces.size());
    for (auto& [faceIndex, face] : orderedFaces)
        outFaces.push_back(std::move(face));

    return true;
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
static std::vector<unsigned int> compactAiMesh(aiMesh* aiM)
{
    if (!aiM || aiM->mNumVertices == 0 || aiM->mNumFaces == 0)
        return {};

    std::vector<bool> used(aiM->mNumVertices, false);
    for (unsigned int f = 0; f < aiM->mNumFaces; ++f)
        for (unsigned int v = 0; v < aiM->mFaces[f].mNumIndices; ++v)
            if (aiM->mFaces[f].mIndices[v] < aiM->mNumVertices)
                used[aiM->mFaces[f].mIndices[v]] = true;

    std::vector<unsigned int> remap(aiM->mNumVertices, UINT_MAX);
    unsigned int newCount = 0;
    for (unsigned int i = 0; i < aiM->mNumVertices; ++i)
        if (used[i]) remap[i] = newCount++;

    if (newCount == aiM->mNumVertices)
        return remap; // nothing to compact

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

    return remap;
}

// Split any aiMesh with > 65535 vertices into ≤65535-vertex chunks before
// export. Assimp's glTF2/OBJ/FBX exporters truncate the VERTEX accessors at
// 65536 while keeping the 32-bit index buffer intact — so indices ≥65536
// dangle and a large generated mesh (e.g. TripoSG/TripoSR at high resolution,
// 80k+ verts) reimports torn into pieces. Splitting per-triangle-group into
// sub-65536-vertex meshes sidesteps the exporter bug entirely; each chunk owns
// a compact, valid vertex range. Faces are the split unit (each face's 3 verts
// are copied into the chunk with local indices), so no index exceeds the limit.
//
// SCOPE: handles POSITION / NORMAL / TEXCOORD0 / COLOR0 meshes — the generated
// image-to-3D output. Meshes carrying BONES or ANIM(morph) meshes are left
// UNTOUCHED (returned as-is): those need the weight/target arrays remapped per
// chunk, which is a heavier follow-up; skinned meshes rarely exceed the limit
// per submesh in practice. Returns the (possibly multiple) replacement meshes;
// the caller rebuilds scene->mMeshes and the node index arrays.
static std::vector<aiMesh*> splitLargeAiMesh(aiMesh* aiM)
{
    constexpr unsigned int kMaxVerts = 65535;
    if (!aiM || aiM->mNumVertices <= kMaxVerts || aiM->mNumFaces == 0)
        return { aiM };
    // Don't risk skinned / morph meshes — leave them for a dedicated pass.
    if (aiM->mNumBones > 0 || aiM->mNumAnimMeshes > 0)
        return { aiM };
    // Only split all-triangle meshes. readSubmeshGeometry deliberately emits
    // quads / n-gons from the cached qtme.faces list; the per-face repack
    // below is triangle-only, so splitting a polygon mesh would DROP those
    // faces. A >65k-vertex quad/n-gon mesh is rare — leave it intact (correct,
    // if still subject to the Assimp exporter cap) rather than lose geometry.
    for (unsigned int f = 0; f < aiM->mNumFaces; ++f)
        if (aiM->mFaces[f].mNumIndices != 3)
            return { aiM };

    const bool hasN  = aiM->mNormals != nullptr;
    const bool hasUV = aiM->mTextureCoords[0] != nullptr;
    const bool hasC  = aiM->mColors[0] != nullptr;

    std::vector<aiMesh*> out;
    unsigned int faceStart = 0;
    while (faceStart < aiM->mNumFaces) {
        // Greedily pack faces into a chunk until adding one more would exceed
        // kMaxVerts unique vertices. Each face contributes ≤3 new verts.
        std::vector<unsigned int> srcVerts;   // chunk-local → source vertex id
        std::unordered_map<unsigned int, unsigned int> remap;  // source → local
        std::vector<std::array<unsigned int, 3>> faces;
        unsigned int f = faceStart;
        for (; f < aiM->mNumFaces; ++f) {
            const aiFace& face = aiM->mFaces[f];
            if (face.mNumIndices != 3) continue;   // triangulated on read
            // Would this face push us over the limit? (≤3 new verts.)
            if (srcVerts.size() + 3 > kMaxVerts && !faces.empty())
                break;
            std::array<unsigned int, 3> lf{};
            for (int v = 0; v < 3; ++v) {
                const unsigned int s = face.mIndices[v];
                auto it = remap.find(s);
                unsigned int local;
                if (it == remap.end()) {
                    local = static_cast<unsigned int>(srcVerts.size());
                    remap.emplace(s, local);
                    srcVerts.push_back(s);
                } else {
                    local = it->second;
                }
                lf[v] = local;
            }
            faces.push_back(lf);
        }

        auto* chunk = new aiMesh();
        chunk->mMaterialIndex = aiM->mMaterialIndex;
        chunk->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
        chunk->mNumVertices = static_cast<unsigned int>(srcVerts.size());
        chunk->mVertices = new aiVector3D[chunk->mNumVertices];
        if (hasN)  chunk->mNormals = new aiVector3D[chunk->mNumVertices];
        if (hasUV) {
            chunk->mNumUVComponents[0] = aiM->mNumUVComponents[0];
            chunk->mTextureCoords[0] = new aiVector3D[chunk->mNumVertices];
        }
        if (hasC)  chunk->mColors[0] = new aiColor4D[chunk->mNumVertices];
        for (unsigned int i = 0; i < chunk->mNumVertices; ++i) {
            const unsigned int s = srcVerts[i];
            chunk->mVertices[i] = aiM->mVertices[s];
            if (hasN)  chunk->mNormals[i]        = aiM->mNormals[s];
            if (hasUV) chunk->mTextureCoords[0][i] = aiM->mTextureCoords[0][s];
            if (hasC)  chunk->mColors[0][i]      = aiM->mColors[0][s];
        }
        chunk->mNumFaces = static_cast<unsigned int>(faces.size());
        chunk->mFaces = new aiFace[chunk->mNumFaces];
        for (unsigned int i = 0; i < chunk->mNumFaces; ++i) {
            chunk->mFaces[i].mNumIndices = 3;
            chunk->mFaces[i].mIndices = new unsigned int[3];
            for (int v = 0; v < 3; ++v)
                chunk->mFaces[i].mIndices[v] = faces[i][v];
        }
        out.push_back(chunk);
        faceStart = f;
    }

    delete aiM;   // replaced by the chunks
    return out;
}

// Rewrite scene->mMeshes (and every node's mMeshes index array) so any mesh
// exceeding the 65535-vertex exporter limit is replaced by its split chunks.
// No-op when nothing exceeds the limit (the common case).
static void splitLargeMeshesForExport(aiScene* scene)
{
    if (!scene || scene->mNumMeshes == 0) return;
    bool anyOver = false;
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
        if (scene->mMeshes[i] && scene->mMeshes[i]->mNumVertices > 65535
            && scene->mMeshes[i]->mNumBones == 0
            && scene->mMeshes[i]->mNumAnimMeshes == 0) { anyOver = true; break; }
    if (!anyOver) return;

    // Build the new mesh list, recording each old index's replacement range.
    std::vector<aiMesh*> newMeshes;
    std::vector<std::vector<unsigned int>> oldToNew(scene->mNumMeshes);
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        std::vector<aiMesh*> parts = splitLargeAiMesh(scene->mMeshes[i]);
        for (aiMesh* p : parts) {
            oldToNew[i].push_back(static_cast<unsigned int>(newMeshes.size()));
            newMeshes.push_back(p);
        }
    }
    delete[] scene->mMeshes;
    scene->mNumMeshes = static_cast<unsigned int>(newMeshes.size());
    scene->mMeshes = new aiMesh*[scene->mNumMeshes];
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
        scene->mMeshes[i] = newMeshes[i];

    // Fix up every node's mesh-index array to point at the new indices.
    std::function<void(aiNode*)> fixNode = [&](aiNode* n) {
        if (!n) return;
        if (n->mNumMeshes > 0) {
            std::vector<unsigned int> mapped;
            for (unsigned int k = 0; k < n->mNumMeshes; ++k) {
                const unsigned int old = n->mMeshes[k];
                if (old < oldToNew.size())
                    for (unsigned int ni : oldToNew[old]) mapped.push_back(ni);
            }
            delete[] n->mMeshes;
            n->mNumMeshes = static_cast<unsigned int>(mapped.size());
            n->mMeshes = new unsigned int[n->mNumMeshes];
            for (unsigned int k = 0; k < n->mNumMeshes; ++k)
                n->mMeshes[k] = mapped[k];
        }
        for (unsigned int c = 0; c < n->mNumChildren; ++c)
            fixNode(n->mChildren[c]);
    };
    fixNode(scene->mRootNode);
}

// Slice A4a: emit per-pose aiAnimMesh entries on an aiMesh, sourced
// from the Ogre mesh's pose list filtered to the matching submesh.
// `aiAnimMesh::mVertices` carries absolute deformed positions (base
// + per-vertex delta from `Ogre::Pose::getVertexOffsets()`); the
// vertex layout matches `aiM->mVertices` at the time of the call,
// so the caller must run this BEFORE `compactAiMesh` and then call
// `remapAiMeshMorphTargets` after to keep the arrays parallel.
//
// `new` allocations cross the Assimp C-API ownership boundary
// (aiScene's dtor frees mAnimMeshes), so smart pointers don't fit
// here — same convention every other aiScene field uses in this
// file.
static void attachMorphTargetsToAiMesh(aiMesh* aiM,
                                       const Ogre::MeshPtr& mesh,
                                       const Ogre::SubMesh* subMesh,
                                       unsigned int si)
{
    if (!aiM || aiM->mNumVertices == 0) return;
    const Ogre::PoseList& poseList = mesh->getPoseList();
    if (poseList.empty()) return;

    const unsigned short targetHandle = subMesh->useSharedVertices
        ? 0
        : static_cast<unsigned short>(si + 1);
    std::vector<Ogre::Pose*> submeshPoses;
    for (Ogre::Pose* p : poseList) {
        if (p && p->getTarget() == targetHandle)
            submeshPoses.push_back(p);
    }
    if (submeshPoses.empty()) return;

    const unsigned int preCompactCount = aiM->mNumVertices;
    aiM->mNumAnimMeshes = static_cast<unsigned int>(submeshPoses.size());
    aiM->mAnimMeshes = new aiAnimMesh*[aiM->mNumAnimMeshes];  // NOSONAR — Assimp owns
    for (size_t pi = 0; pi < submeshPoses.size(); ++pi) {
        const Ogre::Pose* p = submeshPoses[pi];
        auto* am = new aiAnimMesh();  // NOSONAR — Assimp owns
        aiM->mAnimMeshes[pi] = am;
        am->mName = aiString(p->getName());
        am->mNumVertices = preCompactCount;
        am->mVertices = new aiVector3D[preCompactCount];  // NOSONAR — Assimp owns
        // Seed with base positions (vertices the pose doesn't touch
        // keep their base location).
        for (unsigned int v = 0; v < preCompactCount; ++v)
            am->mVertices[v] = aiM->mVertices[v];
        // Apply per-vertex deltas. Pose offsets are keyed by vertex
        // index in submesh-local pre-compact space.
        for (const auto& [vi, delta] : p->getVertexOffsets()) {
            if (vi >= preCompactCount) continue;
            am->mVertices[vi].x += delta.x;
            am->mVertices[vi].y += delta.y;
            am->mVertices[vi].z += delta.z;
        }
        // Non-zero default weight so post-process steps that filter
        // "inactive" anim meshes don't drop the entry. Runtime weight
        // is driven by the consuming app.
        am->mWeight = 1.0f;
    }
}

// Slice A4a: after compactAiMesh has reordered/deduped aiM's
// vertices, apply the same remap to every attached aiAnimMesh so
// the morph-target vertex arrays stay parallel to aiM->mVertices.
static void remapAiMeshMorphTargets(aiMesh* aiM,
                                    const std::vector<unsigned int>& remap,
                                    unsigned int preCompactCount)
{
    if (!aiM || aiM->mNumAnimMeshes == 0) return;
    if (remap.empty() || preCompactCount == 0) return;
    if (aiM->mNumVertices == preCompactCount) return;  // nothing to compact

    for (unsigned int ai = 0; ai < aiM->mNumAnimMeshes; ++ai) {
        aiAnimMesh* am = aiM->mAnimMeshes[ai];
        if (!am || !am->mVertices) continue;
        auto* compact = new aiVector3D[aiM->mNumVertices];  // NOSONAR — Assimp owns
        for (unsigned int oldIdx = 0; oldIdx < preCompactCount; ++oldIdx) {
            if (remap[oldIdx] == UINT_MAX) continue;
            compact[remap[oldIdx]] = am->mVertices[oldIdx];
        }
        delete[] am->mVertices;  // NOSONAR — we own this allocation
        am->mVertices = compact;
        am->mNumVertices = aiM->mNumVertices;
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

// Forward decl — defined below (scene-export section). Emits aiAnimations for
// the SceneManager-level NodeAnimationManager clips (#517 C5) that target the
// given node names.
static std::vector<aiAnimation*> buildNodeClipAnimations(
    const std::set<std::string, std::less<>>& exportedNodeNames);

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

        // Morph targets / blend shapes (slice A4a). Pulled into helpers
        // below to keep this loop's nesting level reasonable and the
        // "build then compact" two-pass structure obvious.
        const unsigned int preCompactCount = aiM->mNumVertices;
        attachMorphTargetsToAiMesh(aiM, mesh, subMesh, si);

        const std::vector<unsigned int> remap = compactAiMesh(aiM);
        remapAiMeshMorphTargets(aiM, remap, preCompactCount);
    }

    // --- Animations ---
    // Skeletal animations first (unchanged), then optionally one morph-weight
    // animation from the mesh's "MorphAnim" clip. Both are gathered into a
    // vector so the mAnimations array is sized to hold exactly what exists —
    // this covers the no-skeleton + morph-only case (mAnimations was previously
    // left null) and the has-skeleton + morph case (grow past the skeleton
    // count) without special-casing either.
    std::vector<aiAnimation*> animations;
    if (hasSkeleton && skeleton->getNumAnimations() > 0)
    {
        for (unsigned short ai = 0; ai < skeleton->getNumAnimations(); ++ai)
            animations.push_back(buildAiAnimation(skeleton->getAnimation(ai)));
    }

    // Node-transform clips (#517 C5): the single-entity export path
    // (Export Selected) must also carry node animation, or it drops. The
    // entity is named after its scene node, so export clips targeting that
    // node. Root node is named after the entity, matching the channel target.
    {
        std::set<std::string, std::less<>> nodeNames;
        nodeNames.insert(std::string(entity->getName()));
        auto nodeAnims = buildNodeClipAnimations(nodeNames);
        animations.insert(animations.end(), nodeAnims.begin(), nodeAnims.end());
    }

    // NOTE: morph-target SHAPES export fine (via aiMesh::mAnimMeshes above),
    // but morph-WEIGHT animation over time is NOT emitted through Assimp:
    // Assimp's glTF2/FBX exporters only translate aiAnimation NODE channels
    // (translation/rotation/scale) — they ignore aiAnimation::mMorphMeshChannels
    // entirely. An aiAnimation carrying only a morph channel also fails Assimp's
    // ValidateDataStructure ("mNumChannels is 0"), and a no-op node channel
    // added to satisfy it exports as a stray TRS animation on the mesh node that
    // corrupts reimport (froze the viewport). So we deliberately do NOT push a
    // morph-weight aiAnimation here. Round-tripping weights needs a post-write
    // glTF JSON injection (target.path="weights") — tracked as a follow-up.
    if (!animations.empty())
    {
        scene->mNumAnimations = static_cast<unsigned int>(animations.size());
        scene->mAnimations = new aiAnimation*[scene->mNumAnimations];  // NOSONAR — Assimp owns
        for (unsigned int ai = 0; ai < scene->mNumAnimations; ++ai)
            scene->mAnimations[ai] = animations[ai];
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

        std::string texName;
        for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
            auto* tus = pass->getTextureUnitState(i);
            const auto& tusName = tus->getName();
            if (tusName == "normal_map" || tusName == "NormalMap") {
                texName = tus->getTextureName();
                break;
            }
        }
        if (texName.empty()) {
            const auto& bindings = pass->getUserObjectBindings();
            auto any = bindings.getUserAny("qtme.normal_map");
            if (any.has_value()) {
                try {
                    texName = Ogre::any_cast<Ogre::String>(any);
                } catch (...) {
                }
            }
        }

        if (texName.empty())
            continue;

        log.logMessage("applyNormalMapsToEntity: normal map tex='" + texName + "' on mat='"
                           + mat->getName() + "'",
                       Ogre::LML_TRIVIAL);
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
        RTShaderHelper::finalizeShaderGenMaterial(mat, texName);
    }
}

void MeshImporterExporter::registerImportResourceDirectory(const QString& path)
{
    if (path.trimmed().isEmpty())
        return;

    const QString absPath = QFileInfo(path).absoluteFilePath();
    const std::string group = absPath.toStdString();
    auto& rgm = Ogre::ResourceGroupManager::getSingleton();

    // If the directory was already registered earlier in this process (common in
    // tests, the CLI, and generate→export→reimport flows), its file index may
    // predate freshly exported `.mesh`/`.material`/texture sidecars. Refresh by
    // ADDING the location again: a re-add re-lists the directory into the
    // group's index while ArchiveManager reuses the same Archive instance.
    //
    // Do NOT removeResourceLocation to force the refresh. The same directory is
    // routinely registered in MULTIPLE groups (this function itself puts it in a
    // dir-named group AND in DEFAULT; MeshGenBuilder registers texture dirs in
    // DEFAULT). Ogre shares ONE Archive object per path across groups, and
    // removeResourceLocation DESTROYS it — every other group's index then holds
    // a dangling Archive* and the next openResource crashes with EXC_BAD_ACCESS
    // inside ResourceGroupManager::openResourceImpl (reproduced under lldb via
    // generate → export .mesh → reload in the same session; same signature as
    // the long-standing "known GL/Xvfb" CI crashes in the CLI coverage suites).
    try {
        rgm.addResourceLocation(group, "FileSystem", group);
        rgm.initialiseResourceGroup(group);
    } catch (const Ogre::Exception& e) {
        Ogre::LogManager::getSingleton().logMessage(
            "Warning during resource group init: " + e.getFullDescription());
    }

    // Assimp's MaterialProcessor loads sidecar textures through the Default
    // group. Cloud downloads land in a cache folder outside resources.cfg, so
    // register the import directory there too (recursive for nested textures/).
    // Same re-add-to-refresh idiom as above — never remove.
    try {
        rgm.addResourceLocation(group,
                                "FileSystem",
                                Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
                                false,
                                true);
        rgm.initialiseResourceGroup(
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    } catch (const Ogre::Exception& e) {
        Ogre::LogManager::getSingleton().logMessage(
            "Warning during Default resource group registration: " + e.getFullDescription());
    }
}

void MeshImporterExporter::rebindEntityMaterials(Ogre::Entity* entity,
                                                   const QStringList& textureSearchRoots)
{
    if (!entity)
        return;
    if (!Ogre::Root::getSingletonPtr() || !Ogre::Root::getSingletonPtr()->getRenderSystem())
        return;

    if (!textureSearchRoots.isEmpty())
        hydrateEntityTexturesFromSearchPaths(entity, textureSearchRoots);

    std::unordered_set<const Ogre::Material*> processedMaterials;
    std::vector<Ogre::MaterialPtr> syncedMaterials;
    syncedMaterials.reserve(entity->getNumSubEntities());

    for (unsigned int si = 0; si < entity->getNumSubEntities(); ++si) {
        Ogre::SubEntity* sub = entity->getSubEntity(si);
        Ogre::MaterialPtr mat = sub->getMaterial();
        if (mat.isNull())
            continue;
        if (!mat->isLoaded()) {
            try {
                mat->load();
            } catch (const Ogre::Exception&) {
                continue;
            }
        }

        if (processedMaterials.insert(mat.get()).second) {
            RTShaderHelper::syncMaterialForViewport(mat);
            syncedMaterials.push_back(mat);
        }
    }

    for (const Ogre::MaterialPtr& mat : syncedMaterials) {
        if (!mat)
            continue;
        const std::string matName = mat->getName();
        for (Ogre::SceneNode* sn : Manager::getSingleton()->getSceneNodes()) {
            if (sn->getName().empty() || sn->getAttachedObjects().empty())
                continue;
            for (auto* obj : sn->getAttachedObjects()) {
                if (!obj || obj->getMovableType() != "Entity")
                    continue;
                auto* sceneEntity = static_cast<Ogre::Entity*>(obj);
                for (unsigned int si = 0; si < sceneEntity->getNumSubEntities(); ++si) {
                    Ogre::SubEntity* sub = sceneEntity->getSubEntity(si);
                    if (sub->getMaterialName() == matName)
                        sub->setMaterial(mat);
                }
            }
        }
    }
}

static QString resolveTexturePathOnDisk(const QString& textureName,
                                          const QHash<QString, QString>& filesByKey)
{
    if (textureName.isEmpty())
        return QString();

    QString key = QDir::fromNativeSeparators(textureName);
    while (key.startsWith(QStringLiteral("./")))
        key = key.mid(2);

    if (filesByKey.contains(key))
        return filesByKey.value(key);

    const QString base = QFileInfo(key).fileName();
    if (!base.isEmpty() && filesByKey.contains(base))
        return filesByKey.value(base);

    return QString();
}

static bool loadTextureImageIntoGroup(const QString& absolutePath,
                                      const Ogre::String& resourceName,
                                      const Ogre::String& group)
{
    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QByteArray bytes = file.readAll();
    file.close();
    if (bytes.isEmpty())
        return false;

    Ogre::Image img;
    Ogre::DataStreamPtr stream(new Ogre::MemoryDataStream(
        const_cast<char*>(bytes.constData()),
        static_cast<size_t>(bytes.size()),
        false,
        true));
    img.load(stream, QFileInfo(absolutePath).suffix().toLower().toStdString());

    auto& tm = Ogre::TextureManager::getSingleton();
    if (tm.resourceExists(resourceName, group)) {
        try {
            tm.remove(resourceName, group);
        } catch (const Ogre::Exception&) {
        }
    }
    tm.loadImage(resourceName, group, img);
    return true;
}

void MeshImporterExporter::hydrateEntityTexturesFromSearchPaths(Ogre::Entity* entity,
                                                                  const QStringList& searchRoots)
{
    if (!entity || searchRoots.isEmpty())
        return;

    // PASS 1 (cheap): collect the texture names this entity references whose
    // image is NOT already available to Ogre. In the common case (textures
    // embedded or sitting next to the mesh) every name is already loaded, so we
    // skip the directory walk entirely. This is the hot path — hydration is only
    // needed for genuinely-missing textures (cloud cache / moved files).
    auto& texMgr = Ogre::TextureManager::getSingleton();
    QSet<QString> neededBaseNames;     // basename (and rel) we still must find
    for (unsigned int si = 0; si < entity->getNumSubEntities(); ++si) {
        Ogre::SubEntity* sub = entity->getSubEntity(si);
        Ogre::MaterialPtr mat = sub->getMaterial();
        if (mat.isNull() || mat->getNumTechniques() == 0)
            continue;
        for (unsigned short ti = 0; ti < mat->getNumTechniques(); ++ti) {
            Ogre::Technique* tech = mat->getTechnique(ti);
            if (!tech) continue;
            for (unsigned short pi = 0; pi < tech->getNumPasses(); ++pi) {
                Ogre::Pass* pass = tech->getPass(pi);
                if (!pass) continue;
                for (unsigned short ui = 0; ui < pass->getNumTextureUnitStates(); ++ui) {
                    Ogre::TextureUnitState* tus = pass->getTextureUnitState(ui);
                    if (!tus) continue;
                    const std::string texName = tus->getTextureName();
                    if (texName.empty()) continue;
                    // Already loaded/known to Ogre? Then it's not missing.
                    if (texMgr.resourceExists(texName, mat->getGroup())
                        || texMgr.getByName(texName))
                        continue;
                    QString key = QDir::fromNativeSeparators(QString::fromStdString(texName));
                    while (key.startsWith(QStringLiteral("./"))) key = key.mid(2);
                    neededBaseNames.insert(QFileInfo(key).fileName());
                    neededBaseNames.insert(key);
                }
            }
        }
    }
    if (neededBaseNames.isEmpty())
        return;   // nothing missing — no directory walk needed

    // PASS 2 (bounded walk): only now scan the roots, and ONLY for the missing
    // names. Stop early once every needed name is found, and cap the total files
    // visited so a pathological import root (e.g. a huge repo/home dir with tens
    // of thousands of files) can't freeze the UI for minutes. We index any file
    // whose basename or relative path is one we're looking for.
    constexpr int kMaxFilesScanned = 20000;   // safety cap
    QHash<QString, QString> filesByKey;
    int scanned = 0;
    bool capped = false;
    for (const QString& rootRaw : searchRoots) {
        if (filesByKey.size() >= neededBaseNames.size()) break;  // found everything
        const QString root = QDir::fromNativeSeparators(QFileInfo(rootRaw).absoluteFilePath());
        if (root.isEmpty() || !QFileInfo(root).isDir())
            continue;
        QDir rootDir(root);
        QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            if (++scanned > kMaxFilesScanned) { capped = true; break; }
            const QString path = it.next();
            const QString base = QFileInfo(path).fileName();
            if (neededBaseNames.contains(base) && !filesByKey.contains(base))
                filesByKey.insert(base, path);
            const QString relNorm = QDir::fromNativeSeparators(rootDir.relativeFilePath(path));
            if (neededBaseNames.contains(relNorm) && !filesByKey.contains(relNorm))
                filesByKey.insert(relNorm, path);
            // Early-out: found a path for every needed name.
            if (filesByKey.size() >= neededBaseNames.size())
                break;
        }
        if (capped) break;
    }

    if (capped) {
        SentryReporter::addBreadcrumb(
            QStringLiteral("file.import"),
            QStringLiteral("Texture hydration walk hit the %1-file cap; some "
                           "missing textures may be unresolved").arg(kMaxFilesScanned));
    }
    if (filesByKey.isEmpty())
        return;

    auto& rgm = Ogre::ResourceGroupManager::getSingleton();
    QSet<Ogre::Material*> hydratedMaterials;

    for (unsigned int si = 0; si < entity->getNumSubEntities(); ++si) {
        Ogre::SubEntity* sub = entity->getSubEntity(si);
        Ogre::MaterialPtr mat = sub->getMaterial();
        if (mat.isNull() || mat->getNumTechniques() == 0)
            continue;

        const Ogre::String matGroup = mat->getGroup();
        if (!rgm.resourceGroupExists(matGroup))
            rgm.createResourceGroup(matGroup);

        for (unsigned short ti = 0; ti < mat->getNumTechniques(); ++ti) {
            Ogre::Technique* tech = mat->getTechnique(ti);
            if (!tech)
                continue;
            for (unsigned short pi = 0; pi < tech->getNumPasses(); ++pi) {
                Ogre::Pass* pass = tech->getPass(pi);
                if (!pass)
                    continue;
                for (unsigned short ui = 0; ui < pass->getNumTextureUnitStates(); ++ui) {
                    Ogre::TextureUnitState* tus = pass->getTextureUnitState(ui);
                    if (!tus)
                        continue;
                    const std::string texName = tus->getTextureName();
                    if (texName.empty())
                        continue;
                    const QString srcPath =
                        resolveTexturePathOnDisk(QString::fromStdString(texName), filesByKey);
                    if (srcPath.isEmpty())
                        continue;

                    const QFileInfo fi(srcPath);
                    try {
                        rgm.addResourceLocation(fi.absolutePath().toStdString(),
                                                "FileSystem",
                                                matGroup);
                    } catch (const Ogre::Exception&) {
                    }

                    if (loadTextureImageIntoGroup(srcPath, texName, matGroup))
                        hydratedMaterials.insert(mat.get());
                }
            }
        }
    }

    if (!hydratedMaterials.isEmpty()) {
        SentryReporter::addBreadcrumb(
            QStringLiteral("file.import"),
            QStringLiteral("Hydrated textures for %1 material(s) from cloud/import paths")
                .arg(hydratedMaterials.size()));
    }
}

static void ensureResourceGroup(const QString &path)
{
    MeshImporterExporter::registerImportResourceDirectory(path);
}

QString cloudProjectCacheRoot(const QString& localPath)
{
    const QString normalized = QDir::fromNativeSeparators(QFileInfo(localPath).absoluteFilePath());
    const QString marker = QStringLiteral("/cloud/");
    const int cloudIdx = normalized.indexOf(marker);
    if (cloudIdx < 0)
        return QString();

    const QString tail = normalized.mid(cloudIdx + marker.size());
    const int ownerEnd = tail.indexOf(QLatin1Char('/'));
    if (ownerEnd <= 0)
        return QString();
    const int slugEnd = tail.indexOf(QLatin1Char('/'), ownerEnd + 1);
    if (slugEnd < 0)
        return normalized;
    return normalized.left(cloudIdx + marker.size() + slugEnd);
}

QStringList MeshImporterExporter::textureSearchRootsForImportFile(const QString& localPath)
{
    QStringList roots;
    const QFileInfo fileInfo(localPath);
    if (!fileInfo.exists())
        return roots;

    roots << fileInfo.absolutePath();
    const QString cloudRoot = cloudProjectCacheRoot(fileInfo.absoluteFilePath());
    if (!cloudRoot.isEmpty() && cloudRoot != fileInfo.absolutePath())
        roots << cloudRoot;
    return roots;
}

QStringList MeshImporterExporter::textureSearchRootsForEntity(const Ogre::Entity* entity)
{
    if (!entity)
        return {};
    const Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh)
        return {};
    const Ogre::Any& any = mesh->getUserObjectBindings().getUserAny("qtme.source_path");
    if (!any.has_value())
        return {};
    try {
        const std::string sourcePath = Ogre::any_cast<std::string>(any);
        if (sourcePath.empty())
            return {};
        return textureSearchRootsForImportFile(QString::fromStdString(sourcePath));
    } catch (const Ogre::Exception&) {
        return {};
    }
}

void MeshImporterExporter::prepareCloudCachedImport(const QString& localMainFile)
{
    for (const QString& root : textureSearchRootsForImportFile(localMainFile))
        registerImportResourceDirectory(root);
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
/// Builds an Ogre texture resource name scoped to the source asset path so that two RSDs
/// referencing different files that happen to share a basename (e.g. `Wood.jpg`) don't
/// collide in Ogre's global texture registry. Returns a name of the form
/// `<basename>__<8charHashOfAbsPath>.<suffix>` which preserves the human-readable basename
/// for inspection while guaranteeing uniqueness across assets. The `__<hash>` portion is
/// stripped by `rsdResourceNameToBasename()` on export.
static QString scopedRsdResourceName(const QString& texPath)
{
    const QFileInfo fi(texPath);
    const QString abs = fi.absoluteFilePath();
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(abs.toUtf8(), QCryptographicHash::Sha1).toHex()).left(8);
    QString suffix = fi.suffix().toLower();
    if (suffix.isEmpty())
        suffix = QStringLiteral("tex");
    return fi.completeBaseName() + QStringLiteral("__") + hash
           + QStringLiteral(".") + suffix;
}

/// Inverse of `scopedRsdResourceName()` — strips the `__<8hexchars>` segment a scoped
/// resource name carries so we can recover the original `Wood.jpg`-style basename when
/// writing sidecar files next to an exported RSD. Names that don't match the scoped
/// pattern are returned as `QFileInfo::fileName(name)` (i.e. a best-effort basename) so
/// non-RSD textures still get a sensible filename.
static QString rsdResourceNameToBasename(const QString& resName)
{
    static const QRegularExpression scoped(
        QStringLiteral("^(.+)__[0-9a-f]{8}\\.([^.]+)$"));
    const auto m = scoped.match(resName);
    if (m.hasMatch())
        return m.captured(1) + QStringLiteral(".") + m.captured(2);
    return QFileInfo(resName).fileName();
}

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

// Reconstruct NodeAnimationManager clips (#517) from a freshly-imported
// aiScene. On export, node-transform animation is written as aiAnimations whose
// aiNodeAnim channels target the mesh's scene NODE (not a bone). The skeletal
// AnimationProcessor deliberately skips those channels (Skeleton::hasBone
// returns false), so without this they were lost — or, on a skeleton-less mesh,
// mis-imported as a bogus skeletal clip. Here we walk the aiScene's animations,
// pick channels whose target node name matches the just-created SceneNode AND
// is NOT a bone on the mesh's skeleton, and rebuild them as node clips so they
// show in the animation list + dope-sheet node band and drive the SceneNode.
//
// The node was auto-scaled by `nodeScale` (sub-unit mesh fix). A node clip
// animates the node's OWN local transform; its translation lives in PARENT
// space and is unaffected by the node's own scale, so translations are taken
// verbatim (no *nodeScale) — the previous "100x off" report came from the
// channel being dropped/misrouted, not a scale factor here.
static void reconstructNodeClipsFromAiScene(const aiScene* aiscene,
                                            Ogre::SceneNode* sn,
                                            const Ogre::Entity* en)
{
    if (!aiscene || !sn || !en || aiscene->mNumAnimations == 0) return;
    auto* nam = NodeAnimationManager::instance();
    if (!nam) return;

    const std::string nodeName = sn->getName();
    Ogre::Skeleton* skel =
        (en->getMesh() && en->getMesh()->getSkeleton())
            ? en->getMesh()->getSkeleton().get() : nullptr;

    for (unsigned int a = 0; a < aiscene->mNumAnimations; ++a) {
        const aiAnimation* anim = aiscene->mAnimations[a];
        if (!anim) continue;
        const double ticksPerSec =
            (anim->mTicksPerSecond > 0.0) ? anim->mTicksPerSecond : 1.0;

        // Gather channels targeting THIS scene node that are not bones.
        std::vector<const aiNodeAnim*> nodeChannels;
        for (unsigned int c = 0; c < anim->mNumChannels; ++c) {
            const aiNodeAnim* ch = anim->mChannels[c];
            if (!ch) continue;
            const std::string chName = ch->mNodeName.C_Str();
            if (chName != nodeName) continue;
            if (skel && skel->hasBone(chName)) continue;  // belongs to the skeleton
            nodeChannels.push_back(ch);
        }
        if (nodeChannels.empty()) continue;

        // Clip name: the aiAnimation name, made unique on the scene.
        std::string clipName = anim->mName.C_Str();
        if (clipName.empty()) clipName = "NodeClip";
        std::string unique = clipName;
        int suffix = 1;
        auto* scene = Manager::getSingletonPtr() ? Manager::getSingletonPtr()->getSceneMgr() : nullptr;
        while (scene && scene->hasAnimation(unique))
            unique = clipName + "_" + std::to_string(suffix++);

        const double lengthSec = (anim->mDuration > 0.0)
            ? anim->mDuration / ticksPerSec : 0.0;
        if (lengthSec <= 0.0) continue;
        if (!nam->createClip(QString::fromStdString(unique), lengthSec)) continue;

        for (const aiNodeAnim* ch : nodeChannels) {
            // Collect the union of key times across P/R/S, then sample each.
            std::set<double> times;
            for (unsigned int k = 0; k < ch->mNumPositionKeys; ++k)
                times.insert(ch->mPositionKeys[k].mTime / ticksPerSec);
            for (unsigned int k = 0; k < ch->mNumRotationKeys; ++k)
                times.insert(ch->mRotationKeys[k].mTime / ticksPerSec);
            for (unsigned int k = 0; k < ch->mNumScalingKeys; ++k)
                times.insert(ch->mScalingKeys[k].mTime / ticksPerSec);

            auto sampleVec = [](const aiVectorKey* keys, unsigned int n,
                                double t, double tps, const Ogre::Vector3& def) {
                if (n == 0) return def;
                // nearest key (clips are exported per-key, no interpolation gaps)
                double best = 1e30; Ogre::Vector3 out = def;
                for (unsigned int k = 0; k < n; ++k) {
                    const double kt = keys[k].mTime / tps;
                    const double d = std::abs(kt - t);
                    if (d < best) { best = d; out = Ogre::Vector3(
                        keys[k].mValue.x, keys[k].mValue.y, keys[k].mValue.z); }
                }
                return out;
            };
            auto sampleQuat = [](const aiQuatKey* keys, unsigned int n,
                                 double t, double tps) {
                if (n == 0) return Ogre::Quaternion::IDENTITY;
                double best = 1e30; Ogre::Quaternion out = Ogre::Quaternion::IDENTITY;
                for (unsigned int k = 0; k < n; ++k) {
                    const double kt = keys[k].mTime / tps;
                    const double d = std::abs(kt - t);
                    if (d < best) { best = d; out = Ogre::Quaternion(
                        keys[k].mValue.w, keys[k].mValue.x,
                        keys[k].mValue.y, keys[k].mValue.z); }
                }
                out.normalise();
                return out;
            };

            for (double t : times) {
                const Ogre::Vector3 pos = sampleVec(ch->mPositionKeys,
                    ch->mNumPositionKeys, t, ticksPerSec, Ogre::Vector3::ZERO);
                const Ogre::Quaternion rot = sampleQuat(ch->mRotationKeys,
                    ch->mNumRotationKeys, t, ticksPerSec);
                const Ogre::Vector3 scl = sampleVec(ch->mScalingKeys,
                    ch->mNumScalingKeys, t, ticksPerSec, Ogre::Vector3(1, 1, 1));
                nam->addKeyframe(QString::fromStdString(unique),
                                 QString::fromStdString(nodeName),
                                 t, pos, rot, scl);
            }
        }
    }
}

// File-reading wrapper: the plain-import path (importer()) doesn't retain the
// aiScene, so do an independent no-process Assimp read purely to recover the
// node-transform channels, then delegate to the core above.
static void reconstructNodeClipsFromFile(const QString& filePath,
                                         Ogre::SceneNode* sn,
                                         const Ogre::Entity* en)
{
    if (filePath.isEmpty() || !sn || !en) return;
    Assimp::Importer aimp;
    const aiScene* aiscene = aimp.ReadFile(filePath.toStdString(), 0);
    reconstructNodeClipsFromAiScene(aiscene, sn, en);
}

// ─── Node-transform-clip sidecar (FBX / .mesh workaround, #517) ─────────
// glTF/glb carry node-transform clips natively (buildNodeClipAnimations →
// aiNodeAnim channels). The custom FBXExporter and Ogre's .mesh serializer
// have no concept of SceneManager-level node animation, so for those formats
// we persist the clips to a `<basename>.nodeanim.json` sidecar — exactly the
// pattern SceneLightsIO uses for `.lights.json` — and rebuild them on import.
static QString nodeAnimSidecarPath(const QString& meshPath)
{
    const QFileInfo fi(meshPath);
    return fi.absoluteDir().filePath(fi.completeBaseName()
                                     + QStringLiteral(".nodeanim.json"));
}

// Serialise every SceneManager node clip that targets `sn` to the sidecar.
// No-op (and removes any stale sidecar) when the node has no clips, so a
// re-export after deleting the animation doesn't leave a ghost file.
static void writeNodeAnimSidecar(const QString& meshPath, Ogre::SceneNode* sn)
{
    if (meshPath.isEmpty() || !sn) return;
    auto* mgr = Manager::getSingletonPtr();
    auto* scene = mgr ? mgr->getSceneMgr() : nullptr;
    if (!scene) return;

    const std::string nodeName = sn->getName();
    QJsonArray clipsJson;

    for (unsigned short ai = 0; ai < scene->getNumAnimations(); ++ai) {
        Ogre::Animation* anim = scene->getAnimation(ai);
        if (!anim) continue;

        // Collect this clip's track(s) that target our scene node (skip bones).
        QJsonArray tracksJson;
        for (const auto& [handle, track] : anim->_getNodeTrackList()) {
            (void)handle;
            if (!track) continue;
            Ogre::Node* target = track->getAssociatedNode();
            if (!target || dynamic_cast<Ogre::Bone*>(target)) continue;
            if (target->getName() != nodeName) continue;

            QJsonArray keysJson;
            for (unsigned short ki = 0; ki < track->getNumKeyFrames(); ++ki) {
                auto* kf = track->getNodeKeyFrame(ki);
                const Ogre::Vector3 p = kf->getTranslate();
                Ogre::Quaternion r = kf->getRotation(); r.normalise();
                const Ogre::Vector3 s = kf->getScale();
                QJsonObject k;
                k["t"] = static_cast<double>(kf->getTime());
                k["p"] = QJsonArray{ p.x, p.y, p.z };
                k["r"] = QJsonArray{ r.w, r.x, r.y, r.z };
                k["s"] = QJsonArray{ s.x, s.y, s.z };
                keysJson.append(k);
            }
            if (keysJson.isEmpty()) continue;
            QJsonObject tr;
            tr["node"] = QString::fromStdString(nodeName);
            tr["keys"] = keysJson;
            tracksJson.append(tr);
        }
        if (tracksJson.isEmpty()) continue;

        QJsonObject clip;
        clip["name"] = QString::fromStdString(anim->getName());
        clip["length"] = static_cast<double>(anim->getLength());
        clip["tracks"] = tracksJson;
        clipsJson.append(clip);
    }

    const QString sidecarPath = nodeAnimSidecarPath(meshPath);
    if (clipsJson.isEmpty()) {
        // Nothing to persist — remove any stale sidecar from a prior export.
        QFile::remove(sidecarPath);
        return;
    }

    QJsonObject root;
    root["schema"] = QStringLiteral("qtmesh.node.animations.v1");
    root["clips"] = clipsJson;

    QFile out(sidecarPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Ogre::LogManager::getSingleton().logWarning(
            "Node-anim sidecar write failed: " + sidecarPath.toStdString());
        return;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    out.close();
    SentryReporter::addBreadcrumb(
        QStringLiteral("file.export"),
        QStringLiteral("Wrote node-anim sidecar (%1 clip(s)) for %2")
            .arg(clipsJson.size()).arg(meshPath));
}

// Rebuild node clips from `<basename>.nodeanim.json` (FBX/.mesh import). Names
// are made unique against the live scene; keyframes go through the normal
// NodeAnimationManager path. No-op when the sidecar is absent.
static void reconstructNodeClipsFromSidecar(const QString& meshPath,
                                            Ogre::SceneNode* sn,
                                            const Ogre::Entity* en)
{
    if (meshPath.isEmpty() || !sn || !en) return;
    const QString sidecarPath = nodeAnimSidecarPath(meshPath);
    QFile f(sidecarPath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
    const QByteArray bytes = f.readAll();
    f.close();

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) return;
    const QJsonObject root = doc.object();
    if (root.value("schema").toString() != QStringLiteral("qtmesh.node.animations.v1"))
        return;

    auto* nam = NodeAnimationManager::instance();
    auto* mgr = Manager::getSingletonPtr();
    auto* scene = mgr ? mgr->getSceneMgr() : nullptr;
    if (!nam || !scene) return;

    const std::string nodeName = sn->getName();

    for (const QJsonValue& cv : root.value("clips").toArray()) {
        const QJsonObject clip = cv.toObject();
        const QJsonArray tracks = clip.value("tracks").toArray();
        if (tracks.isEmpty()) continue;
        const double length = clip.value("length").toDouble();
        if (length <= 0.0) continue;

        // Unique clip name against the live scene.
        std::string base = clip.value("name").toString().toStdString();
        if (base.empty()) base = "NodeClip";
        std::string unique = base;
        int suffix = 1;
        while (scene->hasAnimation(unique))
            unique = base + "_" + std::to_string(suffix++);

        if (!nam->createClip(QString::fromStdString(unique), length)) continue;

        for (const QJsonValue& tv : tracks) {
            const QJsonObject tr = tv.toObject();
            // The sidecar was written for THIS entity's node; retarget its
            // keys onto the (possibly renamed) live scene node.
            for (const QJsonValue& kv : tr.value("keys").toArray()) {
                const QJsonObject k = kv.toObject();
                const double t = k.value("t").toDouble();
                const QJsonArray pa = k.value("p").toArray();
                const QJsonArray ra = k.value("r").toArray();
                const QJsonArray sa = k.value("s").toArray();
                if (pa.size() < 3 || ra.size() < 4 || sa.size() < 3) continue;
                const Ogre::Vector3 p(pa[0].toDouble(), pa[1].toDouble(), pa[2].toDouble());
                Ogre::Quaternion r(ra[0].toDouble(), ra[1].toDouble(),
                                   ra[2].toDouble(), ra[3].toDouble());
                r.normalise();
                const Ogre::Vector3 s(sa[0].toDouble(), sa[1].toDouble(), sa[2].toDouble());
                nam->addKeyframe(QString::fromStdString(unique),
                                 QString::fromStdString(nodeName), t, p, r, s);
            }
        }
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
            registerImportResourceDirectory(file.absolutePath());
            const QString cloudRoot = cloudProjectCacheRoot(file.absoluteFilePath());
            if (!cloudRoot.isEmpty() && cloudRoot != file.absolutePath())
                registerImportResourceDirectory(cloudRoot);

            Ogre::SceneNode *sn;
            const Ogre::Entity *en;

            if(!file.suffix().compare("abc",Qt::CaseInsensitive))
            {
                // Alembic vertex-animation cache (#519 Slice B). Delegates to
                // the guarded reader; a build without -DENABLE_ALEMBIC returns
                // a clear error rather than falling through to Assimp (which
                // can't read .abc). The importer builds the base mesh + a
                // VAT_POSE clip and creates the entity itself.
                QString abcErr;
                Ogre::SceneNode* abcNode =
                    AlembicImporter::importToScene(file.absoluteFilePath(), &abcErr);
                if (!abcNode) {
                    Ogre::LogManager::getSingleton().logError(
                        "Alembic import failed: " + abcErr.toStdString());
                }
                continue;
            }
            else if(!file.suffix().compare("mesh",Qt::CaseInsensitive))
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
                    showImportProblem(
                        QStringLiteral("PlayStation TMD"),
                        QStringLiteral("Could not import %1 — invalid file or unsupported primitive types.")
                            .arg(file.fileName()),
                        QStringLiteral("tmd"));
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
                    showImportProblem(
                        QStringLiteral("PlayStation RSD"),
                        QStringLiteral("Could not parse %1: %2").arg(file.fileName(), err),
                        QStringLiteral("rsd"),
                        err);
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
                    if (texPath.isEmpty() || !QFileInfo::exists(texPath)) {
                        SentryReporter::addBreadcrumb(
                            QStringLiteral("file.import"),
                            QStringLiteral("RSD TEX[%1] missing: %2").arg(ti).arg(texRel));
                        continue;
                    }

                    Ogre::Image img;
                    bool loaded = false;
                    const QString suffix = QFileInfo(texPath).suffix().toLower();
                    QString timErr;
                    if (suffix == QStringLiteral("tim"))
                        loaded = PS1TIM::loadTimToOgreImage(texPath, img, &timErr);

                    // Resource name is scoped by the absolute texture path so two RSDs that
                    // both ship a "Wood.jpg" do not clobber each other in Ogre's global
                    // TextureManager registry.
                    const QString resName = scopedRsdResourceName(texPath);
                    const Ogre::String ogreName = resName.toStdString();

                    SentryReporter::addBreadcrumb(
                        QStringLiteral("file.import"),
                        QStringLiteral("RSD TEX[%1] load: %2").arg(ti).arg(QFileInfo(texPath).fileName()));

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
                        SentryReporter::addBreadcrumb(
                            QStringLiteral("file.import"),
                            QStringLiteral("RSD TEX[%1] TIM loaded (%2x%3)")
                                .arg(ti).arg(rsdTexSlots[ti].width).arg(rsdTexSlots[ti].height));
                    } else if (suffix == QStringLiteral("tim")) {
                        SentryReporter::addBreadcrumb(
                            QStringLiteral("file.import"),
                            QStringLiteral("RSD TEX[%1] TIM load failed: %2").arg(ti).arg(timErr));
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
                            SentryReporter::addBreadcrumb(
                                QStringLiteral("file.import"),
                                QStringLiteral("RSD TEX[%1] external loaded (%2x%3)")
                                    .arg(ti).arg(rsdTexSlots[ti].width).arg(rsdTexSlots[ti].height));
                        } else {
                            Ogre::LogManager::getSingleton().logMessage(
                                "Warning: RSD texture load failed for "
                                + texPath.toStdString() + ": " + extErr.toStdString());
                            SentryReporter::addBreadcrumb(
                                QStringLiteral("file.import"),
                                QStringLiteral("RSD TEX[%1] external load failed: %2").arg(ti).arg(extErr));
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
                    showImportProblem(
                        QStringLiteral("PlayStation RSD"),
                        QStringLiteral("RSD does not reference an existing geometry file (PLY=...)."),
                        QStringLiteral("rsd"));
                    continue;
                }

                // `useTexturedMatPath`: after import, bind TIM slots to `_texN` submesh materials.
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
                // Psy-Q `G` / `H` carry per-corner RGB. Those live in `MatEntry::vertColors`, while
                // `MatEntry::rgb` is only the first corner (back-compat / flat preview). If we skip
                // `importPsyqPlyWithFaceMaterials` whenever no face is textured, untextured smooth
                // MAT rows collapse to `importPsyqPlyWithFaceColors(rsdFaceColors)` — one flat colour
                // per face — which is wrong for Blender's Example Project cube (all `F G` rows).
                const bool useFaceMaterialsImport = !rsdMatEntries.isEmpty();

                Ogre::MeshPtr mesh;
                const QFileInfo geomFi(geomPath);
                if (!geomFi.suffix().compare(QStringLiteral("tmd"), Qt::CaseInsensitive)) {
                    const std::string meshName = (file.baseName() + QStringLiteral("_rsd_tmd")).toStdString();
                    mesh = PS1TMD::importTmd(geomPath, meshName);
                } else if (!geomFi.suffix().compare(QStringLiteral("ply"), Qt::CaseInsensitive)
                           && PS1PLY::isPsyqPlyFile(geomPath)) {
                    const std::string meshName = (file.baseName() + QStringLiteral("_rsd_ply")).toStdString();
                    if (useFaceMaterialsImport) {
                        // Each MAT entry maps 1:1 to a PLY face (declaration order). UVs are
                        // normalised by the bound texture's width/height when textured.
                        QVector<PS1PLY::FaceMaterial> faceMats(rsdMatEntries.size());
                        for (int fi = 0; fi < rsdMatEntries.size(); ++fi) {
                            const PS1MAT::MatEntry& me = rsdMatEntries[fi];
                            PS1PLY::FaceMaterial fm;
                            // Sanitise per-face texture references: a single MAT entry that
                            // claims to be textured but points at a missing slot would force
                            // importPsyqPlyWithFaceMaterials() to bin every face from that
                            // batch into a malformed submesh. Downgrade such faces to solid
                            // so the rest of the mesh still gets its proper textures.
                            const bool hasValidTexture =
                                me.textured
                                && me.textureIndex >= 0
                                && me.textureIndex < static_cast<int>(rsdTexSlots.size())
                                && !rsdTexSlots[me.textureIndex].resourceName.isEmpty();
                            fm.textured = hasValidTexture;
                            fm.textureIndex = hasValidTexture ? me.textureIndex : -1;
                            int texW = 256, texH = 256;
                            if (hasValidTexture) {
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
                            fm.unlit = me.unlit;
                            faceMats[fi] = fm;
                        }
                        mesh = PS1PLY::importPsyqPlyWithFaceMaterials(geomPath, meshName, faceMats);
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
                    showImportProblem(
                        QStringLiteral("PlayStation RSD"),
                        QStringLiteral("Could not import geometry referenced by %1").arg(file.fileName()),
                        QStringLiteral("rsd"));
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
                    // shaped `PLY/<meshName>_texN` or `_texN_nl` (unlit); untextured use `_solid` / `_solid_nl`.
                    // Bind the matching RSD texture slot to each textured submesh's first texture unit.
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
                            QStringLiteral("_tex(\\d+)(_nl)?$"));
                        const auto m = kTexSlotRe.match(mname);
                        if (!m.hasMatch())
                            continue; // untextured submesh — leave as is.
                        bool ok = false;
                        const int slot = m.captured(1).toInt(&ok);
                        const bool slotUnlit = m.captured(2) == QStringLiteral("_nl");
                        if (!ok || slot < 0 || slot >= static_cast<int>(rsdTexSlots.size())
                            || rsdTexSlots[slot].resourceName.isEmpty())
                            continue;

                        // Replace any existing texture unit states so re-imports refresh cleanly.
                        pass->removeAllTextureUnitStates();
                        pass->createTextureUnitState(rsdTexSlots[slot].resourceName.toStdString());
                        const Ogre::SubMesh* sm = en->getMesh()->getSubMesh(si);
                        const Ogre::VertexData* vdSm =
                            (sm && sm->useSharedVertices) ? en->getMesh()->sharedVertexData
                                                          : (sm ? sm->vertexData : nullptr);
                        const bool hasVC = vdSm && vdSm->vertexDeclaration
                            && vdSm->vertexDeclaration->findElementBySemantic(Ogre::VES_DIFFUSE);
                        PS1PLY::configurePsyqRsdMaterialPass(pass, hasVC, slotUnlit);
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
                    showImportProblem(
                        QStringLiteral("PlayStation PLY"),
                        QStringLiteral("Could not import %1 — invalid Psy-Q PLY (expected @PLY header and "
                                       "face lines from the Psy-Q / RSD toolchain, not Stanford PLY).")
                            .arg(file.fileName()),
                        QStringLiteral("ply"));
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
                    SceneLightsIO::importLightsFromFile(file.filePath(), false);
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
                        showImportProblem(
                            QStringLiteral("Import PLY"),
                            QStringLiteral("Could not import %1. PlayStation Psy-Q PLY uses an @PLY header; "
                                           "Stanford PLY must start with \"ply\".")
                                .arg(file.fileName()),
                            QStringLiteral("ply"));
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
            // dimension lands at ~3 units. Threshold of 0.01 avoids
            // touching sensible-scale assets (anything from a few cm up).
            if (en && en->getMesh()) {
                const auto& bbSize = en->getBoundingBox().getSize();
                const Ogre::Real maxExtent = std::max({bbSize.x, bbSize.y, bbSize.z});
                if (maxExtent > 0.0f && maxExtent < 0.01f) {
                    const Ogre::Real factor = 3.0f / maxExtent;
                    sn->setScale(factor, factor, factor);
                    Ogre::LogManager::getSingleton().logMessage(
                        "MeshImporterExporter: auto-scaled '" + en->getName() +
                        "' by " + std::to_string(factor) +
                        " (source max-extent " + std::to_string(maxExtent) +
                        " was inside the near-clip plane)");
                }
            }

            // Recover node-transform animation clips (#517) that the skeletal
            // import path drops (channels target the scene node, not a bone).
            // glTF/glb carry them in the file (Assimp read below finds them);
            // FBX/.mesh carry them in a `.nodeanim.json` sidecar instead.
            reconstructNodeClipsFromFile(file.filePath(), sn, en);
            reconstructNodeClipsFromSidecar(file.filePath(), sn, en);

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
    QStringList globs;                // "*.ext" for the "All supported" row
    QSet<QString> present;            // lower-cased ".ext" set for lookups
    globs.reserve(parts.size());
    for (QString ext : parts) {
        ext = ext.trimmed().toLower();
        if (ext.startsWith('.')) {
            globs.append(QLatin1Char('*') + ext);
            present.insert(ext);
        }
    }
    const QString allSupported = globs.join(QLatin1Char(' '));

    // Named per-format rows, in a sensible priority order. Each is emitted only
    // if at least one of its extensions is in the valid list (so the dialog
    // never advertises a format the loader can't handle). Grouped rows (e.g.
    // glTF *.gltf/*.glb) show if ANY member is present. Anything valid but not
    // named here still loads via "All supported" / "All files".
    struct NamedFilter { const char* label; QStringList exts; };
    const QList<NamedFilter> named = {
        {"FBX (*.fbx)",                    {".fbx"}},
        {"glTF 2.0 (*.gltf *.glb *.vrm)",  {".gltf", ".glb", ".vrm"}},
        {"Wavefront OBJ (*.obj)",          {".obj"}},
        {"Collada (*.dae)",                {".dae"}},
        {"Ogre Mesh (*.mesh *.mesh.xml)",  {".mesh"}},
        {"STL (*.stl)",                    {".stl"}},
        {"PLY (*.ply)",                    {".ply"}},
        {"3DS (*.3ds)",                    {".3ds"}},
        {"Blender (*.blend)",              {".blend"}},
        {"DirectX X (*.x)",                {".x"}},
        {"Biovision BVH (*.bvh)",          {".bvh"}},
        {"LightWave (*.lwo *.lws *.lxo)",  {".lwo", ".lws", ".lxo"}},
        {"Alembic vertex cache (*.abc)",   {".abc"}},
        {"PlayStation RSD / TMD / Psy-Q PLY (*.rsd *.tmd *.ply)", {".rsd", ".tmd", ".ply"}},
    };

    QStringList rows;
    rows.reserve(named.size() + 2);
    rows << QStringLiteral("All supported (%1)").arg(allSupported);
    for (const NamedFilter& nf : named) {
        bool any = false;
        for (const QString& e : nf.exts)
            if (present.contains(e)) { any = true; break; }
        if (any)
            rows << QString::fromLatin1(nf.label);
    }
    rows << QStringLiteral("All files (*.*)");
    return rows.join(QStringLiteral(";;"));
}

QString MeshImporterExporter::importFileDialogFilter()
{
    return importFileDialogFilterFromExtensionList(Manager::getSingleton()->getValidFileExtention());
}

namespace {

// ─── glTF morph-target WEIGHT animation post-processor ───────────────────────
//
// Assimp's glTF2 exporter cannot write morph-weight animation channels (its
// ExportAnimations only emits node TRS channels), so after Assimp writes the
// .gltf/.glb we inject the standard glTF `weights` animation channel into the
// JSON ourselves. See the glTF 2.0 spec — "Animations" / morph-target weights.
//
// Data source (Ogre): morph WEIGHT clips are MESH-level Ogre::Animations that
// are NOT named after a pose (a pose-named animation is a per-target SHAPE
// clip). Each weight clip carries one Ogre::VertexAnimationTrack (VAT_POSE)
// per submesh target handle; each Ogre::VertexPoseKeyFrame has a time and a
// list of {poseIndex, influence} references — influence == that target's
// weight at that time. Poses are enumerated in mesh->getPoseList() order,
// which matches the glTF morph target order Assimp exported from
// aiMesh::mAnimMeshes.
//
// Robustness: the whole helper is wrapped in try/catch and never throws out of
// the exporter. On any parse/shape failure it logs a warning and leaves the
// file exactly as Assimp wrote it (shapes still round-trip).

// A decoded morph weight clip: sorted distinct keyframe times + a flat weight
// matrix (K rows × numTargets cols, row-major).
struct MorphWeightClip {
    std::string name;
    std::vector<float> times;    // K
    std::vector<float> weights;  // K * numTargets
};

// Is `animName` a per-target SHAPE clip (named exactly a pose name)?
bool gltfIsPoseShapeClip(const Ogre::Mesh* mesh, const std::string& animName)
{
    for (const Ogre::Pose* p : mesh->getPoseList())
        if (p && p->getName() == animName) return true;
    return false;
}

// Enumerate the entity's mesh morph WEIGHT clips and decode each into a
// dense time/weight matrix of width `numTargets` (pose-list order).
std::vector<MorphWeightClip> collectMorphWeightClips(const Ogre::Entity* entity,
                                                     int numTargets)
{
    std::vector<MorphWeightClip> out;
    if (!entity || numTargets <= 0) return out;
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return out;

    for (unsigned short ai = 0; ai < mesh->getNumAnimations(); ++ai) {
        Ogre::Animation* anim = mesh->getAnimation(ai);
        if (!anim) continue;
        const std::string nm = anim->getName();
        if (gltfIsPoseShapeClip(mesh.get(), nm)) continue;  // shape clip, not a weight clip

        // Gather the sorted set of distinct keyframe times across every
        // VAT_POSE track on this clip, and record each pose reference.
        std::set<float> timeSet;
        for (const auto& [handle, track] : anim->_getVertexTrackList()) {
            (void)handle;
            if (!track || track->getAnimationType() != Ogre::VAT_POSE) continue;
            for (unsigned short k = 0; k < track->getNumKeyFrames(); ++k)
                timeSet.insert(track->getKeyFrame(k)->getTime());
        }
        if (timeSet.empty()) continue;  // no keyed tracks → nothing to export

        std::vector<float> times(timeSet.begin(), timeSet.end());

        MorphWeightClip clip;
        clip.name = nm;
        clip.times = times;
        clip.weights.assign(times.size() * static_cast<size_t>(numTargets), 0.0f);

        // For each time build the weight vector from the pose references.
        for (const auto& [handle, track] : anim->_getVertexTrackList()) {
            (void)handle;
            if (!track || track->getAnimationType() != Ogre::VAT_POSE) continue;
            for (unsigned short k = 0; k < track->getNumKeyFrames(); ++k) {
                auto* kf = static_cast<Ogre::VertexPoseKeyFrame*>(track->getKeyFrame(k));
                const float t = kf->getTime();
                // Locate the row index for this time.
                auto it = std::lower_bound(times.begin(), times.end(), t);
                if (it == times.end()) continue;
                const size_t row = static_cast<size_t>(it - times.begin());
                for (const auto& ref : kf->getPoseReferences()) {
                    if (ref.poseIndex >= static_cast<unsigned short>(numTargets))
                        continue;  // out of range for this glTF mesh — skip
                    clip.weights[row * static_cast<size_t>(numTargets) + ref.poseIndex]
                        = ref.influence;
                }
            }
        }
        if (std::getenv("QTMESH_MOCAP_DEBUG")) {
            float mn = 1e9f, mx = -1e9f;
            for (float w : clip.weights) { mn = std::min(mn, w); mx = std::max(mx, w); }
            std::fprintf(stderr, "[export] clip '%s' rows=%zu targets=%d "
                         "influenceRange=[%.3f,%.3f]\n", nm.c_str(),
                         clip.times.size(), numTargets, mn, mx);
        }
        out.push_back(std::move(clip));
    }
    return out;
}

// Serialise a float vector little-endian into a byte buffer.
void appendFloatsLE(QByteArray& buf, const std::vector<float>& v)
{
    const size_t base = static_cast<size_t>(buf.size());
    buf.resize(static_cast<int>(base + v.size() * sizeof(float)));
    std::memcpy(buf.data() + base, v.data(), v.size() * sizeof(float));
}

// Find the glTF node that owns the morph mesh. Strategy: prefer the node whose
// referenced mesh's first primitive has a `targets` array of length ==
// poseCount; fall back to the first node that references a mesh with any
// targets. Sets *outNumTargets. Returns the node index, or -1 on failure.
int findMorphNode(const QJsonObject& root, int poseCount, int* outNumTargets)
{
    if (!root.contains("nodes") || !root.contains("meshes")) return -1;
    const QJsonArray nodes = root.value("nodes").toArray();
    const QJsonArray meshes = root.value("meshes").toArray();

    auto targetsForMesh = [&](int meshIdx) -> int {
        if (meshIdx < 0 || meshIdx >= meshes.size()) return 0;
        const QJsonObject m = meshes.at(meshIdx).toObject();
        const QJsonArray prims = m.value("primitives").toArray();
        if (prims.isEmpty()) return 0;
        const QJsonObject p0 = prims.at(0).toObject();
        if (!p0.contains("targets")) return 0;
        return p0.value("targets").toArray().size();
    };

    int fallbackNode = -1, fallbackTargets = 0;
    for (int n = 0; n < nodes.size(); ++n) {
        const QJsonObject node = nodes.at(n).toObject();
        if (!node.contains("mesh")) continue;
        const int meshIdx = node.value("mesh").toInt(-1);
        const int nt = targetsForMesh(meshIdx);
        if (nt <= 0) continue;
        if (nt == poseCount) { *outNumTargets = nt; return n; }  // exact match
        if (fallbackNode < 0) { fallbackNode = n; fallbackTargets = nt; }
    }
    if (fallbackNode >= 0) { *outNumTargets = fallbackTargets; return fallbackNode; }
    return -1;
}

// Inject morph-weight animations into an already-written .gltf/.glb file.
// No-op (leaves the file untouched) if the entity has no morph weight clips,
// the file has no morph targets, or anything fails to parse.
void injectMorphWeightAnimations(const QString& filePath,
                                 const Ogre::Entity* entity,
                                 bool isBinary)
{
    try {
        if (!entity) return;
        Ogre::MeshPtr mesh = entity->getMesh();
        if (!mesh || mesh->getPoseCount() == 0) return;

        // Quick early-out: does the mesh carry any weight clips at all?
        // (Cheaper than a full parse; poseCount already > 0 here.)
        bool anyWeightClip = false;
        for (unsigned short ai = 0; ai < mesh->getNumAnimations(); ++ai) {
            Ogre::Animation* a = mesh->getAnimation(ai);
            if (a && !gltfIsPoseShapeClip(mesh.get(), a->getName())) {
                anyWeightClip = true;
                break;
            }
        }
        if (!anyWeightClip) return;

        // ── Read the file (GLB: split header/JSON/BIN; glTF: whole file) ──
        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly)) return;
        const QByteArray whole = f.readAll();
        f.close();

        QByteArray jsonBytes;
        QByteArray binChunk;          // GLB buffer 0 (copied verbatim)
        bool haveBinChunk = false;

        if (isBinary) {
            // GLB layout: 12-byte header, then chunks (4-byte length + 4-byte
            // type + data). JSON chunk type 0x4E4F534A ("JSON"), BIN chunk
            // type 0x004E4942 ("BIN\0").
            if (whole.size() < 12) return;
            const auto rdU32 = [&](int off) -> quint32 {
                return static_cast<quint8>(whole[off])
                     | (static_cast<quint8>(whole[off + 1]) << 8)
                     | (static_cast<quint8>(whole[off + 2]) << 16)
                     | (static_cast<quint8>(whole[off + 3]) << 24);
            };
            const quint32 magic = rdU32(0);
            if (magic != 0x46546C67u /*'glTF'*/) return;
            int off = 12;
            while (off + 8 <= whole.size()) {
                const quint32 clen = rdU32(off);
                const quint32 ctype = rdU32(off + 4);
                const int dataOff = off + 8;
                if (dataOff + static_cast<int>(clen) > whole.size()) return;
                const QByteArray data = whole.mid(dataOff, static_cast<int>(clen));
                if (ctype == 0x4E4F534Au) {          // JSON
                    jsonBytes = data;
                } else if (ctype == 0x004E4942u) {   // BIN
                    binChunk = data;
                    haveBinChunk = true;
                }
                off = dataOff + static_cast<int>(clen);
            }
            if (jsonBytes.isEmpty()) return;
        } else {
            jsonBytes = whole;
        }

        QJsonParseError perr;
        QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) return;
        QJsonObject root = doc.object();

        // ── Locate the morph node + authoritative target count ──
        int numTargets = 0;
        const int nodeIdx = findMorphNode(root, static_cast<int>(mesh->getPoseCount()),
                                          &numTargets);
        if (nodeIdx < 0 || numTargets <= 0) return;

        // ── Decode the weight clips against numTargets ──
        std::vector<MorphWeightClip> clips = collectMorphWeightClips(entity, numTargets);
        if (clips.empty()) return;

        // ── Build the extra data-URI buffer + accessors/bufferViews ──
        // All weight data lives in a NEW buffer encoded as a base64 data URI,
        // so we never touch the existing GLB BIN chunk (buffer 0) or the
        // existing .gltf buffers — much simpler and safe.
        QByteArray extraBuf;

        QJsonArray accessors = root.value("accessors").toArray();
        QJsonArray bufferViews = root.value("bufferViews").toArray();
        QJsonArray buffers = root.value("buffers").toArray();
        QJsonArray animations = root.value("animations").toArray();

        // Guard against a duplicate animation NAME. Assimp's glTF2 exporter can
        // leave an empty animation carrying a morph clip's name (a channel-less
        // skeletal clip that leaked through buildAiScene). If we then append our
        // real weights animation under the same name, the file has two same-named
        // glTF animations — which Ogre refuses to re-import (it throws
        // "animation already exists" and aborts the whole load → 0 entities).
        // Drop any existing animation whose name matches a weight clip we're
        // about to inject, so ours is the only one. (issue #517)
        {
            QSet<QString> injectNames;
            for (const MorphWeightClip& clip : clips)
                injectNames.insert(QString::fromStdString(clip.name));
            QJsonArray kept;
            for (const QJsonValue& av : animations) {
                const QString nm = av.toObject().value("name").toString();
                if (injectNames.contains(nm))
                    continue;  // will be re-added below (deduped)
                kept.append(av);
            }
            animations = kept;
        }

        const int newBufferIndex = buffers.size();  // appended below

        for (const MorphWeightClip& clip : clips) {
            const int K = static_cast<int>(clip.times.size());
            if (K == 0) continue;

            // — input (times) accessor —
            const int timesByteOffset = extraBuf.size();
            appendFloatsLE(extraBuf, clip.times);
            const int timesBv = bufferViews.size();
            {
                QJsonObject bv;
                bv["buffer"] = newBufferIndex;
                bv["byteOffset"] = timesByteOffset;
                bv["byteLength"] = static_cast<int>(clip.times.size() * sizeof(float));
                bufferViews.append(bv);
            }
            float tmin = clip.times.front(), tmax = clip.times.front();
            for (float t : clip.times) { tmin = std::min(tmin, t); tmax = std::max(tmax, t); }
            const int inputAccessor = accessors.size();
            {
                QJsonObject acc;
                acc["bufferView"] = timesBv;
                acc["byteOffset"] = 0;
                acc["componentType"] = 5126;   // FLOAT
                acc["count"] = K;
                acc["type"] = "SCALAR";
                acc["min"] = QJsonArray{ static_cast<double>(tmin) };
                acc["max"] = QJsonArray{ static_cast<double>(tmax) };
                accessors.append(acc);
            }

            // — output (weights) accessor —
            const int weightsByteOffset = extraBuf.size();
            appendFloatsLE(extraBuf, clip.weights);
            const int weightsBv = bufferViews.size();
            {
                QJsonObject bv;
                bv["buffer"] = newBufferIndex;
                bv["byteOffset"] = weightsByteOffset;
                bv["byteLength"] = static_cast<int>(clip.weights.size() * sizeof(float));
                bufferViews.append(bv);
            }
            const int outputAccessor = accessors.size();
            {
                QJsonObject acc;
                acc["bufferView"] = weightsBv;
                acc["byteOffset"] = 0;
                acc["componentType"] = 5126;   // FLOAT
                acc["count"] = K * numTargets;
                acc["type"] = "SCALAR";
                accessors.append(acc);
            }

            // — animation entry (sampler index is animation-local) —
            QJsonObject sampler;
            sampler["input"] = inputAccessor;
            sampler["output"] = outputAccessor;
            sampler["interpolation"] = "LINEAR";

            QJsonObject targetObj;
            targetObj["node"] = nodeIdx;
            targetObj["path"] = "weights";

            QJsonObject channel;
            channel["sampler"] = 0;   // first (only) sampler within this animation
            channel["target"] = targetObj;

            QJsonObject animObj;
            animObj["name"] = QString::fromStdString(clip.name);
            animObj["samplers"] = QJsonArray{ sampler };
            animObj["channels"] = QJsonArray{ channel };
            animations.append(animObj);
        }

        if (extraBuf.isEmpty()) return;  // nothing to inject after all

        // — append the data-URI buffer —
        {
            QJsonObject buf;
            const QByteArray b64 = extraBuf.toBase64();
            buf["uri"] = QStringLiteral("data:application/octet-binary;base64,")
                         + QString::fromLatin1(b64);
            buf["byteLength"] = extraBuf.size();
            buffers.append(buf);
        }

        root["accessors"] = accessors;
        root["bufferViews"] = bufferViews;
        root["buffers"] = buffers;
        root["animations"] = animations;

        // ── Re-serialise ──
        const QByteArray newJson =
            QJsonDocument(root).toJson(QJsonDocument::Compact);

        QFile out(filePath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;

        if (isBinary) {
            // Re-emit the GLB container: header + JSON chunk (space-padded to
            // 4-byte alignment) + the ORIGINAL unchanged BIN chunk (buffer 0).
            // The injected weight data lives in the data-URI buffer inside the
            // JSON, so the BIN chunk is copied byte-for-byte.
            QByteArray jsonPadded = newJson;
            while (jsonPadded.size() % 4 != 0) jsonPadded.append(' ');

            const auto wrU32 = [&](QByteArray& b, quint32 v) {
                b.append(static_cast<char>(v & 0xFF));
                b.append(static_cast<char>((v >> 8) & 0xFF));
                b.append(static_cast<char>((v >> 16) & 0xFF));
                b.append(static_cast<char>((v >> 24) & 0xFF));
            };

            // BIN chunk data padded to 4-byte alignment with zero bytes.
            QByteArray binPadded;
            if (haveBinChunk) {
                binPadded = binChunk;
                while (binPadded.size() % 4 != 0) binPadded.append('\0');
            }

            quint32 total = 12;                             // header
            total += 8 + static_cast<quint32>(jsonPadded.size());  // JSON chunk
            if (haveBinChunk)
                total += 8 + static_cast<quint32>(binPadded.size());  // BIN chunk

            QByteArray glb;
            wrU32(glb, 0x46546C67u);   // magic 'glTF'
            wrU32(glb, 2u);            // version
            wrU32(glb, total);         // total length

            wrU32(glb, static_cast<quint32>(jsonPadded.size()));
            wrU32(glb, 0x4E4F534Au);   // 'JSON'
            glb.append(jsonPadded);

            if (haveBinChunk) {
                wrU32(glb, static_cast<quint32>(binPadded.size()));
                wrU32(glb, 0x004E4942u);   // 'BIN\0'
                glb.append(binPadded);
            }

            out.write(glb);
        } else {
            out.write(newJson);
        }
        out.close();

        SentryReporter::addBreadcrumb(
            QStringLiteral("file.export"),
            QStringLiteral("Injected %1 morph-weight animation(s) into %2")
                .arg(clips.size()).arg(filePath));
    } catch (const std::exception& ex) {
        Ogre::LogManager::getSingleton().logWarning(
            std::string("injectMorphWeightAnimations failed (left file as-is): ")
            + ex.what());
    } catch (...) {
        Ogre::LogManager::getSingleton().logWarning(
            "injectMorphWeightAnimations failed with unknown exception "
            "(left file as-is)");
    }
}

} // namespace

QString MeshImporterExporter::exporter(const Ogre::SceneNode *_sn, QWidget* parent)
{
    if(!_sn)
    {
        QMessageBox::warning(parent,"No object","Which object are you trying to export?",QMessageBox::Ok);
        return QString();
    }

    QString filter = "Ogre Mesh (*.mesh)";
    QString fileName = QFileDialog::getSaveFileName(parent, QObject::tr("Export Mesh"),
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
    if (!_sn) return -1;
    if (_uri.isEmpty()) return -1;

    QFileInfo file;
    file.setFile(_uri);

    if(!Manager::getSingleton()->getSceneMgr()->hasEntity(_sn->getName())) {
        return -1;
    }
    const Ogre::Entity *e = Manager::getSingleton()->getSceneMgr()->getEntity(_sn->getName());
    if (!e) return -1;

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
        // .mesh (Ogre serializer) cannot store SceneManager node-transform
        // clips — persist them to a `.nodeanim.json` sidecar (#517).
        writeNodeAnimSidecar(_uri, const_cast<Ogre::SceneNode*>(_sn));
    } else if (_format == "FBX Binary (*.fbx)") {
        bool ok = FBXExporter::exportFBX(e, _uri);
        // FBXExporter embeds textures (Video.Content) so avoid emitting sidecar
        // .material and extracted image files next to the FBX.
        if (!ok)
            return -1;
        SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                                      QStringLiteral("Exported FBX: %1").arg(_uri));
        if (!SceneLightsIO::writeLightsSidecar(_uri))
        {
            Ogre::LogManager::getSingleton().logWarning(
                "FBX exported but lights sidecar write failed: " + _uri.toStdString());
        }
        // The custom FBXExporter has no node-transform-clip path — persist
        // SceneManager node clips to a `.nodeanim.json` sidecar (#517).
        writeNodeAnimSidecar(_uri, const_cast<Ogre::SceneNode*>(_sn));
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
        std::unordered_map<int, bool> submeshUnlit; ///< Psy-Q MAT no-light bit per submesh.

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
                // Strip any asset-scoping (`__<hash>`) we may have added at import time so
                // the sidecar lands next to the .rsd with a clean, human-readable filename.
                // For non-RSD textures this is a best-effort basename of whatever the
                // texture resource was registered under.
                QString candidate = rsdResourceNameToBasename(ot.resourceName);
                // Two distinct textures can collapse to the same stripped basename (e.g. two
                // different imports both shipping a `Wood.jpg`). Detect collisions against
                // previously-claimed outFiles and fall back to the scoped resource name so
                // each RSD TEX[] slot writes to its own sidecar instead of clobbering it.
                if (std::any_of(rsdOutTextures.begin(), rsdOutTextures.end(),
                                [&candidate](const OutTex& prev) {
                                    return prev.outFile == candidate;
                                }))
                    candidate = QFileInfo(ot.resourceName).fileName();
                ot.outFile = candidate;
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

        static const QRegularExpression kPsyqTexUnlitSuffix(
            QStringLiteral("_tex\\d+(_nl)?$"));
        static const QRegularExpression kPsyqSolidUnlitSuffix(
            QStringLiteral("_solid(_nl)?$"));
        for (unsigned int si = 0; si < e->getNumSubEntities(); ++si) {
            const Ogre::SubEntity* se = e->getSubEntity(si);
            if (!se)
                continue;
            bool unlit = false;
            const QString mname = QString::fromStdString(se->getMaterialName());
            const auto texM = kPsyqTexUnlitSuffix.match(mname);
            if (texM.hasMatch() && texM.captured(1) == QStringLiteral("_nl"))
                unlit = true;
            else {
                const auto solM = kPsyqSolidUnlitSuffix.match(mname);
                if (solM.hasMatch() && solM.captured(1) == QStringLiteral("_nl"))
                    unlit = true;
            }
            if (!unlit) {
                Ogre::MaterialPtr mat = se->getMaterial();
                if (!mat.isNull() && mat->getNumTechniques() > 0
                    && mat->getTechnique(0)->getNumPasses() > 0) {
                    const Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
                    if (pass && !pass->getLightingEnabled())
                        unlit = true;
                }
            }
            submeshUnlit[static_cast<int>(si)] = unlit;
        }

        // Synthesise MAT entries: one per output PLY face. Pick the most specific Psy-Q
        // type that preserves the source data:
        //   * Textured + smooth corner colours -> 'H' (textured smooth, per-corner tint)
        //   * Textured + uniform corner colour -> 'D' (textured flat colour)
        //   * Textured + no colour              -> 'T' (textured, no colour)
        //   * Untextured + smooth corner colours -> 'G' (smooth Gouraud)
        //   * Untextured + uniform colour       -> 'C' (flat colour)
        // Without this, every face would collapse to 'T' or 'C', losing baked AO / vertex-
        // shading gradients (e.g. the wall corners in the Blender RSD example).
        const bool haveColors  = !faceColors.isEmpty()
                               && faceColors.size() == static_cast<int>(faceTexInfos.size());
        const bool haveTexInfo = !faceTexInfos.isEmpty();

        // Two corner colours are considered the same when their 8-bit channels match exactly.
        // Tolerance kept tight so PS1's 8-bit-per-channel input doesn't degrade Gouraud info.
        auto cornersUniform = [](const PS1PLY::ExportFaceTexture& eft) {
            if (!eft.hasCornerColors)
                return true;
            const int n = std::clamp(eft.cornerCount, 1, 4);
            const QColor& c0 = eft.cornerColors[0];
            for (int k = 1; k < n; ++k) {
                const QColor& ck = eft.cornerColors[static_cast<size_t>(k)];
                if (c0.red() != ck.red() || c0.green() != ck.green() || c0.blue() != ck.blue())
                    return false;
            }
            return true;
        };

        QVector<PS1MAT::MatEntry> entries;
        if (haveTexInfo || haveColors) {
            const int nFaces = haveTexInfo ? faceTexInfos.size() : faceColors.size();
            entries.reserve(nFaces);
            for (int fi = 0; fi < nFaces; ++fi) {
                PS1MAT::MatEntry me;

                const PS1PLY::ExportFaceTexture& eft = haveTexInfo
                    ? faceTexInfos[fi]
                    : PS1PLY::ExportFaceTexture{};
                const QColor faceColor = haveColors ? faceColors[fi] : QColor(255, 255, 255);
                const int corners = (eft.cornerCount == 4) ? 4 : 3;
                const bool gotCornerColors = eft.hasCornerColors;
                const bool smoothShade = gotCornerColors && !cornersUniform(eft);

                const int smIdx = (fi < faceTexInfos.size()) ? faceTexInfos[fi].submeshIndex : -1;
                if (smIdx >= 0) {
                    const auto uit = submeshUnlit.find(smIdx);
                    if (uit != submeshUnlit.end())
                        me.unlit = uit->second;
                }

                const int slotIt = (eft.textured && submeshToTexSlot.count(eft.submeshIndex))
                    ? submeshToTexSlot[eft.submeshIndex]
                    : -1;
                // shadingChar stays 'F' (flat normals) to match the Blender RSD exporter
                // convention: Psy-Q PLY stores per-face normals, so Gouraud-style smoothness
                // is encoded purely via the typeChar (G/H), not via normal interpolation.
                me.shadingChar = 'F';
                if (eft.textured && slotIt >= 0) {
                    me.textured = true;
                    me.textureIndex = slotIt;
                    const OutTex& ot = rsdOutTextures[slotIt];
                    const int texW = ot.width > 0 ? ot.width : 256;
                    const int texH = ot.height > 0 ? ot.height : 256;
                    me.uvs.resize(corners);
                    for (int k = 0; k < corners; ++k) {
                        me.uvs[k].u = static_cast<int>(std::lround(double(eft.u[k]) * double(texW)));
                        me.uvs[k].v = static_cast<int>(std::lround(double(eft.v[k]) * double(texH)));
                    }
                    if (smoothShade) {
                        me.typeChar = 'H';                    // textured smooth (per-corner tint)
                        me.vertColors.reserve(corners);
                        for (int k = 0; k < corners; ++k)
                            me.vertColors.push_back(eft.cornerColors[static_cast<size_t>(k)]);
                        me.rgb = me.vertColors.first();
                    } else if (gotCornerColors) {
                        me.typeChar = 'D';                    // textured flat colour
                        const QColor c = eft.cornerColors[0].isValid() ? eft.cornerColors[0]
                                                                       : QColor(255, 255, 255);
                        me.vertColors.push_back(c);
                        me.rgb = c;
                    } else {
                        me.typeChar = 'T';                    // textured, no colour
                        me.rgb = QColor(255, 255, 255);
                    }
                } else if (smoothShade) {
                    me.typeChar = 'G';                        // smooth Gouraud
                    me.vertColors.reserve(corners);
                    for (int k = 0; k < corners; ++k)
                        me.vertColors.push_back(eft.cornerColors[static_cast<size_t>(k)]);
                    me.rgb = me.vertColors.first();
                } else {
                    me.typeChar = 'C';                        // flat colour
                    const QColor c = gotCornerColors ? eft.cornerColors[0] : faceColor;
                    me.vertColors.push_back(c.isValid() ? c : QColor(255, 255, 255));
                    me.rgb = me.vertColors.first();
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
        // PS1 hardware/emulators only consume TIM, so always emit a 16bpp TIM regardless of
        // the source image format (JPG/PNG/BMP/TGA/etc.) — the legacy PNG sidecar code is
        // kept only as a defensive fallback when the TIM writer fails (e.g. zero-byte
        // image, exception in pixel conversion).
        //
        // A sidecar write failure must abort the whole RSD export: writing the descriptor
        // anyway would leave TEX[] entries pointing to nonexistent / stale files, breaking
        // any later round-trip through the RSD importer or third-party tools.
        const auto writePngFallback = [](const Ogre::Image& img, const QString& pngPath) {
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
            return qi.copy().save(pngPath, "PNG");
        };

        // Returns true on success and updates ot.outFile to the actual on-disk filename.
        // Returns false when both the TIM and PNG fallback writes fail — caller must abort.
        const auto writeSidecar = [&](OutTex& ot) -> bool {
            auto tex = Ogre::TextureManager::getSingleton().getByName(ot.resourceName.toStdString());
            if (!tex)
                return true; // No bound texture — leave ot.outFile alone, descriptor still references the previous file.
            Ogre::Image img;
            tex->convertToImage(img, true);
            if (img.getWidth() == 0 || img.getHeight() == 0)
                return true;

            const QString basename = QFileInfo(ot.outFile).completeBaseName();
            const QString dirSep = QDir::separator();

            // Primary: 16bpp PS1 TIM. PS1 emulators / hardware can't decode JPG/PNG, so
            // this is what every well-formed RSD ships next to the descriptor.
            const QString tim = outFi.absolutePath() + dirSep + basename + QStringLiteral(".tim");
            QString timErr;
            if (PS1TIM::saveOgreImageToTim16(img, tim, &timErr)) {
                ot.outFile = QFileInfo(tim).fileName();
                return true;
            }
            Ogre::LogManager::getSingleton().logError(
                "RSD TIM sidecar write failed: " + timErr.toStdString()
                + " — falling back to PNG so the .rsd still references a real file.");
            SentryReporter::addBreadcrumb(
                QStringLiteral("file.export"),
                QStringLiteral("RSD TIM sidecar write failed, falling back to PNG: %1")
                    .arg(timErr));

            const QString png = outFi.absolutePath() + dirSep + basename + QStringLiteral(".png");
            if (writePngFallback(img, png)) {
                ot.outFile = QFileInfo(png).fileName();
                return true;
            }
            Ogre::LogManager::getSingleton().logError(
                "Failed to write RSD texture sidecar (TIM and PNG both failed): "
                + png.toStdString());
            SentryReporter::addBreadcrumb(
                QStringLiteral("file.export"),
                QStringLiteral("RSD texture sidecar write failed: %1")
                    .arg(QFileInfo(png).fileName()));
            return false;
        };

        bool sidecarWriteFailed = false;
        for (auto& ot : rsdOutTextures) {
            bool ok = false;
            try {
                ok = writeSidecar(ot);
            } catch (const Ogre::Exception& ex) {
                Ogre::LogManager::getSingleton().logError(
                    std::string("RSD texture sidecar Ogre exception: ") + ex.what());
                SentryReporter::addBreadcrumb(
                    QStringLiteral("file.export"),
                    QStringLiteral("RSD texture sidecar Ogre exception: %1")
                        .arg(QString::fromUtf8(ex.what())));
            }
            if (!ok) {
                sidecarWriteFailed = true;
                break;
            }
        }
        if (sidecarWriteFailed)
            return -1;

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
            // glTF/GLB and DirectX (.x) are right-handed like Ogre — skip
            // handedness conversion. For .x specifically, Assimp's exporter
            // handles the RH→LH conversion internally, so applying
            // ConvertToLeftHanded would double-flip the geometry. For glTF
            // (also RH), ConvertToLeftHanded would flip the X axis AND
            // reverse triangle winding, which silently breaks any consumer
            // that pairs the export with a separately-computed vertex
            // stream — e.g. the OpenVAT bake, whose texture columns are
            // indexed by Ogre's original RH vertex order. Only apply LH
            // conversion for formats that genuinely expect left-handed
            // coords (the Assimp pose exporter has the same skip list).
            bool rightHanded = (formatId == "x"
                                || formatId == "gltf2"
                                || formatId == "glb2");
            unsigned int exportFlags = rightHanded
                ? 0 : aiProcess_ConvertToLeftHanded;

            // glTF spec mandates V=0 at the top-left of the texture
            // (DirectX-style), while Ogre stores UVs as authored —
            // typically with V=0 at the bottom (OpenGL-style). The
            // import-time `aiProcess_ConvertToLeftHanded` was historically
            // responsible for flipping V via its bundled FlipUVs step;
            // dropping LH conversion above also drops that flip, leaving
            // the exported glTF with Ogre's V values. Every glTF consumer
            // (Godot, three.js, Blender) then V-flips on import, landing
            // the texture upside-down relative to the bind pose. Restore
            // the V flip explicitly for glTF/GLB so the round-trip
            // remains correct — this is unrelated to handedness and the
            // OpenVAT bake doesn't care about UV0 (the bake encodes
            // positions + normals by column, not by UV lookup).
            if (formatId == "gltf2" || formatId == "glb2")
                exportFlags |= aiProcess_FlipUVs;
            // Split >65535-vertex meshes so Assimp's exporters don't truncate
            // the vertex accessors at 65536 (which tears large meshes apart).
            splitLargeMeshesForExport(scene);
            aiReturn result = exporter.Export(scene, formatId.toStdString().c_str(),
                                             file.filePath().toStdString().c_str(),
                                             exportFlags);
            if (result != AI_SUCCESS)
            {
                auto msg = QString("Assimp export to %1 failed (code %2): %3")
                    .arg(formatId).arg(result).arg(exporter.GetErrorString());
                Ogre::LogManager::getSingleton().logError(msg.toStdString());
                SentryReporter::captureMessage(msg, "error");
                FeedbackReportHelper::showFailureWithReportOption(
                    nullptr,
                    QObject::tr("Export failed"),
                    msg,
                    FeedbackReportHelper::exportFailurePrefill(_format, msg, QString::number(result)));
            }
            else
            {
                // Export texture files alongside the model.
                // For FBX we aim for a single-file export (embedded textures), so skip sidecars.
                if (_format != "FBX Binary (*.fbx)")
                    exportMaterial(e, file);

                // Assimp's glTF2 exporter drops morph-target WEIGHT animation
                // channels. Post-process the written file to inject the
                // standard glTF `weights` channels from Ogre's mesh weight
                // clips. No-op when the mesh has no morph weight animation;
                // never throws out of the exporter.
                if (formatId == "gltf2" || formatId == "glb2")
                    injectMorphWeightAnimations(file.filePath(), e,
                                                /*isBinary=*/formatId == "glb2");

                // Morph-target NAME sidecar: Assimp's glTF2 exporter also
                // drops `targetNames`, so a rigged mesh re-imported from this
                // export would degrade to "Shape_N" names. Persist the
                // ordered pose names next to the file (the same
                // `<file>.arkit.json` the CLI/MCP face-rig paths write); the
                // importer restores them on load.
                if (e->getMesh() && e->getMesh()->getPoseCount() > 0)
                {
                    std::vector<QString> poseNames;
                    QSet<QString> seen;
                    for (const auto* pose : e->getMesh()->getPoseList())
                    {
                        const QString n = QString::fromStdString(pose->getName());
                        if (n.isEmpty() || seen.contains(n)) continue;
                        seen.insert(n);
                        poseNames.push_back(n);
                    }
                    if (!poseNames.empty())
                        FaceRig::writeArkitSidecar(file.filePath(), poseNames);
                }
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
            // Split >65535-vertex meshes (Assimp exporter vertex-accessor cap).
            splitLargeMeshesForExport(scene);
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

// Build aiAnimations for the SceneManager-level node-transform clips
// (NodeAnimationManager, #517 slice C / C5 export). These are distinct
// from skeletal animations: their tracks target real Ogre::SceneNodes
// (animated props/doors/lights/cameras), not bones. glTF/FBX/DAE all
// support plain node TRS animation natively, so each clip becomes one
// aiAnimation whose aiNodeAnim channels target the scene node by name —
// the SAME name buildSceneAiScene() gives each entity's aiNode, so the
// importer rebinds them on round-trip.
//
// Unlike buildAiAnimation() (skeletal), node keyframes store the node's
// ABSOLUTE local TRS (that's what NodeAnimationManager writes via
// setTranslate/setRotation/setScale = node->getPosition()/Orientation()/
// Scale()), so we emit them directly with no bind-pose offset.
//
// `exportedNodeNames` gates channels to nodes actually being exported, so
// a clip that references a since-deleted node contributes nothing rather
// than an orphan channel. Empty clips (no surviving channels) are skipped.
static std::vector<aiAnimation*> buildNodeClipAnimations(
    const std::set<std::string, std::less<>>& exportedNodeNames)
{
    std::vector<aiAnimation*> out;
    auto* manager = Manager::getSingletonPtr();
    if (!manager) return out;
    auto* scene = manager->getSceneMgr();
    if (!scene) return out;

    for (unsigned short ai = 0; ai < scene->getNumAnimations(); ++ai)
    {
        Ogre::Animation* ogreAnim = scene->getAnimation(ai);
        if (!ogreAnim) continue;

        std::vector<aiNodeAnim*> channels;
        for (const auto& [handle, track] : ogreAnim->_getNodeTrackList())
        {
            if (!track) continue;
            Ogre::Node* target = track->getAssociatedNode();
            if (!target) continue;
            // Skip bone tracks — those belong to the skeletal path. Node
            // clips target SceneNodes. (A bone is-a Node, so exclude it.)
            if (dynamic_cast<Ogre::Bone*>(target)) continue;
            const std::string nodeName = target->getName();
            if (exportedNodeNames.find(nodeName) == exportedNodeNames.end())
                continue;

            const unsigned short numKeys = track->getNumKeyFrames();
            if (numKeys == 0) continue;

            auto* ch = new aiNodeAnim();
            ch->mNodeName = aiString(nodeName);
            ch->mNumPositionKeys = numKeys;
            ch->mNumRotationKeys = numKeys;
            ch->mNumScalingKeys  = numKeys;
            ch->mPositionKeys = new aiVectorKey[numKeys];
            ch->mRotationKeys = new aiQuatKey[numKeys];
            ch->mScalingKeys  = new aiVectorKey[numKeys];

            for (unsigned short ki = 0; ki < numKeys; ++ki)
            {
                auto* kf = track->getNodeKeyFrame(ki);
                const double t = kf->getTime();

                const Ogre::Vector3 pos = kf->getTranslate();
                ch->mPositionKeys[ki].mTime = t;
                ch->mPositionKeys[ki].mValue = aiVector3D(pos.x, pos.y, pos.z);

                Ogre::Quaternion rot = kf->getRotation();
                rot.normalise();
                ch->mRotationKeys[ki].mTime = t;
                ch->mRotationKeys[ki].mValue = aiQuaternion(rot.w, rot.x, rot.y, rot.z);

                const Ogre::Vector3 scl = kf->getScale();
                ch->mScalingKeys[ki].mTime = t;
                ch->mScalingKeys[ki].mValue = aiVector3D(scl.x, scl.y, scl.z);
            }
            channels.push_back(ch);
        }

        if (channels.empty()) continue;

        auto* anim = new aiAnimation();
        anim->mName = aiString(ogreAnim->getName());
        anim->mTicksPerSecond = 1.0;  // key times are in seconds
        anim->mDuration = ogreAnim->getLength();
        anim->mNumChannels = static_cast<unsigned int>(channels.size());
        anim->mChannels = new aiNodeAnim*[anim->mNumChannels];
        for (unsigned int ci = 0; ci < anim->mNumChannels; ++ci)
            anim->mChannels[ci] = channels[ci];
        out.push_back(anim);
    }
    return out;
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
        SceneLightsIO::appendLightsToAiScene(scene, SceneLightsIO::captureFromScene());
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
        SceneLightsIO::appendLightsToAiScene(scene, SceneLightsIO::captureFromScene());
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
    // Names of the aiNodes we emit (one per scene node) — used to bind
    // NodeAnimationManager node-transform clips (#517 C5) on export.
    std::set<std::string, std::less<>> exportedNodeNames;

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
        aiNode* meshOwnerNode = nullptr;
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
            meshOwnerNode = meshNode;

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
            meshOwnerNode = entityNode;
        }

        Ogre::Matrix4 nodeTransform;
        nodeTransform.makeTransform(sn->getPosition(), sn->getScale(), sn->getOrientation());
        entityNode->mTransformation = toAiMatrix(nodeTransform);
        exportedNodeNames.insert(std::string(sn->getName()));

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
            readSubmeshGeometry(aiM, vData, subMesh, entity, si, matIndexMap, false);

            if (hasSkeleton)
                assignBoneWeights(aiM, subMesh, mesh, skeleton, boneHandleToName);

            // Morph-target SHAPES (blend shapes). The single-entity buildAiScene
            // attaches these; the SCENE path (save_scene / glb) did NOT, so
            // exported scenes dropped every blend shape and morph animation had
            // no targets to drive (bug: "morph carried the T-pose"). Attach
            // before compaction, then remap the target vertex arrays like the
            // single-entity path.
            const unsigned int preCompactCount = aiM->mNumVertices;
            attachMorphTargetsToAiMesh(aiM, mesh, subMesh, si);

            const std::vector<unsigned int> remap = compactAiMesh(aiM);
            remapAiMeshMorphTargets(aiM, remap, preCompactCount);

            std::vector<std::vector<unsigned int>> ngonFaces;
            if (meshOwnerNode
                && readNgonFacesFromMesh(mesh.get(), si, ngonFaces)
                && !ngonFaces.empty()
                && remapNgonFaces(remap, ngonFaces))
            {
                if (!meshOwnerNode->mMetaData)
                    meshOwnerNode->mMetaData = new aiMetadata();
                for (size_t faceIndex = 0; faceIndex < ngonFaces.size(); ++faceIndex)
                {
                    if (ngonFaces[faceIndex].size() < 3)
                        continue;
                    meshOwnerNode->mMetaData->Add(
                        sceneNgonFaceMetaKey(si, static_cast<unsigned int>(faceIndex)),
                        encodeSceneNgonFaceMetadata(ngonFaces[faceIndex]));
                }
            }
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

    // Append SceneManager-level node-transform clips (#517 C5). These
    // target the scene-node aiNodes by name and coexist with skeletal
    // animations in the same glTF `animations` array.
    {
        auto nodeClipAnims = buildNodeClipAnimations(exportedNodeNames);
        allAnimations.insert(allAnimations.end(),
                             nodeClipAnims.begin(), nodeClipAnims.end());
    }

    // Assign animations to scene
    if (!allAnimations.empty())
    {
        scene->mNumAnimations = static_cast<unsigned int>(allAnimations.size());
        scene->mAnimations = new aiAnimation*[scene->mNumAnimations];
        for (unsigned int i = 0; i < scene->mNumAnimations; ++i)
            scene->mAnimations[i] = allAnimations[i];
    }

    SceneLightsIO::appendLightsToAiScene(scene, SceneLightsIO::captureFromScene());

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

        // Split >65535-vertex meshes (Assimp exporter vertex-accessor cap).
        splitLargeMeshesForExport(scene);
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

        // Assimp's glTF2 exporter drops morph-target WEIGHT animation channels
        // (it only emits node TRS). The single-entity exporter() path patches
        // them back in via injectMorphWeightAnimations; do the same for the
        // SCENE path so morph-weight clips (Inspector / MCP set_morph_weight_
        // keyframe) survive save_scene now that the shapes export too. No-op
        // when an entity has no weight clips.
        for (const auto& [snPair, entityPair] : entities)
        {
            (void)snPair;
            if (entityPair && entityPair->getMesh() && entityPair->getMesh()->getPoseCount() > 0)
                injectMorphWeightAnimations(file.filePath(), entityPair,
                                            /*isBinary=*/formatId == "glb2");
        }

        // Assimp's glb2 writer may drop custom aiMetadata; persist a sidecar
        // (same strategy as FBX export) so user-added lights always round-trip.
        if (!SceneLightsIO::writeLightsSidecar(_uri))
        {
            Ogre::LogManager::getSingleton().logError(
                "Scene exported but lights sidecar write failed: " + _uri.toStdString());
            return -1;
        }
        reportProgress(100, QStringLiteral("Done."));
    } catch (const std::exception& ex) {
        auto msg = QString("Scene export failed: %1").arg(ex.what());
        Ogre::LogManager::getSingleton().logError(msg.toStdString());
        SentryReporter::captureMessage(msg, "error");
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

    // Preserve the vertex/submesh indexing encoded in qtme.faces metadata.
    // Topology-rewriting post-process steps like JoinIdenticalVertices and
    // OptimizeMeshes can invalidate those indices before we restore them.
    unsigned int flags = aiProcess_CalcTangentSpace |
                         aiProcess_Triangulate |
                         aiProcess_RemoveComponent |
                         aiProcess_GenSmoothNormals |
                         aiProcess_ValidateDataStructure |
                         aiProcess_LimitBoneWeights |
                         aiProcess_SortByPType |
                         aiProcess_ImproveCacheLocality |
                         aiProcess_FixInfacingNormals |
                         aiProcess_PopulateArmatureData |
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
    if (auto* lights = LightManager::getSingletonPtr())
        lights->deleteAllUserLights();
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

            std::vector<EditableSubMesh> importedNgonFaces(ogreMesh->getNumSubMeshes());
            bool haveImportedNgons = false;
            const unsigned int importedSubCount =
                std::min<unsigned int>(node->mNumMeshes, ogreMesh->getNumSubMeshes());
            for (unsigned int subIdx = 0; subIdx < importedSubCount; ++subIdx)
            {
                std::vector<std::vector<unsigned int>> faces;
                if (!decodeSceneNgonFacesMetadata(node->mMetaData, subIdx, faces))
                    continue;

                importedNgonFaces[subIdx].faces.reserve(faces.size());
                for (auto& poly : faces)
                {
                    EditableFace face;
                    face.indices = std::move(poly);
                    importedNgonFaces[subIdx].faces.push_back(std::move(face));
                }
                haveImportedNgons = haveImportedNgons || !importedNgonFaces[subIdx].faces.empty();
            }
            if (haveImportedNgons)
                writeNgonFacesToMesh(ogreMesh.get(), importedNgonFaces);

            // Create scene node with decomposed world transform
            Ogre::SceneNode* sn = manager->addSceneNode(nodeName);

            Ogre::Vector3 pos;
            Ogre::Quaternion orient;
            Ogre::Vector3 scale;
            decomposeTransform(entry.worldTransform, pos, orient, scale);
            sn->setPosition(pos);
            sn->setOrientation(orient);
            sn->setScale(scale);

            Ogre::Entity* nodeEnt = manager->createEntity(sn, ogreMesh);

            // Recover node-transform animation clips (#517) for THIS node from
            // the aiScene we already have. Open Scene (sceneImporter) is a
            // separate path from plain Import (importer) — without this, node
            // anim was dropped on the File > Save Scene / Open Scene round-trip.
            reconstructNodeClipsFromAiScene(scene, sn, nodeEnt);
        }

        if (!SceneLightsIO::importLightsSidecar(_uri, true))
            SceneLightsIO::importFromAssimpScene(scene, true);

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
