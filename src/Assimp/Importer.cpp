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
#include "AnimationProcessor.h"

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

    // Create a new skeleton
    skeleton = Ogre::SkeletonManager::getSingleton().create(modelName+".skeleton", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);

    createOgreBones(scene);

    // Process the root node recursively (meshes)
    processNode(scene->mRootNode, scene);

    // Process the root bones first
    for(auto i = 0u; i < scene->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[i];
        for(auto j = 0u; j < mesh->mNumBones; j++) {
            aiBone* bone = mesh->mBones[j];
            if(!skeleton->hasBone(bone->mNode->mParent->mName.C_Str())) {
                createBone(bone->mNode->mParent->mName.C_Str());
                processBoneHierarchy(bone, scene);
            }
        }
    }

    // Create the mesh
    Ogre::MeshPtr ogreMesh = createMesh();

    // Process animations
    AnimationProcessor animationProcessor(skeleton);
    animationProcessor.processAnimations(scene);

    // clean up to avoid memory leaks.
    subMeshesData.clear();

    return ogreMesh;
}

void AssimpToOgreImporter::processBoneHierarchy(aiBone* bone, const aiScene* scene) {
    auto boneNameStr = bone->mName.C_Str();
    auto boneIter = boneNameToSubMeshes.find(boneNameStr);

    // Process the bone node
    if(boneIter != boneNameToSubMeshes.end() && !boneIter->second.empty()) {
        for(auto subMeshData : boneIter->second) {
            processBoneNode(subMeshData->mapAiBone[boneNameStr], *subMeshData);
        }
        boneNameToSubMeshes.erase(boneIter);
    }

    // Recursively process children bones
    for(auto i = 0u; i < bone->mNode->mNumChildren; i++) {
        aiNode* childNode = bone->mNode->mChildren[i];
        if(aiBonesMap.find(childNode->mName.C_Str()) != aiBonesMap.end()) {
            processBoneHierarchy(aiBonesMap[childNode->mName.C_Str()], scene);
        }
    }
}

void AssimpToOgreImporter::processNode(aiNode* node, const aiScene* scene) {
    // Process each mesh located at the current node
    for(auto i = 0u; i < node->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        subMeshesData.push_back(processMesh(mesh, scene));
    }

    // After we've processed all of the meshes (if any) we then recursively process each of the children nodes
    for(auto i = 0u; i < node->mNumChildren; i++) {
        processNode(node->mChildren[i], scene);
    }
}

