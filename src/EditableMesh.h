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
 * @brief A polygonal face referencing N >= 3 vertices by index.
 *
 * Used for the n-gon (quad-aware) representation that complements the
 * legacy triangle-only path. While the editor is being migrated to
 * quads, both `EditableSubMesh::faces` and `EditableSubMesh::triangles`
 * coexist:
 *
 *  - When `faces` is empty, the submesh is in legacy triangle-only mode
 *    and `triangles` is the canonical storage.
 *  - When `faces` is non-empty, `faces` is canonical and `triangles` is
 *    a fan-triangulated mirror that downstream code (GPU upload, render,
 *    legacy topology ops) consumes. Helper utilities keep the two
 *    representations in sync.
 *
 * The fan-triangulation rule is `[v0, vi, vi+1]` for `i in [1, N-1)`,
 * matching what `HalfEdgeMesh::appendFace` and `fillSelection` already
 * produce. Convex polygons (the only case the importer is expected to
 * produce) survive that rule cleanly; concave n-gons need a future
 * ear-clip pass.
 */
struct EditableFace {
    std::vector<unsigned int> indices;

    /// @return true if this face has at least 3 vertices and no
    ///         consecutive duplicate index pair (naive sanity check).
    bool isValid() const
    {
        if (indices.size() < 3) return false;
        for (size_t i = 0; i < indices.size(); ++i) {
            if (indices[i] == indices[(i + 1) % indices.size()]) return false;
        }
        return true;
    }

    /// @return Number of vertices on this face (>= 3 when valid).
    size_t vertexCount() const { return indices.size(); }
};

/**
 * @brief An editable copy of a single Ogre SubMesh.
 *
 * Contains all vertices, triangles, faces, and the material name. Tracks
 * whether the original SubMesh used shared vertex data.
 *
 * The `faces` (n-gon) array is the canonical face storage when it is
 * non-empty; `triangles` then mirrors it as a fan-triangulation kept in
 * sync by `triangulateFaces()`. When `faces` is empty, `triangles` is
 * canonical and the submesh is in legacy triangle-only mode.
 */
struct EditableSubMesh {
    std::vector<EditableVertex> vertices;
    std::vector<EditableTriangle> triangles;
    std::vector<EditableFace> faces;
    std::string materialName;
    bool usesSharedVertices = false;
};

/**
 * @brief Fan-triangulate every face in `faces` into `triangles`.
 *
 * Triangulation rule: each face emits `n - 2` triangles fanned from
 * `indices[0]`, i.e. `(v0, v_i, v_{i+1})` for `i in [1, n-1)`. This
 * matches `HalfEdgeMesh::appendFace` and `fillSelection` so a face that
 * survives an HE round-trip recovers the same triangulation it started
 * with.
 *
 * Caller's responsibility: call this whenever `faces` is mutated to
 * keep `triangles` in sync. Code paths that don't yet understand n-gons
 * (GPU upload, legacy topology ops) consume `triangles`.
 *
 * Skips invalid faces (`vertexCount < 3`) silently. Replaces any
 * existing contents of `triangles`.
 *
 * @param sub The submesh whose `faces` will be triangulated. After
 *            return, `sub.triangles` contains the fan-triangulation.
 */
void triangulateFaces(EditableSubMesh& sub);

/**
 * @brief Build trivial single-triangle faces from existing `triangles`.
 *
 * Inverse of `triangulateFaces` — used when a legacy triangle-only
 * submesh needs to be promoted into the n-gon representation. After the
 * call, every triangle has a corresponding 3-index `EditableFace`. The
 * existing `triangles` array is preserved (the result still satisfies
 * the canonical-faces invariant: `triangles[i] == fan(faces[i])`).
 *
 * @param sub The submesh to promote. Existing contents of `sub.faces`
 *            are replaced.
 */
void promoteTrianglesToFaces(EditableSubMesh& sub);

/**
 * @brief Re-triangulate every submesh whose `faces` is non-empty.
 *
 * Convenience over `triangulateFaces(sub)` for a whole mesh: walks the
 * submesh array and resyncs each one whose canonical face storage has
 * changed. No-op for legacy triangle-only submeshes.
 *
 * Free function rather than a method on `EditableMesh` to keep the
 * class size below SonarQube's 35-method ceiling.
 *
 * @param subMeshes The submesh vector to sync (typically `mesh.subMeshes()`).
 */
