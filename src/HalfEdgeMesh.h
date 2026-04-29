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
     *                 profile curve). Clamped to [1, 16] — the UI SpinBox
     *                 cap; higher values can overflow the hole-filler's
     *                 64-vertex loop budget.
     * @param profile Profile-curve shape in [0, 1]. 0.5 = flat (linear
     *                interpolation, identical geometry to the single-segment
     *                case at any segments value); >0.5 bulges outward
     *                (convex / fillet-like); <0.5 bulges inward (concave /
     *                groove-like). Clamped to [0, 1].
     * @param profilePoints Optional per-interior-point profile values (size
     *                must equal segments - 1, each in [0, 1]). When empty,
     *                `profile` is used with a sin envelope so a single
     *                number controls the full curve. When supplied, each
     *                value is used directly and `profile` is ignored.
     * @return Indices of the newly created vertices (the chamfer corners
     *         and any per-segment intermediate vertices). Empty if the
     *         operation was skipped or failed.
     */
    std::vector<int> bevelEdges(const std::vector<int>& edgeIndices,
                                float width,
                                int segments = 1,
                                float profile = 0.5f,
                                const std::vector<float>& profilePoints = {});

    /**
     * @brief n-gon-aware edge bevel for meshes whose adjacent faces are
     *        arbitrary polygons.
     *
     * Simpler counterpart to `bevelEdges` — handles any face arity ≥ 3
     * by treating each beveled face's vertex loop as opaque. Use this
     * when at least one face adjacent to a beveled edge is a quad or
     * higher-arity n-gon; the triangle-only `bevelEdges` makes
     * incorrect assumptions about "third vertex" in that case.
     *
     * MVP scope: isolated bevels only (input edges sharing endpoints
     * are rejected); flat single-segment chamfer; `profile` and
     * `profilePoints` are reserved for a future extension.
     *
     * @return Indices of the newly created vertices. Empty on failure.
     */
    std::vector<int> bevelEdgesNgon(const std::vector<int>& edgeIndices,
                                    float width,
                                    int segments = 1,
                                    float profile = 0.5f,
                                    const std::vector<float>& profilePoints = {});

    /**
     * @brief Bevel selected vertices (corner cut).
     *
     * For each selected vertex v of valence N >= 3, replaces v with an
     * N-gon face. Each edge incident to v is split at distance `width`
     * from v (clamped to half the shortest incident edge), giving N new
     * vertices. Each face originally at v is retriangulated to exchange
     * v's corner for the two edge offsets on its two v-edges. Then the
     * N offsets are triangulated into a "cap" face sitting where v used
     * to be.
     *
     * Limitations (MVP):
     * - Valence < 3 vertices are skipped (nothing sensible to bevel).
     * - Boundary vertices are skipped.
     * - `segments` and `profilePoints` are reserved for a future
     *   rounded-dome implementation; this MVP always emits a flat cap.
     *
     * Multi-vertex selections are processed sequentially (one vertex at
     * a time, rebuilding the half-edge structure between iterations).
     * This keeps shared-edge pairs manifold at the cost of slight O(N²)
     * growth with selection size, which is fine for interactive use.
     *
     * @param vertexIndices The vertex indices to bevel.
     * @param width The offset distance along each incident edge. Clamped
     *              per-vertex to min(shortest_edge) / 2 to avoid self-
     *              intersection with the neighbor's own bevel.
     * @param segments Radial subdivisions of the cap (1 = flat N-gon,
     *                 2+ = rounded dome). Clamped to [1, 16].
     * @param profile Profile-curve shape in [0, 1]. Same semantics as
     *                edge bevel.
     * @param profilePoints Optional per-ring profile values (size = segments - 1).
     *                When empty, `profile` drives the sin envelope.
     * @return Indices of the newly created vertices. Empty if the
     *         operation was skipped or failed.
     */
    std::vector<int> bevelVertices(const std::vector<int>& vertexIndices,
                                   float width,
                                   int segments = 1,
                                   float profile = 0.5f,
                                   const std::vector<float>& profilePoints = {});

    /**
     * @brief n-gon-aware vertex bevel for meshes whose incident faces are
     *        arbitrary polygons.
     *
     * Simpler counterpart to `bevelVertices`. Replaces v with one inner
     * vertex per incident face (placed at distance `width` along the
     * direction toward each face's centroid) plus a single n-gon cap
     * face walking those inner vertices in ring order. Each incident
     * face keeps its original arity — only its v-corner moves inward.
     *
     * MVP scope: flat single-segment cap, valence ≥ 3, non-boundary
     * vertices only. `segments` and `profilePoints` are reserved for a
     * future rounded-cap extension.
     *
     * @return Indices of the newly created vertices. Empty on failure.
     */
    std::vector<int> bevelVerticesNgon(const std::vector<int>& vertexIndices,
                                       float width,
                                       int segments = 1,
                                       float profile = 0.5f,
                                       const std::vector<float>& profilePoints = {});

    /**
     * @brief Insert a new vertex on an edge at parametric position t, then
     *        split each triangle that used the edge into two triangles.
     *
     * Interpolates the new vertex's position, normal, UV, color, tangent,
     * and bone weights between the edge's endpoints using t. After the call
     * the edge is replaced by two edges meeting at the new vertex, and
     * each of the 1–2 adjacent triangles has been replaced by two
     * triangles sharing the split point.
     *
     * MVP limits: works only on edges whose adjacent faces are triangles
     * (n-gons would need ear-clip-aware splitting). Returns -1 on failure.
     *
     * @param edgeIdx The edge to split.
     * @param t Parametric position along the edge in [0, 1]. Clamped into
     *          the (epsilon, 1 - epsilon) interior so the resulting faces
     *          aren't degenerate.
     * @return The newly created vertex's index, or -1 on failure.
     */
    int splitEdge(int edgeIdx, float t);

    /**
     * @brief Split an n-gon face by inserting a diagonal edge between two
     *        of its boundary vertices, where n ∈ {3, 4}.
     *
     * Retires the old face and appends two new faces sharing the new
     * diagonal edge. Both vertices must be on the face's boundary loop
     * and must NOT already be adjacent on that loop (connected by a
     * boundary edge of the face) — either would duplicate an existing
     * edge or collapse one of the new faces.
     *
     * Consequence: on a triangle every pair of vertices is adjacent, so
     * splitFace always rejects triangles. The knife pipeline doesn't
     * need that case anyway — two splitEdges on one triangle already
     * leave the cut segment as a real edge (see the
     * TwoSplitEdgesOnOneTriangleProduceMidpointEdge invariant).
     *
     * Faces with more than 4 boundary vertices are also rejected: the
     * current pipeline doesn't produce them, and splitting them cleanly
     * would need an ear-clip-aware rewire. appendFace itself accepts
     * any n ≥ 3, so the cap here is a scope choice and can be lifted
     * later without changing this method's contract.
     *
     * @param faceIdx The face to split.
     * @param vA First boundary vertex.
     * @param vB Second boundary vertex.
     * @return true on success.
     */
    bool splitFace(int faceIdx, int vA, int vB);

    /**
     * @brief A single click on a mesh edge, given as the edge index plus the
     *        parametric position along it in [0,1]. The walk-and-cut knife
     *        algorithm takes a list of these and produces a continuous chain
     *        of real mesh edges between consecutive clicks.
     */
    struct CutPoint {
        int edgeIndex = -1;
        float t = 0.5f;
    };

    /**
     * @brief Apply a knife cut defined by a sequence of edge clicks.
     *
     * For each pair of consecutive `CutPoint`s the algorithm:
     *  - runs `splitEdge` at both endpoints (skipping duplicates the second
     *    time an endpoint is encountered),
     *  - walks triangles between the two new vertices along the 3D line that
     *    connects them, running `splitEdge` at every interior edge the line
     *    crosses,
     *  - stops when the current triangle contains both the previous cut
     *    vertex and the next endpoint — at that point the existing
     *    splitEdge semantics already produce the closing cut edge.
     *
     * MVP scope:
     *  - All intersections must land strictly inside edge segments. Cuts
     *    that graze a vertex are not yet snapped.
     *  - The algorithm walks only one face at a time; it doesn't bridge
     *    disconnected submeshes.
     *
     * @param points Ordered clicks. Each must reference a currently-valid
     *               edge of the mesh (indices are NOT re-validated against
     *               mutations inside this call — callers must pass the list
     *               resolved against the pre-cut mesh).
     * @return Indices of every newly created vertex (endpoints + interior
     *         crossings), in creation order. Empty on failure.
     */
    std::vector<int> cutPath(const std::vector<CutPoint>& points);

    /**
     * @brief Merge a set of vertices into a single survivor at `targetPos`.
     *
     * Picks `vertexIndices[0]` as the survivor, copies `targetPos` into its
     * position, re-points every half-edge that referenced a doomed vertex
     * to the survivor, retires triangles that become degenerate (two of
     * their three vertices identical after the rewrite), then rebuilds the
     * edge/twin/boundary tables. UVs/normals/bone weights/tangents on the
     * survivor are kept as-is so the user gets a deterministic outcome —
     * pre-merge attribute interpolation is a follow-up concern.
     *
     * Refuses any merge that would cross a submesh boundary, since
     * dropping a vertex per submesh would silently fuse UV seams /
     * material groups. Caller can group selections per-submesh upfront.
     *
     * @param vertexIndices The HE-vertex indices to merge. Order matters
     *        only in that vertexIndices[0] becomes the survivor; the rest
     *        are retired. Duplicates and invalid indices are ignored.
     * @param targetPos The local-space position assigned to the survivor.
     * @return Number of vertices that were actually retired (0 on no-op
     *         or refusal). The survivor is not counted.
     */
    int mergeVertices(const std::vector<int>& vertexIndices,
                      const Ogre::Vector3& targetPos);

    /**
     * @brief Find pairs of vertices within `threshold` of each other and
     *        merge each cluster to its centroid. Operates only on the
     *        provided candidate set (so the caller controls scope —
     *        usually the current selection). Cross-submesh pairs are
     *        skipped, same as `mergeVertices`.
     *
     * Implementation: union-find by spatial proximity. A vertex landing
     * in two clusters joins both; the merged cluster collapses to the
     * combined centroid.
     *
     * @param vertexIndices Candidate vertices.
     * @param threshold World-space distance under which a pair fuses.
     *        Defaults to 1e-4 (≈0.1 mm at meter scale).
     * @return Number of vertices retired across all clusters.
     */
    int mergeVerticesByDistance(const std::vector<int>& vertexIndices,
                                float threshold = 1e-4f);

    /**
     * @brief Delete a set of faces (triangles).
     *
     * Retires each face's half-edges and the face itself. Vertices and
     * edges that no longer participate in any face are also retired so
     * `toEditableMesh` doesn't carry orphaned geometry forward. Surviving
     * vertices on the deletion boundary become boundary vertices.
     *
     * @param faceIndices Faces to retire. Out-of-range / already-retired
     *        entries are ignored.
     * @return Number of faces actually retired.
     */
    int deleteFaces(const std::vector<int>& faceIndices);

    /**
     * @brief Delete a set of edges, removing their adjacent faces.
     *
     * For each edge, retires both adjacent faces (or one, on a boundary
     * edge). Vertices that lose all incident faces are also retired.
     *
     * @param edgeIndices Edges to delete.
     * @return Number of faces removed across all edges.
     */
    int deleteEdges(const std::vector<int>& edgeIndices);

    /**
     * @brief Delete a set of vertices, removing every adjacent face.
     *
     * Mirrors Blender's "Delete Vertices": every face that touches one
     * of the targeted vertices is retired, and the vertices themselves
     * are retired afterwards. Edges that fall out of all surviving
     * faces are dropped during the standard rebuild.
     *
     * @param vertexIndices Vertices to delete.
     * @return Number of vertices actually retired.
     */
    int deleteVertices(const std::vector<int>& vertexIndices);

    /**
     * @brief Dissolve a set of edges, merging each edge's two adjacent
     *        triangles into a single face.
     *
     * For each interior edge whose two adjacent faces are both triangles,
     * retires the pair and emits the merged quad fan-triangulated into
     * two triangles that share the *other* diagonal. The result removes
     * the dissolved edge from the mesh while keeping a watertight
     * triangulation. Boundary edges and edges with one or zero adjacent
     * faces are skipped.
     *
     * Edges that share a vertex with another edge in the input are
     * processed sequentially against the live topology — earlier
     * dissolves may invalidate later ones; those late entries become
     * no-ops rather than errors.
     *
     * @param edgeIndices Edges to dissolve.
     * @return Number of edges actually dissolved.
     */
    int dissolveEdges(const std::vector<int>& edgeIndices);

    /**
     * @brief Dissolve a set of interior vertices, merging the surrounding
     *        face fan into a single triangulated polygon.
     *
     * For each non-boundary vertex of valence N >= 3, retires the N
     * triangles around it and re-triangulates the resulting N-gon hole
     * via a fan whose apex is the loop's first vertex in winding order
     * (i.e. the first non-`v` vertex of the lowest-indexed incident
     * face). Boundary vertices and vertices of valence < 3 are skipped.
     *
     * @param vertexIndices Vertices to dissolve.
     * @return Number of vertices actually dissolved.
     */
    int dissolveVertices(const std::vector<int>& vertexIndices);

    /**
     * @brief Subdivide selected triangles by 1-to-4 split.
     *
     * For each selected triangle, inserts a midpoint vertex on each of its
     * three edges and replaces the triangle with four sub-triangles: three
     * corner triangles plus a central one whose three vertices are the
     * midpoints. Midpoints on shared edges are reused so adjacent selected
     * triangles tile cleanly.
     *
     * Adjacent NON-selected triangles whose edges land on a midpoint are
     * also retriangulated to avoid T-junctions:
     *  - 1 split edge → split into 2 triangles fanned from the midpoint.
     *  - 2 split edges → split into 3 triangles.
     *  - 3 split edges → behaves like a selected face (4 sub-triangles).
     *
     * UVs / normals / bone weights are interpolated linearly along each
     * subdivided edge by `interpolateVertex`. The midpoints land at edge
     * centers (t = 0.5).
     *
     * MVP scope: faces must be triangles. N-gon faces are skipped. Faces
     * spanning multiple submeshes still tile correctly because midpoints
     * are keyed off vertex pairs (which inherit the face's submesh).
     *
     * @param faceIndices Triangles to subdivide.
     * @return Indices of every newly created midpoint vertex (in creation
     *         order). Empty on no-op.
     */
    std::vector<int> subdivideFaces(const std::vector<int>& faceIndices);

    /**
     * @brief Subdivide each selected n-gon face into N sub-quads, linearly.
     *
     * Geometrically equivalent to one Catmull-Clark step ON THE
     * SELECTED FACES ONLY, minus the smoothing rules — face points and
     * edge points land at the arithmetic mean of their inputs (linear
     * subdivide, no chord/face-point blending into surrounding
     * vertices). Output is always quads regardless of input N.
     *
     * Use this when you want to preserve quad structure on a partial
     * selection without smoothing the corners. For triangle inputs the
     * existing `subdivideFaces` 1-to-4 split is usually a better fit;
     * for quad inputs `subdivideFaces` skips them as non-triangles, so
     * this method fills that gap.
     *
     * Adjacent NON-selected faces sharing an edge with a subdivided
     * face are NOT retriangulated — this means you'll get T-junctions
     * along the boundary between selected and unselected regions on
     * the same submesh. Most users will want to either (a) select
     * full neighbour rings, or (b) use the whole-mesh
     * `subdivideCatmullClark` instead. T-junction prevention is a
     * follow-up.
     *
     * MVP scope:
     *  - Faces must have 3+ corners. (3 → 3 quads, 4 → 4, N → N.)
     *  - Cross-submesh boundary handling: edge midpoints are shared
     *    only between selected faces in the same submesh. Cross-
     *    submesh edges fall back to per-face edge points (no
     *    sharing), which still produces a valid mesh — just slightly
     *    duplicated vertices at material seams.
     *
     * @param faceIndices The HE face indices to subdivide.
     * @return Indices of every newly created vertex (face points
     *         followed by edge points). Empty on no-op.
     */
    std::vector<int> subdivideFacesToQuads(const std::vector<int>& faceIndices);

    /**
     * @brief Subdivide every face by one Catmull-Clark step.
     *
     * The classic Catmull-Clark scheme. For each face F:
     *  - Compute a face point Fp = average of its corner positions.
     *  - For each edge E with endpoints (a, b) and adjacent face points
     *    (Fp1, Fp2), compute the edge point Ep = (a + b + Fp1 + Fp2) / 4
     *    on interior edges, or Ep = (a + b) / 2 on boundary edges.
     *  - For each original vertex V of valence n with adjacent face
     *    points Fi (mean F) and edge midpoints Ri (mean R, taken on
     *    PRE-update positions), update V to (F + 2R + (n-3) V) / n on
     *    interior vertices, or (V + Em1 + Em2) / 4 on boundary
     *    vertices using its two boundary edge midpoints.
     *  - Replace each face F (with n corners) by n quads, each formed
     *    from (corner, neighbouring-edge-point, face-point,
     *    other-neighbouring-edge-point).
     *
     * Output is ALWAYS quads, regardless of input — a triangle becomes
     * 3 quads, a quad becomes 4 quads, an N-gon becomes N quads.
     * Submesh assignments are preserved (each output quad inherits its
     * source face's submesh).
     *
     * UVs / normals / colors / bone weights / tangents are blended
     * with the same weights as positions (face point: avg of corners;
     * edge point: avg of endpoints+adjacent face points; updated
     * vertex: weighted blend per the rule above). For boundary
     * vertices the chord rule keeps UV seams reasonable; geometry on
     * a closed manifold is C¹ continuous in the limit.
     *
     * Skips faces that span multiple submeshes (would silently weld
     * material groups). Cross-submesh edges are treated as boundaries
     * for the smoothing rule, so submesh boundaries stay sharp.
     *
     * @return Indices of every newly created vertex (face points,
     *         edge points, in creation order). The original vertex
     *         slots are reused for the smoothed positions; their
     *         indices are unchanged. Empty on no-op (all submeshes
     *         empty / all faces invalid).
     */
    std::vector<int> subdivideCatmullClark();

    /**
     * @brief Fill a face from selected vertices or a closed edge loop.
     *
     * Two input modes, picked by the caller:
     *  - Vertex fill: 3 vertices → emits a single triangle. 4 vertices →
     *    emits two triangles fan-triangulated from the first input vertex
     *    in the order given. The caller is responsible for choosing a
     *    sensible winding (the fill follows it verbatim).
     *  - Loop fill: a closed boundary loop of any length N ≥ 3 →
     *    fan-triangulated from `vertexIndices[0]`. Pass the loop's
     *    boundary vertices in winding order.
     *
     * Both modes refuse cross-submesh inputs (would silently weld
     * material groups), require all vertices alive, and reject inputs
     * that would duplicate an existing face.
     *
     * UV / normals / bone weights of the new vertices are NOT modified —
     * the new face inherits attributes from the existing per-vertex data.
     *
     * @param vertexIndices Boundary vertices in winding order. 3, 4, or
     *        any N ≥ 3 for loop fill.
     * @return Number of triangles created (1 for a 3-vertex fill, 2 for a
     *         quad, N-2 for a longer loop). 0 on rejection.
     */
    int fillSelection(const std::vector<int>& vertexIndices);

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

    /// Append a polygon with N vertices (N >= 3) as a single face with
    /// N half-edges. Returns the new face index, or -1 if vertices.size()
    /// is invalid. Like appendTriangle, callers must run the
    /// rebuild/compact/boundary/fix-vertex cleanup afterwards.
    int appendFace(const std::vector<int>& vertices, int subMeshIndex);

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

    // When true, bevelVertices trusts the caller's width and skips its
    // per-vertex "min(width, 0.499 × minEdgeLen)" safety clamp. The
    // multi-vertex pre-budgeted path sets this for its inner recursive
    // single-vertex call so pre-budgeted values aren't re-clamped
    // against a mutated mesh's edge lengths; it resets to false
    // afterwards.
    bool m_skipVertexBevelClamp = false;
};

#endif // HALFEDGEMESH_H
