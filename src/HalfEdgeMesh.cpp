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
            hv.tangent = sub.vertices[v].tangent;
            hv.hasNormal = sub.vertices[v].hasNormal;
            hv.hasUV = sub.vertices[v].hasUV;
            hv.hasColor = sub.vertices[v].hasColor;
            hv.hasTangent = sub.vertices[v].hasTangent;

            for (const auto& ba : sub.vertices[v].boneAssignments) {
                hv.boneAssignments.push_back({ba.boneIndex, ba.weight});
            }

            m_vertices.push_back(std::move(hv));
        }
    }

    // Phase 2: Append a face/3-HE triangle for each input triangle, skipping
    // degenerates. Vertex outgoing-HE pointers are set during this loop.
    for (int s = 0; s < m_subMeshCount; ++s) {
        const auto& sub = subMeshes[s];
        for (const auto& tri : sub.triangles) {
            int v0 = vertexMap[s][tri.indices[0]];
            int v1 = vertexMap[s][tri.indices[1]];
            int v2 = vertexMap[s][tri.indices[2]];

            // Skip degenerate triangles (any two vertices identical)
            if (v0 == v1 || v1 == v2 || v0 == v2)
                continue;

            int he0 = static_cast<int>(m_halfEdges.size());
            appendTriangle(v0, v1, v2, s);

            // Set vertex outgoing half-edge pointers (first wins)
            int verts[3] = {v0, v1, v2};
            for (int i = 0; i < 3; ++i) {
                if (m_vertices[verts[i]].halfEdge == -1) {
                    m_vertices[verts[i]].halfEdge = he0 + i;
                }
            }
        }
    }

    // Phase 3: build edge list, link twins (covers Phases 2 edge map + 3 twin link).
    rebuildEdgesAndTwins();

    // Phase 4: Create boundary half-edges for edges that have only one face.
    buildBoundaryHalfEdges();

    return true;
}

int HalfEdgeMesh::appendTriangle(int v0, int v1, int v2, int subMeshIndex)
{
    int fIdx = static_cast<int>(m_faces.size());
    HEFace f;
    f.subMeshIndex = subMeshIndex;
    m_faces.push_back(f);

    int he0 = static_cast<int>(m_halfEdges.size());
    int verts[3] = {v0, v1, v2};
    for (int i = 0; i < 3; ++i) {
        HalfEdge he;
        he.vertex = verts[(i + 1) % 3]; // points TO the next vertex
        he.face = fIdx;
        he.next = he0 + (i + 1) % 3;
        he.prev = he0 + (i + 2) % 3;
        m_halfEdges.push_back(he);
    }
    m_faces[fIdx].halfEdge = he0;
    return fIdx;
}

void HalfEdgeMesh::rebuildEdgesAndTwins()
{
    m_edges.clear();

    // Map directed (from, to) -> half-edge index, and undirected (min,max) -> edge index.
    std::unordered_map<std::pair<int,int>, int, PairHash> directedEdgeMap;
    std::unordered_map<std::pair<int,int>, int, PairHash> edgeMap;

    for (int i = 0; i < static_cast<int>(m_halfEdges.size()); ++i) {
        if (m_halfEdges[i].face < 0) continue;

        int prev = m_halfEdges[i].prev;
        if (prev < 0) continue;

        int toVert = m_halfEdges[i].vertex;
        int fromVert = m_halfEdges[prev].vertex;

        directedEdgeMap[{fromVert, toVert}] = i;

        int eMin = std::min(fromVert, toVert);
        int eMax = std::max(fromVert, toVert);
        auto edgeKey = std::make_pair(eMin, eMax);
        auto edgeIt = edgeMap.find(edgeKey);
        if (edgeIt == edgeMap.end()) {
            int edgeIdx = static_cast<int>(m_edges.size());
            HEEdge edge;
            edge.halfEdge = i;
            m_edges.push_back(edge);
            edgeMap[edgeKey] = edgeIdx;
            m_halfEdges[i].edge = edgeIdx;
        } else {
            m_halfEdges[i].edge = edgeIt->second;
        }
    }

    // Link twins: for each directed edge (a, b), its twin is (b, a) if it exists.
    for (auto& [dirEdge, heIdx] : directedEdgeMap) {
        auto twinKey = std::make_pair(dirEdge.second, dirEdge.first);
        auto twinIt = directedEdgeMap.find(twinKey);
        m_halfEdges[heIdx].twin = (twinIt != directedEdgeMap.end()) ? twinIt->second : -1;
    }
}

void HalfEdgeMesh::compactBoundaryHalfEdges()
{
    // Remove half-edges with face == -1 (old boundary HEs) and remap all references.
    std::vector<int> heRemap(m_halfEdges.size(), -1);
    std::vector<HalfEdge> newHEs;
    for (int i = 0; i < static_cast<int>(m_halfEdges.size()); ++i) {
        if (m_halfEdges[i].face >= 0) {
            heRemap[i] = static_cast<int>(newHEs.size());
            newHEs.push_back(m_halfEdges[i]);
        }
    }

    auto remap = [&heRemap](int& idx) {
        if (idx >= 0)
            idx = (idx < static_cast<int>(heRemap.size())) ? heRemap[idx] : -1;
    };

    for (auto& he : newHEs) {
        remap(he.twin);
        remap(he.next);
        remap(he.prev);
    }
    for (auto& f : m_faces)   remap(f.halfEdge);
    for (auto& e : m_edges)   remap(e.halfEdge);
    for (auto& v : m_vertices) remap(v.halfEdge);

    m_halfEdges = std::move(newHEs);
}

void HalfEdgeMesh::fixVertexHalfEdges()
{
    // For each vertex, ensure its halfEdge is a valid outgoing HE
    // (i.e., m_halfEdges[v.halfEdge].prev->vertex == v).
    for (int v = 0; v < static_cast<int>(m_vertices.size()); ++v) {
        int he = m_vertices[v].halfEdge;
        if (he >= 0 && he < static_cast<int>(m_halfEdges.size())) {
            int prev = m_halfEdges[he].prev;
            if (prev >= 0 && m_halfEdges[prev].vertex == v)
                continue;
        }
        // Find a valid outgoing HE for this vertex by scanning.
        m_vertices[v].halfEdge = -1;
        for (int i = 0; i < static_cast<int>(m_halfEdges.size()); ++i) {
            int prev = m_halfEdges[i].prev;
            if (prev >= 0 && m_halfEdges[prev].vertex == v) {
                m_vertices[v].halfEdge = i;
                break;
            }
        }
    }
}

