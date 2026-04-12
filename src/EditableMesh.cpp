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

#include "EditableMesh.h"
#include "SubMeshTransform.h"
#include <OgreSubEntity.h>
#include <limits>
#include <cmath>
#include <cstring>

bool EditableMesh::loadFromEntity(Ogre::Entity* entity)
{
    if (!entity)
        return false;

    Ogre::MeshPtr meshPtr = entity->getMesh();
    if (!meshPtr)
        return false;

    m_sourceEntity = entity;
    m_subMeshes.clear();

    Ogre::Mesh* mesh = meshPtr.get();

    // Read shared vertex data once if any submesh uses it
    std::vector<EditableVertex> sharedVertices;
    bool hasSharedVertexData = (mesh->sharedVertexData != nullptr);
    if (hasSharedVertexData) {
        readVertexData(mesh->sharedVertexData, sharedVertices);
    }

    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
        Ogre::SubMesh* subMesh = mesh->getSubMesh(i);
        EditableSubMesh editSub;

        editSub.usesSharedVertices = subMesh->useSharedVertices;
        editSub.materialName = subMesh->getMaterialName();

        // Read vertices
        if (subMesh->useSharedVertices) {
            editSub.vertices = sharedVertices;
        } else {
            readVertexData(subMesh->vertexData, editSub.vertices);
        }

        // Read bone assignments for this submesh
        if (mesh->hasSkeleton()) {
            const Ogre::SubMesh::VertexBoneAssignmentList& boneAssignments =
                subMesh->useSharedVertices ? mesh->getBoneAssignments() : subMesh->getBoneAssignments();

            for (auto it = boneAssignments.begin(); it != boneAssignments.end(); ++it) {
                const Ogre::VertexBoneAssignment& vba = it->second;
                if (vba.vertexIndex < editSub.vertices.size()) {
                    EditableBoneAssignment eba;
                    eba.boneIndex = vba.boneIndex;
                    eba.weight = vba.weight;
                    editSub.vertices[vba.vertexIndex].boneAssignments.push_back(eba);
                }
            }
        }

        // Read triangles
        if (subMesh->indexData) {
            readIndexData(subMesh->indexData, editSub.triangles);
        }

        m_subMeshes.push_back(std::move(editSub));
    }

    return true;
}

bool EditableMesh::commitToEntity(Ogre::Entity* entity)
{
    if (!entity)
        return false;

    Ogre::MeshPtr meshPtr = entity->getMesh();
    if (!meshPtr)
        return false;

    Ogre::Mesh* mesh = meshPtr.get();

    if (m_subMeshes.size() != static_cast<size_t>(mesh->getNumSubMeshes()))
        return false;

    // Recalculate normals before writing back (respects current mode)
    if (m_flatNormals)
        recalculateNormalsFlat();
    else
        recalculateNormals();

    // For submeshes that use shared vertices, write the first such submesh's
    // data back to the shared buffer (all such submeshes share the same vertex data).
    bool wroteShared = false;

    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
        Ogre::SubMesh* subMesh = mesh->getSubMesh(i);
        const EditableSubMesh& editSub = m_subMeshes[i];

        if (subMesh->useSharedVertices) {
            if (!wroteShared && mesh->sharedVertexData) {
                writeVertexData(mesh->sharedVertexData, editSub.vertices);
                wroteShared = true;
            }
        } else {
            if (subMesh->vertexData) {
                writeVertexData(subMesh->vertexData, editSub.vertices);
            }
        }
    }

    // Recalculate mesh bounds
    SubMeshTransform::recalculateMeshBounds(mesh);

    // For skeletal entities, also update the SubEntity animation blend buffers
    // (Ogre renders from these, not the mesh VBO). Same pattern as SubMeshTransform.
    if (entity->hasSkeleton()) {
        for (unsigned short i = 0; i < entity->getNumSubEntities(); ++i) {
            Ogre::SubEntity* sub = entity->getSubEntity(i);
            Ogre::VertexData* animData = sub->_getSkelAnimVertexData();
            if (!animData || animData->vertexCount == 0) continue;

            const auto* posElem = animData->vertexDeclaration
                ->findElementBySemantic(Ogre::VES_POSITION);
            if (!posElem) continue;

            unsigned short source = posElem->getSource();
            auto animBuf = animData->vertexBufferBinding->getBuffer(source);
            size_t animBufSize = animBuf->getSizeInBytes();
            size_t animVertSize = animBuf->getVertexSize();

            std::vector<unsigned char> animCopy(animBufSize);
            animBuf->readData(0, animBufSize, animCopy.data());

            const auto& verts = m_subMeshes[i].vertices;
            size_t count = std::min(verts.size(), static_cast<size_t>(animData->vertexCount));
            for (size_t j = 0; j < count; ++j) {
                Ogre::Real* pReal;
                posElem->baseVertexPointerToElement(animCopy.data() + j * animVertSize, &pReal);
                pReal[0] = verts[j].position.x;
                pReal[1] = verts[j].position.y;
                pReal[2] = verts[j].position.z;
            }

            auto* dest = static_cast<unsigned char*>(
                animBuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
            memcpy(dest, animCopy.data(), animBufSize);
            animBuf->unlock();
        }
    }

    return true;
}

