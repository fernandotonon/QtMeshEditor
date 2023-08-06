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

    modelName = scene->mName.C_Str();
    if(modelName.empty()) modelName="importedModel";

    if(!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
        Ogre::LogManager::getSingleton().logError("ERROR::ASSIMP::" + std::string(importer.GetErrorString()));
        return Ogre::MeshPtr();
    }

    // Process materials
    processMaterials(scene);

    // Create a new skeleton
    skeleton = Ogre::SkeletonManager::getSingleton().create(modelName+".skeleton", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);

    // Create the root bone
    skeleton->createBone("RootNode");

    // Process the root node recursively
    processNode(scene->mRootNode, scene);

    // Create the mesh
    Ogre::MeshPtr ogreMesh = createMesh();

    // Process animations
    processAnimations(scene);

    return ogreMesh;
}

void AssimpToOgreImporter::processNode(aiNode* node, const aiScene* scene) {
    // Process each mesh located at the current node
    for(unsigned int i = 0; i < node->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        subMeshesData.push_back(processMesh(mesh, scene));
    }

    // After we've processed all of the meshes (if any) we then recursively process each of the children nodes
    for(unsigned int i = 0; i < node->mNumChildren; i++) {
        processNode(node->mChildren[i], scene);
    }
}

SubMeshData AssimpToOgreImporter::processMesh(aiMesh* mesh, const aiScene* scene) {
    SubMeshData subMeshData;
    // Initialize blend indices and blend weights
    for(unsigned int i = 0; i < mesh->mNumVertices; i++) {
        subMeshData.blendIndices.push_back(Ogre::Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        subMeshData.blendWeights.push_back(Ogre::Vector4(0.0f, 0.0f, 0.0f, 0.0f));

        // Process vertices
        subMeshData.vertices.push_back(Ogre::Vector3(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z));

        // Process normals
        if(mesh->HasNormals()) {
            subMeshData.normals.push_back(Ogre::Vector3(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z));
        }

        // Process texture coordinates
        if(mesh->HasTextureCoords(0)) {
            subMeshData.texCoords.push_back(Ogre::Vector2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y));
        }
    }

    // Load blend weights and blend indices
    if(mesh->HasBones()) {
        for(unsigned int i = 0; i < mesh->mNumBones; i++) {
            aiBone* bone = mesh->mBones[i];
            for(unsigned int j = 0; j < bone->mNumWeights; j++) {
                aiVertexWeight weight = bone->mWeights[j];
                int freeIndex = -1;
                for(int k = 0; k < 4; k++) {
                    if(subMeshData.blendWeights[weight.mVertexId][k] == 0.0f) {
                        freeIndex = k;
                        break;
                    }
                }
                if(freeIndex != -1) {
                    subMeshData.blendIndices[weight.mVertexId][freeIndex] = i;
                    subMeshData.blendWeights[weight.mVertexId][freeIndex] = weight.mWeight;
                } else {
                    // Find the smallest weight, replace it if the current weight is larger
                    int smallest = 0;
                    for(int k = 1; k < 4; k++) {
                        if(subMeshData.blendWeights[weight.mVertexId][k] < subMeshData.blendWeights[weight.mVertexId][smallest]) {
                            smallest = k;
                        }
                    }
                    if(subMeshData.blendWeights[weight.mVertexId][smallest] < weight.mWeight) {
                        subMeshData.blendIndices[weight.mVertexId][smallest] = i;
                        subMeshData.blendWeights[weight.mVertexId][smallest] = weight.mWeight;
                    }
                }
            }
        }
    }

    // Normalize the blend weights so they sum to 1
    for(unsigned int i = 0; i < mesh->mNumVertices; i++) {
        float sum = 0.0f;
        for(int j = 0; j < 4; j++) {
            sum += subMeshData.blendWeights[i][j];
        }
        if(sum > 0.0f) {
            for(int j = 0; j < 4; j++) {
                subMeshData.blendWeights[i][j] /= sum;
            }
        }
    }

    // Process indices
    for(unsigned int i = 0; i < mesh->mNumFaces; i++) {
        aiFace face = mesh->mFaces[i];
        for(unsigned int j = 0; j < face.mNumIndices; j++) {
            subMeshData.indices.push_back(face.mIndices[j]);
        }
    }

    // Process Material Index
    subMeshData.materialIndex = mesh->mMaterialIndex;

    // Process tangents and bitangents
    if(mesh->HasTangentsAndBitangents()) {
        for(unsigned int i = 0; i < mesh->mNumVertices; i++) {
            subMeshData.tangents.push_back(Ogre::Vector3(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z));
            subMeshData.bitangents.push_back(Ogre::Vector3(mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z));
        }
    }

    // Process vertex colors
    if(mesh->HasVertexColors(0)) {
        for(unsigned int i = 0; i < mesh->mNumVertices; i++) {
            subMeshData.colors.push_back(Ogre::ColourValue(mesh->mColors[0][i].r, mesh->mColors[0][i].g, mesh->mColors[0][i].b, mesh->mColors[0][i].a));
        }
    }

    // Create a map of aiNode* to BoneNode*
    std::map<aiNode*, BoneNode*> nodeToBoneNode;

    // First pass: create all bones
    for(unsigned int i = 0; i < mesh->mNumBones; i++) {
        aiBone* bone = mesh->mBones[i];
        BoneNode* boneNode = new BoneNode;
        boneNode->bone = bone;
        boneNodes[bone->mName.C_Str()] = boneNode;
        nodeToBoneNode[bone->mNode] = boneNode;  // Populate the nodeToBoneNode map
        createBone(bone, scene);
    }

    // Second pass: set bone properties and relationships
    for(auto& pair : boneNodes) {
        BoneNode* boneNode = pair.second;
        aiBone* bone = boneNode->bone;

        if(bone->mNode->mParent) {
            BoneNode* parentBoneNode = nodeToBoneNode[bone->mNode->mParent];  // Look up the parent bone node in the nodeToBoneNode map
            if(parentBoneNode) {
                parentBoneNode->children.push_back(boneNode);
            }
        }
    }

    // process the BoneNode objects in a depth-first manner
    for(auto& pair : boneNodes) {
        BoneNode* boneNode = pair.second;
        processBoneNode(boneNode, scene, subMeshData);
    }

    // delete the BoneNode objects to avoid memory leaks.
    for(auto& pair : boneNodes) {
        delete pair.second;
    }
    boneNodes.clear();

    return subMeshData;
}

