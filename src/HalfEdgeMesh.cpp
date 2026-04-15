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

#include "HalfEdgeMesh.h"
#include "EditableMesh.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <unordered_set>

bool HalfEdgeMesh::buildFromEditableMesh(const EditableMesh& editableMesh)
{
    m_halfEdges.clear();
    m_vertices.clear();
    m_faces.clear();
    m_edges.clear();
    m_vertexOrigins.clear();
    m_materialNames.clear();

    const auto& subMeshes = editableMesh.subMeshes();
    m_subMeshCount = static_cast<int>(subMeshes.size());

    if (m_subMeshCount == 0)
        return false;

    // Phase 1: Build a unified vertex list.
    // Each submesh has its own vertex array. We create one HEVertex per
    // (submesh, localVertex) pair. Vertices at the same position across
    // submeshes are NOT merged — they stay separate to preserve UV seams,
    // material boundaries, and bone weight differences.
    //
    // Within a single submesh, vertices are already unique (Ogre guarantees this).

    // Map from (subMeshIndex, localVertexIndex) -> global HE vertex index
    std::vector<std::vector<int>> vertexMap(m_subMeshCount);

    for (int s = 0; s < m_subMeshCount; ++s) {
        const auto& sub = subMeshes[s];
        m_materialNames.push_back(sub.materialName);
        vertexMap[s].resize(sub.vertices.size());

        for (size_t v = 0; v < sub.vertices.size(); ++v) {
            int heIdx = static_cast<int>(m_vertices.size());
            vertexMap[s][v] = heIdx;

            HEVertex hv;
            hv.position = sub.vertices[v].position;
            hv.normal = sub.vertices[v].normal;
            hv.uv = sub.vertices[v].uv;
            hv.color = sub.vertices[v].color;
            hv.hasNormal = sub.vertices[v].hasNormal;
            hv.hasUV = sub.vertices[v].hasUV;
            hv.hasColor = sub.vertices[v].hasColor;

            for (const auto& ba : sub.vertices[v].boneAssignments) {
                hv.boneAssignments.push_back({ba.boneIndex, ba.weight});
            }

            m_vertices.push_back(std::move(hv));

            VertexOrigin origin;
            origin.subMeshIndex = s;
            origin.localVertexIndex = static_cast<int>(v);
            m_vertexOrigins.push_back(origin);
        }
    }

    // Phase 2: Create faces and half-edges from triangles.
    // For each triangle, create 3 half-edges forming a loop.
    // Use (min_vertex, max_vertex) as the edge key to find twins.

    // Map from directed edge (from, to) -> half-edge index
    std::unordered_map<std::pair<int,int>, int, PairHash> directedEdgeMap;

    // Map from undirected edge (min, max) -> logical edge index
    std::unordered_map<std::pair<int,int>, int, PairHash> edgeMap;

    for (int s = 0; s < m_subMeshCount; ++s) {
        const auto& sub = subMeshes[s];

        for (size_t t = 0; t < sub.triangles.size(); ++t) {
            const auto& tri = sub.triangles[t];

            // Map local vertex indices to global HE vertex indices
            int v0 = vertexMap[s][tri.indices[0]];
            int v1 = vertexMap[s][tri.indices[1]];
            int v2 = vertexMap[s][tri.indices[2]];

            // Skip degenerate triangles
            if (v0 == v1 || v1 == v2 || v0 == v2)
                continue;

            // Create the face
            int faceIdx = static_cast<int>(m_faces.size());
            HEFace hf;
            hf.subMeshIndex = s;
            m_faces.push_back(hf);

            // Create 3 half-edges for this triangle
            int he0 = static_cast<int>(m_halfEdges.size());
            int he1 = he0 + 1;
            int he2 = he0 + 2;

            int verts[3] = {v0, v1, v2};

            for (int i = 0; i < 3; ++i) {
                HalfEdge he;
                he.vertex = verts[(i + 1) % 3]; // points TO the next vertex
                he.face = faceIdx;
                he.next = he0 + (i + 1) % 3;
                he.prev = he0 + (i + 2) % 3;
                m_halfEdges.push_back(he);
            }

            // Set the face's half-edge pointer
            m_faces[faceIdx].halfEdge = he0;

            // Set vertex outgoing half-edge pointers (if not already set)
            for (int i = 0; i < 3; ++i) {
                int fromVert = verts[i];
                int heIdx = he0 + i;
                if (m_vertices[fromVert].halfEdge == -1) {
                    m_vertices[fromVert].halfEdge = heIdx;
                }
            }

            // Register directed edges and find/create logical edges
            for (int i = 0; i < 3; ++i) {
                int from = verts[i];
                int to = verts[(i + 1) % 3];
                int heIdx = he0 + i;

                directedEdgeMap[{from, to}] = heIdx;

                // Find or create logical edge
                int eMin = std::min(from, to);
                int eMax = std::max(from, to);
                auto edgeKey = std::make_pair(eMin, eMax);

                auto edgeIt = edgeMap.find(edgeKey);
                if (edgeIt == edgeMap.end()) {
                    int edgeIdx = static_cast<int>(m_edges.size());
                    HEEdge edge;
                    edge.halfEdge = heIdx;
                    m_edges.push_back(edge);
                    edgeMap[edgeKey] = edgeIdx;
                    m_halfEdges[heIdx].edge = edgeIdx;
                } else {
                    m_halfEdges[heIdx].edge = edgeIt->second;
                }
            }
        }
    }

    // Phase 3: Link twin half-edges.
    // For each directed edge (from, to), its twin is the directed edge (to, from).
    for (auto& [dirEdge, heIdx] : directedEdgeMap) {
        auto twinKey = std::make_pair(dirEdge.second, dirEdge.first);
        auto twinIt = directedEdgeMap.find(twinKey);
        if (twinIt != directedEdgeMap.end()) {
            m_halfEdges[heIdx].twin = twinIt->second;
        }
    }

    // Phase 4: Create boundary half-edges for edges that have only one face.
    buildBoundaryHalfEdges();

    return true;
}

