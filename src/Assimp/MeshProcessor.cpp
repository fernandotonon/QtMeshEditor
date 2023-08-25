#include "MeshProcessor.h"

MeshProcessor::MeshProcessor(Ogre::SkeletonPtr skeleton)
{
    this->skeleton = skeleton;
}

void MeshProcessor::processNode(aiNode* node, const aiScene* scene) {
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

SubMeshData* MeshProcessor::processMesh(aiMesh* mesh, const aiScene* scene) {
    SubMeshData* subMeshData = new SubMeshData();

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
        // Retrieve the bone (it should already exist)
        Ogre::Bone* ogreBone = skeleton->getBone(bone->mName.C_Str());
        for(auto j = 0u; j < bone->mNumWeights; j++) {
            aiVertexWeight weight = bone->mWeights[j];
            int index = -1;
            int freeIndex = -1;
            for(int k = 0; k < 4; k++) {
                if(subMeshData->blendWeights[weight.mVertexId][k] == 0.0f) {
                    freeIndex = k;
                    break;
                }
            }

            if(freeIndex != -1) {
                index = freeIndex;
            } else {
                // Find the smallest weight, replace it if the current weight is larger
                int smallest = 0;
                for(int k = 1; k < 4; k++) {
                    if(subMeshData->blendWeights[weight.mVertexId][k] < subMeshData->blendWeights[weight.mVertexId][smallest]) {
                        smallest = k;
                    }
                }
                if(subMeshData->blendWeights[weight.mVertexId][smallest] < weight.mWeight) {
                    index = smallest;
                }
            }

            if(index != -1) {
                subMeshData->blendIndices[weight.mVertexId][index] = i;
                subMeshData->blendWeights[weight.mVertexId][index] = weight.mWeight;
            }

            Ogre::VertexBoneAssignment vba;
            vba.vertexIndex = weight.mVertexId;
            vba.boneIndex = ogreBone->getHandle();
            vba.weight = weight.mWeight;

            subMeshData->boneAssignments.push_back(vba);
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

    return subMeshData;
}

Ogre::MeshPtr MeshProcessor::createMesh(const Ogre::String& name, const Ogre::String& group, MaterialProcessor& materialProcessor) {
    // Create the mesh
    Ogre::MeshPtr ogreMesh = Ogre::MeshManager::getSingleton().createManual(name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

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

    // clean up to avoid memory leaks.
    subMeshesData.clear();

    return ogreMesh;
}
