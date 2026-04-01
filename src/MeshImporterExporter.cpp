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
#include <QMessageBox>
#include <QFileDialog>
#include <QDebug>
#include <set>

#include "OgreXML/OgreXMLMeshSerializer.h"
#include "OgreXML/OgreXMLSkeletonSerializer.h"
#include "OgreXML/pugixml.hpp"

#include "Manager.h"
#include "SelectionSet.h"
#include "AnimationMerger.h"
#include "SentryReporter.h"
#include "Assimp/Importer.h"
#include "Assimp/MaterialProcessor.h"
#include "Assimp/MeshProcessor.h"
#include "Assimp/BoneProcessor.h"
#include "Assimp/AnimationProcessor.h"

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
    {"glTF 2.0 (*.gltf2)", ".gltf2"},
    {"glTF 2.0 Binary (*.glb2)", ".glb2"},
    {"Assimp Binary (*.assbin)", ".assbin"},
    {"FBX Binary (*.fbx)", ".fbx"}
};

void MeshImporterExporter::configureCamera(const Ogre::Entity *en)
{
    Ogre::Real size = std::max(std::max(en->getBoundingBox().getSize().y,en->getBoundingBox().getSize().x),en->getBoundingBox().getSize().z)    ;
    auto cameras = Manager::getSingleton()->getSceneMgr()->getCameras();
    for(const auto &[_, camera] : cameras)
    {
        const Ogre::Radian fov = camera->getFOVy();
        Ogre::Real distance = size/(2*std::tan(fov.valueRadians()/2));
        camera->getParentSceneNode()->setPosition(0,0,-distance);
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

        unsigned short diffuseIdx = 0;
        unsigned short normalIdx = 0;
        for (unsigned short ti = 0; ti < pass->getNumTextureUnitStates(); ++ti)
        {
            auto* tus = pass->getTextureUnitState(ti);
            if (tus->getContentType() == Ogre::TextureUnitState::CONTENT_NAMED)
            {
                QString safeName = MeshImporterExporter::exportTextureName(
                    QString::fromStdString(tus->getTextureName()));
                aiString texPath(safeName.toStdString());
                const auto& tusName = tus->getName();
                if (tusName == "normal_map" || tusName == "NormalMap")
                {
                    aiMat->AddProperty(&texPath, AI_MATKEY_TEXTURE(aiTextureType_NORMALS, normalIdx));
                    ++normalIdx;
                }
                else
                {
                    aiMat->AddProperty(&texPath, AI_MATKEY_TEXTURE(aiTextureType_DIFFUSE, diffuseIdx));
                    ++diffuseIdx;
                }
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

    // Read indices
    const Ogre::IndexData* iData = subMesh->indexData;
    if (iData && iData->indexCount > 0)
    {
        aiM->mNumFaces = static_cast<unsigned int>(iData->indexCount / 3);
        aiM->mFaces = new aiFace[aiM->mNumFaces];
        auto ibuf = iData->indexBuffer;
        auto* ibase = static_cast<const unsigned char*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        bool use32 = ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT;

        for (unsigned int f = 0; f < aiM->mNumFaces; ++f)
        {
            aiM->mFaces[f].mNumIndices = 3;
            aiM->mFaces[f].mIndices = new unsigned int[3];
            for (unsigned int v = 0; v < 3; ++v)
            {
                unsigned int idx = use32
                    ? reinterpret_cast<const uint32_t*>(ibase)[f * 3 + v]
                    : reinterpret_cast<const uint16_t*>(ibase)[f * 3 + v];
                aiM->mFaces[f].mIndices[v] = idx;
            }
        }
        ibuf->unlock();
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

static void ensureResourceGroup(const QString &path)
{
    auto group = path.toStdString();
    auto &rgm = Ogre::ResourceGroupManager::getSingleton();
    if (!rgm.resourceLocationExists(group, group))
    {
        rgm.addResourceLocation(group, "FileSystem", group);
        try {
            rgm.initialiseResourceGroup(group);
        } catch (Ogre::Exception &e) {
            Ogre::LogManager::getSingleton().logMessage(
                "Warning during resource group init: " + e.getFullDescription());
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

            QFileInfo file;
            file.setFile(fileName);
            ensureResourceGroup(file.path());

            Ogre::SceneNode *sn;
            const Ogre::Entity *en;

            if(!file.suffix().compare("mesh",Qt::CaseInsensitive))
            {
                sn = Manager::getSingleton()->addSceneNode(QString(file.baseName()));
                en = Manager::getSingleton()->createEntity(sn, Ogre::MeshManager::getSingleton().load(file.fileName().toStdString().data(), file.path().toStdString().data()));
            }
            else if(!file.suffix().compare("xml",Qt::CaseInsensitive))
            {
                auto meshName = file.baseName();
                Ogre::MeshPtr mesh = importOgreXmlMesh(file.filePath(), meshName.toStdString());
                if (!mesh) return;

                sn = Manager::getSingleton()->addSceneNode(meshName);
                en = Manager::getSingleton()->createEntity(sn, mesh);
            }
            else
            {
                AssimpToOgreImporter importer;
                // DirectX .x is natively left-handed — skip ConvertToLeftHanded
                // to avoid double-flipping geometry and UVs.
                bool convertLH = (file.suffix().compare("x", Qt::CaseInsensitive) != 0);
                Ogre::MeshPtr mesh = importer.loadModel(file.filePath().toStdString(), convertLH, additionalFlags);
                // Read coordinate system from metadata immediately — valid for both mesh and animation-only files.
                if (outUpAxis) *outUpAxis = importer.getSceneUpAxis();
                if (!mesh) {
                    // Animation-only file: skeleton/animations were loaded, but there is no mesh.
                    Ogre::SkeletonPtr skel = importer.getLoadedSkeleton();
                    if (skel) {
                        if (outAnimOnlySkeletons) {
                            outAnimOnlySkeletons->append(skel);
                        } else {
                            unsigned short numAnims = skel->getNumAnimations();
                            QString animList;
                            for (unsigned short i = 0; i < numAnims; ++i)
                                animList += "\n  \u2022 " + QString::fromStdString(skel->getAnimation(i)->getName());

                            // If there is a selected entity with a skeleton, offer to merge immediately.
                            Ogre::Entity* baseEntity = nullptr;
                            auto selected = SelectionSet::getSingleton()->getResolvedEntities();
                            for (Ogre::Entity* e : selected) {
                                if (e && e->hasSkeleton()) { baseEntity = e; break; }
                            }

                            if (baseEntity) {
                                auto btn = QMessageBox::question(nullptr, "Animation-only file",
                                    QString("'%1' contains no mesh geometry \u2014 %2 animation(s) were found:%3\n\n"
                                            "Merge these animations into '%4'?")
                                        .arg(file.fileName())
                                        .arg(numAnims)
                                        .arg(animList)
                                        .arg(baseEntity->getName().c_str()),
                                    QMessageBox::Yes | QMessageBox::No);
                                if (btn == QMessageBox::Yes) {
                                    QString errMsg;
                                    AnimationMerger::mergeAnimations(baseEntity, {}, {skel}, errMsg);
                                    if (!errMsg.isEmpty())
                                        QMessageBox::warning(nullptr, "Merge failed", errMsg);
                                }
                            } else {
                                QMessageBox::information(nullptr, "Animation-only file",
                                    QString("'%1' contains no mesh geometry \u2014 %2 animation(s) were imported:%3\n\n"
                                            "To use these animations: import the target mesh, select it, "
                                            "then import this file again.")
                                        .arg(file.fileName())
                                        .arg(numAnims)
                                        .arg(animList));
                            }
                        }
                    }
                    continue;
                }

                auto meshName = file.baseName();
                sn = Manager::getSingleton()->addSceneNode(QString(meshName));
                en = Manager::getSingleton()->createEntity(sn, mesh);

                // Auto-correct Z-up coordinate systems (e.g. Unreal Engine FBX exports).
                // Rotate the scene node so the mesh stands upright in Ogre's Y-up world.
                // Bone data and animations remain in their native space — the node rotation
                // is purely a display correction and does not affect animation playback.
                if (importer.getSceneUpAxis() == 2)
                    sn->rotate(Ogre::Vector3::UNIT_X, Ogre::Degree(90));
            }

            sn->setPosition(0,0,0);
            configureCamera(en);
        }
    } 
    catch(Ogre::Exception &e)
    {
        Ogre::LogManager::getSingleton().logMessage(e.getFullDescription());
    }
}

QString MeshImporterExporter::formatFileURI(const QString &_uri, const QString &_format)
{
    if(_uri.isEmpty()) return "";
    const auto ext = exportFormats[_format];
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

int MeshImporterExporter::exporter(const Ogre::SceneNode *_sn, const QString &_uri, const QString &_format)
{
    if(!_sn) return -1;

    if(_uri.isEmpty()) return -1;

    QFileInfo file;
    file.setFile(_uri);

    if(!Manager::getSingleton()->getSceneMgr()->hasEntity(_sn->getName())) return -1;
    const Ogre::Entity *e = Manager::getSingleton()->getSceneMgr()->getEntity(_sn->getName());
    if(!e) return -1;

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
        if (ok)
            exportMaterial(e, file);
        else
            return -1;
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

            // Map format display name to Assimp export format ID
            static const QMap<QString, QString> assimpFormatIds = {
                {"Collada (*.dae)", "collada"},
                {"X (*.x)", "x"},
                {"OBJ (*.obj)", "obj"},
                {"OBJ without MTL (*.objnomtl)", "objnomtl"},
                {"STL (*.stl)", "stl"},
                {"PLY (*.ply)", "ply"},
                {"3DS (*.3ds)", "3ds"},
                {"glTF 2.0 (*.gltf2)", "gltf2"},
                {"glTF 2.0 Binary (*.glb2)", "glb2"},
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
                // Export texture files alongside the model
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