void HalfEdgeMesh::buildBoundaryHalfEdges()
{
    // Find all half-edges without twins (boundary edges).
    // For each, create a boundary half-edge going in the opposite direction.
    std::vector<int> boundaryHEs;
    for (int i = 0; i < static_cast<int>(m_halfEdges.size()); ++i) {
        if (m_halfEdges[i].twin == -1 && m_halfEdges[i].face != -1) {
            boundaryHEs.push_back(i);
        }
    }

    if (boundaryHEs.empty())
        return;

    // Create boundary half-edges
    // A boundary half-edge goes from he.vertex back to the vertex that he starts from.
    // The "from" vertex of a half-edge he is: m_halfEdges[he.prev].vertex
    std::unordered_map<int, int> vertexToBoundaryHE; // vertex -> boundary half-edge starting at that vertex

    for (int interiorHE : boundaryHEs) {
        int bheIdx = static_cast<int>(m_halfEdges.size());

        HalfEdge bhe;
        // The boundary half-edge goes from he.vertex to the "from" vertex of he
        int fromVertex = m_halfEdges[interiorHE].vertex;
        int toVertex = m_halfEdges[m_halfEdges[interiorHE].prev].vertex;
        // Actually: the "from" of interior he is prev->vertex... let me reconsider.
        // Interior half-edge goes: prevVertex -> he.vertex
        // Boundary twin goes: he.vertex -> prevVertex
        // So boundary he points TO the "from" vertex of the interior he.
        bhe.vertex = toVertex;
        bhe.twin = interiorHE;
        bhe.face = -1; // boundary
        bhe.edge = m_halfEdges[interiorHE].edge;

        m_halfEdges[interiorHE].twin = bheIdx;
        m_halfEdges.push_back(bhe);

        // Track that this boundary HE starts at fromVertex
        vertexToBoundaryHE[fromVertex] = bheIdx;

        // For boundary vertices, prefer to store a boundary half-edge as outgoing
        // so that isVertexBoundary() can work efficiently
        m_vertices[fromVertex].halfEdge = bheIdx;
    }

    // Link boundary half-edges into loops via next/prev.
    // Each boundary HE starts at some vertex. Its "next" boundary HE starts
    // at the vertex this one points to.
    for (int i = 0; i < static_cast<int>(m_halfEdges.size()); ++i) {
        if (m_halfEdges[i].face != -1)
            continue; // not a boundary HE

        int targetVertex = m_halfEdges[i].vertex;
        auto nextIt = vertexToBoundaryHE.find(targetVertex);
        if (nextIt != vertexToBoundaryHE.end()) {
            m_halfEdges[i].next = nextIt->second;
            m_halfEdges[nextIt->second].prev = i;
        }
    }
}