SubMeshData* AssimpToOgreImporter::processMesh(aiMesh* mesh, const aiScene* scene) {
    SubMeshData* subMeshData = new SubMeshData();
    subMeshData->mName = mesh->mName.C_Str();

    // Initialize blend indices and blend weights
    for(auto i = 0u; i < mesh->mNumVertices; i++) {
        subMeshData->blendIndices.push_back(Ogre::Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        subMeshData->blendWeights.push_back(Ogre::Vector4(0.0f, 0.0f, 0.0f, 0.0f));

        // Process vertices
        subMeshData->vertices.push_back(Ogre::Vector3(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z));

        // Process normals
        if(mesh->HasNormals()) {
            subMeshData->normals.push_back(Ogre::Vector3(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z));
        }

        // Process texture coordinates
        if(mesh->HasTextureCoords(0)) {
            subMeshData->texCoords.push_back(Ogre::Vector2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y));
        }
    }

    // Load blend weights and blend indices
    for(auto i = 0u; i < mesh->mNumBones; i++) {
        aiBone* bone = mesh->mBones[i];
        for(auto j = 0u; j < bone->mNumWeights; j++) {
            aiVertexWeight weight = bone->mWeights[j];
            int freeIndex = -1;
            for(int k = 0; k < 4; k++) {
                if(subMeshData->blendWeights[weight.mVertexId][k] == 0.0f) {
                    freeIndex = k;
                    break;
                }
            }
            if(freeIndex != -1) {
                subMeshData->blendIndices[weight.mVertexId][freeIndex] = i;
                subMeshData->blendWeights[weight.mVertexId][freeIndex] = weight.mWeight;
            } else {
                // Find the smallest weight, replace it if the current weight is larger
                int smallest = 0;
                for(int k = 1; k < 4; k++) {
                    if(subMeshData->blendWeights[weight.mVertexId][k] < subMeshData->blendWeights[weight.mVertexId][smallest]) {
                        smallest = k;
                    }
                }
                if(subMeshData->blendWeights[weight.mVertexId][smallest] < weight.mWeight) {
                    subMeshData->blendIndices[weight.mVertexId][smallest] = i;
                    subMeshData->blendWeights[weight.mVertexId][smallest] = weight.mWeight;
                }
            }
        }
    }

    // Normalize the blend weights so they sum to 1
    for(auto i = 0u; i < mesh->mNumVertices; i++) {
        float sum = 0.0f;
        for(int j = 0; j < 4; j++) {
            sum += subMeshData->blendWeights[i][j];
        }
        if(sum > 0.0f) {
            for(int j = 0; j < 4; j++) {
                subMeshData->blendWeights[i][j] /= sum;
            }
        }
    }

    // Process indices
    for(auto i = 0u; i < mesh->mNumFaces; i++) {
        aiFace face = mesh->mFaces[i];
        for(auto j = 0u; j < face.mNumIndices; j++) {
            subMeshData->indices.push_back(face.mIndices[j]);
        }
    }

    // Process Material Index
    subMeshData->materialIndex = mesh->mMaterialIndex;

    // Process tangents and bitangents
    if(mesh->HasTangentsAndBitangents()) {
        for(auto i = 0u; i < mesh->mNumVertices; i++) {
            subMeshData->tangents.push_back(Ogre::Vector3(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z));
            subMeshData->bitangents.push_back(Ogre::Vector3(mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z));
        }
    }

    // Process vertex colors
    if(mesh->HasVertexColors(0)) {
        for(auto i = 0u; i < mesh->mNumVertices; i++) {
            subMeshData->colors.push_back(Ogre::ColourValue(mesh->mColors[0][i].r, mesh->mColors[0][i].g, mesh->mColors[0][i].b, mesh->mColors[0][i].a));
        }
    }

    // Add aiBones to the submesh
    for(auto i = 0u; i < mesh->mNumBones; i++) {
        aiBone* bone = mesh->mBones[i];
        subMeshData->mapAiBone[bone->mName.C_Str()] = bone;
    }

    // add the submesh to the boneNameToSubMeshes map
    for(auto i = 0u; i < mesh->mNumBones; i++) {
        aiBone* bone = mesh->mBones[i];
        if(boneNameToSubMeshes.find(bone->mName.C_Str()) == boneNameToSubMeshes.end()){
            boneNameToSubMeshes[bone->mName.C_Str()] = std::vector<SubMeshData*>();
        }
        boneNameToSubMeshes[bone->mName.C_Str()].push_back(subMeshData);
    }

    return subMeshData;
}

void AssimpToOgreImporter::createOgreBones(const aiScene *scene) {
    for(auto i = 0u; i < scene->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[i];
        for(auto j = 0u; j < mesh->mNumBones; j++) {
            aiBone* bone = mesh->mBones[j];
            // Check if the bone already exists
            if(!skeleton->hasBone(bone->mName.C_Str())) {
                aiBonesMap[bone->mName.C_Str()] = bone;
                createBone(bone->mName.C_Str());
            }
        }
    }

    for(auto i = 0u; i < scene->mNumAnimations; i++) {
        aiAnimation* anim = scene->mAnimations[i];
        for(auto j = 0u; j < anim->mNumChannels; j++) {
            aiNodeAnim* nodeAnim = anim->mChannels[j];
            createBone(nodeAnim->mNodeName.C_Str());
        }
    }
}

void AssimpToOgreImporter::createBone(const std::string& boneName) {
    // Check if the bone already exists
    if(!skeleton->hasBone(boneName)) {
        // If the bone does not exist, create it
        skeleton->createBone(boneName);
    }
}