size_t EditableMesh::totalVertexCount() const
{
    size_t total = 0;
    for (const auto& sub : m_subMeshes)
        total += sub.vertices.size();
    return total;
}

size_t EditableMesh::totalTriangleCount() const
{
    size_t total = 0;
    for (const auto& sub : m_subMeshes)
        total += sub.triangles.size();
    return total;
}

void EditableMesh::setVertexPosition(size_t subMeshIndex, size_t vertexIndex, const Ogre::Vector3& pos)
{
    if (subMeshIndex < m_subMeshes.size() && vertexIndex < m_subMeshes[subMeshIndex].vertices.size())
        m_subMeshes[subMeshIndex].vertices[vertexIndex].position = pos;
}

Ogre::Vector3 EditableMesh::getVertexPosition(size_t subMeshIndex, size_t vertexIndex) const
{
    if (subMeshIndex < m_subMeshes.size() && vertexIndex < m_subMeshes[subMeshIndex].vertices.size())
        return m_subMeshes[subMeshIndex].vertices[vertexIndex].position;
    return Ogre::Vector3::ZERO;
}

void EditableMesh::setVertexNormal(size_t subMeshIndex, size_t vertexIndex, const Ogre::Vector3& normal)
{
    if (subMeshIndex < m_subMeshes.size() && vertexIndex < m_subMeshes[subMeshIndex].vertices.size()) {
        m_subMeshes[subMeshIndex].vertices[vertexIndex].normal = normal;
        m_subMeshes[subMeshIndex].vertices[vertexIndex].hasNormal = true;
    }
}

Ogre::Vector3 EditableMesh::getVertexNormal(size_t subMeshIndex, size_t vertexIndex) const
{
    if (subMeshIndex < m_subMeshes.size() && vertexIndex < m_subMeshes[subMeshIndex].vertices.size())
        return m_subMeshes[subMeshIndex].vertices[vertexIndex].normal;
    return Ogre::Vector3::ZERO;
}

void EditableMesh::setVertexUV(size_t subMeshIndex, size_t vertexIndex, const Ogre::Vector2& uv)
{
    if (subMeshIndex < m_subMeshes.size() && vertexIndex < m_subMeshes[subMeshIndex].vertices.size()) {
        m_subMeshes[subMeshIndex].vertices[vertexIndex].uv = uv;
        m_subMeshes[subMeshIndex].vertices[vertexIndex].hasUV = true;
    }
}

Ogre::Vector2 EditableMesh::getVertexUV(size_t subMeshIndex, size_t vertexIndex) const
{
    if (subMeshIndex < m_subMeshes.size() && vertexIndex < m_subMeshes[subMeshIndex].vertices.size())
        return m_subMeshes[subMeshIndex].vertices[vertexIndex].uv;
    return Ogre::Vector2::ZERO;
}

void EditableMesh::recalculateNormals()
{
    for (auto& sub : m_subMeshes) {
        // Zero out all normals
        for (auto& v : sub.vertices) {
            v.normal = Ogre::Vector3::ZERO;
            v.hasNormal = true;
        }

        // Accumulate face normals (area-weighted)
        for (const auto& tri : sub.triangles) {
            if (tri.indices[0] >= sub.vertices.size() ||
                tri.indices[1] >= sub.vertices.size() ||
                tri.indices[2] >= sub.vertices.size())
                continue;

            const Ogre::Vector3& v0 = sub.vertices[tri.indices[0]].position;
            const Ogre::Vector3& v1 = sub.vertices[tri.indices[1]].position;
            const Ogre::Vector3& v2 = sub.vertices[tri.indices[2]].position;

            // Cross product gives area-weighted face normal
            Ogre::Vector3 faceNormal = (v1 - v0).crossProduct(v2 - v0);

            sub.vertices[tri.indices[0]].normal += faceNormal;
            sub.vertices[tri.indices[1]].normal += faceNormal;
            sub.vertices[tri.indices[2]].normal += faceNormal;
        }

        // Normalize
        for (auto& v : sub.vertices) {
            Ogre::Real len = v.normal.length();
            if (len > 1e-8f)
                v.normal /= len;
        }
    }
}

