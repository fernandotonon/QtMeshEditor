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

#include "MeshTransform.h"

#include "Manager.h"
#include "SkeletonTransform.h"

namespace {

Ogre::Quaternion buildRotationQuat(const Ogre::Vector3 &rotate)
{
    if(rotate.x != 0)
        return {Ogre::Degree(rotate.x), Ogre::Vector3::UNIT_Y};
    if(rotate.y != 0)
        return {Ogre::Degree(rotate.y), Ogre::Vector3::UNIT_Z};
    if(rotate.z != 0)
        return {Ogre::Degree(rotate.z), Ogre::Vector3::UNIT_X};
    return Ogre::Quaternion::IDENTITY;
}

// Iterates all unique vertex data blocks in a mesh, calling transformFn(pos) for each
// vertex position. Writes back the transformed position and updates mesh bounds.
template<typename TransformFn>
void transformPositions(Ogre::Mesh *mesh, TransformFn transformFn)
{
    bool addedShared = false;
    auto minimum = mesh->getBounds().getMaximum();
    auto maximum = mesh->getBounds().getMinimum();

    for(int i = 0; i < mesh->getNumSubMeshes(); i++)
    {
        auto *submesh = mesh->getSubMesh(i);
        if(submesh->useSharedVertices && addedShared) continue;
        if(submesh->useSharedVertices) addedShared = true;

        auto *vertexData = submesh->useSharedVertices ? mesh->sharedVertexData : submesh->vertexData;
        const auto *posElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        auto vbuf = vertexData->vertexBufferBinding->getBuffer(posElem->getSource());
        auto *vertex = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_NORMAL));

        for(size_t j = 0; j < vertexData->vertexCount; ++j, vertex += vbuf->getVertexSize())
        {
            Ogre::Real *pReal;
            posElem->baseVertexPointerToElement(vertex, &pReal);

            Ogre::Vector3 result = transformFn(Ogre::Vector3(pReal[0], pReal[1], pReal[2]));
            pReal[0] = result.x;
            pReal[1] = result.y;
            pReal[2] = result.z;

            minimum.makeFloor(result);
            maximum.makeCeil(result);
        }
        vbuf->unlock();
    }
    mesh->_setBounds(Ogre::AxisAlignedBox(minimum, maximum), false);
}

void rotateNormals(Ogre::Mesh *mesh, const Ogre::Quaternion &quat)
{
    bool addedShared = false;

    for(int i = 0; i < mesh->getNumSubMeshes(); i++)
    {
        auto *submesh = mesh->getSubMesh(i);
        if(submesh->useSharedVertices && addedShared) continue;
        if(submesh->useSharedVertices) addedShared = true;

        auto *vertexData = submesh->useSharedVertices ? mesh->sharedVertexData : submesh->vertexData;
        const auto *normElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL);
        if(!normElem) continue;

        // Only write float3 normals; packed formats (e.g. VET_UBYTE4_NORM) have
        // different storage layouts and writing 3 floats would corrupt the buffer.
        if(normElem->getType() != Ogre::VET_FLOAT3) continue;

        auto nbuf = vertexData->vertexBufferBinding->getBuffer(normElem->getSource());
        auto *nVertex = static_cast<unsigned char*>(nbuf->lock(Ogre::HardwareBuffer::HBL_NORMAL));

        for(size_t j = 0; j < vertexData->vertexCount; ++j, nVertex += nbuf->getVertexSize())
        {
            Ogre::Real *nReal;
            normElem->baseVertexPointerToElement(nVertex, &nReal);

            Ogre::Vector3 rnorm = quat * Ogre::Vector3(nReal[0], nReal[1], nReal[2]);
            nReal[0] = rnorm.x;
            nReal[1] = rnorm.y;
            nReal[2] = rnorm.z;
        }
        nbuf->unlock();
    }
}

} // anonymous namespace

void MeshTransform::scaleMesh(const Ogre::Entity *_ent, const Ogre::Vector3 &_scale)
{
    scaleMesh(_ent->getMesh().get(), _scale);
    SkeletonTransform::scaleSkeleton(_ent, _scale);
}

void MeshTransform::scaleMesh(Ogre::Mesh *_mesh, const Ogre::Vector3 &_scale)
{
    transformPositions(_mesh, [&](const Ogre::Vector3 &pos) {
        return pos * _scale;
    });
}

void MeshTransform::translateMesh(const Ogre::Entity *_ent, const Ogre::Vector3 &_translate)
{
    transformPositions(_ent->getMesh().get(), [&](const Ogre::Vector3 &pos) {
        return pos + _translate;
    });
    SkeletonTransform::translateSkeleton(_ent, _translate);
}

void MeshTransform::rotateMesh(const Ogre::Entity *_ent, const Ogre::Vector3 &_rotate)
{
    auto *mesh = _ent->getMesh().get();
    auto center = mesh->getBounds().getCenter();
    auto quat = buildRotationQuat(_rotate);

    transformPositions(mesh, [&](const Ogre::Vector3 &pos) {
        return quat * (pos - center) + center;
    });

    rotateNormals(mesh, quat);
    SkeletonTransform::rotateSkeleton(_ent, _rotate);
}
