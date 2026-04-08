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

#ifndef SUBMESHTRANSFORM_H
#define SUBMESHTRANSFORM_H

#include <Ogre.h>

/**
 * @brief Static utility for transforming vertices belonging to a single SubMesh.
 *
 * Unlike MeshTransform which operates on all sub-meshes of a Mesh, this class
 * targets a specific SubMesh identified by index within the parent Mesh.
 *
 * Handles the special case where the sub-mesh uses shared vertex data: in that
 * case, the entire shared vertex buffer is transformed (since individual vertex
 * ownership cannot be determined without index buffer analysis).
 */
class SubMeshTransform
{
public:
    SubMeshTransform() = delete;

    /// Translate all vertices in the given sub-mesh by the delta vector.
    static void translateSubMesh(Ogre::Entity* entity, unsigned int subMeshIndex,
                                 const Ogre::Vector3& delta);

    /// Scale all vertices in the given sub-mesh around their centroid.
    static void scaleSubMesh(Ogre::Entity* entity, unsigned int subMeshIndex,
                             const Ogre::Vector3& scale);

    /// Rotate all vertices in the given sub-mesh around their centroid.
    static void rotateSubMesh(Ogre::Entity* entity, unsigned int subMeshIndex,
                              const Ogre::Quaternion& rotation);

    /// Compute the centroid of vertices belonging to the given sub-mesh.
    static Ogre::Vector3 getSubMeshCenter(Ogre::Entity* entity, unsigned int subMeshIndex);

    /// Read all vertex positions from a sub-mesh into a vector (for undo snapshots).
    static std::vector<Ogre::Vector3> readPositions(Ogre::Entity* entity, unsigned int subMeshIndex);

    /// Write vertex positions back to a sub-mesh from a vector (for undo restore).
    static void writePositions(Ogre::Entity* entity, unsigned int subMeshIndex,
                               const std::vector<Ogre::Vector3>& positions);

    /// Recalculate the parent mesh's bounding box after sub-mesh modification.
    static void recalculateMeshBounds(Ogre::Mesh* mesh);

private:
    /// Get the vertex data pointer for a given sub-mesh (handles shared vs own).
    static Ogre::VertexData* getVertexData(Ogre::Mesh* mesh, unsigned int subMeshIndex);
};

#endif // SUBMESHTRANSFORM_H