void EditableMesh::recalculateNormalsFlat()
{
    for (auto& sub : m_subMeshes) {
        // Zero out all normals
        for (auto& v : sub.vertices) {
            v.normal = Ogre::Vector3::ZERO;
            v.hasNormal = true;
        }

        // Assign face normal to each vertex of each triangle
        for (const auto& tri : sub.triangles) {
            if (tri.indices[0] >= sub.vertices.size() ||
                tri.indices[1] >= sub.vertices.size() ||
                tri.indices[2] >= sub.vertices.size())
                continue;

            const Ogre::Vector3& v0 = sub.vertices[tri.indices[0]].position;
            const Ogre::Vector3& v1 = sub.vertices[tri.indices[1]].position;
            const Ogre::Vector3& v2 = sub.vertices[tri.indices[2]].position;

            Ogre::Vector3 faceNormal = (v1 - v0).crossProduct(v2 - v0);
            Ogre::Real len = faceNormal.length();
            if (len > 1e-8f)
                faceNormal /= len;

            // Flat shading: each vertex gets the face normal directly
            sub.vertices[tri.indices[0]].normal = faceNormal;
            sub.vertices[tri.indices[1]].normal = faceNormal;
            sub.vertices[tri.indices[2]].normal = faceNormal;
        }
    }
}

int EditableMesh::countDegenerateTriangles(float epsilon) const
{
    int count = 0;
    for (const auto& sub : m_subMeshes) {
        for (const auto& tri : sub.triangles) {
            if (tri.indices[0] >= sub.vertices.size() ||
                tri.indices[1] >= sub.vertices.size() ||
                tri.indices[2] >= sub.vertices.size())
                continue;

            const Ogre::Vector3& v0 = sub.vertices[tri.indices[0]].position;
            const Ogre::Vector3& v1 = sub.vertices[tri.indices[1]].position;
            const Ogre::Vector3& v2 = sub.vertices[tri.indices[2]].position;

            float area = (v1 - v0).crossProduct(v2 - v0).length();
            if (area < epsilon)
                ++count;
        }
    }
    return count;
}

int EditableMesh::removeDegenerateTriangles(float epsilon)
{
    int totalRemoved = 0;
    for (auto& sub : m_subMeshes) {
        std::vector<EditableTriangle> kept;
        kept.reserve(sub.triangles.size());
        for (const auto& tri : sub.triangles) {
            if (tri.indices[0] >= sub.vertices.size() ||
                tri.indices[1] >= sub.vertices.size() ||
                tri.indices[2] >= sub.vertices.size()) {
                ++totalRemoved;
                continue;
            }

            const Ogre::Vector3& v0 = sub.vertices[tri.indices[0]].position;
            const Ogre::Vector3& v1 = sub.vertices[tri.indices[1]].position;
            const Ogre::Vector3& v2 = sub.vertices[tri.indices[2]].position;

            float area = (v1 - v0).crossProduct(v2 - v0).length();
            if (area < epsilon) {
                ++totalRemoved;
            } else {
                kept.push_back(tri);
            }
        }
        sub.triangles = std::move(kept);
    }
    return totalRemoved;
}

Ogre::AxisAlignedBox EditableMesh::calculateBounds() const
{
    Ogre::Vector3 minimum(std::numeric_limits<Ogre::Real>::max());
    Ogre::Vector3 maximum(std::numeric_limits<Ogre::Real>::lowest());

    bool hasVertex = false;
    for (const auto& sub : m_subMeshes) {
        for (const auto& v : sub.vertices) {
            minimum.makeFloor(v.position);
            maximum.makeCeil(v.position);
            hasVertex = true;
        }
    }

    if (!hasVertex)
        return Ogre::AxisAlignedBox();

    return Ogre::AxisAlignedBox(minimum, maximum);
}

