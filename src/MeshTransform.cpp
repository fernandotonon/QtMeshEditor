/*/////////////////////////////////////////////////////////////////////////////////
/// A QtMeshEditor file
///
/// Copyright (c) HogPog Team (www.hogpog.com.br)
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

#include "MeshTransform.h"

#include "Manager.h"
#include "SkeletonTransform.h"

MeshTransform::MeshTransform()
{
}

void MeshTransform::scaleMesh(const Ogre::Entity *_ent, const Ogre::Vector3 &_scale)
{
    Ogre::Mesh* mesh = _ent->getMesh().getPointer();
    scaleMesh(mesh,_scale);

    //Process Skeleton
    SkeletonTransform::scaleSkeleton(_ent,_scale);
}

void MeshTransform::scaleMesh(Ogre::Mesh *_mesh, const Ogre::Vector3 &_scale)
{
    bool added_shared = false;

    Ogre::Vector3 Minimum=_mesh->getBounds().getMaximum();
    Ogre::Vector3 Maximum=_mesh->getBounds().getMinimum();

    // Process Mesh
    // Run through the submeshes, modifying the data
    for(int i = 0;i < _mesh->getNumSubMeshes();i++)
    {
        Ogre::SubMesh* submesh = _mesh->getSubMesh(i);

        Ogre::VertexData* vertex_data = submesh->useSharedVertices ? _mesh->sharedVertexData : submesh->vertexData;
        if((!submesh->useSharedVertices)||(submesh->useSharedVertices && !added_shared))
        {
            if(submesh->useSharedVertices)
            {
                added_shared = true;
            }

            const Ogre::VertexElement* posElem = vertex_data->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
            Ogre::HardwareVertexBufferSharedPtr vbuf = vertex_data->vertexBufferBinding->getBuffer(posElem->getSource());
            // lock buffer for read and write access
            unsigned char* vertex = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_NORMAL));
            Ogre::Real* pReal;

            for(size_t j = 0; j < vertex_data->vertexCount; ++j, vertex += vbuf->getVertexSize())
            {
                posElem->baseVertexPointerToElement(vertex, &pReal);

                // modify x coord
                (*pReal) *= _scale.x;
                ++pReal;

                // modify y coord
                (*pReal) *= _scale.y;
                ++pReal;

                // modify z coord
                (*pReal) *= _scale.z;
                pReal-=2;

                Minimum.x=Minimum.x<(*pReal)?Minimum.x:(*pReal);
                Maximum.x=Maximum.x>(*pReal)?Maximum.x:(*pReal);
                ++pReal;
                Minimum.y=Minimum.y<(*pReal)?Minimum.y:(*pReal);
                Maximum.y=Maximum.y>(*pReal)?Maximum.y:(*pReal);
                ++pReal;
                Minimum.z=Minimum.z<(*pReal)?Minimum.z:(*pReal);
                Maximum.z=Maximum.z>(*pReal)?Maximum.z:(*pReal);
            }
            vbuf->unlock();
        }
    }
    _mesh->_setBounds(Ogre::AxisAlignedBox(Minimum,Maximum),false);
}

void MeshTransform::translateMesh(const Ogre::Entity *_ent, const Ogre::Vector3 &_translate)
{
    bool added_shared = false;
    Ogre::Mesh* mesh = _ent->getMesh().getPointer();
    Ogre::Vector3 Minimum=mesh->getBounds().getMaximum();
    Ogre::Vector3 Maximum=mesh->getBounds().getMinimum();

    // Run through the submeshes, modifying the data
    for(int i = 0;i < mesh->getNumSubMeshes();i++)
    {
        Ogre::SubMesh* submesh = mesh->getSubMesh(i);

        Ogre::VertexData* vertex_data = submesh->useSharedVertices ? mesh->sharedVertexData : submesh->vertexData;
        if((!submesh->useSharedVertices)||(submesh->useSharedVertices && !added_shared))
        {
            if(submesh->useSharedVertices)
            {
                added_shared = true;
            }

            const Ogre::VertexElement* posElem = vertex_data->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
            Ogre::HardwareVertexBufferSharedPtr vbuf = vertex_data->vertexBufferBinding->getBuffer(posElem->getSource());
            // lock buffer for read and write access
            unsigned char* vertex = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_NORMAL));
            Ogre::Real* pReal;

            for(size_t j = 0; j < vertex_data->vertexCount; ++j, vertex += vbuf->getVertexSize())
            {
                posElem->baseVertexPointerToElement(vertex, &pReal);

                // modify x coord
                (*pReal) += _translate.x;
                ++pReal;

                // modify y coord
                (*pReal) += _translate.y;
                ++pReal;

                // modify z coord
                (*pReal) += _translate.z;
                pReal-=2;

                Minimum.x=Minimum.x<(*pReal)?Minimum.x:(*pReal);
                Maximum.x=Maximum.x>(*pReal)?Maximum.x:(*pReal);
                ++pReal;
                Minimum.y=Minimum.y<(*pReal)?Minimum.y:(*pReal);
                Maximum.y=Maximum.y>(*pReal)?Maximum.y:(*pReal);
                ++pReal;
                Minimum.z=Minimum.z<(*pReal)?Minimum.z:(*pReal);
                Maximum.z=Maximum.z>(*pReal)?Maximum.z:(*pReal);
            }
            vbuf->unlock();
        }
    }
    mesh->_setBounds(Ogre::AxisAlignedBox(Minimum,Maximum),false);

    //Process Skeleton
    SkeletonTransform::translateSkeleton(_ent,_translate);
}

void MeshTransform::rotateMesh(const Ogre::Entity *_ent, const Ogre::Vector3 &_rotate)
{
    bool added_shared = false;
    Ogre::Mesh* mesh = _ent->getMesh().getPointer();
    Ogre::Vector3 Minimum = mesh->getBounds().getMaximum();
    Ogre::Vector3 Maximum = mesh->getBounds().getMinimum();
    Ogre::Vector3 Center = mesh->getBounds().getCenter();

    // Run through the submeshes, modifying the data
    for(int i = 0;i < mesh->getNumSubMeshes();i++)
    {
        Ogre::SubMesh* submesh = mesh->getSubMesh(i);

        Ogre::VertexData* vertex_data = submesh->useSharedVertices ? mesh->sharedVertexData : submesh->vertexData;
        if((!submesh->useSharedVertices)||(submesh->useSharedVertices && !added_shared))
        {
            if(submesh->useSharedVertices)
            {
                added_shared = true;
            }

            const Ogre::VertexElement* posElem = vertex_data->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
            Ogre::HardwareVertexBufferSharedPtr vbuf = vertex_data->vertexBufferBinding->getBuffer(posElem->getSource());
            // lock buffer for read and write access
            unsigned char* vertex = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_NORMAL));
            Ogre::Real* pReal;

            for(size_t j = 0; j < vertex_data->vertexCount; ++j, vertex += vbuf->getVertexSize())
            {
                posElem->baseVertexPointerToElement(vertex, &pReal);

                Ogre::Vector3 pos;
                Ogre::Vector3 rpos;
                pos.x =(*pReal);
                ++pReal;
                pos.y =(*pReal);
                ++pReal;
                pos.z =(*pReal);
                pReal-=2;

                if(_rotate.x!=0)
                    rpos = (Ogre::Quaternion(Ogre::Degree(_rotate.x), Ogre::Vector3::UNIT_Y)*(pos-Center));
                else if(_rotate.y!=0)
                    rpos = (Ogre::Quaternion(Ogre::Degree(_rotate.y), Ogre::Vector3::UNIT_Z)*(pos-Center));
                else if(_rotate.z!=0)
                    rpos = (Ogre::Quaternion(Ogre::Degree(_rotate.z), Ogre::Vector3::UNIT_X)*(pos-Center));
                else
                    rpos = pos-Center;

                rpos += Center;

                // modify x coord
                (*pReal) = rpos.x;
                ++pReal;

                // modify y coord
                (*pReal) = rpos.y;
                ++pReal;

                // modify z coord
                (*pReal) = rpos.z;

                Minimum.x=Minimum.x<rpos.x?Minimum.x:rpos.x;
                Minimum.y=Minimum.y<rpos.y?Minimum.y:rpos.y;
                Minimum.z=Minimum.z<rpos.z?Minimum.z:rpos.z;
                Maximum.x=Maximum.x>rpos.x?Maximum.x:rpos.x;
                Maximum.y=Maximum.y>rpos.y?Maximum.y:rpos.y;
                Maximum.z=Maximum.z>rpos.z?Maximum.z:rpos.z;
            }
            vbuf->unlock();
        }
    }
    mesh->_setBounds(Ogre::AxisAlignedBox(Minimum,Maximum),false);

    //Process Skeleton
    SkeletonTransform::rotateSkeleton(_ent,_rotate);
}
