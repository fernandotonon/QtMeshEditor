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

#ifndef HALFEDGEMESH_H
#define HALFEDGEMESH_H

#include <OgreVector.h>
#include <OgreColourValue.h>
#include <vector>
#include <unordered_map>
#include <utility>
#include <string>

struct EditableSubMesh;
class EditableMesh;

/**
 * @brief A single half-edge in the half-edge mesh.
 *
 * Each half-edge points from one vertex to another. It stores pointers
 * (as indices) to: the vertex it points TO, its twin (opposite) half-edge,
 * the next half-edge around the same face, and the face it belongs to.
 *
 * A half-edge with face == -1 is a boundary half-edge.
 */
struct HalfEdge {
    int vertex = -1;   ///< Index of the vertex this half-edge points TO
    int twin = -1;     ///< Index of the twin (opposite) half-edge (-1 if boundary)
    int next = -1;     ///< Index of the next half-edge around the same face
    int prev = -1;     ///< Index of the previous half-edge around the same face
    int face = -1;     ///< Index of the face this half-edge belongs to (-1 = boundary)
    int edge = -1;     ///< Index of the logical edge this half-edge belongs to
};

/**
 * @brief A vertex in the half-edge mesh.
 *
 * Stores position, normal, UV, color, bone assignments, and a pointer
 * to one outgoing half-edge (for traversal).
 */
struct HEVertex {
    Ogre::Vector3 position = Ogre::Vector3::ZERO;
    Ogre::Vector3 normal = Ogre::Vector3::ZERO;
    Ogre::Vector2 uv = Ogre::Vector2::ZERO;
    Ogre::ColourValue color = Ogre::ColourValue::White;
    Ogre::Vector4 tangent = Ogre::Vector4::ZERO; // w = parity

    bool hasNormal = false;
    bool hasUV = false;
    bool hasColor = false;
    bool hasTangent = false;

    /// Bone assignments stored as (boneIndex, weight) pairs.
    std::vector<std::pair<unsigned short, float>> boneAssignments;

    int halfEdge = -1;  ///< Index of one outgoing half-edge from this vertex
};

/**
 * @brief A face (triangle) in the half-edge mesh.
 *
 * Stores a pointer to one of the half-edges bounding this face.
 * The submesh index tracks which material group this face belongs to.
 */
struct HEFace {
    int halfEdge = -1;   ///< Index of one half-edge bounding this face
    int subMeshIndex = 0; ///< Which submesh (material group) this face belongs to
};

/**
 * @brief A logical edge in the half-edge mesh.
 *
 * An edge connects two vertices. It stores a pointer to one of its
 * two half-edges. The other can be found via halfEdge->twin.
 */
struct HEEdge {
    int halfEdge = -1;  ///< Index of one of the two half-edges for this edge
};

/**
 * @brief Half-edge mesh data structure for topology queries and editing.
 *
 * Converts from EditableMesh's indexed triangle representation to a half-edge
 * structure that supports efficient adjacency queries. Converts back to
 * EditableMesh when done.
 *
 * Key operations:
 * - Adjacency: faces around vertex, edges around vertex, vertices around vertex
 * - Boundary detection: is a vertex/edge on the boundary?
 * - Topology modification: vertex split, edge collapse, face subdivision
 *
 * All topology elements are stored in flat vectors indexed by integer IDs.
 * Deleted elements are marked with a flag rather than removed, to avoid
 * invalidating indices during operations.
 */
class HalfEdgeMesh
{
public:
    HalfEdgeMesh() = default;
    ~HalfEdgeMesh() = default;

    // Non-copyable, movable
    HalfEdgeMesh(const HalfEdgeMesh&) = delete;
    HalfEdgeMesh& operator=(const HalfEdgeMesh&) = delete;
    HalfEdgeMesh(HalfEdgeMesh&&) = default;
    HalfEdgeMesh& operator=(HalfEdgeMesh&&) = default;

    /**
     * @brief Build the half-edge structure from an EditableMesh.
     *
     * Creates one HEVertex per (submesh, localVertex) pair without merging
     * vertices across submeshes, preserving UV seams, material boundaries,
     * and bone weight differences. Tracks submesh provenance per face.
     *
     * @param editableMesh The source mesh.
     * @return true on success, false if the mesh is empty or malformed.
     */
    bool buildFromEditableMesh(const EditableMesh& editableMesh);