void AssimpToOgreImporter::createBone(aiBone* bone, const aiScene* scene) {
    // Check if the bone already exists
    if(!skeleton->hasBone(bone->mName.C_Str())) {
        // If the bone does not exist, create it
        skeleton->createBone(bone->mName.C_Str());
    }
}

void AssimpToOgreImporter::processBoneNode(BoneNode* boneNode, const aiScene* scene, SubMeshData& subMeshData) {
    aiBone* bone = boneNode->bone;

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
    for(unsigned int i = 0; i < bone->mNumWeights; i++) {
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
            bool isChild = false;
            for(const auto& childNode: parentBone->getChildren()){
                if (childNode->getName() == ogreBone->getName()) {
                    isChild = true;
                    break;
                }
            }
            if(!isChild) {
                parentBone->addChild(ogreBone);
            }
        }
    }
}

void AssimpToOgreImporter::processAnimations(const aiScene* scene) {
    for(unsigned int i = 0; i < scene->mNumAnimations; i++) {
        aiAnimation* animation = scene->mAnimations[i];
        processAnimation(animation, scene);
    }
}

void AssimpToOgreImporter::processAnimation(aiAnimation* animation, const aiScene* scene) {
    // Create the animation
    Ogre::Animation* ogreAnimation = skeleton->createAnimation(animation->mName.C_Str(), animation->mDuration/10.0f);

    // Process the animation channels
    for(unsigned int i = 0; i < animation->mNumChannels; i++) {
        aiNodeAnim* nodeAnim = animation->mChannels[i];
        processAnimationChannel(nodeAnim, ogreAnimation, scene, i);
    }
}