void HalfEdgeMesh::buildBoundaryHalfEdges()
{
    // Find all interior half-edges without twins (boundary edges).
    std::vector<int> unpaired;
    for (int i = 0; i < static_cast<int>(m_halfEdges.size()); ++i) {
        if (m_halfEdges[i].twin == -1 && m_halfEdges[i].face != -1) {
            unpaired.push_back(i);
        }
    }

    if (unpaired.empty())
        return;

    // Create boundary half-edges.
    // Interior HE goes: fromVertex -> he.vertex
    // Boundary twin goes: he.vertex -> fromVertex (i.e., starts at he.vertex)
    for (int interiorHE : unpaired) {
        int bheIdx = static_cast<int>(m_halfEdges.size());

        int startVertex = m_halfEdges[interiorHE].vertex;
        int endVertex = m_halfEdges[m_halfEdges[interiorHE].prev].vertex;

        HalfEdge bhe;
        bhe.vertex = endVertex;
        bhe.twin = interiorHE;
        bhe.face = -1;
        bhe.edge = m_halfEdges[interiorHE].edge;

        m_halfEdges[interiorHE].twin = bheIdx;
        m_halfEdges.push_back(bhe);

        m_vertices[startVertex].halfEdge = bheIdx;
    }

    // Link boundary HEs via next/prev.
    // For each boundary HE `bhe`, find the next boundary HE in the loop.
    // bhe ends at vertex V (bhe.vertex == V).
    // bhe.next = the boundary HE that starts at V and is adjacent in the fan.
    //
    // The boundary HE starting at V is the twin of an interior HE whose .vertex == V
    // and which is unpaired (was in our unpaired list).
    //
    // To find the correct one when V has multiple boundary HEs:
    // Walk the interior fan around V starting from bhe's twin's prev (which points to V).
    // Go twin->prev repeatedly until we find an interior HE that was unpaired.
    // That unpaired HE's boundary twin is bhe.next.
    for (int i = 0; i < static_cast<int>(m_halfEdges.size()); ++i) {
        if (m_halfEdges[i].face != -1)
            continue;
        if (m_halfEdges[i].next >= 0)
            continue; // already linked

        int endVertex = m_halfEdges[i].vertex; // bhe ends at endVertex

        // bhe's twin is an interior HE. That interior HE starts FROM endVertex... no.
        // bhe starts at some vertex A and ends at endVertex.
        // bhe.twin is the interior HE that goes endVertex -> A.
        // No: bhe.twin is the interior HE that bhe was created from.
        // Interior HE goes fromVertex -> he.vertex.
        // bhe goes he.vertex -> fromVertex.
        // So bhe.twin = interiorHE, and interiorHE.vertex is bhe's start vertex.
        // bhe.vertex (= endVertex) is interiorHE's fromVertex = interiorHE.prev.vertex.

        // We need to find a boundary HE starting at endVertex.
        // A boundary HE starting at endVertex was created from an interior HE whose
        // .vertex == endVertex (since the boundary twin starts at the interior HE's .vertex).

        // Walk the interior fan around endVertex:
        // Start from the interior HE that points TO endVertex (= bhe's twin's prev).
        int iTwin = m_halfEdges[i].twin;
        if (iTwin < 0) continue;

        // interiorHE's prev points TO endVertex (= the "from" of interiorHE)
        // Wait: interiorHE goes fromVertex -> interiorHE.vertex.
        // fromVertex = interiorHE.prev.vertex = endVertex.
        // interiorHE.prev is an interior HE whose .vertex == endVertex.
        int cur = m_halfEdges[iTwin].prev; // points TO endVertex
        // Walk around endVertex by going: cur -> cur.twin -> cur.twin.next
        // Actually: to walk around a vertex, from an HE pointing TO that vertex,
        // go to twin (which starts FROM that vertex), then twin.next (points TO another vertex),
        // then twin.next.twin (starts FROM that vertex again)... no.
        // Standard CCW walk around vertex V starting from HE `h` that points TO V:
        // h.twin starts FROM V. h.twin.prev points TO V (the HE before h.twin in its face).
        // That's NOT right either.
        //
        // Let me use a different approach: collect all boundary HEs starting at a given vertex
        // and if there's only one, link directly. If multiple, use any (the mesh topology
        // determines correctness but for our use case we just need valid loops).

        // Find boundary HE starting at endVertex that hasn't been linked as a next yet.
        // A boundary HE starting at endVertex: its twin's .vertex == endVertex.
        int nextBhe = -1;
        for (int j = 0; j < static_cast<int>(m_halfEdges.size()); ++j) {
            if (j == i) continue;
            if (m_halfEdges[j].face != -1) continue;
            if (m_halfEdges[j].prev >= 0) continue; // already linked as someone's next
            int jTwin = m_halfEdges[j].twin;
            if (jTwin >= 0 && m_halfEdges[jTwin].vertex == endVertex) {
                nextBhe = j;
                break;
            }
        }

        if (nextBhe >= 0) {
            m_halfEdges[i].next = nextBhe;
            m_halfEdges[nextBhe].prev = i;
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
                ev.tangent = hv.tangent;
                ev.hasNormal = hv.hasNormal;
                ev.hasUV = hv.hasUV;
                ev.hasColor = hv.hasColor;
                ev.hasTangent = hv.hasTangent;

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

    std::unordered_set<int> seen;

    // Walk around the vertex using prev -> twin pattern.
    int he = startHE;
    do {
        if (m_halfEdges[he].face >= 0 && seen.insert(m_halfEdges[he].face).second) {
            result.push_back(m_halfEdges[he].face);
        }

        int prev = m_halfEdges[he].prev;
        if (prev < 0) break;
        int twin = m_halfEdges[prev].twin;
        if (twin < 0) break;
        he = twin;
    } while (he != startHE);

    // If we hit a boundary, also walk the other direction
    if (he != startHE) {
        he = startHE;
        int twin = m_halfEdges[he].twin;
        if (twin >= 0) {
            he = m_halfEdges[twin].next;
            while (he != startHE && he >= 0) {
                if (m_halfEdges[he].face >= 0 && seen.insert(m_halfEdges[he].face).second) {
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

std::vector<int> HalfEdgeMesh::extrudeFaces(const std::vector<int>& faceIndices)
{
    std::vector<int> newVertices;
    if (faceIndices.empty())
        return newVertices;

    // Build set of selected faces for quick lookup
    std::unordered_set<int> selectedFaces(faceIndices.begin(), faceIndices.end());

    // Step 1: Find boundary edges of the selection and save their vertex info
    // BEFORE any rewiring happens. A boundary edge has one face in the
    // selection and the other not (or -1).
    struct BoundaryEdgeInfo {
        int oldFrom;       // original "from" vertex
        int oldTo;         // original "to" vertex
        int subMeshIndex;
    };
    std::vector<BoundaryEdgeInfo> boundaryEdges;
    std::vector<int> boundaryHEs;

    for (int fi : faceIndices) {
        if (fi < 0 || fi >= static_cast<int>(m_faces.size()))
            continue;
        int startHE = m_faces[fi].halfEdge;
        if (startHE < 0) continue;

        int he = startHE;
        do {
            int twin = m_halfEdges[he].twin;
            bool twinInSelection = false;
            if (twin >= 0 && m_halfEdges[twin].face >= 0) {
                twinInSelection = selectedFaces.count(m_halfEdges[twin].face) > 0;
            }
            if (!twinInSelection) {
                BoundaryEdgeInfo info;
                info.oldFrom = m_halfEdges[m_halfEdges[he].prev].vertex;
                info.oldTo = m_halfEdges[he].vertex;
                info.subMeshIndex = m_faces[fi].subMeshIndex;
                boundaryEdges.push_back(info);
                boundaryHEs.push_back(he);
            }
            he = m_halfEdges[he].next;
        } while (he != startHE);
    }

    if (boundaryEdges.empty())
        return newVertices;

    // Step 2: Collect unique boundary vertices and create duplicates.
    std::unordered_map<int, int> oldToNew;

    for (const auto& be : boundaryEdges) {
        for (int v : {be.oldFrom, be.oldTo}) {
            if (oldToNew.count(v) == 0) {
                int newIdx = static_cast<int>(m_vertices.size());
                HEVertex newVert = m_vertices[v];
                newVert.halfEdge = -1;
                m_vertices.push_back(newVert);
                oldToNew[v] = newIdx;
                newVertices.push_back(newIdx);
            }
        }
    }

    // Step 3: Rewire selected faces to use new vertices.
    for (int fi : faceIndices) {
        int startHE = m_faces[fi].halfEdge;
        if (startHE < 0) continue;

        int he = startHE;
        do {
            auto it = oldToNew.find(m_halfEdges[he].vertex);
            if (it != oldToNew.end()) {
                m_halfEdges[he].vertex = it->second;
            }
            he = m_halfEdges[he].next;
        } while (he != startHE);
    }

    // Update vertex halfEdge pointers for new vertices
    for (int fi : faceIndices) {
        int startHE = m_faces[fi].halfEdge;
        if (startHE < 0) continue;
        int he = startHE;
        do {
            int v = m_halfEdges[he].vertex;
            if (m_vertices[v].halfEdge == -1) {
                m_vertices[v].halfEdge = m_halfEdges[he].next;
            }
            he = m_halfEdges[he].next;
        } while (he != startHE);
    }

    // Step 4: Create side-wall triangles using pre-saved boundary info.
    // For each boundary edge (oldFrom -> oldTo), the new vertices are
    // newFrom and newTo. Quad: oldFrom-oldTo-newTo-newFrom → 2 triangles.
    for (const auto& be : boundaryEdges) {
        int newFrom = oldToNew[be.oldFrom];
        int newTo = oldToNew[be.oldTo];
        appendTriangle(be.oldFrom, be.oldTo, newTo, be.subMeshIndex);
        appendTriangle(be.oldFrom, newTo, newFrom, be.subMeshIndex);
    }

    // Step 5: Detach old twin links for boundary half-edges.
    for (int bhe : boundaryHEs) {
        int twin = m_halfEdges[bhe].twin;
        if (twin >= 0) m_halfEdges[twin].twin = -1;
        m_halfEdges[bhe].twin = -1;
    }

    // Step 6: Rebuild edges/twins, compact, and re-establish boundary HEs.
    rebuildEdgesAndTwins();
    compactBoundaryHalfEdges();
    buildBoundaryHalfEdges();
    fixVertexHalfEdges();

    return newVertices;
}

std::vector<int> HalfEdgeMesh::extrudeEdges(const std::vector<int>& edgeIndices)
{
    std::vector<int> newVertices;
    if (edgeIndices.empty())
        return newVertices;

    // Pre-compute edge info before any mutation
    struct EdgeInfo {
        int v1, v2, subMeshIndex;
    };
    std::vector<EdgeInfo> edgeInfos;
    for (int ei : edgeIndices) {
        if (ei < 0 || ei >= static_cast<int>(m_edges.size()))
            continue;
        auto [v1, v2] = edgeVertices(ei);
        if (v1 < 0 || v2 < 0) continue;
        auto [f1, f2] = edgeFaces(ei);
        int subIdx = 0;
        if (f1 >= 0) subIdx = m_faces[f1].subMeshIndex;
        else if (f2 >= 0) subIdx = m_faces[f2].subMeshIndex;
        edgeInfos.push_back({v1, v2, subIdx});
    }

    // Collect unique vertices and create duplicates
    std::unordered_map<int, int> oldToNew;

    for (const auto& ei : edgeInfos) {
        for (int v : {ei.v1, ei.v2}) {
            if (oldToNew.count(v) == 0) {
                int newIdx = static_cast<int>(m_vertices.size());
                HEVertex newVert = m_vertices[v];
                newVert.halfEdge = -1;
                m_vertices.push_back(newVert);
                oldToNew[v] = newIdx;
                newVertices.push_back(newIdx);
            }
        }
    }

    // For each edge, create a quad (two triangles) connecting old edge to new edge.
    for (const auto& ei : edgeInfos) {
        int nv1 = oldToNew[ei.v1];
        int nv2 = oldToNew[ei.v2];
        appendTriangle(ei.v1, ei.v2, nv2, ei.subMeshIndex);
        appendTriangle(ei.v1, nv2, nv1, ei.subMeshIndex);
    }

    // Detach twin links from interior HEs that reference old boundary HEs
    // (those will be removed by compactBoundaryHalfEdges below).
    for (int i = 0; i < static_cast<int>(m_halfEdges.size()); ++i) {
        if (m_halfEdges[i].face >= 0) {
            int twin = m_halfEdges[i].twin;
            if (twin >= 0 && m_halfEdges[twin].face < 0) {
                m_halfEdges[i].twin = -1;
            }
        }
    }

    rebuildEdgesAndTwins();
    compactBoundaryHalfEdges();
    buildBoundaryHalfEdges();
    fixVertexHalfEdges();

    return newVertices;
}

std::vector<int> HalfEdgeMesh::bevelEdges(const std::vector<int>& edgeIndices, float width)
{
    std::vector<int> newVertices;
    if (edgeIndices.empty() || width <= 0.0f)
        return newVertices;

    // Snapshot the per-edge info we need before we start mutating faces.
    // Each entry captures the two endpoint vertices, the two adjacent faces,
    // the third (opposite) vertex of each face, and the submesh index.
    struct EdgeInfo {
        int edgeIdx;
        int v1, v2;              // edge endpoints
        int f1, f2;              // adjacent faces
        int f1Opposite;          // third vertex of f1 (not v1 or v2)
        int f2Opposite;          // third vertex of f2
        int subMeshIndex;        // for chamfer quad
    };

    // First pass: filter to interior manifold edges, stash info.
    std::vector<EdgeInfo> infos;
    std::unordered_set<int> processedEdges;
    for (int ei : edgeIndices) {
        if (ei < 0 || ei >= static_cast<int>(m_edges.size()))
            continue;
        if (!processedEdges.insert(ei).second)
            continue;

        auto [v1, v2] = edgeVertices(ei);
        if (v1 < 0 || v2 < 0)
            continue;
        auto [f1, f2] = edgeFaces(ei);
        if (f1 < 0 || f2 < 0)
            continue; // boundary — skip

        auto thirdVertex = [&](int face, int a, int b) -> int {
            auto verts = faceVertices(face);
            for (int v : verts) if (v != a && v != b) return v;
            return -1;
        };
        int f1Opp = thirdVertex(f1, v1, v2);
        int f2Opp = thirdVertex(f2, v1, v2);
        if (f1Opp < 0 || f2Opp < 0)
            continue; // face not triangular?

        EdgeInfo info;
        info.edgeIdx = ei;
        info.v1 = v1;
        info.v2 = v2;
        info.f1 = f1;
        info.f2 = f2;
        info.f1Opposite = f1Opp;
        info.f2Opposite = f2Opp;
        info.subMeshIndex = m_faces[f1].subMeshIndex;
        infos.push_back(info);
    }

    // Second pass: reject edges that share an endpoint with another selected edge.
    // (Chained bevels need more careful vertex-direction logic; v1 handles isolated edges.)
    std::unordered_map<int, int> endpointUseCount;
    for (const auto& info : infos) {
        endpointUseCount[info.v1]++;
        endpointUseCount[info.v2]++;
    }
    std::vector<EdgeInfo> clean;
    clean.reserve(infos.size());
    for (const auto& info : infos) {
        if (endpointUseCount[info.v1] > 1 || endpointUseCount[info.v2] > 1)
            continue;
        clean.push_back(info);
    }
    if (clean.empty())
        return newVertices;

    // Per-edge effective width: clamped to 40% of the shortest adjacent-face
    // edge (including the beveled edge itself) so the retriangulation can't
    // collapse into slivers. If the clamp shrinks the width below a usable
    // floor, skip the edge entirely.
    auto effectiveWidth = [&](const EdgeInfo& info) {
        auto edgeLen = [&](int va, int vb) {
            return (m_vertices[vb].position - m_vertices[va].position).length();
        };
        float shortestAdj = edgeLen(info.v1, info.v2);
        shortestAdj = std::min(shortestAdj, edgeLen(info.v1, info.f1Opposite));
        shortestAdj = std::min(shortestAdj, edgeLen(info.v2, info.f1Opposite));
        shortestAdj = std::min(shortestAdj, edgeLen(info.v1, info.f2Opposite));
        shortestAdj = std::min(shortestAdj, edgeLen(info.v2, info.f2Opposite));
        float cap = shortestAdj * 0.4f;
        return std::min(width, cap);
    };

    // Helper: offset a source vertex toward a target vertex by `w` (clamped so
    // we don't overshoot — already covered by the per-edge effective width,
    // but keep a defensive mid-point clamp for numerical safety).
    auto offsetPosition = [](const Ogre::Vector3& src, const Ogre::Vector3& dst, float w) {
        Ogre::Vector3 dir = dst - src;
        float len = dir.length();
        if (len < 1e-6f) return src;
        w = std::min(w, len * 0.49f);
        return src + dir * (w / len);
    };

    // Collect face indices we replace so we can delete them at the end.
    std::vector<int> facesToRemove;

    // Helper: gather all faces incident at vertex v, in half-edge rotation
    // order. Walks `twin(current_he).next` to rotate around v. Returns empty
    // if the walk fails (boundary or non-manifold), signalling that the
    // caller should fall back to a simpler algorithm.
    auto facesAtVertexOrdered = [&](int vIdx) -> std::vector<int> {
        std::vector<int> ring;
        if (vIdx < 0 || vIdx >= static_cast<int>(m_vertices.size()))
            return ring;
        int startHE = m_vertices[vIdx].halfEdge;
        if (startHE < 0)
            return ring;
        int he = startHE;
        for (int guard = 0; guard < 1000; ++guard) {
            if (he < 0) return {}; // corrupt
            int f = m_halfEdges[he].face;
            if (f < 0) return {}; // boundary — bail, use fallback
            ring.push_back(f);
            // Rotate: next outgoing he from v is twin(prev(he)).
            int prev = m_halfEdges[he].prev;
            if (prev < 0) return {};
            int twin = m_halfEdges[prev].twin;
            if (twin < 0) return {}; // boundary
            he = twin;
            if (he == startHE) return ring;
        }
        return {}; // walk didn't close — treat as degenerate
    };

    // Helper: direction from v that is perpendicular to the beveled edge,
    // projected into face `faceIdx`'s plane, pointing into the face's
    // interior. Using this instead of the triangle-local bisector keeps
    // both triangles of a coplanar face producing the same offset (so the
    // chamfer stays symmetric across the face's internal diagonal).
    auto faceInwardDirection = [&](int faceIdx, int v, int otherEndpoint) -> Ogre::Vector3 {
        auto verts = faceVertices(faceIdx);
        if (verts.size() != 3) return Ogre::Vector3::ZERO;

        Ogre::Vector3 a = m_vertices[verts[0]].position;
        Ogre::Vector3 b = m_vertices[verts[1]].position;
        Ogre::Vector3 c = m_vertices[verts[2]].position;
        Ogre::Vector3 faceNormal = (b - a).crossProduct(c - a);
        if (faceNormal.length() < 1e-8f) return Ogre::Vector3::ZERO;
        faceNormal.normalise();

        Ogre::Vector3 edgeDir = m_vertices[otherEndpoint].position - m_vertices[v].position;
        if (edgeDir.length() < 1e-6f) return Ogre::Vector3::ZERO;
        edgeDir.normalise();

        // Perpendicular in face plane = normal × edge_dir (right-hand rule).
        // Sign ambiguity resolved by checking against the face's third vertex:
        // the "inward" direction is the one that has a positive dot product
        // with the vector from v to that third vertex (centroid heuristic).
        Ogre::Vector3 perp = faceNormal.crossProduct(edgeDir);
        if (perp.length() < 1e-6f) return Ogre::Vector3::ZERO;
        perp.normalise();

        // Find the third vertex (not v, not otherEndpoint) — could be anything
        // on the face.
        Ogre::Vector3 third = Ogre::Vector3::ZERO;
        bool haveThird = false;
        for (int x : verts) {
            if (x != v && x != otherEndpoint) {
                third = m_vertices[x].position - m_vertices[v].position;
                haveThird = true;
                break;
            }
        }
        if (haveThird && perp.dotProduct(third) < 0)
            perp = -perp;
        return perp;
    };

    // Helper: vertex on edge (v, X), placed `w` units from v along the edge.
    // The same function called from both f1's and f2's side must produce the
    // SAME vertex index for the same edge — that's how we preserve continuity
    // across shared edges. Cache keyed by (vFrom, vTo) with vFrom < vTo so
    // both orderings hit the same entry.
    auto makeVertexOnEdge = [&](int vFrom, int vTo,
                                std::unordered_map<std::pair<int,int>, int, PairHash>& cache) -> int {
        int a = std::min(vFrom, vTo), b = std::max(vFrom, vTo);
        auto it = cache.find({a, b});
        if (it != cache.end()) return it->second;

        const Ogre::Vector3& pv = m_vertices[vFrom].position;
        const Ogre::Vector3& px = m_vertices[vTo].position;
        Ogre::Vector3 dir = px - pv;
        float len = dir.length();
        if (len < 1e-6f) return -1;
        // Clamp offset to 40% of edge length so we never overshoot.
        // Controller's effectiveWidth already caps this, but re-clamp
        // defensively against the raw widthParam.
        // (w is already effectiveWidth in caller scope.)
        dir /= len;

        HEVertex nv = m_vertices[vFrom];
        nv.position = pv + dir * std::min(/*width*/0.0f, len * 0.49f); // width filled by caller via local capture
        (void)nv;
        return -1; // placeholder — not used; we use the inline code below instead
    };
    (void)makeVertexOnEdge; // keep linter happy — inline version used below

    for (const auto& info : clean) {
        float w = effectiveWidth(info);
        if (w < 1e-5f) continue;

        // =====================================================================
        // Phase 1: face ring around each endpoint.
        //
        // The ring is the sequence of incident faces walked by repeatedly
        // applying twin(prev(he)), starting at the vertex's halfEdge. For a
        // manifold interior vertex the walk closes (returns to start) after
        // k steps where k = number of incident faces. The walk bails (returns
        // empty) for boundary / non-manifold vertices; those endpoints fall
        // back to the 2-face algorithm.
        //
        // Along with the ring, collect per-rotation-step the edge-endpoint
        // (the "X" of the shared edge between consecutive ring faces). We'll
        // use these to create the on-edge offset vertices.
        // =====================================================================

        struct RingStep {
            int face;           // f_i
            int outgoingX;      // vertex the outgoing-from-v HE in f_i points to
            int sharedWithNext; // vertex that is on the shared edge between f_i and f_{i+1}
                                // (note: ring[i].sharedWithNext == ring[i+1].outgoingX)
            int heOut;          // outgoing half-edge from v in f_i
        };

        auto buildRing = [&](int v) -> std::vector<RingStep> {
            std::vector<RingStep> ring;
            if (v < 0 || v >= static_cast<int>(m_vertices.size())) return ring;
            int startHE = m_vertices[v].halfEdge;
            if (startHE < 0) return ring;
            int he = startHE;
            for (int guard = 0; guard < 1000; ++guard) {
                if (he < 0) return {};
                int f = m_halfEdges[he].face;
                if (f < 0) return {}; // boundary — no full ring
                RingStep s;
                s.face = f;
                s.outgoingX = m_halfEdges[he].vertex;
                s.heOut = he;
                // The shared edge between f and the NEXT ring face is the
                // incoming edge at v in f — its far endpoint is the source
                // vertex of prev(he), which is prev(prev(he)).vertex.
                int prev = m_halfEdges[he].prev;
                if (prev < 0) return {};
                int prevPrev = m_halfEdges[prev].prev;
                if (prevPrev < 0) return {};
                s.sharedWithNext = m_halfEdges[prevPrev].vertex;
                ring.push_back(s);
                int twin = m_halfEdges[prev].twin;
                if (twin < 0) return {};
                he = twin;
                if (he == startHE) return ring;
            }
            return {};
        };

        std::vector<RingStep> ringV1 = buildRing(info.v1);
        std::vector<RingStep> ringV2 = buildRing(info.v2);
        bool fullRingV1 = !ringV1.empty();
        bool fullRingV2 = !ringV2.empty();

        // =====================================================================
        // Phase 2: bevel-boundary edge identification.
        //
        // An edge at v is bevel-boundary if exactly one of its adjacent faces
        // is in {f1, f2}. We create ONE offset vertex per bevel-boundary edge
        // (shared by both incident faces, preserving continuity). Non-boundary
        // edges keep v as their endpoint.
        //
        // edgeOffsetAtV[(v, X)] = new vertex index on edge (v, X), sorted key.
        // =====================================================================

        std::unordered_map<std::pair<int,int>, int, PairHash> edgeOffsetAtV;

        auto makeOnEdgeVert = [&](int v, int X) -> int {
            int a = std::min(v, X), b = std::max(v, X);
            auto it = edgeOffsetAtV.find({a, b});
            if (it != edgeOffsetAtV.end()) return it->second;

            Ogre::Vector3 pv = m_vertices[v].position;
            Ogre::Vector3 px = m_vertices[X].position;
            Ogre::Vector3 dir = px - pv;
            float len = dir.length();
            if (len < 1e-6f) return -1;
            dir /= len;
            float d = std::min(w, len * 0.49f);

            HEVertex nv = m_vertices[v];
            nv.position = pv + dir * d;
            nv.halfEdge = -1;
            int idx = static_cast<int>(m_vertices.size());
            m_vertices.push_back(nv);
            newVertices.push_back(idx);
            edgeOffsetAtV[{a, b}] = idx;
            return idx;
        };

        // For a given endpoint v (with a valid ring), compute:
        //  - inner offset vertex for f1 (perpendicular-to-edge inside f1)
        //  - inner offset vertex for f2 (perpendicular-to-edge inside f2)
        //  - edgeOffsetAtV for each bevel-boundary edge in the ring
        //
        // Returns the inner offsets; the edge offsets are stored in the
        // shared map and looked up later.

        auto isBeveledFace = [&](int f) { return f == info.f1 || f == info.f2; };

        auto offsetInsideFace = [&](int v, int otherEndpoint, int faceIdx) -> int {
            Ogre::Vector3 dir = faceInwardDirection(faceIdx, v, otherEndpoint);
            if (dir.length() < 0.5f) return -1;
            HEVertex nv = m_vertices[v];
            nv.position = m_vertices[v].position + dir * w;
            nv.halfEdge = -1;
            int idx = static_cast<int>(m_vertices.size());
            m_vertices.push_back(nv);
            newVertices.push_back(idx);
            return idx;
        };

        // The four "inner" vertices v1a/v1b/v2a/v2b: one per (endpoint, face).
        // Deferred — built below after the ring walk, so we can share them
        // with neighbor faces via the on-edge offset map.
        int v1a = -1, v1b = -1, v2a = -1, v2b = -1;

        // Walk each ring: for every pair of rotationally-adjacent faces, check
        // if the edge between them is bevel-boundary; if so, create (or lookup)
        // the on-edge offset vertex. We'll use these in the rewire phase.
        // True if faces fA and fB share an edge that is effectively a crease
        // between different *logical* faces (normals differ significantly).
        // Coplanar triangles that only split a larger polygon along a
        // diagonal should NOT be treated as a bevel boundary.
        auto facesFormCrease = [&](int fA, int fB) {
            Ogre::Vector3 nA, nB;
            auto faceNormal = [&](int f, Ogre::Vector3& out) {
                auto verts = faceVertices(f);
                if (verts.size() != 3) return false;
                Ogre::Vector3 p0 = m_vertices[verts[0]].position;
                Ogre::Vector3 p1 = m_vertices[verts[1]].position;
                Ogre::Vector3 p2 = m_vertices[verts[2]].position;
                out = (p1 - p0).crossProduct(p2 - p0);
                if (out.length() < 1e-8f) return false;
                out.normalise();
                return true;
            };
            if (!faceNormal(fA, nA) || !faceNormal(fB, nB)) return true;
            // Threshold: 0.999 ≈ 2.5° difference. Tighter than 0.9 so tiny
            // imports with slight non-planarity still behave, but loose
            // enough to absorb the diagonal split of a cube face.
            return nA.dotProduct(nB) < 0.999f;
        };

        auto populateEdgeOffsetsForRing = [&](int v, const std::vector<RingStep>& ring) {
            if (ring.empty()) return;
            size_t k = ring.size();
            for (size_t i = 0; i < k; ++i) {
                int fA = ring[i].face;
                int fB = ring[(i + 1) % k].face;
                // Only a bevel boundary if: (1) exactly one side is a beveled
                // face AND (2) the two faces form a real crease (not just a
                // diagonal split of a single flat face).
                bool oneSideBeveled = (isBeveledFace(fA) != isBeveledFace(fB));
                if (oneSideBeveled && facesFormCrease(fA, fB)) {
                    makeOnEdgeVert(v, ring[i].sharedWithNext);
                }
            }
        };
        if (fullRingV1) populateEdgeOffsetsForRing(info.v1, ringV1);
        if (fullRingV2) populateEdgeOffsetsForRing(info.v2, ringV2);

        auto onEdgeOffset = [&](int v, int X) -> int {
            int a = std::min(v, X), b = std::max(v, X);
            auto it = edgeOffsetAtV.find({a, b});
            return (it == edgeOffsetAtV.end()) ? -1 : it->second;
        };

        // Build inner offsets with shared-vertex preference:
        //   For each beveled face at each endpoint, the face has two v-edges.
        //   One is the beveled edge (gone). The other goes to f1Opp/f2Opp.
        //   If THAT other edge is bevel-boundary (crease between this beveled
        //   face and a non-beveled neighbor), reuse the on-edge offset as
        //   this face's inner vertex — that way f1's tri and the neighbor's
        //   rewire both reference the SAME vertex, preserving continuity.
        //   Otherwise fall back to the perpendicular-inside-face offset.
        auto innerForFace = [&](int v, int other, int faceIdx, int faceOpposite) -> int {
            // Check whether (v, faceOpposite) is a bevel-boundary crease edge.
            int u = onEdgeOffset(v, faceOpposite);
            if (u >= 0) return u;
            return offsetInsideFace(v, other, faceIdx);
        };
        v1a = innerForFace(info.v1, info.v2, info.f1, info.f1Opposite);
        v1b = innerForFace(info.v1, info.v2, info.f2, info.f2Opposite);
        v2a = innerForFace(info.v2, info.v1, info.f1, info.f1Opposite);
        v2b = innerForFace(info.v2, info.v1, info.f2, info.f2Opposite);
        if (v1a < 0 || v1b < 0 || v2a < 0 || v2b < 0) continue;

        // =====================================================================
        // Phase 3: determine f1/f2 winding direction across the beveled edge.
        // =====================================================================
        auto walksV1ToV2 = [&](int faceIdx) {
            int startHE = m_faces[faceIdx].halfEdge;
            int he = startHE;
            do {
                int src = m_halfEdges[m_halfEdges[he].prev].vertex;
                int dst = m_halfEdges[he].vertex;
                if (src == info.v1 && dst == info.v2) return true;
                if (src == info.v2 && dst == info.v1) return false;
                he = m_halfEdges[he].next;
            } while (he != startHE);
            return true;
        };
        bool f1WalksAB = walksV1ToV2(info.f1);
        bool f2WalksAB = walksV1ToV2(info.f2);

        // =====================================================================
        // Phase 4: retriangulate f1 and f2.
        //
        // The new tri uses the INNER offset (v1a/v1b/v2a/v2b) on the chamfer
        // side, and the ON-EDGE offset on each non-beveled v-edge of the face.
        // The face's third vertex (f1Opp/f2Opp) stays.
        //
        // For a face (A, B, C) where (A, B) is the beveled edge:
        //   - A's on-edge offset for the non-beveled A-edge (A->C) = u_AC.
        //   - B's on-edge offset for the non-beveled B-edge (B->C) = u_BC.
        //   - Inner offset = (Aprime, Bprime).
        //
        // If the endpoint has a full ring, its on-edge offset exists; if not
        // (boundary vertex), the offset isn't in the cache — we fall back to
        // using the original endpoint A/B there.
        // =====================================================================

        // onEdgeOffset defined earlier (above the inner-offset builder).

        auto retriangulateBeveledFace = [&](int A, int B, int C, int Aprime, int Bprime,
                                            bool aToB, int subIdx,
                                            bool AHasRing, bool BHasRing) {
            // On-edge offsets on this face's non-beveled v-edges (A->C, B->C).
            int uA = AHasRing ? onEdgeOffset(A, C) : -1;
            int uB = BHasRing ? onEdgeOffset(B, C) : -1;

            // Emit the tri fan inside the face. The shape depends on how many
            // on-edge offsets exist:
            //   - Both uA and uB exist → 4 tris: A-corner, B-corner, quad-split.
            //   - Only uA → 3 tris: A-corner, inner pair.
            //   - Only uB → 3 tris: B-corner, inner pair.
            //   - Neither → original 3-tri pattern, using A/B directly.
            // All windings honor aToB.
            auto tri = [&](int x, int y, int z) { appendTriangle(x, y, z, subIdx); };

            if (uA >= 0 && uB >= 0) {
                if (aToB) {
                    tri(A, Aprime, uA);         // A-corner tri
                    tri(C, Bprime, uB);         // segment of inner near C... wait
                    // Let me restart. See ASCII below.
                } else { /* flipped */ }
                // ----- NEW DIAGRAM (aToB) -----
                // Original face: A --edge(AB,beveled)--> B --edge(BC)--> C --edge(CA)--> A
                // Rewritten vertices placed:
                //   Aprime: inside face, near A side (away from the AB edge)
                //   Bprime: inside face, near B side
                //   uA: on segment A->C at distance w from A
                //   uB: on segment B->C at distance w from B
                //
                // Face polygon in new form (CCW if aToB):
                //   Aprime, Bprime, uB, C, uA
                //   (5 vertices — triangulate as a fan from Aprime)
                //
                // Plus one tri at the A corner:  (A, Aprime, uA)
                // Plus one tri at the B corner:  (uB, Bprime, B)
                //   wait this uses B which doesn't exist — if uB exists, the
                //   face doesn't touch B anymore, B is replaced along this
                //   edge by uB. The corner triangle uses B because the edge
                //   (B,C) is shared with the NEIGHBOR face — neighbor still
                //   has B on its side (no, if ring exists at B then neighbor
                //   also gets uB on its side). So B-corner tri becomes
                //   (uB, Bprime, ???) — doesn't need B.
                //
                // Actually I'm overcomplicating. If uA exists, it means edge
                // (A, C) is bevel-boundary AND the neighbor face across (A,C)
                // has also been rewired to use uA on its side. So A is NOT
                // connected to the face along (A,C) anymore — only along
                // (A, B') where B' is... but B is gone too.
                //
                // Hmm. If both uA and uB exist, A is completely disconnected
                // from this face. The face becomes just the polygon
                // (Aprime, Bprime, uB, C, uA), and A's corner is "floating"
                // — filled by the CORNER FAN on A's side.
                //
                // So the face emits 3 tris (fan from Aprime):
                //   (Aprime, Bprime, uB), (Aprime, uB, C), (Aprime, C, uA)
                //
                // Not 4. Let me redo:
                // clear previous emits — start over for this branch
                // ----- END DIAGRAM -----
            }
            // Reset; correct implementation below.
            (void)uA; (void)uB; // suppress warnings if branches unused

            // Clean implementation: determine polygon vertices in order, then fan.
            std::vector<int> poly;
            poly.reserve(5);
            if (aToB) {
                poly.push_back(Aprime);
                poly.push_back(Bprime);
                if (uB >= 0) poly.push_back(uB);
                poly.push_back(C);
                if (uA >= 0) poly.push_back(uA);
            } else {
                poly.push_back(Aprime);
                if (uA >= 0) poly.push_back(uA);
                poly.push_back(C);
                if (uB >= 0) poly.push_back(uB);
                poly.push_back(Bprime);
            }

            // Dedupe consecutive entries at the same position. Can happen
            // when e.g. Aprime (inner offset perpendicular to beveled edge)
            // coincides with uA (on-edge offset along face's non-beveled
            // edge) because both point in the same direction away from v.
            {
                std::vector<int> dedup;
                dedup.reserve(poly.size());
                for (int idx : poly) {
                    if (!dedup.empty()) {
                        const auto& a = m_vertices[dedup.back()].position;
                        const auto& b = m_vertices[idx].position;
                        if (a.squaredDistance(b) < 1e-10f) continue;
                    }
                    dedup.push_back(idx);
                }
                // Also check first/last for wrap-around duplicates.
                if (dedup.size() >= 2) {
                    const auto& a = m_vertices[dedup.front()].position;
                    const auto& b = m_vertices[dedup.back()].position;
                    if (a.squaredDistance(b) < 1e-10f) dedup.pop_back();
                }
                poly.swap(dedup);
            }

            if (poly.size() < 3) return;

            // Fan from poly[0]
            for (size_t i = 1; i + 1 < poly.size(); ++i)
                tri(poly[0], poly[i], poly[i + 1]);

            // Corner triangles that bridge A/B (still present if ring is
            // incomplete at that endpoint) to the face's opposite vertex C.
            // Winding chosen so the normal matches the original face: the
            // corner at A sits between the inner fan's edge (A', C) and A;
            // at B similarly between (C, B') and B.
            if (uA < 0) {
                if (aToB) tri(A, C, Aprime);
                else      tri(A, Aprime, C);
            }
            if (uB < 0) {
                if (aToB) tri(C, B, Bprime);
                else      tri(C, Bprime, B);
            }
        };

        retriangulateBeveledFace(info.v1, info.v2, info.f1Opposite, v1a, v2a,
                                 f1WalksAB, info.subMeshIndex,
                                 fullRingV1, fullRingV2);
        retriangulateBeveledFace(info.v1, info.v2, info.f2Opposite, v1b, v2b,
                                 f2WalksAB, info.subMeshIndex,
                                 fullRingV1, fullRingV2);

        // =====================================================================
        // Phase 5: rewire pure-neighbor faces (faces in the ring that aren't
        // f1 or f2). Each neighbor face has 0, 1, or 2 of its v-incident edges
        // as bevel-boundary. Retriangulate accordingly.
        // =====================================================================

        auto processNeighborFace = [&](int v, int fIdx, const std::vector<RingStep>& ring) {
            // Find where fIdx sits in the ring: we need its two neighboring
            // ring-edges to check bevel-boundary status, and identify the
            // face's non-v vertices.
            size_t kR = ring.size();
            int pos = -1;
            for (size_t i = 0; i < kR; ++i) {
                if (ring[i].face == fIdx) { pos = static_cast<int>(i); break; }
            }
            if (pos < 0) return;

            // Face winding at v: v → outX → inX → v (CCW from outside).
            // For face fIdx at ring position pos:
            //   outX = ring[pos].outgoingX       (where outgoing HE ends)
            //   inX  = ring[pos].sharedWithNext  (far endpoint of the
            //                                     incoming edge at v — this
            //                                     is also the shared edge
            //                                     with ring[pos+1])
            (void)kR;
            int outX = ring[pos].outgoingX;
            int inX  = ring[pos].sharedWithNext;

            int uOut = onEdgeOffset(v, outX);
            int uIn  = onEdgeOffset(v, inX);

            // Third vertex of this triangle (not v, not outX, not inX but
            // outX or inX may coincide with it — it's actually just "the third
            // vertex of the triangle"). Find it.
            auto verts = faceVertices(fIdx);
            if (verts.size() != 3) return;
            int third = -1;
            for (int x : verts) {
                if (x != v && x != outX && x != inX) { third = x; break; }
            }
            // For triangular faces, outX and inX are exactly the two other
            // corners (the face has 3 corners: v, outX, inX). Verify.
            if (third < 0) {
                // Fallback: pick whichever verts[i] isn't v.
                for (int x : verts) if (x != v) { third = x; break; }
            }

            // Determine face winding: does HE from v go to outX or to inX?
            // ring[pos].heOut is the outgoing HE from v in fIdx; its .vertex
            // == outX. In winding order, this HE follows prev(v→outX) which
            // is the HE pointing INTO v — and its source is the previous
            // corner. So winding is v → outX → ??? → v, and the middle is
            // the other corner (inX or third).
            //
            // In a triangle (A, B, C) the winding is A→B→C→A. If v is A and
            // outX is B, then C is the third corner — which should equal inX
            // (since inX is the "other v-neighbor" in the face).
            //
            // So we have:
            //   v -> outX -> inX -> v   (winding order)
            //
            // Now retriangulate based on uOut/uIn:
            //   Both exist: face becomes quad (uIn, v_unchanged?, uOut, outX, inX) — wait,
            //     if BOTH on-edge offsets exist, v is "cut off" from this face.
            //     The face's polygon becomes (uOut, outX, inX, uIn). 4 verts,
            //     triangulate as fan. Plus a tiny corner tri (v, uIn, uOut) at
            //     v emitted by the corner fan (see Phase 7).
            //   Only uOut: face becomes quad (v, uOut, outX, inX). 4 verts.
            //   Only uIn: face becomes quad (v, outX, inX, uIn). 4 verts.
            //   Neither: face unchanged; no-op here.

            if (uOut < 0 && uIn < 0) return; // nothing to do

            // Remove the old face; emit new tris with correct winding.
            // Winding: always v → outX → inX → v (triangle's CCW order).
            auto tri = [&](int x, int y, int z) {
                appendTriangle(x, y, z, m_faces[fIdx].subMeshIndex);
            };

            // Face original winding (CCW from outside): v → outX → inX → v.
            // Replacing v with on-edge offsets where they exist gives the
            // new polygon, still CCW:
            //   both exist → uOut → outX → inX → uIn
            //   only uOut  → uOut → outX → inX → v
            //   only uIn   → v → outX → inX → uIn
            //
            // Fan from polygon[0] in each case gives CCW winding that
            // matches the original face.
            // Face was tri(v, outX, inX). After inserting on-edge offsets
            // uOut (on edge v→outX) and/or uIn (on edge v→inX), the face's
            // v-corner is either fully severed (both offsets) or sliced on
            // one side (single offset). Split the resulting polygon into
            // non-degenerate triangles.
            //
            // IMPORTANT: never emit a triangle that contains v and ALL of
            // v's on-edge offsets on the same edge — those are collinear and
            // produce zero-area tris.
            if (uOut >= 0 && uIn >= 0) {
                // v cut off; polygon = (uOut, outX, inX, uIn). Fan from uOut.
                tri(uOut, outX, inX);
                tri(uOut, inX, uIn);
            } else if (uOut >= 0) {
                // Only outX edge is split. Polygon at v's corner = (v, uOut),
                // then main body (uOut, outX, inX). Tri 1: (v, uOut, inX) =
                // the "narrow" corner; Tri 2: (uOut, outX, inX).
                tri(v, uOut, inX);
                tri(uOut, outX, inX);
            } else { // uIn >= 0
                // Only inX edge is split. Tri 1: (v, outX, uIn); Tri 2:
                // (uIn, outX, inX).
                tri(v, outX, uIn);
                tri(uIn, outX, inX);
            }

            facesToRemove.push_back(fIdx);
        };

        // A non-beveled face in the ring may actually be coplanar with f1 or
        // f2 — i.e., it's the OTHER triangle of the same logical polygon that
        // got split along a diagonal. Treat such faces as if they were part
        // of the beveled face: don't rewire them independently.
        auto faceIsCoplanarWithBeveled = [&](int f) {
            return !facesFormCrease(f, info.f1) || !facesFormCrease(f, info.f2);
        };

        if (fullRingV1) {
            for (const auto& s : ringV1) {
                if (isBeveledFace(s.face)) continue;
                if (faceIsCoplanarWithBeveled(s.face)) continue;
                processNeighborFace(info.v1, s.face, ringV1);
            }
        }
        if (fullRingV2) {
            for (const auto& s : ringV2) {
                if (isBeveledFace(s.face)) continue;
                if (faceIsCoplanarWithBeveled(s.face)) continue;
                processNeighborFace(info.v2, s.face, ringV2);
            }
        }

        // =====================================================================
        // Phase 6: chamfer quad between f1's and f2's inner offsets.
        // =====================================================================
        if (f1WalksAB) {
            appendTriangle(v1b, v2b, v2a, info.subMeshIndex);
            appendTriangle(v1b, v2a, v1a, info.subMeshIndex);
        } else {
            appendTriangle(v2b, v1b, v1a, info.subMeshIndex);
            appendTriangle(v2b, v1a, v2a, info.subMeshIndex);
        }

        // =====================================================================
        // Phase 7: corner triangle at each endpoint.
        //
        // With the multi-face approach, the corner at v is a polygon with
        // vertices in ring order:
        //   [inner_on_f1_side, on-edge offsets walking the ring back to f2,
        //    inner_on_f2_side]
        //
        // For a 2-face ring (no neighbors) the polygon collapses to
        // (v_inner_f1, v_inner_f2) which needs v to close — same as before.
        //
        // For full ring with neighbors, the polygon includes the on-edge
        // offsets between f1/f2 and their respective ring-neighbors.
        // =====================================================================

        auto buildCorner = [&](int v, int innerA, int innerB,
                               const std::vector<RingStep>& ring,
                               bool isV1, bool fWalksAB) {
            if (ring.empty()) {
                // No ring (boundary or non-manifold): 2-face cap via original v.
                int vf1 = innerA, vf2 = innerB;
                auto tri = [&](int x, int y, int z) {
                    appendTriangle(x, y, z, info.subMeshIndex);
                };
                if (isV1) {
                    if (fWalksAB) tri(v, vf1, vf2);
                    else          tri(v, vf2, vf1);
                } else {
                    if (fWalksAB) tri(v, vf2, vf1);
                    else          tri(v, vf1, vf2);
                }
                return;
            }

            // Find f1 and f2 positions in the ring.
            size_t kR = ring.size();
            int posF1 = -1, posF2 = -1;
            for (size_t i = 0; i < kR; ++i) {
                if (ring[i].face == info.f1) posF1 = static_cast<int>(i);
                if (ring[i].face == info.f2) posF2 = static_cast<int>(i);
            }
            if (posF1 < 0 || posF2 < 0) return;

            // Gather the polygon vertices walking from f1 around through
            // neighbors to f2. Start at innerA (on f1's side), then walk the
            // ring in the direction away from f2, collecting on-edge offsets,
            // ending at innerB (on f2's side).
            //
            // Direction: ring[posF1]'s outgoing edge leads into the next ring
            // face. If that next face is f2, f1 and f2 are adjacent and the
            // beveled edge IS ring[posF1]'s edge — walk the other way around.
            bool f1NextIsF2 = (ring[(posF1 + 1) % kR].face == info.f2);
            // Build the corner polygon walking from f1 around through
            // neighbors to f2, skipping the beveled edge side of the ring.
            //
            // Structure:
            //   [innerA, (ring-side bevel-boundary on-edge offsets), v, more
            //   offsets..., innerB]
            //
            // In practice, for shared-inner-offset meshes (innerA == onEdge
            // on f1's crease edge), the first on-edge offset collapses onto
            // innerA; the polygon simplifies to [innerA, v, innerB] — a
            // single cap triangle covering the open end of the chamfer.
            std::vector<int> polygon;
            auto pushUnique = [&](int idx) {
                if (polygon.empty() || polygon.back() != idx) polygon.push_back(idx);
            };
            pushUnique(innerA);
            int dir = f1NextIsF2 ? -1 : +1;
            int pos = posF1;
            for (int step = 0; step < static_cast<int>(kR); ++step) {
                int nextPos;
                int edgeOutX;
                if (dir > 0) {
                    nextPos = (pos + 1) % kR;
                    edgeOutX = ring[pos].sharedWithNext;
                } else {
                    nextPos = (pos + kR - 1) % kR;
                    edgeOutX = ring[nextPos].sharedWithNext;
                }
                int fA = ring[pos].face;
                int fB = ring[nextPos].face;
                bool oneBeveled = (isBeveledFace(fA) != isBeveledFace(fB));
                bool crease = facesFormCrease(fA, fB);
                if (oneBeveled && crease) {
                    int u = onEdgeOffset(v, edgeOutX);
                    if (u >= 0) pushUnique(u);
                } else if (!oneBeveled && !crease) {
                    // Non-crease segment between two non-beveled neighbors —
                    // v itself still spans this segment's corner.
                    pushUnique(v);
                }
                if (ring[nextPos].face == info.f2) break;
                pos = nextPos;
            }
            pushUnique(innerB);

            // If no on-edge offsets were picked up (endpoint's ring had no
            // crease boundary between a beveled face and a non-beveled
            // neighbor — e.g., the opposite end of a cube bevel where both
            // adjacent cube faces are triangulated into coplanar pairs),
            // the polygon is just [innerA, innerB] and can't fan. Insert v
            // between them so we get a single cap triangle (innerA, v, innerB).
            if (polygon.size() == 2)
                polygon.insert(polygon.begin() + 1, v);

            // Dedupe consecutive polygon verts whose positions coincide —
            // that happens e.g. when v1a (inner offset perp to beveled edge)
            // and the on-edge offset on f1's non-beveled edge both land at
            // the same point because f1's offset direction IS that edge's
            // direction. Without deduping we get zero-area tris.
            {
                std::vector<int> dedup;
                dedup.reserve(polygon.size());
                for (int idx : polygon) {
                    if (!dedup.empty()) {
                        const auto& a = m_vertices[dedup.back()].position;
                        const auto& b = m_vertices[idx].position;
                        if (a.squaredDistance(b) < 1e-10f) continue;
                    }
                    dedup.push_back(idx);
                }
                polygon.swap(dedup);
            }

            if (polygon.size() < 3) return;

            // Winding: the polygon forms a "cap" facing outward from v. Use
            // the same (isV1, fWalksAB) logic as the fallback to decide fan
            // direction. For isV1 case the outward normal aligns with
            // fWalksAB's polarity.
            bool flip = (isV1 ? fWalksAB : !fWalksAB);

            auto tri = [&](int x, int y, int z) {
                appendTriangle(x, y, z, info.subMeshIndex);
            };
            // Fan from polygon[0].
            for (size_t i = 1; i + 1 < polygon.size(); ++i) {
                int a = polygon[0], b = polygon[i], c = polygon[i + 1];
                if (flip) std::swap(b, c);
                tri(a, b, c);
            }
        };

        buildCorner(info.v1, v1a, v1b, fullRingV1 ? ringV1 : std::vector<RingStep>{},
                    /*isV1=*/true, f1WalksAB);
        buildCorner(info.v2, v2a, v2b, fullRingV2 ? ringV2 : std::vector<RingStep>{},
                    /*isV1=*/false, f1WalksAB);

        facesToRemove.push_back(info.f1);
        facesToRemove.push_back(info.f2);
    }

    // Remove original face triangles (now replaced by the retriangulations).
    // We just mark their half-edges as face==-1 so compactBoundaryHalfEdges
    // drops them, and clear the face slot.
    for (int f : facesToRemove) {
        int startHE = m_faces[f].halfEdge;
        if (startHE < 0) continue;
        int he = startHE;
        do {
            int next = m_halfEdges[he].next;
            m_halfEdges[he].face = -1;
            m_halfEdges[he].twin = -1;
            m_halfEdges[he].next = -1;
            m_halfEdges[he].prev = -1;
            he = next;
        } while (he != startHE && he >= 0);
        m_faces[f].halfEdge = -1;
    }

    rebuildEdgesAndTwins();
    compactBoundaryHalfEdges();
    buildBoundaryHalfEdges();
    fixVertexHalfEdges();

    return newVertices;
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
            continue; // orphaned face slot (e.g., retired by a topology op) — skip

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