bool EditableMesh::readVertexData(Ogre::VertexData* vertexData, std::vector<EditableVertex>& vertices)
{
    if (!vertexData || vertexData->vertexCount == 0)
        return false;

    vertices.resize(vertexData->vertexCount);

    auto* decl = vertexData->vertexDeclaration;
    auto* binding = vertexData->vertexBufferBinding;

    // Read positions
    const auto* posElem = decl->findElementBySemantic(Ogre::VES_POSITION);
    if (posElem) {
        auto vbuf = binding->getBuffer(posElem->getSource());
        auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (size_t j = 0; j < vertexData->vertexCount; ++j) {
            Ogre::Real* pReal;
            posElem->baseVertexPointerToElement(base + j * vbuf->getVertexSize(), &pReal);
            vertices[j].position = Ogre::Vector3(pReal[0], pReal[1], pReal[2]);
        }
        vbuf->unlock();
    }

    // Read normals
    const auto* normElem = decl->findElementBySemantic(Ogre::VES_NORMAL);
    if (normElem) {
        auto vbuf = binding->getBuffer(normElem->getSource());
        auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (size_t j = 0; j < vertexData->vertexCount; ++j) {
            Ogre::Real* pReal;
            normElem->baseVertexPointerToElement(base + j * vbuf->getVertexSize(), &pReal);
            vertices[j].normal = Ogre::Vector3(pReal[0], pReal[1], pReal[2]);
            vertices[j].hasNormal = true;
        }
        vbuf->unlock();
    }

    // Read UVs
    const auto* uvElem = decl->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES);
    if (uvElem) {
        auto vbuf = binding->getBuffer(uvElem->getSource());
        auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (size_t j = 0; j < vertexData->vertexCount; ++j) {
            Ogre::Real* pReal;
            uvElem->baseVertexPointerToElement(base + j * vbuf->getVertexSize(), &pReal);
            vertices[j].uv = Ogre::Vector2(pReal[0], pReal[1]);
            vertices[j].hasUV = true;
        }
        vbuf->unlock();
    }

    // Read vertex colors
    const auto* colElem = decl->findElementBySemantic(Ogre::VES_DIFFUSE);
    if (colElem) {
        auto vbuf = binding->getBuffer(colElem->getSource());
        auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (size_t j = 0; j < vertexData->vertexCount; ++j) {
            Ogre::RGBA* pRGBA;
            colElem->baseVertexPointerToElement(base + j * vbuf->getVertexSize(), &pRGBA);
            Ogre::ColourValue cv;
            cv.setAsRGBA(*pRGBA);
            vertices[j].color = cv;
            vertices[j].hasColor = true;
        }
        vbuf->unlock();
    }

    return true;
}

bool EditableMesh::readIndexData(Ogre::IndexData* indexData, std::vector<EditableTriangle>& triangles)
{
    if (!indexData || indexData->indexCount == 0)
        return false;

    // Only support triangle lists (3 indices per triangle)
    if (indexData->indexCount % 3 != 0)
        return false;

    size_t triCount = indexData->indexCount / 3;
    triangles.resize(triCount);

    auto ibuf = indexData->indexBuffer;
    bool use32bit = (ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT);

    auto* data = static_cast<unsigned char*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));

    for (size_t t = 0; t < triCount; ++t) {
        if (use32bit) {
            auto* idx = reinterpret_cast<uint32_t*>(data) + t * 3;
            triangles[t].indices[0] = idx[0];
            triangles[t].indices[1] = idx[1];
            triangles[t].indices[2] = idx[2];
        } else {
            auto* idx = reinterpret_cast<uint16_t*>(data) + t * 3;
            triangles[t].indices[0] = idx[0];
            triangles[t].indices[1] = idx[1];
            triangles[t].indices[2] = idx[2];
        }
    }

    ibuf->unlock();
    return true;
}