void AssimpToOgreImporter::processBoneNode(aiBone* bone, SubMeshData& subMeshData) {
    // Convert the aiBone's offset matrix to an Ogre::Matrix4
    Ogre::Matrix4 offsetMatrix(
        bone->mOffsetMatrix.a1, bone->mOffsetMatrix.a2, bone->mOffsetMatrix.a3, bone->mOffsetMatrix.a4,
        bone->mOffsetMatrix.b1, bone->mOffsetMatrix.b2, bone->mOffsetMatrix.b3, bone->mOffsetMatrix.b4,
        bone->mOffsetMatrix.c1, bone->mOffsetMatrix.c2, bone->mOffsetMatrix.c3, bone->mOffsetMatrix.c4,
        bone->mOffsetMatrix.d1, bone->mOffsetMatrix.d2, bone->mOffsetMatrix.d3, bone->mOffsetMatrix.d4
        );

    // Invert the offset matrix to get the global transformation of the bone
    Ogre::Matrix4 globalTransform = offsetMatrix.inverse();

    // If the bone has a parent, multiply the global transformation of the bone with the inverse of the global transformation of the parent to get the local transformation
    if(bone->mNode->mParent && bone->mNode->mParent->mName.length) {
        Ogre::Bone* parentBone = skeleton->getBone(bone->mNode->mParent->mName.C_Str());
        if(parentBone) {
            Ogre::Matrix4 parentGlobalTransform = parentBone->_getFullTransform().inverse();
            globalTransform = parentGlobalTransform * globalTransform;
        }
    }

    // Convert the Ogre::Matrix4 to an Ogre::Affine3
    Ogre::Affine3 affine(globalTransform);

    // Decompose the offset matrix into position, scale, and orientation
    Ogre::Vector3 position, scale;
    Ogre::Quaternion orientation;
    affine.decomposition(position, scale, orientation);

    // Retrieve the bone (it should already exist)
    Ogre::Bone* ogreBone = skeleton->getBone(bone->mName.C_Str());

    // Set the bone's position, orientation, and scale
    ogreBone->setPosition(position);
    ogreBone->setOrientation(orientation);
    ogreBone->setScale(scale);

    // Set the bone weights
    for(auto i = 0u; i < bone->mNumWeights; i++) {
        aiVertexWeight weight = bone->mWeights[i];

        Ogre::VertexBoneAssignment vba;
        vba.vertexIndex = weight.mVertexId;
        vba.boneIndex = ogreBone->getHandle();
        vba.weight = weight.mWeight;

        subMeshData.boneAssignments.push_back(vba);

        // Update the blend indices and blend weights
        Ogre::Vector4& blendIndices = subMeshData.blendIndices[weight.mVertexId];
        Ogre::Vector4& blendWeights = subMeshData.blendWeights[weight.mVertexId];
        for(int j = 0; j < 4; j++) {
            if(blendWeights[j] == 0.0f) {
                blendIndices[j] = ogreBone->getHandle();
                blendWeights[j] = weight.mWeight;
                break;
            }
        }
    }

    // Add the bone to the parent bone, if it exists
    if(bone->mNode->mParent && bone->mNode->mParent->mName.length) {
        Ogre::Bone* parentBone = skeleton->getBone(bone->mNode->mParent->mName.C_Str());
        if(parentBone) {
            // Check if ogreBone is already a child of parentBone
            if (!std::any_of(parentBone->getChildren().begin(), parentBone->getChildren().end(),
                             [&ogreBone](const auto& childNode) {
                                 return childNode->getName() == ogreBone->getName();
                             })) {
                parentBone->addChild(ogreBone);
            }
        }
    }
}