void AssimpToOgreImporter::processAnimationChannel(aiNodeAnim* nodeAnim, Ogre::Animation* animation, const aiScene* scene, unsigned int channelIndex) {
    // Create the animation track
    Ogre::Bone* bone = skeleton->getBone(nodeAnim->mNodeName.C_Str());
    Ogre::NodeAnimationTrack* track = animation->createNodeTrack(bone->getHandle(), bone);

    // Create a map to store keyframes by time
    std::map<double, std::tuple<Ogre::Vector3, Ogre::Quaternion, Ogre::Vector3>> keyframes;

    // Process the position keys
    for(unsigned int i = 0; i < nodeAnim->mNumPositionKeys; i++) {
        aiVectorKey positionKey = nodeAnim->mPositionKeys[i];
        Ogre::Vector3 position(positionKey.mValue.x, positionKey.mValue.y, positionKey.mValue.z);

        // Get the bone's T-pose position
        auto boneTPosePosition = bone->getPosition();

        // Convert the position from local space to model space
        position = position - boneTPosePosition;

        keyframes[positionKey.mTime] = std::make_tuple(
            position,
            Ogre::Quaternion::IDENTITY,
            Ogre::Vector3::UNIT_SCALE
            );
    }

    // Process the rotation keys
    for(unsigned int i = 0; i < nodeAnim->mNumRotationKeys; i++) {
        aiQuatKey rotationKey = nodeAnim->mRotationKeys[i];
        Ogre::Quaternion boneTPoseRotation = bone->getOrientation();
        Ogre::Quaternion rot(rotationKey.mValue.w, rotationKey.mValue.x, rotationKey.mValue.y, rotationKey.mValue.z);
        rot = boneTPoseRotation.Inverse() * rot; // Convert from local space to model space
        rot.normalise(); // Normalize the quaternion
        if (keyframes.find(rotationKey.mTime) == keyframes.end()) {
            keyframes[rotationKey.mTime] = std::make_tuple(
                Ogre::Vector3::ZERO,
                rot,
                Ogre::Vector3::UNIT_SCALE
                );
        } else {
            std::get<1>(keyframes[rotationKey.mTime]) = rot;
        }
    }


    // Process the scaling keys
    for(unsigned int i = 0; i < nodeAnim->mNumScalingKeys; i++) {
        aiVectorKey scalingKey = nodeAnim->mScalingKeys[i];
        Ogre::Vector3 scale(scalingKey.mValue.x, scalingKey.mValue.y, scalingKey.mValue.z);
        if (keyframes.find(scalingKey.mTime) == keyframes.end()) {
            keyframes[scalingKey.mTime] = std::make_tuple(
                Ogre::Vector3::ZERO,
                Ogre::Quaternion::IDENTITY,
                scale
                );
        } else {
            std::get<2>(keyframes[scalingKey.mTime]) = scale;
        }
    }

    // Now create the keyframes in the track
    for(auto& [time, transform] : keyframes) {
        Ogre::TransformKeyFrame* keyFrame = track->createNodeKeyFrame(time/10.0f);
        keyFrame->setTranslate(std::get<0>(transform));
        keyFrame->setRotation(std::get<1>(transform));
        keyFrame->setScale(std::get<2>(transform));
    }
}


void AssimpToOgreImporter::processMaterials(const aiScene* scene) {
    for(unsigned int i = 0; i < scene->mNumMaterials; i++) {
        aiMaterial* material = scene->mMaterials[i];
        Ogre::MaterialPtr ogreMaterial = processMaterial(material);
        materials.push_back(ogreMaterial);
    }
}