void HalfEdgeMesh::linkPrevPointers()
{
    // For interior half-edges, prev is already set during construction.
    // This method can be called if prev pointers need to be recomputed.
    for (int i = 0; i < static_cast<int>(m_halfEdges.size()); ++i) {
        int next = m_halfEdges[i].next;
        if (next >= 0) {
            m_halfEdges[next].prev = i;
        }
    }
}

bool HalfEdgeMesh::toEditableMesh(EditableMesh& editableMesh) const
{
    auto& outSubMeshes = editableMesh.subMeshes();
    outSubMeshes.clear();
    outSubMeshes.resize(m_subMeshCount);

    // Initialize submesh material names
    for (int s = 0; s < m_subMeshCount; ++s) {
        if (s < static_cast<int>(m_materialNames.size())) {
            outSubMeshes[s].materialName = m_materialNames[s];
        }
        outSubMeshes[s].usesSharedVertices = false;
    }

    // For each face, determine which submesh it belongs to,
    // and collect the 3 vertices. We need to create per-submesh
    // vertex arrays (possibly duplicating vertices shared across faces
    // in the same submesh if their attributes differ).

    // Per-submesh: map from HE vertex index -> local vertex index
    std::vector<std::unordered_map<int, int>> vertexRemap(m_subMeshCount);

    for (int f = 0; f < static_cast<int>(m_faces.size()); ++f) {
        const auto& heFace = m_faces[f];
        int subIdx = heFace.subMeshIndex;
        auto& outSub = outSubMeshes[subIdx];

        auto verts = faceVertices(f);
        if (verts.size() != 3)
            continue;

        EditableTriangle tri;
        for (int i = 0; i < 3; ++i) {
            int heVertIdx = verts[i];

            auto it = vertexRemap[subIdx].find(heVertIdx);
            if (it == vertexRemap[subIdx].end()) {
                // Add this vertex to the submesh
                int localIdx = static_cast<int>(outSub.vertices.size());
                vertexRemap[subIdx][heVertIdx] = localIdx;

                EditableVertex ev;
                const auto& hv = m_vertices[heVertIdx];
                ev.position = hv.position;
                ev.normal = hv.normal;
                ev.uv = hv.uv;
                ev.color = hv.color;
                ev.hasNormal = hv.hasNormal;
                ev.hasUV = hv.hasUV;
                ev.hasColor = hv.hasColor;

                for (const auto& [boneIdx, weight] : hv.boneAssignments) {
                    EditableBoneAssignment eba;
                    eba.boneIndex = boneIdx;
                    eba.weight = weight;
                    ev.boneAssignments.push_back(eba);
                }

                outSub.vertices.push_back(std::move(ev));
                tri.indices[i] = localIdx;
            } else {
                tri.indices[i] = it->second;
            }
        }

        outSub.triangles.push_back(tri);
    }

    return true;
}

size_t HalfEdgeMesh::vertexCount() const
{
    return m_vertices.size();
}

size_t HalfEdgeMesh::faceCount() const
{
    return m_faces.size();
}

