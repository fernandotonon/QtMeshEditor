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
#include "Assimp/Importer.h"

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
    {"STP (*.stp)", ".stp"},
    {"OBJ (*.obj)", ".obj"},
    {"OBJ without MTL (*.objnomtl)", ".obj"},
    {"STL (*.stl)", ".stl"},
    {"STL Binary (*.stlb)", ".stlb"},
    {"PLY (*.ply)", ".ply"},
    {"PLY Binary (*.plyb)", ".plyb"},
    {"3DS (*.3ds)", ".3ds"},
    {"glTF 2.0 (*.gltf2)", ".gltf2"},
    {"glTF 2.0 Binary (*.glb2)", ".glb2"},
    {"glTF 1.0 (*.gltf)", ".gltf"},
    {"glTF 1.0 Binary (*.glb)", ".glb"},
    {"Assimp Binary (*.assbin)", ".assbin"}
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
                    img.save((file.path() + "/" + tex->getName().c_str()).toStdString());
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

// Build bone node hierarchy recursively
static aiNode* buildBoneNode(Ogre::Bone* bone, aiNode* parent)
{
    auto* node = new aiNode(std::string(bone->getName()));
    node->mParent = parent;

    // Set local transform from bone bind pose
    Ogre::Matrix4 localTransform;
    localTransform.makeTransform(bone->getPosition(), bone->getScale(), bone->getOrientation());
    node->mTransformation = toAiMatrix(localTransform);

    // Process children
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
                node->mChildren[ci++] = buildBoneNode(childBone, node);
        }
        node->mNumChildren = ci;
    }

    return node;
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
    Ogre::Skeleton* skeleton = hasSkeleton ? entity->getSkeleton() : nullptr;

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
    {
        auto* aiMat = new aiMaterial();
        aiString matName(materials[i]->getName());
        aiMat->AddProperty(&matName, AI_MATKEY_NAME);

        auto* tech = materials[i]->getTechnique(0);
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

            // Add texture references
            for (unsigned short ti = 0; ti < pass->getNumTextureUnitStates(); ++ti)
            {
                auto* tus = pass->getTextureUnitState(ti);
                if (tus->getContentType() == Ogre::TextureUnitState::CONTENT_NAMED)
                {
                    aiString texPath(tus->getTextureName());
                    aiMat->AddProperty(&texPath, AI_MATKEY_TEXTURE(aiTextureType_DIFFUSE, ti));
                }
            }
        }
        scene->mMaterials[i] = aiMat;
    }

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
    scene->mMeshes = new aiMesh*[numSub];

    for (unsigned int si = 0; si < numSub; ++si)
    {
        const Ogre::SubMesh* subMesh = mesh->getSubMesh(si);
        const Ogre::VertexData* vData = subMesh->useSharedVertices
            ? mesh->sharedVertexData : subMesh->vertexData;
        if (!vData) { scene->mMeshes[si] = new aiMesh(); continue; }

        auto* aiM = new aiMesh();
        scene->mMeshes[si] = aiM;
        aiM->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
        aiM->mNumVertices = static_cast<unsigned int>(vData->vertexCount);
        aiM->mVertices = new aiVector3D[aiM->mNumVertices];

        // Assign material index
        const auto* subEnt = entity->getSubEntity(si);
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

        // Bone weights for this submesh
        if (hasSkeleton)
        {
            // Group bone assignments by bone handle
            const auto& boneAssignments = subMesh->getBoneAssignments();
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

                    // Offset matrix = inverse of the bone's global bind pose
                    auto* bone = skeleton->getBone(handle);
                    Ogre::Matrix4 globalTransform = bone->_getFullTransform();
                    aiBoneObj->mOffsetMatrix = toAiMatrix(globalTransform.inverse());

                    // Copy vertex weights
                    aiBoneObj->mNumWeights = static_cast<unsigned int>(weights.size());
                    aiBoneObj->mWeights = new aiVertexWeight[aiBoneObj->mNumWeights];
                    for (unsigned int wi = 0; wi < aiBoneObj->mNumWeights; ++wi)
                        aiBoneObj->mWeights[wi] = weights[wi];

                    aiM->mBones[bi++] = aiBoneObj;
                }
            }
        }
    }

    // --- Animations ---
    if (hasSkeleton && skeleton->getNumAnimations() > 0)
    {
        scene->mNumAnimations = skeleton->getNumAnimations();
        scene->mAnimations = new aiAnimation*[scene->mNumAnimations];

        for (unsigned short ai = 0; ai < skeleton->getNumAnimations(); ++ai)
        {
            auto* ogreAnim = skeleton->getAnimation(ai);
            auto* anim = new aiAnimation();
            scene->mAnimations[ai] = anim;

            anim->mName = aiString(ogreAnim->getName());
            anim->mTicksPerSecond = 1.0;
            anim->mDuration = ogreAnim->getLength();

            std::vector<aiNodeAnim*> channels;

            for (const auto& [handle, track] : ogreAnim->_getNodeTrackList())
            {
                auto* bone = dynamic_cast<Ogre::Bone*>(track->getAssociatedNode());
                if (!bone) continue;

                auto* nodeAnim = new aiNodeAnim();
                nodeAnim->mNodeName = aiString(bone->getName());

                auto numKeyFrames = track->getNumKeyFrames();
                nodeAnim->mNumPositionKeys = numKeyFrames;
                nodeAnim->mNumRotationKeys = numKeyFrames;
                nodeAnim->mNumScalingKeys = numKeyFrames;
                nodeAnim->mPositionKeys = new aiVectorKey[numKeyFrames];
                nodeAnim->mRotationKeys = new aiQuatKey[numKeyFrames];
                nodeAnim->mScalingKeys = new aiVectorKey[numKeyFrames];

                // Ogre stores keyframes as deltas from bind pose
                // Convert back to absolute bone-local transforms for Assimp
                Ogre::Vector3 bindPos = bone->getPosition();
                Ogre::Quaternion bindRot = bone->getOrientation();

                for (unsigned short ki = 0; ki < numKeyFrames; ++ki)
                {
                    auto* kf = track->getNodeKeyFrame(ki);
                    double time = kf->getTime();

                    // Position: absolute = bind + delta
                    Ogre::Vector3 pos = bindPos + kf->getTranslate();
                    nodeAnim->mPositionKeys[ki].mTime = time;
                    nodeAnim->mPositionKeys[ki].mValue = aiVector3D(pos.x, pos.y, pos.z);

                    // Rotation: absolute = bind * delta
                    Ogre::Quaternion rot = bindRot * kf->getRotation();
                    rot.normalise();
                    nodeAnim->mRotationKeys[ki].mTime = time;
                    nodeAnim->mRotationKeys[ki].mValue = aiQuaternion(rot.w, rot.x, rot.y, rot.z);

                    // Scale: stored as-is in Ogre
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
        }
    }

    return scene;
}

// ─── Custom Ogre XML mesh importer ───────────────────────────────
// Reads .mesh.xml using pugixml and builds the Ogre mesh using the same
// HardwareBufferManager calls that MeshProcessor uses (which work reliably).
// This avoids XMLMeshSerializer::readGeometry() whose GL buffer lock fails.
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

    if (!skelName.empty() && QFileInfo::exists(skelPath))
    {
        // Remove stale skeleton
        if (auto oldSkel = Ogre::SkeletonManager::getSingleton().getByName(skelName))
            Ogre::SkeletonManager::getSingleton().remove(oldSkel);

        try {
            // Create as manual resource so Ogre won't try to auto-load from disk
            auto skelPtr = Ogre::SkeletonManager::getSingleton().create(
                skelName, "General", true);
            Ogre::XMLSkeletonSerializer xmlSS;
            xmlSS.importSkeleton(skelPath.toStdString().c_str(), skelPtr.get());
            // Mark as loaded so the mesh finds it ready
            skelPtr->load();
            // Store name — setSkeletonName is called after mesh->load() below
            // so that isLoaded()==true and the skeleton pointer is properly resolved.
            fprintf(stderr, "XML import: skeleton loaded OK, bones=%d anims=%d\n",
                    skelPtr->getNumBones(), skelPtr->getNumAnimations());
        } catch (Ogre::Exception& e) {
            skelName.clear(); // don't link a failed skeleton
            fprintf(stderr, "XML import: skeleton FAILED: %s\n",
                    e.getFullDescription().c_str());
        }
    }

    // Helper lambda: parse a <geometry>/<vertexbuffer> into vectors
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

    // Parse shared geometry (if any)
    GeomData sharedGeom;
    auto sharedGeomNode = root.child("sharedgeometry");
    if (sharedGeomNode)
        sharedGeom = parseGeometry(sharedGeomNode);

    Ogre::Vector3 minCoords(Ogre::Vector3::ZERO), maxCoords(Ogre::Vector3::ZERO);
    bool firstVertex = true;

    // Helper: build vertex/index hardware buffers from parsed data (MeshProcessor style)
    auto buildSubMeshBuffers = [&](Ogre::SubMesh* subMesh, const GeomData& geom,
                                    const std::vector<unsigned int>& indices)
    {
        Ogre::VertexData* vertexData = new Ogre::VertexData();
        subMesh->useSharedVertices = false;
        subMesh->vertexData = vertexData;

        // Vertex declaration
        Ogre::VertexDeclaration* decl = vertexData->vertexDeclaration;
        size_t offset = decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION).getSize();
        if (!geom.normals.empty())
            offset += decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL).getSize();
        if (!geom.texCoords.empty())
            decl->addElement(0, offset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

        vertexData->vertexCount = geom.positions.size();

        // Create vertex buffer
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

            // Update bounds
            const auto& pos = geom.positions[i];
            if (firstVertex) {
                minCoords = maxCoords = pos;
                firstVertex = false;
            } else {
                minCoords.makeFloor(pos);
                maxCoords.makeCeil(pos);
            }
        }
        vbuf->unlock();
        vertexData->vertexBufferBinding->setBinding(0, vbuf);

        // Create index buffer
        if (!indices.empty())
        {
            bool use32bit = vertexData->vertexCount > 65535;
            auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
                use32bit ? Ogre::HardwareIndexBuffer::IT_32BIT : Ogre::HardwareIndexBuffer::IT_16BIT,
                indices.size(), Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);

            if (use32bit) {
                auto* pIdx = static_cast<unsigned int*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
                for (auto idx : indices) *pIdx++ = idx;
            } else {
                auto* pIdx = static_cast<unsigned short*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
                for (auto idx : indices) *pIdx++ = static_cast<unsigned short>(idx);
            }
            ibuf->unlock();
            subMesh->indexData->indexCount = indices.size();
            subMesh->indexData->indexBuffer = ibuf;
        }
    };

    // Parse submeshes
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

        // Geometry: per-submesh or shared
        bool useShared = Ogre::StringConverter::parseBool(smElem.attribute("usesharedvertices").value());
        GeomData geom = useShared ? sharedGeom : parseGeometry(smElem.child("geometry"));

        if (geom.positions.empty()) continue;

        buildSubMeshBuffers(subMesh, geom, indices);

        // Bone assignments
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

    // Shared bone assignments
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

    // If a skeleton is present, ensure every submesh has bone assignments.
    // Submeshes without them cause softwareVertexBlend to assert on missing
    // blend indices/weights.  Assign all vertices to bone 0 as a fallback.
    if (!skelName.empty())
    {
        for (unsigned i = 0; i < mesh->getNumSubMeshes(); ++i)
        {
            auto* sm = mesh->getSubMesh(i);
            if (sm->useSharedVertices) continue; // handled by shared bone assigns
            if (!sm->getBoneAssignments().empty()) continue;

            for (size_t v = 0; v < sm->vertexData->vertexCount; ++v)
            {
                Ogre::VertexBoneAssignment vba;
                vba.vertexIndex = v;
                vba.boneIndex = 0;
                vba.weight = 1.0f;
                sm->addBoneAssignment(vba);
            }
        }
    }

    // Set bounds
    mesh->_setBounds(Ogre::AxisAlignedBox(minCoords, maxCoords));
    mesh->_setBoundingSphereRadius((maxCoords - minCoords).length() / 2.0f);
    mesh->load();

    // Link skeleton AFTER load() so that isLoaded()==true and the
    // skeleton pointer is resolved (not just the name stored).
    if (!skelName.empty())
    {
        mesh->setSkeletonName(skelName);
        // Compile bone assignments into blend indices/weights in vertex buffers.
        // Without this, software vertex blending asserts on missing blend data.
        mesh->_updateCompiledBoneAssignments();
        fprintf(stderr, "XML import: setSkeletonName done, hasSkeleton=%d getSkeleton=%p\n",
                mesh->hasSkeleton(), mesh->getSkeleton().get());
    }

    fprintf(stderr, "XML import: mesh ready, submeshes=%d bounds=%s\n",
            mesh->getNumSubMeshes(),
            mesh->getBounds().isFinite() ? "finite" : "infinite/null");

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