    /**
     * @brief Convert back to an EditableMesh.
     *
     * Splits the unified half-edge mesh back into per-submesh vertex/triangle
     * arrays, preserving material assignments, UVs, normals, and bone weights.
     *
     * @param[out] editableMesh The target mesh. Previous contents are replaced.
     * @return true on success.
     */
    bool toEditableMesh(EditableMesh& editableMesh) const;

    /// @name Element counts
    /// @{
    size_t vertexCount() const;
    size_t faceCount() const;
    size_t edgeCount() const;
    size_t halfEdgeCount() const { return m_halfEdges.size(); }
    /// @}

    /// @name Element access (unchecked — caller must ensure valid indices)
    /// @{
    const HEVertex& vertex(int idx) const { return m_vertices[idx]; }
    HEVertex& vertex(int idx) { return m_vertices[idx]; }
    const HEFace& face(int idx) const { return m_faces[idx]; }
    HEFace& face(int idx) { return m_faces[idx]; }
    const HEEdge& edge(int idx) const { return m_edges[idx]; }
    HEEdge& edge(int idx) { return m_edges[idx]; }
    const HalfEdge& halfEdge(int idx) const { return m_halfEdges[idx]; }
    HalfEdge& halfEdge(int idx) { return m_halfEdges[idx]; }
    /// @}

    /// @name Adjacency queries
    /// @{

    /**
     * @brief Get all faces adjacent to a vertex.
     * @param vertexIdx The vertex index.
     * @return Vector of face indices (in no particular order).
     */
    std::vector<int> facesAroundVertex(int vertexIdx) const;

    /**
     * @brief Get all edges incident to a vertex.
     * @param vertexIdx The vertex index.
     * @return Vector of edge indices.
     */
    std::vector<int> edgesAroundVertex(int vertexIdx) const;

    /**
     * @brief Get all vertices adjacent to a vertex (1-ring neighborhood).
     * @param vertexIdx The vertex index.
     * @return Vector of vertex indices forming the 1-ring.
     */
    std::vector<int> verticesAroundVertex(int vertexIdx) const;

    /**
     * @brief Get the three vertices of a face.
     * @param faceIdx The face index.
     * @return Vector of 3 vertex indices (in winding order).
     */
    std::vector<int> faceVertices(int faceIdx) const;

    /**
     * @brief Get the three edges of a face.
     * @param faceIdx The face index.
     * @return Vector of 3 edge indices.
     */
    std::vector<int> faceEdges(int faceIdx) const;

    /**
     * @brief Get the two faces adjacent to an edge.
     * @param edgeIdx The edge index.
     * @return Pair of face indices. Second is -1 if the edge is on the boundary.
     */
    std::pair<int, int> edgeFaces(int edgeIdx) const;

    /**
     * @brief Get the two vertices of an edge.
     * @param edgeIdx The edge index.
     * @return Pair of vertex indices.
     */
    std::pair<int, int> edgeVertices(int edgeIdx) const;

    /// @}

    /// @name Boundary queries
    /// @{

    /**
     * @brief Check if a vertex is on the mesh boundary.
     */
    bool isVertexBoundary(int vertexIdx) const;

    /**
     * @brief Check if an edge is on the mesh boundary.
     */
    bool isEdgeBoundary(int edgeIdx) const;

    /**
     * @brief Get all boundary edge loops.
     *
     * Each loop is a vector of vertex indices forming a closed boundary loop
     * (or open chain for non-manifold cases).
     *
     * @return Vector of boundary loops.
     */
    std::vector<std::vector<int>> boundaryLoops() const;

    /// @}

    /// @name Topology operations
    /// @{

    /**
     * @brief Extrude selected faces.
     *
     * Duplicates the selected faces, creating new vertices at the same
     * positions as the originals. Connects the old boundary edges to the
     * new faces with side-wall quads (split into triangles).
     *
     * After extrusion, the new (top) vertices can be translated to create
     * the extruded shape.
     *
     * @param faceIndices The face indices to extrude.
     * @return Indices of the newly created vertices (the "top" of the extrusion).
     *         Empty if the operation failed.
     */
    std::vector<int> extrudeFaces(const std::vector<int>& faceIndices);

