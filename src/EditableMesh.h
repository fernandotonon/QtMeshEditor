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
    Ogre::Vector4 tangent = Ogre::Vector4::ZERO; // w = parity
    std::vector<EditableBoneAssignment> boneAssignments;

    bool hasNormal = false;
    bool hasUV = false;
    bool hasColor = false;
    bool hasTangent = false;
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
     * Only works when vertex/triangle counts haven't changed. Use
     * resizeEntityBuffers() for topology modifications.
     *
     * @param entity The target entity (should be the same entity used in loadFromEntity).
     * @return true on success, false on failure.
     */
    bool commitToEntity(Ogre::Entity* entity);

    /**
     * @brief Create a new Ogre::Mesh from the current editable data.
     *
     * Creates a fresh Mesh resource with a unique name. Use this when you
     * need a detached copy of the mesh; the live-edit path uses
     * resizeEntityBuffers() instead to keep Entity SubEntity caches valid.
     *
     * @param baseName Base name for the new mesh (a suffix is appended for uniqueness).
     * @return The new MeshPtr, or null on failure.
     */
    Ogre::MeshPtr createNewMesh(const std::string& baseName);

    /**
     * @brief Resize and update existing Ogre::Mesh buffers in-place.
     *
     * Replaces each SubMesh's vertex and index buffers with new buffers
     * containing the current EditableMesh data, without destroying the
     * SubMesh objects. The Entity's SubEntity list remains valid because
     * SubMesh pointers don't change.
     *
     * Requires that the submesh count matches the current Ogre mesh.
     * For skeletal meshes, re-registers bone assignments and calls
     * Mesh::_compileBoneAssignments() so skinning continues to work.
     * Writes position / normal / uv / tangent (when available) from the
     * EditableVertex attributes; bone indices/weights are populated by
     * _compileBoneAssignments().
     *
     * @param entity The target entity.
     * @return true on success, false on failure.
     */
    bool resizeEntityBuffers(Ogre::Entity* entity);

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
     * @brief Recalculate all vertex normals using flat shading.
     *
     * Each vertex gets the face normal of its triangle (no averaging).
     * Note: this means shared vertices get the normal of the last triangle processed.
     */
    void recalculateNormalsFlat();

    /// Set the normals mode (true=flat, false=smooth). Used by commitToEntity().
    void setFlatNormals(bool flat) { m_flatNormals = flat; }
    bool isFlatNormals() const { return m_flatNormals; }

    /**
     * @brief Count the number of degenerate triangles (area below epsilon).
     */
    int countDegenerateTriangles(float epsilon = 1e-6f) const;

    /**
     * @brief Remove degenerate triangles (area below epsilon).
     * @return Number of triangles removed.
     */
    int removeDegenerateTriangles(float epsilon = 1e-6f);

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

    /**
     * @brief Build vertex/index hardware buffers for a single submesh.
     *
     * Creates a fresh Ogre::VertexData with a position/normal/uv/tangent
     * declaration (only including attributes the EditableSubMesh has) and
     * fills the interleaved buffer. Replaces subMesh->vertexData.
     *
     * Used by both rebuildEntityMesh-style code paths to avoid duplication.
     *
     * @param subMesh The Ogre SubMesh to populate.
     * @param editSub The source editable submesh.
     */
    static void buildSubMeshBuffers(Ogre::SubMesh* subMesh,
                                    const EditableSubMesh& editSub);

    std::vector<EditableSubMesh> m_subMeshes;
    Ogre::Entity* m_sourceEntity = nullptr;
    bool m_flatNormals = false; // true = flat normals, false = smooth
};

#endif // EDITABLEMESH_H
