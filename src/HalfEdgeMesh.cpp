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

    for (const auto& info : clean) {
        float w = effectiveWidth(info);
        if (w < 1e-5f) continue;

        // Phase 1: gather face ring at each endpoint. A previous pass
        // experimented with rewiring every incident face to its own
        // per-face inward-offset vertex (full multi-face bevel). On meshes
        // with coplanar-per-face topology that works, but on general meshes
        // (cubes, skeletal models) faces that shared non-bevel edges ended
        // up with different vertex endpoints and the mesh fractured into
        // disconnected strips. Until we add inter-face stitching along
        // those shared edges, fall back to the simpler 2-face algorithm:
        // only f1 and f2 get vertex splits; other incident faces keep the
        // original endpoint, accepting a small "pinch" at corners where a
        // third face meets.
        std::vector<int> ringV1; // intentionally empty for now
        std::vector<int> ringV2;
        (void)facesAtVertexOrdered; // keep helper live for the planned fix
        bool fullBevelV1 = false;
        bool fullBevelV2 = false;

        // Phase 2: per-face offset vertices for each endpoint, keyed by face.
        // faceOffsetV1[f] = new HE vertex index that replaces v1 in face f.
        // Same for V2. Always create at least the f1/f2 entries (those are
        // needed by the chamfer + retriangulation regardless of fallback).
        auto makeOffsetAlongDir = [&](int baseIdx, const Ogre::Vector3& unitDir, float dist) {
            HEVertex nv = m_vertices[baseIdx];
            nv.position = m_vertices[baseIdx].position + unitDir * dist;
            nv.halfEdge = -1;
            int idx = static_cast<int>(m_vertices.size());
            m_vertices.push_back(nv);
            newVertices.push_back(idx);
            return idx;
        };
        auto offsetForFaceAtV = [&](int faceIdx, int v, int otherEndpoint) -> int {
            Ogre::Vector3 dir = faceInwardDirection(faceIdx, v, otherEndpoint);
            if (dir.length() < 0.5f) {
                // Fallback: offset toward face centroid. Rare but keeps us
                // producing *some* inward offset even on degenerate tris.
                auto verts = faceVertices(faceIdx);
                if (verts.size() != 3) return -1;
                Ogre::Vector3 c = (m_vertices[verts[0]].position +
                                   m_vertices[verts[1]].position +
                                   m_vertices[verts[2]].position) / 3.0f;
                dir = c - m_vertices[v].position;
                if (dir.length() < 1e-6f) return -1;
                dir.normalise();
            }
            return makeOffsetAlongDir(v, dir, w);
        };

        std::unordered_map<int, int> faceOffsetV1;
        std::unordered_map<int, int> faceOffsetV2;

        int v1a = offsetForFaceAtV(info.f1, info.v1, info.v2);
        int v1b = offsetForFaceAtV(info.f2, info.v1, info.v2);
        int v2a = offsetForFaceAtV(info.f1, info.v2, info.v1);
        int v2b = offsetForFaceAtV(info.f2, info.v2, info.v1);
        if (v1a < 0 || v1b < 0 || v2a < 0 || v2b < 0) continue;
        faceOffsetV1[info.f1] = v1a;
        faceOffsetV1[info.f2] = v1b;
        faceOffsetV2[info.f1] = v2a;
        faceOffsetV2[info.f2] = v2b;

        // Extra offsets for the other faces in the ring (multi-face corner).
        if (fullBevelV1) {
            for (int f : ringV1) {
                if (f == info.f1 || f == info.f2) continue;
                int idx = offsetForFaceAtV(f, info.v1, info.v2);
                if (idx >= 0) faceOffsetV1[f] = idx;
            }
        }
        if (fullBevelV2) {
            for (int f : ringV2) {
                if (f == info.f1 || f == info.f2) continue;
                int idx = offsetForFaceAtV(f, info.v2, info.v1);
                if (idx >= 0) faceOffsetV2[f] = idx;
            }
        }

        // Phase 3: determine f1/f2 winding direction across the beveled edge.
        // Same logic as before — needed for chamfer and retriangulation.
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

        // Phase 4: retriangulate f1 and f2. Emit up to 3 tris per face:
        //   - corner-A tri (keeps A connected to the opposite C via edge A-C);
        //     skipped when A's ring is fully beveled (the corner fan at A
        //     provides the alternative bridge).
        //   - corner-B tri (analogous); skipped when B's ring is fully beveled.
        //   - inner tri (always emitted).
        auto retriangulateFace = [&](int A, int B, int C, int Aprime, int Bprime,
                                     bool aToB, int subIdx,
                                     bool skipCornerA, bool skipCornerB) {
            if (aToB) {
                if (!skipCornerA) appendTriangle(A, Aprime, C, subIdx);
                if (!skipCornerB) appendTriangle(C, Bprime, B, subIdx);
                appendTriangle(Aprime, Bprime, C, subIdx);
            } else {
                if (!skipCornerA) appendTriangle(A, C, Aprime, subIdx);
                if (!skipCornerB) appendTriangle(C, B, Bprime, subIdx);
                appendTriangle(Aprime, C, Bprime, subIdx);
            }
        };
        retriangulateFace(info.v1, info.v2, info.f1Opposite, v1a, v2a,
                          f1WalksAB, info.subMeshIndex,
                          fullBevelV1, fullBevelV2);
        retriangulateFace(info.v1, info.v2, info.f2Opposite, v1b, v2b,
                          f2WalksAB, info.subMeshIndex,
                          fullBevelV1, fullBevelV2);

        // Phase 5: rewire the *other* faces in each ring so their v corner
        // points to the new per-face offset vertex instead of v.
        auto rewireFaceVertex = [&](int faceIdx, int oldV, int newV) {
            int startHE = m_faces[faceIdx].halfEdge;
            if (startHE < 0) return;
            int he = startHE;
            do {
                if (m_halfEdges[he].vertex == oldV)
                    m_halfEdges[he].vertex = newV;
                he = m_halfEdges[he].next;
            } while (he != startHE);
        };
        if (fullBevelV1) {
            for (auto& [f, newV] : faceOffsetV1) {
                if (f == info.f1 || f == info.f2) continue; // handled by retriangulate
                rewireFaceVertex(f, info.v1, newV);
            }
        }
        if (fullBevelV2) {
            for (auto& [f, newV] : faceOffsetV2) {
                if (f == info.f1 || f == info.f2) continue;
                rewireFaceVertex(f, info.v2, newV);
            }
        }

        // Phase 6: chamfer quad between f1's and f2's new vertices. Winding
        // picked so each chamfer edge twins with the adjacent face's edge.
        if (f1WalksAB) {
            appendTriangle(v1b, v2b, v2a, info.subMeshIndex);
            appendTriangle(v1b, v2a, v1a, info.subMeshIndex);
        } else {
            appendTriangle(v2b, v1b, v1a, info.subMeshIndex);
            appendTriangle(v2b, v1a, v2a, info.subMeshIndex);
        }

        // Phase 7: corner fan at each endpoint.
        // - If multi-face: fan across all offset vertices in ring order.
        // - Otherwise: fall back to a single end-cap triangle between
        //   v, v_f1, v_f2 (previous behavior).
        auto buildCornerFan = [&](int vOrig, const std::vector<int>& ring,
                                  std::unordered_map<int,int>& faceToOffset,
                                  bool isV1, bool fWalksAB) {
            // Ring-order winding: picked so the fan's outward normal aligns
            // with the averaged face normal at vOrig. `fWalksAB` encodes that
            // for the bevel-adjacent face; we use it to pick the cap winding.
            if (ring.size() >= 3) {
                // Gather ordered offset verts that we actually created.
                std::vector<int> orderedOffsets;
                orderedOffsets.reserve(ring.size());
                for (int f : ring) {
                    auto it = faceToOffset.find(f);
                    if (it != faceToOffset.end()) orderedOffsets.push_back(it->second);
                }
                if (orderedOffsets.size() < 3) return; // shouldn't happen
                // Triangle fan from orderedOffsets[0].
                bool flip = (isV1 ? !fWalksAB : fWalksAB);
                for (size_t k = 1; k + 1 < orderedOffsets.size(); ++k) {
                    int a = orderedOffsets[0];
                    int b = orderedOffsets[k];
                    int c = orderedOffsets[k + 1];
                    if (flip) std::swap(b, c);
                    appendTriangle(a, b, c, info.subMeshIndex);
                }
            } else {
                // Fallback: 2-face cap using v_f1 and v_f2.
                int vf1 = faceToOffset[info.f1];
                int vf2 = faceToOffset[info.f2];
                if (isV1) {
                    if (fWalksAB) appendTriangle(vOrig, vf1, vf2, info.subMeshIndex);
                    else          appendTriangle(vOrig, vf2, vf1, info.subMeshIndex);
                } else {
                    if (fWalksAB) appendTriangle(vOrig, vf2, vf1, info.subMeshIndex);
                    else          appendTriangle(vOrig, vf1, vf2, info.subMeshIndex);
                }
            }
        };
        if (fullBevelV1)
            buildCornerFan(info.v1, ringV1, faceOffsetV1, /*isV1=*/true, f1WalksAB);
        else
            buildCornerFan(info.v1, {}, faceOffsetV1, /*isV1=*/true, f1WalksAB);
        if (fullBevelV2)
            buildCornerFan(info.v2, ringV2, faceOffsetV2, /*isV1=*/false, f1WalksAB);
        else
            buildCornerFan(info.v2, {}, faceOffsetV2, /*isV1=*/false, f1WalksAB);

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