Ogre::MaterialPtr AssimpToOgreImporter::processMaterial(aiMaterial* material) {
    aiColor3D color(0.f, 0.f, 0.f);
    float shininess = 0.0f;

    material->Get(AI_MATKEY_COLOR_DIFFUSE, color);
    Ogre::MaterialPtr ogreMaterial = Ogre::MaterialManager::getSingleton().create("MyMaterial" + std::to_string(materials.size()), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ogreMaterial->getTechnique(0)->getPass(0)->setDiffuse(color.r, color.g, color.b, 1.0f);

    if(AI_SUCCESS == material->Get(AI_MATKEY_COLOR_AMBIENT, color)) {
        ogreMaterial->getTechnique(0)->getPass(0)->setAmbient(color.r, color.g, color.b);
    }

    if(AI_SUCCESS == material->Get(AI_MATKEY_COLOR_SPECULAR, color)) {
        ogreMaterial->getTechnique(0)->getPass(0)->setSpecular(color.r, color.g, color.b, 1.0f);
    }

    if(AI_SUCCESS == material->Get(AI_MATKEY_COLOR_EMISSIVE, color)) {
        ogreMaterial->getTechnique(0)->getPass(0)->setSelfIllumination(color.r, color.g, color.b);
    }

    if(AI_SUCCESS == material->Get(AI_MATKEY_SHININESS, shininess)) {
        ogreMaterial->getTechnique(0)->getPass(0)->setShininess(shininess);
    }

    // Handle textures
    aiString path;
    if(AI_SUCCESS == material->GetTexture(aiTextureType_DIFFUSE, 0, &path)) {
        std::string texturePath = path.C_Str();
        std::string textureFilename = texturePath.substr(texturePath.find_last_of("/\\") + 1);
        Ogre::TexturePtr texture = Ogre::TextureManager::getSingleton().load(textureFilename, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        ogreMaterial->getTechnique(0)->getPass(0)->createTextureUnitState(texture->getName());
    }

    return ogreMaterial;
}


Ogre::MeshPtr AssimpToOgreImporter::createMesh() {
    // Create the mesh
    Ogre::MeshPtr ogreMesh = Ogre::MeshManager::getSingleton().createManual(modelName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    // Initialize the min and max coordinates to the first vertex
    Ogre::Vector3 minCoords = subMeshesData[0].vertices[0];
    Ogre::Vector3 maxCoords = subMeshesData[0].vertices[0];

    size_t submeshCount = 0;
    for(const SubMeshData& subMeshData : subMeshesData) {
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
        vertexData->vertexCount = subMeshData.vertices.size();

        // Create the vertex buffer and set the vertex data
        Ogre::HardwareVertexBufferSharedPtr vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(vertexDecl->getVertexSize(0), vertexData->vertexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float* pVertex = static_cast<float*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));

        // Set the vertex positions, blend weights, normals, texture coordinates, and blend indices
        for(size_t i = 0; i < subMeshData.vertices.size(); i++) {
            *pVertex++ = subMeshData.vertices[i].x;
            *pVertex++ = subMeshData.vertices[i].y;
            *pVertex++ = subMeshData.vertices[i].z;

            *pVertex++ = subMeshData.normals[i].x;
            *pVertex++ = subMeshData.normals[i].y;
            *pVertex++ = subMeshData.normals[i].z;

            *pVertex++ = subMeshData.texCoords[i].x;
            *pVertex++ = subMeshData.texCoords[i].y;
        }

        vbuf->unlock();
        vertexData->vertexBufferBinding->setBinding(0, vbuf);

        // Create the tangent buffer and set the tangent data
        Ogre::HardwareVertexBufferSharedPtr tangentBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3), vertexData->vertexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float* pTangent = static_cast<float*>(tangentBuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for(const Ogre::Vector3& tangent : subMeshData.tangents) {
            *pTangent++ = tangent.x;
            *pTangent++ = tangent.y;
            *pTangent++ = tangent.z;
        }
        tangentBuf->unlock();
        vertexData->vertexBufferBinding->setBinding(1, tangentBuf);

        // Create the bitangent buffer and set the bitangent data
        Ogre::HardwareVertexBufferSharedPtr bitangentBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3), vertexData->vertexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float* pBitangent = static_cast<float*>(bitangentBuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for(const Ogre::Vector3& bitangent : subMeshData.bitangents) {
            *pBitangent++ = bitangent.x;
            *pBitangent++ = bitangent.y;
            *pBitangent++ = bitangent.z;
        }
        bitangentBuf->unlock();
        vertexData->vertexBufferBinding->setBinding(2, bitangentBuf);

        // Create the color buffer and set the color data
        Ogre::HardwareVertexBufferSharedPtr colorBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(Ogre::VertexElement::getTypeSize(Ogre::VET_COLOUR), vertexData->vertexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        Ogre::RGBA* pColor = static_cast<Ogre::RGBA*>(colorBuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for(const Ogre::ColourValue& color : subMeshData.colors) {
            Ogre::ColourValue finalColor(color.r, color.g, color.b, color.a);
            *pColor++ = finalColor.getAsARGB();
        }
        colorBuf->unlock();
        vertexData->vertexBufferBinding->setBinding(3, colorBuf);

        // Create the index data and set it to the submesh
        Ogre::IndexData* indexData = subMesh->indexData;

        // Set the index count
        indexData->indexCount = subMeshData.indices.size();

        // Create the index buffer and set the index data
        Ogre::HardwareIndexBufferSharedPtr ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(Ogre::HardwareIndexBuffer::IT_16BIT, indexData->indexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        unsigned short* pIndices = static_cast<unsigned short*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));

        // Set the indices
        for(size_t i = 0; i < subMeshData.indices.size(); i++) {
            *pIndices++ = subMeshData.indices[i];
        }

        ibuf->unlock();
        indexData->indexBuffer = ibuf;

        // Update the min and max coordinates
        for(const Ogre::Vector3& vertex : subMeshData.vertices) {
            minCoords.x = std::min(minCoords.x, vertex.x);
            minCoords.y = std::min(minCoords.y, vertex.y);
            minCoords.z = std::min(minCoords.z, vertex.z);
            maxCoords.x = std::max(maxCoords.x, vertex.x);
            maxCoords.y = std::max(maxCoords.y, vertex.y);
            maxCoords.z = std::max(maxCoords.z, vertex.z);
        }

        // Set the bone assignments
        for(const Ogre::VertexBoneAssignment& vba : subMeshData.boneAssignments) {
            subMesh->addBoneAssignment(vba);
        }

        // Assign the material
        subMesh->setMaterialName(materials[subMeshData.materialIndex]->getName());

        ++submeshCount;
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