Ogre::MeshPtr AssimpToOgreImporter::createMesh() {
    // Create the mesh
    Ogre::MeshPtr ogreMesh = Ogre::MeshManager::getSingleton().createManual(modelName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    // Initialize the min and max coordinates to the first vertex
    Ogre::Vector3 minCoords = subMeshesData[0]->vertices[0];
    Ogre::Vector3 maxCoords = subMeshesData[0]->vertices[0];

    for(const auto& subMeshData : subMeshesData) {
        // Create a submesh
        Ogre::SubMesh* subMesh = ogreMesh->createSubMesh();

        // Create the vertex data
        Ogre::VertexData* vertexData = new Ogre::VertexData();
        subMesh->useSharedVertices = false;
        subMesh->vertexData = vertexData;

        // Define the vertex format
        Ogre::VertexDeclaration* vertexDecl = vertexData->vertexDeclaration;
        size_t currOffset = vertexDecl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION).getSize();
        currOffset += vertexDecl->addElement(0, currOffset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL).getSize();
        vertexDecl->addElement(0, currOffset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES).getSize();

        // Set the vertex count
        vertexData->vertexCount = subMeshData->vertices.size();

        // Create the vertex buffer and set the vertex data
        Ogre::HardwareVertexBufferSharedPtr vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(vertexDecl->getVertexSize(0), vertexData->vertexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float* pVertex = static_cast<float*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));

        // Set the vertex positions, blend weights, normals, texture coordinates, and blend indices
        for(size_t i = 0; i < subMeshData->vertices.size(); i++) {
            *pVertex++ = subMeshData->vertices[i].x;
            *pVertex++ = subMeshData->vertices[i].y;
            *pVertex++ = subMeshData->vertices[i].z;

            *pVertex++ = subMeshData->normals[i].x;
            *pVertex++ = subMeshData->normals[i].y;
            *pVertex++ = subMeshData->normals[i].z;

            *pVertex++ = subMeshData->texCoords[i].x;
            *pVertex++ = subMeshData->texCoords[i].y;
        }

        vbuf->unlock();
        vertexData->vertexBufferBinding->setBinding(0, vbuf);

        // Create the tangent buffer and set the tangent data
        Ogre::HardwareVertexBufferSharedPtr tangentBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3), vertexData->vertexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float* pTangent = static_cast<float*>(tangentBuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for(const Ogre::Vector3& tangent : subMeshData->tangents) {
            *pTangent++ = tangent.x;
            *pTangent++ = tangent.y;
            *pTangent++ = tangent.z;
        }
        tangentBuf->unlock();
        vertexData->vertexBufferBinding->setBinding(1, tangentBuf);

        // Create the bitangent buffer and set the bitangent data
        Ogre::HardwareVertexBufferSharedPtr bitangentBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3), vertexData->vertexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float* pBitangent = static_cast<float*>(bitangentBuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for(const Ogre::Vector3& bitangent : subMeshData->bitangents) {
            *pBitangent++ = bitangent.x;
            *pBitangent++ = bitangent.y;
            *pBitangent++ = bitangent.z;
        }
        bitangentBuf->unlock();
        vertexData->vertexBufferBinding->setBinding(2, bitangentBuf);

        // Create the color buffer and set the color data
        Ogre::HardwareVertexBufferSharedPtr colorBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(Ogre::VertexElement::getTypeSize(Ogre::VET_COLOUR), vertexData->vertexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        Ogre::RGBA* pColor = static_cast<Ogre::RGBA*>(colorBuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for(const Ogre::ColourValue& color : subMeshData->colors) {
            Ogre::ColourValue finalColor(color.r, color.g, color.b, color.a);
            *pColor++ = finalColor.getAsARGB();
        }
        colorBuf->unlock();
        vertexData->vertexBufferBinding->setBinding(3, colorBuf);

        // Create the index data and set it to the submesh
        Ogre::IndexData* indexData = subMesh->indexData;

        // Set the index count
        indexData->indexCount = subMeshData->indices.size();

        // Create the index buffer and set the index data
        Ogre::HardwareIndexBufferSharedPtr ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(Ogre::HardwareIndexBuffer::IT_16BIT, indexData->indexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        unsigned short* pIndices = static_cast<unsigned short*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));

        // Set the indices
        for(size_t i = 0; i < subMeshData->indices.size(); i++) {
            *pIndices++ = subMeshData->indices[i];
        }

        ibuf->unlock();
        indexData->indexBuffer = ibuf;

        // Update the min and max coordinates
        for(const Ogre::Vector3& vertex : subMeshData->vertices) {
            minCoords.x = std::min(minCoords.x, vertex.x);
            minCoords.y = std::min(minCoords.y, vertex.y);
            minCoords.z = std::min(minCoords.z, vertex.z);
            maxCoords.x = std::max(maxCoords.x, vertex.x);
            maxCoords.y = std::max(maxCoords.y, vertex.y);
            maxCoords.z = std::max(maxCoords.z, vertex.z);
        }

        // Set the bone assignments
        for(const Ogre::VertexBoneAssignment& vba : subMeshData->boneAssignments) {
            subMesh->addBoneAssignment(vba);
        }

        // Assign the material
        subMesh->setMaterialName(materialProcessor[subMeshData->materialIndex]->getName());
    }

    // Set the bounding box and bounding sphere radius
    ogreMesh->_setBounds(Ogre::AxisAlignedBox(minCoords, maxCoords));
    ogreMesh->_setBoundingSphereRadius((maxCoords - minCoords).length() / 2.0f);

    // Set the skeleton
    ogreMesh->setSkeletonName(skeleton->getName());

    // Compile the mesh
    ogreMesh->load();

    return ogreMesh;
}