size_t HalfEdgeMesh::edgeCount() const
{
    return m_edges.size();
}

std::vector<int> HalfEdgeMesh::facesAroundVertex(int vertexIdx) const
{
    std::vector<int> result;
    if (vertexIdx < 0 || vertexIdx >= static_cast<int>(m_vertices.size()))
        return result;

    int startHE = m_vertices[vertexIdx].halfEdge;
    if (startHE < 0)
        return result;

    // Walk around the vertex using twin->next pattern.
    // From an outgoing half-edge, its face is the face to the "left".
    // To get to the next outgoing half-edge, go: prev -> twin.
    int he = startHE;
    do {
        if (m_halfEdges[he].face >= 0) {
            result.push_back(m_halfEdges[he].face);
        }

        // Move to next outgoing half-edge: go to prev, then twin
        int prev = m_halfEdges[he].prev;
        if (prev < 0) break;
        int twin = m_halfEdges[prev].twin;
        if (twin < 0) break;
        he = twin;
    } while (he != startHE);

    // If we hit a boundary, also walk the other direction
    if (he != startHE) {
        // Walk the other way: twin -> next
        he = startHE;
        int twin = m_halfEdges[he].twin;
        if (twin >= 0) {
            he = m_halfEdges[twin].next;
            while (he != startHE && he >= 0) {
                if (m_halfEdges[he].face >= 0) {
                    result.push_back(m_halfEdges[he].face);
                }
                int prevHE = m_halfEdges[he].prev;
                if (prevHE < 0) break;
                twin = m_halfEdges[prevHE].twin;
                if (twin < 0) break;
                he = twin;
            }
        }
    }

    return result;
}

std::vector<int> HalfEdgeMesh::edgesAroundVertex(int vertexIdx) const
{
    std::vector<int> result;
    if (vertexIdx < 0 || vertexIdx >= static_cast<int>(m_vertices.size()))
        return result;

    int startHE = m_vertices[vertexIdx].halfEdge;
    if (startHE < 0)
        return result;

    int he = startHE;
    do {
        if (m_halfEdges[he].edge >= 0) {
            result.push_back(m_halfEdges[he].edge);
        }

        int prev = m_halfEdges[he].prev;
        if (prev < 0) break;
        int twin = m_halfEdges[prev].twin;
        if (twin < 0) {
            // Also add the edge of the prev (boundary edge)
            if (m_halfEdges[prev].edge >= 0) {
                result.push_back(m_halfEdges[prev].edge);
            }
            break;
        }
        he = twin;
    } while (he != startHE);

    return result;
}

std::vector<int> HalfEdgeMesh::verticesAroundVertex(int vertexIdx) const
{
    std::vector<int> result;
    if (vertexIdx < 0 || vertexIdx >= static_cast<int>(m_vertices.size()))
        return result;

    int startHE = m_vertices[vertexIdx].halfEdge;
    if (startHE < 0)
        return result;

    // Each outgoing half-edge points to a neighbor vertex
    int he = startHE;
    do {
        result.push_back(m_halfEdges[he].vertex);

        int prev = m_halfEdges[he].prev;
        if (prev < 0) break;
        int twin = m_halfEdges[prev].twin;
        if (twin < 0) {
            // On boundary: the prev half-edge's "from" vertex is also a neighbor
            // but we get that from the prev's prev's vertex... actually:
            // prev goes from some vertex to vertexIdx. The "from" of prev is
            // prev->prev->vertex (which is actually the vertex at the tail of prev).
            // For boundary, add the vertex that prev comes FROM.
            // prev points TO vertexIdx, so prev starts FROM m_halfEdges[prev].prev->vertex
            // But prev->prev might be -1 on boundary. Instead, the "from" vertex
            // of half-edge `prev` is the vertex that prev's face loop comes from,
            // which is m_halfEdges[m_halfEdges[prev].prev].vertex — but prev is
            // an interior edge here so prev->prev is valid.
            if (m_halfEdges[prev].prev >= 0) {
                result.push_back(m_halfEdges[m_halfEdges[prev].prev].vertex);
            }
            break;
        }
        he = twin;
    } while (he != startHE);

    return result;
}