bool EditableMesh::writeVertexData(Ogre::VertexData* vertexData, const std::vector<EditableVertex>& vertices)
{
    if (!vertexData || vertices.empty())
        return false;

    auto* decl = vertexData->vertexDeclaration;
    auto* binding = vertexData->vertexBufferBinding;

    // Write positions
    const auto* posElem = decl->findElementBySemantic(Ogre::VES_POSITION);
    if (posElem) {
        unsigned short source = posElem->getSource();
        auto vbuf = binding->getBuffer(source);
        size_t bufSize = vbuf->getSizeInBytes();
        size_t vertexSize = vbuf->getVertexSize();

        // Upgrade static buffers to dynamic for immediate GPU visibility
        // (same pattern as SubMeshTransform::writePositions)
        if (vbuf->getUsage() & Ogre::HardwareBuffer::HBU_STATIC) {
            std::vector<unsigned char> oldData(bufSize);
            vbuf->readData(0, bufSize, oldData.data());

            auto newBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
                vertexSize, vertexData->vertexCount,
                Ogre::HardwareBuffer::HBU_DYNAMIC_WRITE_ONLY, true);
            newBuf->writeData(0, bufSize, oldData.data(), true);
            binding->setBinding(source, newBuf);
            vbuf = newBuf;
        }

        // Read full buffer, modify positions, write back with DISCARD
        std::vector<unsigned char> bufCopy(bufSize);
        vbuf->readData(0, bufSize, bufCopy.data());

        size_t count = std::min(vertices.size(), static_cast<size_t>(vertexData->vertexCount));
        for (size_t j = 0; j < count; ++j) {
            Ogre::Real* pReal;
            posElem->baseVertexPointerToElement(bufCopy.data() + j * vertexSize, &pReal);
            pReal[0] = vertices[j].position.x;
            pReal[1] = vertices[j].position.y;
            pReal[2] = vertices[j].position.z;
        }

        auto* dest = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        memcpy(dest, bufCopy.data(), bufSize);
        vbuf->unlock();
    }

    // Write normals
    const auto* normElem = decl->findElementBySemantic(Ogre::VES_NORMAL);
    if (normElem) {
        unsigned short source = normElem->getSource();
        auto vbuf = binding->getBuffer(source);
        size_t bufSize = vbuf->getSizeInBytes();
        size_t vertexSize = vbuf->getVertexSize();

        if (vbuf->getUsage() & Ogre::HardwareBuffer::HBU_STATIC) {
            std::vector<unsigned char> oldData(bufSize);
            vbuf->readData(0, bufSize, oldData.data());

            auto newBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
                vertexSize, vertexData->vertexCount,
                Ogre::HardwareBuffer::HBU_DYNAMIC_WRITE_ONLY, true);
            newBuf->writeData(0, bufSize, oldData.data(), true);
            binding->setBinding(source, newBuf);
            vbuf = newBuf;
        }

        std::vector<unsigned char> bufCopy(bufSize);
        vbuf->readData(0, bufSize, bufCopy.data());

        size_t count = std::min(vertices.size(), static_cast<size_t>(vertexData->vertexCount));
        for (size_t j = 0; j < count; ++j) {
            if (!vertices[j].hasNormal) continue;
            Ogre::Real* pReal;
            normElem->baseVertexPointerToElement(bufCopy.data() + j * vertexSize, &pReal);
            pReal[0] = vertices[j].normal.x;
            pReal[1] = vertices[j].normal.y;
            pReal[2] = vertices[j].normal.z;
        }

        auto* dest = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        memcpy(dest, bufCopy.data(), bufSize);
        vbuf->unlock();
    }

    // Write UVs
    const auto* uvElem = decl->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES);
    if (uvElem) {
        unsigned short source = uvElem->getSource();
        auto vbuf = binding->getBuffer(source);
        size_t bufSize = vbuf->getSizeInBytes();
        size_t vertexSize = vbuf->getVertexSize();

        if (vbuf->getUsage() & Ogre::HardwareBuffer::HBU_STATIC) {
            std::vector<unsigned char> oldData(bufSize);
            vbuf->readData(0, bufSize, oldData.data());

            auto newBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
                vertexSize, vertexData->vertexCount,
                Ogre::HardwareBuffer::HBU_DYNAMIC_WRITE_ONLY, true);
            newBuf->writeData(0, bufSize, oldData.data(), true);
            binding->setBinding(source, newBuf);
            vbuf = newBuf;
        }

        std::vector<unsigned char> bufCopy(bufSize);
        vbuf->readData(0, bufSize, bufCopy.data());

        size_t count = std::min(vertices.size(), static_cast<size_t>(vertexData->vertexCount));
        for (size_t j = 0; j < count; ++j) {
            if (!vertices[j].hasUV) continue;
            Ogre::Real* pReal;
            uvElem->baseVertexPointerToElement(bufCopy.data() + j * vertexSize, &pReal);
            pReal[0] = vertices[j].uv.x;
            pReal[1] = vertices[j].uv.y;
        }

        auto* dest = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        memcpy(dest, bufCopy.data(), bufSize);
        vbuf->unlock();
    }

    return true;
}
