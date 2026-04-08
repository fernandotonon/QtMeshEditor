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

#include "SubMeshTransform.h"
#include <limits>

Ogre::VertexData* SubMeshTransform::getVertexData(Ogre::Mesh* mesh, unsigned int subMeshIndex)
{
    if (subMeshIndex >= static_cast<unsigned int>(mesh->getNumSubMeshes()))
        return nullptr;

    Ogre::SubMesh* subMesh = mesh->getSubMesh(subMeshIndex);
    if (subMesh->useSharedVertices)
        return mesh->sharedVertexData;
    return subMesh->vertexData;
}

Ogre::Vector3 SubMeshTransform::getSubMeshCenter(Ogre::Entity* entity, unsigned int subMeshIndex)
{
    if (!entity) return Ogre::Vector3::ZERO;

    Ogre::Mesh* mesh = entity->getMesh().get();
    Ogre::VertexData* vertexData = getVertexData(mesh, subMeshIndex);
    if (!vertexData || vertexData->vertexCount == 0)
        return Ogre::Vector3::ZERO;

    const auto* posElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (!posElem) return Ogre::Vector3::ZERO;

    auto vbuf = vertexData->vertexBufferBinding->getBuffer(posElem->getSource());
    auto* vertex = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));

    Ogre::Vector3 center = Ogre::Vector3::ZERO;
    for (size_t j = 0; j < vertexData->vertexCount; ++j, vertex += vbuf->getVertexSize())
    {
        Ogre::Real* pReal;
        posElem->baseVertexPointerToElement(vertex, &pReal);
        center += Ogre::Vector3(pReal[0], pReal[1], pReal[2]);
    }
    vbuf->unlock();

    center /= static_cast<Ogre::Real>(vertexData->vertexCount);
    return center;
}

std::vector<Ogre::Vector3> SubMeshTransform::readPositions(Ogre::Entity* entity, unsigned int subMeshIndex)
{
    std::vector<Ogre::Vector3> positions;
    if (!entity) return positions;

    Ogre::Mesh* mesh = entity->getMesh().get();
    Ogre::VertexData* vertexData = getVertexData(mesh, subMeshIndex);
    if (!vertexData) return positions;

    const auto* posElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (!posElem) return positions;

    auto vbuf = vertexData->vertexBufferBinding->getBuffer(posElem->getSource());
    auto* vertex = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));

    positions.reserve(vertexData->vertexCount);
    for (size_t j = 0; j < vertexData->vertexCount; ++j, vertex += vbuf->getVertexSize())
    {
        Ogre::Real* pReal;
        posElem->baseVertexPointerToElement(vertex, &pReal);
        positions.emplace_back(pReal[0], pReal[1], pReal[2]);
    }
    vbuf->unlock();

    return positions;
}

void SubMeshTransform::writePositions(Ogre::Entity* entity, unsigned int subMeshIndex,
                                       const std::vector<Ogre::Vector3>& positions)
{
    if (!entity) return;

    Ogre::Mesh* mesh = entity->getMesh().get();
    Ogre::VertexData* vertexData = getVertexData(mesh, subMeshIndex);
    if (!vertexData) return;

    const auto* posElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (!posElem) return;

    auto vbuf = vertexData->vertexBufferBinding->getBuffer(posElem->getSource());
    auto* vertex = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_NORMAL));

    size_t count = std::min(positions.size(), static_cast<size_t>(vertexData->vertexCount));
    for (size_t j = 0; j < count; ++j, vertex += vbuf->getVertexSize())
    {
        Ogre::Real* pReal;
        posElem->baseVertexPointerToElement(vertex, &pReal);
        pReal[0] = positions[j].x;
        pReal[1] = positions[j].y;
        pReal[2] = positions[j].z;
    }
    vbuf->unlock();

    recalculateMeshBounds(mesh);
    entity->getParentSceneNode()->needUpdate(true);
}

void SubMeshTransform::translateSubMesh(Ogre::Entity* entity, unsigned int subMeshIndex,
                                         const Ogre::Vector3& delta)
{
    if (!entity) return;

    Ogre::Mesh* mesh = entity->getMesh().get();
    Ogre::VertexData* vertexData = getVertexData(mesh, subMeshIndex);
    if (!vertexData) return;

    const auto* posElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (!posElem) return;

    auto vbuf = vertexData->vertexBufferBinding->getBuffer(posElem->getSource());
    auto* vertex = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_NORMAL));

    for (size_t j = 0; j < vertexData->vertexCount; ++j, vertex += vbuf->getVertexSize())
    {
        Ogre::Real* pReal;
        posElem->baseVertexPointerToElement(vertex, &pReal);
        pReal[0] += delta.x;
        pReal[1] += delta.y;
        pReal[2] += delta.z;
    }
    vbuf->unlock();

    recalculateMeshBounds(mesh);
    entity->getParentSceneNode()->needUpdate(true);
}