std::vector<int> HalfEdgeMesh::faceVertices(int faceIdx) const
{
    std::vector<int> result;
    if (faceIdx < 0 || faceIdx >= static_cast<int>(m_faces.size()))
        return result;

    int startHE = m_faces[faceIdx].halfEdge;
    if (startHE < 0)
        return result;

    // Walk the face loop collecting the "from" vertex of each half-edge.
    // The "from" vertex of he is: m_halfEdges[he.prev].vertex
    // But more simply, each half-edge points TO a vertex, and we want
    // the vertices in winding order. The vertex that half-edge `he` goes
    // FROM is the prev half-edge's target vertex.
    // Simpler: collect he.vertex for each he in the loop — this gives
    // the vertices in the order they are pointed TO, which matches
    // the winding order (shifted by one position, but consistent).

    // Actually for a triangle with vertices A->B->C:
    // he0: A->B (vertex=B), he1: B->C (vertex=C), he2: C->A (vertex=A)
    // Walking he0->he1->he2 gives us B, C, A. We want A, B, C.
    // So collect prev->vertex instead: he0.prev=he2 (vertex=A), he1.prev=he0 (vertex=B), etc.
    // Or just: use the prev chain starting from startHE.

    // Simplest approach: collect he.prev.vertex for each he in the face loop.
    int he = startHE;
    do {
        // The vertex that this half-edge goes FROM
        int fromVert = m_halfEdges[m_halfEdges[he].prev].vertex;
        result.push_back(fromVert);
        he = m_halfEdges[he].next;
    } while (he != startHE);

    return result;
}

std::vector<int> HalfEdgeMesh::faceEdges(int faceIdx) const
{
    std::vector<int> result;
    if (faceIdx < 0 || faceIdx >= static_cast<int>(m_faces.size()))
        return result;

    int startHE = m_faces[faceIdx].halfEdge;
    if (startHE < 0)
        return result;

    int he = startHE;
    do {
        if (m_halfEdges[he].edge >= 0) {
            result.push_back(m_halfEdges[he].edge);
        }
        he = m_halfEdges[he].next;
    } while (he != startHE);

    return result;
}

std::pair<int, int> HalfEdgeMesh::edgeFaces(int edgeIdx) const
{
    if (edgeIdx < 0 || edgeIdx >= static_cast<int>(m_edges.size()))
        return {-1, -1};

    int he = m_edges[edgeIdx].halfEdge;
    if (he < 0)
        return {-1, -1};

    int face1 = m_halfEdges[he].face;
    int twin = m_halfEdges[he].twin;
    int face2 = (twin >= 0) ? m_halfEdges[twin].face : -1;

    return {face1, face2};
}

std::pair<int, int> HalfEdgeMesh::edgeVertices(int edgeIdx) const
{
    if (edgeIdx < 0 || edgeIdx >= static_cast<int>(m_edges.size()))
        return {-1, -1};

    int he = m_edges[edgeIdx].halfEdge;
    if (he < 0)
        return {-1, -1};

    int toVert = m_halfEdges[he].vertex;
    int fromVert = -1;

    int prev = m_halfEdges[he].prev;
    if (prev >= 0) {
        fromVert = m_halfEdges[prev].vertex;
    } else if (m_halfEdges[he].twin >= 0) {
        fromVert = m_halfEdges[m_halfEdges[he].twin].vertex;
    }

    return {fromVert, toVert};
}

bool HalfEdgeMesh::isVertexBoundary(int vertexIdx) const
{
    if (vertexIdx < 0 || vertexIdx >= static_cast<int>(m_vertices.size()))
        return false;

    int startHE = m_vertices[vertexIdx].halfEdge;
    if (startHE < 0)
        return false;

    // A vertex is on the boundary if any of its outgoing half-edges is a boundary HE
    int he = startHE;
    do {
        if (m_halfEdges[he].face == -1)
            return true;

        int prev = m_halfEdges[he].prev;
        if (prev < 0) return true;
        int twin = m_halfEdges[prev].twin;
        if (twin < 0) return true;
        he = twin;
    } while (he != startHE);

    return false;
}