void syncTriangulation(std::vector<EditableSubMesh>& subMeshes);

/**
 * @brief Total polygonal-face count across a submesh vector.
 *
 * For each submesh, returns `faces.size()` when n-gons are canonical,
 * else `triangles.size()`. The `EditableMesh::totalTriangleCount()`
 * counterpart still reports the fan-triangulation count regardless of
 * representation.
 *
 * Free function for the same reason as `syncTriangulation` — keeps
 * `EditableMesh` below the class-method limit.
 */
size_t totalFaceCount(const std::vector<EditableSubMesh>& subMeshes);

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
     * @brief Read vertex/index data directly from an Ogre::Mesh (no Entity).
     *
     * Same extraction logic as loadFromEntity, but sourced from a MeshPtr.
     * Useful when building editable data before an Entity exists (e.g.,
     * post-processing procedural primitives before they become a scene node).
     *
     * @param mesh The source mesh. Must not be null.
     * @return true on success.
     */
    bool loadFromMesh(const Ogre::MeshPtr& mesh);

    /**
     * @brief Re-import an asset directly via Assimp, preserving n-gons.
     *
     * Spins up a fresh `Assimp::Importer` and re-reads the source file
     * with the triangulation post-process disabled, so source quads
     * survive into `EditableSubMesh::faces` instead of being collapsed
     * to triangles. The associated Ogre::Mesh in the live scene
     * continues to use the triangulated index buffer for rendering;
     * this path only feeds the editing-time representation.
     *
     * Skips skeleton, animation, material, and tangent processing —
     * Edit Mode operates on positions, normals, UVs, vertex colors,
     * and bone weights only. Materials are taken from the existing
     * Ogre::Mesh by submesh order in `loadFromEntity` / similar paths.
     *
     * Vertices are read out of `aiMesh::mVertices` / `mNormals` /
     * `mTextureCoords[0]` / `mColors[0]` / `mBones[].mWeights`. Faces
     * are read from `aiMesh::mFaces` and stored in
     * `EditableSubMesh::faces` (n-gon canonical), with `triangles`
     * fan-triangulated to maintain the chunk-1 invariant.
     *
     * Cost: a second Assimp parse of the same file. Order of magnitude
     * 10–100ms for typical assets; acceptable as a one-time cost on
     * entering Edit Mode. Big assets (50MB+ FBX) may be noticeable.
     *
     * @param path The path the asset was originally imported from.
     *             Should be the value cached on `Ogre::Mesh` via
     *             `getUserObjectBindings().getUserAny("qtme.source_path")`.
     * @param convertToLeftHanded If true, applies `aiProcess_ConvertToLeftHanded`
     *             so the resulting positions / UVs match Ogre's left-handed
     *             coordinate system. MUST match the flag the original
     *             import used; otherwise the editable representation
     *             will be mirrored (X flipped) relative to the rendered
     *             mesh and the vertex/edge/face overlays will draw
     *             on the wrong side. The original importer caches its
     *             choice at `getUserAny("qtme.source_convert_lh")`.
     * @return true on success; false if the file is missing, can't be
     *         parsed, or contains no mesh data.
     */
    bool loadFromAssimpFile(const std::string& path,
                            bool convertToLeftHanded = true);

    /**
     * @brief Merge vertices at (approximately) coincident positions within
     *        each submesh.
     *
     * For each submesh, groups vertices that share a position within
     * `tolerance` (squared distance) and replaces them with a single
     * representative. Triangles are rewritten to use the new indices.
     * Vertex attributes (normal, UV, bone weights, tangent) are taken from
     * the first-seen vertex in each group; if you care about preserving
     * those, call this before any normal/tangent computation.
     *
     * @param tolerance Squared distance threshold. Defaults to 1e-8.
     */
    void weldByPosition(float tolerance = 1e-8f);

    /**
     * @brief Collapse all submeshes into the first and weld across positions.
     *
     * Intended for newly-generated primitives where every face uses the same
     * material and the "separate submesh per face" topology blocks topology
     * edits (e.g., beveling the shared edge between two cube faces). After
     * this call, the mesh has exactly one submesh with welded vertices.
     *
     * @param tolerance Squared distance threshold for welding.
     */
    void collapseToSingleSubmeshAndWeld(float tolerance = 1e-8f);

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
