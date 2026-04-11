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

#ifndef EDITABLEMESH_H
#define EDITABLEMESH_H

#include <Ogre.h>
#include <vector>
#include <string>

/**
 * @brief Vertex bone assignment data for skeletal meshes.
 */
struct EditableBoneAssignment {
    unsigned short boneIndex = 0;
    float weight = 0.0f;
};

/**
 * @brief A single editable vertex with all supported attributes.
 *
 * Stores the vertex data copied from Ogre's hardware buffers so it can be
 * freely manipulated without GPU buffer locks.
 */
struct EditableVertex {
    Ogre::Vector3 position = Ogre::Vector3::ZERO;
    Ogre::Vector3 normal = Ogre::Vector3::ZERO;
    Ogre::Vector2 uv = Ogre::Vector2::ZERO;
    Ogre::ColourValue color = Ogre::ColourValue::White;
    std::vector<EditableBoneAssignment> boneAssignments;

    bool hasNormal = false;
    bool hasUV = false;
    bool hasColor = false;
};

/**
 * @brief A single triangle referencing three vertices by index.
 */
struct EditableTriangle {
    unsigned int indices[3] = {0, 0, 0};
};

/**
 * @brief An editable copy of a single Ogre SubMesh.
 *
 * Contains all vertices, triangles, and the material name. Tracks whether
 * the original SubMesh used shared vertex data.
 */
struct EditableSubMesh {
    std::vector<EditableVertex> vertices;
    std::vector<EditableTriangle> triangles;
    std::string materialName;
    bool usesSharedVertices = false;
};

/**
 * @brief Indexed mesh representation for topology queries and editing.
 *
 * On Edit Mode enter, converts Ogre::Entity's SubMesh vertex/index buffers
 * into an internal editable format. On Edit Mode exit, writes changes back
 * to the Ogre buffers, recalculates normals and bounds.
 *
 * Supports: vertex positions, normals, UVs, vertex colors, bone weights.
 * Handles multi-submesh entities and shared vertex data.
 */
class EditableMesh
{
public:
    EditableMesh() = default;
    ~EditableMesh() = default;

    // Non-copyable, movable
    EditableMesh(const EditableMesh&) = delete;
    EditableMesh& operator=(const EditableMesh&) = delete;
    EditableMesh(EditableMesh&&) = default;
    EditableMesh& operator=(EditableMesh&&) = default;

    /**
     * @brief Load mesh data from an Ogre::Entity into the editable representation.
     *
     * Reads vertex buffers (positions, normals, UVs, colors) and index buffers
     * from each SubMesh. For submeshes using shared vertex data, duplicates the
     * shared vertices into each submesh's own vertex list, remapping indices.
     *
     * @param entity The source entity. Must not be null.
     * @return true on success, false if the entity has no mesh data.
     */
    bool loadFromEntity(Ogre::Entity* entity);

    /**
     * @brief Write edited mesh data back to the Ogre::Entity's buffers.
     *
     * Recalculates normals and mesh bounds, then writes vertex positions,
     * normals, and UVs back to the hardware buffers. Handles the static-to-dynamic
     * buffer upgrade for immediate GPU visibility.
     *
     * @param entity The target entity (should be the same entity used in loadFromEntity).
     * @return true on success, false on failure.
     */
    bool commitToEntity(Ogre::Entity* entity);

    /// @name Accessors
    /// @{
    const std::vector<EditableSubMesh>& subMeshes() const { return m_subMeshes; }
    std::vector<EditableSubMesh>& subMeshes() { return m_subMeshes; }
    size_t subMeshCount() const { return m_subMeshes.size(); }
    size_t totalVertexCount() const;
    size_t totalTriangleCount() const;
    /// @}

    /// @name Vertex manipulation
    /// @{
    void setVertexPosition(size_t subMeshIndex, size_t vertexIndex, const Ogre::Vector3& pos);
    Ogre::Vector3 getVertexPosition(size_t subMeshIndex, size_t vertexIndex) const;

    void setVertexNormal(size_t subMeshIndex, size_t vertexIndex, const Ogre::Vector3& normal);
    Ogre::Vector3 getVertexNormal(size_t subMeshIndex, size_t vertexIndex) const;

    void setVertexUV(size_t subMeshIndex, size_t vertexIndex, const Ogre::Vector2& uv);
    Ogre::Vector2 getVertexUV(size_t subMeshIndex, size_t vertexIndex) const;
    /// @}

    /**
     * @brief Recalculate all vertex normals from triangle geometry.
     *
     * Computes face normals for each triangle and averages them at each vertex
     * (area-weighted smooth normals).
     */
    void recalculateNormals();

    /**
     * @brief Calculate the axis-aligned bounding box of the entire mesh.
     */
    Ogre::AxisAlignedBox calculateBounds() const;

private:
    /**
     * @brief Read vertex attributes from an Ogre::VertexData into EditableVertex array.
     *
     * @param vertexData The Ogre vertex data to read from.
     * @param[out] vertices Output vector of editable vertices.
     * @return true on success.
     */
    bool readVertexData(Ogre::VertexData* vertexData, std::vector<EditableVertex>& vertices);

    /**
     * @brief Read index data from an Ogre::IndexData into EditableTriangle array.
     *
     * @param indexData The Ogre index data to read from.
     * @param[out] triangles Output vector of triangles.
     * @return true on success.
     */
    bool readIndexData(Ogre::IndexData* indexData, std::vector<EditableTriangle>& triangles);

    /**
     * @brief Write vertex data back to Ogre vertex buffers.
     *
     * Handles static-to-dynamic buffer upgrade for immediate GPU visibility.
     *
     * @param vertexData The Ogre vertex data to write to.
     * @param vertices The source editable vertices.
     * @return true on success.
     */
    bool writeVertexData(Ogre::VertexData* vertexData, const std::vector<EditableVertex>& vertices);

    std::vector<EditableSubMesh> m_subMeshes;
    Ogre::Entity* m_sourceEntity = nullptr;
};

#endif // EDITABLEMESH_H