bool HalfEdgeMesh::isEdgeBoundary(int edgeIdx) const
{
    if (edgeIdx < 0 || edgeIdx >= static_cast<int>(m_edges.size()))
        return false;

    int he = m_edges[edgeIdx].halfEdge;
    if (he < 0)
        return false;

    // An edge is boundary if one of its half-edges has no face
    if (m_halfEdges[he].face == -1)
        return true;

    int twin = m_halfEdges[he].twin;
    if (twin < 0)
        return true;
    if (m_halfEdges[twin].face == -1)
        return true;

    return false;
}

std::vector<std::vector<int>> HalfEdgeMesh::boundaryLoops() const
{
    std::vector<std::vector<int>> loops;
    std::unordered_set<int> visited;

    for (int i = 0; i < static_cast<int>(m_halfEdges.size()); ++i) {
        if (m_halfEdges[i].face != -1)
            continue; // not a boundary HE
        if (visited.count(i))
            continue;

        std::vector<int> loop;
        int he = i;
        while (he >= 0 && !visited.count(he)) {
            visited.insert(he);
            // The "from" vertex of this boundary HE
            int prev = m_halfEdges[he].prev;
            if (prev >= 0) {
                loop.push_back(m_halfEdges[prev].vertex);
            } else {
                // Fallback: get from twin
                int twin = m_halfEdges[he].twin;
                if (twin >= 0) {
                    loop.push_back(m_halfEdges[twin].vertex);
                }
            }
            he = m_halfEdges[he].next;
        }

        if (!loop.empty()) {
            loops.push_back(std::move(loop));
        }
    }

    return loops;
}

bool HalfEdgeMesh::validate() const
{
    // Check 1: Twin symmetry
    for (int i = 0; i < static_cast<int>(m_halfEdges.size()); ++i) {
        int twin = m_halfEdges[i].twin;
        if (twin >= 0) {
            if (twin >= static_cast<int>(m_halfEdges.size()))
                return false;
            if (m_halfEdges[twin].twin != i)
                return false;
        }
    }

    // Check 2: Face loop closure (each face loop returns to start)
    for (int f = 0; f < static_cast<int>(m_faces.size()); ++f) {
        int startHE = m_faces[f].halfEdge;
        if (startHE < 0)
            return false;

        int he = startHE;
        int count = 0;
        do {
            if (m_halfEdges[he].face != f)
                return false;
            he = m_halfEdges[he].next;
            if (he < 0)
                return false;
            ++count;
            if (count > 100) // safety: infinite loop detection
                return false;
        } while (he != startHE);

        // Triangles should have exactly 3 half-edges
        if (count != 3)
            return false;
    }

    // Check 3: Prev/next consistency
    for (int i = 0; i < static_cast<int>(m_halfEdges.size()); ++i) {
        int next = m_halfEdges[i].next;
        if (next >= 0 && next < static_cast<int>(m_halfEdges.size())) {
            if (m_halfEdges[next].prev != i)
                return false;
        }
    }

    // Check 4: Every vertex's half-edge actually starts from that vertex
    // (i.e., the prev of vertex's half-edge points to this vertex)
    for (int v = 0; v < static_cast<int>(m_vertices.size()); ++v) {
        int he = m_vertices[v].halfEdge;
        if (he < 0) continue;
        if (he >= static_cast<int>(m_halfEdges.size()))
            return false;

        // The half-edge stored in vertex should be an outgoing edge FROM this vertex.
        // "from" vertex of he is: prev->vertex
        int prev = m_halfEdges[he].prev;
        if (prev >= 0) {
            if (m_halfEdges[prev].vertex != v)
                return false;
        }
    }

    return true;
}