void SubMeshTransform::scaleSubMesh(Ogre::Entity* entity, unsigned int subMeshIndex,
                                     const Ogre::Vector3& scale)
{
    if (!entity) return;

    Ogre::Vector3 center = getSubMeshCenter(entity, subMeshIndex);

    Ogre::Mesh* mesh = entity->getMesh().get();
    Ogre::VertexData* vertexData = getVertexData(mesh, subMeshIndex);
    if (!vertexData) return;

    const auto* posElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (!posElem) return;

    auto vbuf = vertexData->vertexBufferBinding->getBuffer(posElem->getSource());
    auto* vertex = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_NORMAL));

    for (size_t j = 0; j < vertexData->vertexCount; ++j, vertex += vbuf->getVertexSize())
    {
        Ogre::Real* pReal;
        posElem->baseVertexPointerToElement(vertex, &pReal);

        Ogre::Vector3 pos(pReal[0], pReal[1], pReal[2]);
        Ogre::Vector3 offset = pos - center;
        Ogre::Vector3 result = center + offset * scale;

        pReal[0] = result.x;
        pReal[1] = result.y;
        pReal[2] = result.z;
    }
    vbuf->unlock();

    recalculateMeshBounds(mesh);
    entity->getParentSceneNode()->needUpdate(true);
}

void SubMeshTransform::rotateSubMesh(Ogre::Entity* entity, unsigned int subMeshIndex,
                                      const Ogre::Quaternion& rotation)
{
    if (!entity) return;

    Ogre::Vector3 center = getSubMeshCenter(entity, subMeshIndex);

    Ogre::Mesh* mesh = entity->getMesh().get();
    Ogre::VertexData* vertexData = getVertexData(mesh, subMeshIndex);
    if (!vertexData) return;

    const auto* posElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (!posElem) return;

    auto vbuf = vertexData->vertexBufferBinding->getBuffer(posElem->getSource());
    auto* vertex = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_NORMAL));

    for (size_t j = 0; j < vertexData->vertexCount; ++j, vertex += vbuf->getVertexSize())
    {
        Ogre::Real* pReal;
        posElem->baseVertexPointerToElement(vertex, &pReal);

        Ogre::Vector3 pos(pReal[0], pReal[1], pReal[2]);
        Ogre::Vector3 result = rotation * (pos - center) + center;

        pReal[0] = result.x;
        pReal[1] = result.y;
        pReal[2] = result.z;
    }
    vbuf->unlock();

    // Also rotate normals for this sub-mesh
    const auto* normElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL);
    if (normElem && normElem->getType() == Ogre::VET_FLOAT3)
    {
        auto nbuf = vertexData->vertexBufferBinding->getBuffer(normElem->getSource());
        auto* nVertex = static_cast<unsigned char*>(nbuf->lock(Ogre::HardwareBuffer::HBL_NORMAL));

        for (size_t j = 0; j < vertexData->vertexCount; ++j, nVertex += nbuf->getVertexSize())
        {
            Ogre::Real* nReal;
            normElem->baseVertexPointerToElement(nVertex, &nReal);

            Ogre::Vector3 norm(nReal[0], nReal[1], nReal[2]);
            Ogre::Vector3 rnorm = rotation * norm;
            nReal[0] = rnorm.x;
            nReal[1] = rnorm.y;
            nReal[2] = rnorm.z;
        }
        nbuf->unlock();
    }

    recalculateMeshBounds(mesh);
    entity->getParentSceneNode()->needUpdate(true);
}

void SubMeshTransform::recalculateMeshBounds(Ogre::Mesh* mesh)
{
    Ogre::Vector3 minimum(std::numeric_limits<Ogre::Real>::max());
    Ogre::Vector3 maximum(std::numeric_limits<Ogre::Real>::lowest());
    bool addedShared = false;

    for (int i = 0; i < mesh->getNumSubMeshes(); ++i)
    {
        Ogre::SubMesh* subMesh = mesh->getSubMesh(i);
        if (subMesh->useSharedVertices && addedShared) continue;
        if (subMesh->useSharedVertices) addedShared = true;

        Ogre::VertexData* vertexData = subMesh->useSharedVertices
            ? mesh->sharedVertexData : subMesh->vertexData;

        const auto* posElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        if (!posElem) continue;

        auto vbuf = vertexData->vertexBufferBinding->getBuffer(posElem->getSource());
        auto* vertex = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));

        for (size_t j = 0; j < vertexData->vertexCount; ++j, vertex += vbuf->getVertexSize())
        {
            Ogre::Real* pReal;
            posElem->baseVertexPointerToElement(vertex, &pReal);
            Ogre::Vector3 pos(pReal[0], pReal[1], pReal[2]);
            minimum.makeFloor(pos);
            maximum.makeCeil(pos);
        }
        vbuf->unlock();
    }

    mesh->_setBounds(Ogre::AxisAlignedBox(minimum, maximum), false);
}