    /**
     * @brief Extrude selected edges.
     *
     * Creates new faces by duplicating the selected edges and connecting
     * old edge vertices to new edge vertices with quads (split into triangles).
     *
     * @param edgeIndices The edge indices to extrude.
     * @return Indices of the newly created vertices.
     *         Empty if the operation failed.
     */
    std::vector<int> extrudeEdges(const std::vector<int>& edgeIndices);

    /**
     * @brief Bevel selected edges (flat chamfer).
     *
     * For each selected interior (two-face) edge, splits the edge in place
     * into two parallel edges offset by `width` toward each adjacent face's
     * interior, and inserts a chamfer quad (two triangles) between them.
     * Each adjacent face is retriangulated so its shared edge moves from
     * (v1, v2) to either (v1a, v2a) or (v1b, v2b).
     *
     * Limitations:
     * - Boundary edges and non-manifold edges are skipped.
     * - Edges that share an endpoint with another selected edge are skipped
     *   (the corner would be pulled in inconsistent directions).
     *
     * @param edgeIndices The edge indices to bevel.
     * @param width The offset distance, in world units, by which each side
     *              of the chamfer is pulled away from the original edge.
     * @param segments Number of chamfer strips between the two inner offsets
     *                 (1 = single-strip flat chamfer, the original behavior;
     *                 2+ subdivides the chamfer into N strips along the
     *                 profile curve). Clamped to >= 1.
     * @param profile Profile-curve shape in [0, 1]. 0.5 = flat (linear
     *                interpolation, identical geometry to the single-segment
     *                case at any segments value); >0.5 bulges outward
     *                (convex / fillet-like). Concave (<0.5) is currently
     *                clamped to flat — a downstream Phase-7 winding edge
     *                case turns inverted triangles loose for any inward
     *                bulge; tracked as a follow-up. Clamped to [0, 1].
     * @return Indices of the newly created vertices (the chamfer corners
     *         and any per-segment intermediate vertices). Empty if the
     *         operation was skipped or failed.
     */
    std::vector<int> bevelEdges(const std::vector<int>& edgeIndices,
                                float width,
                                int segments = 1,
                                float profile = 0.5f);

    /// @}

    /// @name Validation
    /// @{
    /**
     * @brief Validate the half-edge structure for consistency.
     *
     * Checks twin symmetry, face loop closure, vertex connectivity.
     * @return true if the structure is consistent.
     */
    bool validate() const;
    /// @}

    /// @name Submesh info
    /// @{
    int subMeshCount() const { return m_subMeshCount; }
    const std::vector<std::string>& materialNames() const { return m_materialNames; }
    /// @}

private:
    /// Hash function for (int, int) pairs used as edge keys.
    struct PairHash {
        size_t operator()(const std::pair<int, int>& p) const {
            size_t h1 = std::hash<int>{}(p.first);
            size_t h2 = std::hash<int>{}(p.second);
            return h1 ^ (h2 * 2654435761u);
        }
    };

    /// Build boundary half-edges for edges that only have one face.
    void buildBoundaryHalfEdges();

    /// Link prev pointers for all half-edge loops.
    void linkPrevPointers();

    /// Append a triangle (3 half-edges + 1 face) to the structure.
    /// Returns the new face index. Does NOT update vertex or edge data —
    /// callers must rebuild edges/twins after adding all triangles.
    int appendTriangle(int v0, int v1, int v2, int subMeshIndex);

    /// Rebuild m_edges, half-edge twin/edge fields, and clear edge state.
    /// Skips half-edges with face < 0. After this, every interior HE has
    /// a valid edge index and twin (or twin == -1 if no opposite found).
    void rebuildEdgesAndTwins();

    /// Remove all boundary half-edges (face == -1), remap all references.
    /// Used after extrude operations that detach old boundary HEs.
    void compactBoundaryHalfEdges();

    /// Ensure every vertex's halfEdge pointer satisfies the invariant:
    /// m_halfEdges[v.halfEdge].prev->vertex == v. Searches for a valid
    /// outgoing HE if the current pointer is stale or invalid.
    void fixVertexHalfEdges();

    std::vector<HalfEdge> m_halfEdges;
    std::vector<HEVertex> m_vertices;
    std::vector<HEFace> m_faces;
    std::vector<HEEdge> m_edges;

    int m_subMeshCount = 0;
    std::vector<std::string> m_materialNames;
};

#endif // HALFEDGEMESH_H