void MeshImporterExporter::importer(const QStringList &_uriList)
{
    try{
        foreach(const QString &fileName,_uriList)
        {
            if(!fileName.size()) continue;

            QFileInfo file;
            file.setFile(fileName);
            fprintf(stderr, "IMPORT: file='%s' suffix='%s' baseName='%s'\n",
                    fileName.toStdString().c_str(), file.suffix().toStdString().c_str(),
                    file.baseName().toStdString().c_str());

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
                fprintf(stderr, "XML import: entity created, hasSkeleton=%d\n",
                        en->hasSkeleton());
            }
            else
            {
                AssimpToOgreImporter importer;
                Ogre::MeshPtr mesh = importer.loadModel(file.filePath().toStdString());
                if (!mesh) return;

                auto meshName = file.baseName();
                sn = Manager::getSingleton()->addSceneNode(QString(meshName));
                en = Manager::getSingleton()->createEntity(sn, mesh);
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
            Ogre::XMLSkeletonSerializer xmlSS;
            xmlSS.exportSkeleton(e->getSkeleton(),(_uri.left(_uri.length()-8)+"skeleton.xml").toStdString().data());
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
            Ogre::SkeletonSerializer ss;
            ss.exportSkeleton(e->getSkeleton(),QString(file.path()+"/"+e->getMesh().get()->getSkeletonName().c_str()).toStdString().data());
        }

        m.exportMesh(e->getMesh().get(),_uri.toStdString().data(),(Ogre::MeshVersion)version);

        exportMaterial(e, file);
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
                {"STP (*.stp)", "stp"},
                {"OBJ (*.obj)", "obj"},
                {"OBJ without MTL (*.objnomtl)", "objnomtl"},
                {"STL (*.stl)", "stl"},
                {"STL Binary (*.stlb)", "stlb"},
                {"PLY (*.ply)", "ply"},
                {"PLY Binary (*.plyb)", "plyb"},
                {"3DS (*.3ds)", "3ds"},
                {"glTF 2.0 (*.gltf2)", "gltf2"},
                {"glTF 2.0 Binary (*.glb2)", "glb2"},
                {"glTF 1.0 (*.gltf)", "gltf"},
                {"glTF 1.0 Binary (*.glb)", "glb"},
                {"Assimp Binary (*.assbin)", "assbin"},
            };

            QString formatId = assimpFormatIds.value(_format, file.suffix());

            // Formats that do NOT support skeletal data — strip bones & animations
            static const QSet<QString> noSkeletonFormats = {
                "3ds", "obj", "objnomtl", "stl", "stlb", "ply", "plyb", "stp"
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
            // Undo the ConvertToLeftHanded applied during import (negates Z, flips
            // face winding, flips UVs) so the exported file is in standard RH convention
            aiReturn result = exporter.Export(scene, formatId.toStdString().c_str(),
                                             file.filePath().toStdString().c_str(),
                                             aiProcess_ConvertToLeftHanded);
            if (result != AI_SUCCESS)
            {
                Ogre::LogManager::getSingleton().logError(
                    "Assimp export to " + formatId.toStdString() + " failed (code " +
                    std::to_string(result) + "): " + std::string(exporter.GetErrorString()));
            }
            else
            {
                // Export texture files alongside the model
                exportMaterial(e, file);
            }

            delete scene;
        } catch (std::exception& ex) {
            Ogre::LogManager::getSingleton().logError(
                "Assimp export failed with exception: " + std::string(ex.what()));
        } catch (...) {
            Ogre::LogManager::getSingleton().logError("Assimp export failed with unknown exception");
        }
    }

    return 0;
}
