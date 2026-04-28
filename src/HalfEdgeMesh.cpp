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
#include <array>
#include <cassert>
#include <cmath>
#include <functional>
#include <set>
#include <tuple>
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

    // Phase 2: Append a face for each input face. Each submesh prefers
    // `faces` (n-gon canonical storage) when non-empty; otherwise it
    // falls back to `triangles` (legacy storage). The two-stream rule
    // mirrors EditableSubMesh's invariant: `faces` is canonical iff it
    // is non-empty. Skips degenerates and sets vertex outgoing-HE
    // pointers as faces are added.
    auto registerVertexHE = [this](int vGlobal, int heStart) {
        if (m_vertices[vGlobal].halfEdge == -1) {
            m_vertices[vGlobal].halfEdge = heStart;
        }
    };

    for (int s = 0; s < m_subMeshCount; ++s) {
        const auto& sub = subMeshes[s];

        if (!sub.faces.empty()) {
            for (const auto& face : sub.faces) {
                if (!face.isValid()) continue;
                std::vector<int> verts;
                verts.reserve(face.indices.size());
                bool degenerate = false;
                for (unsigned int local : face.indices) {
                    if (local >= vertexMap[s].size()) {
                        degenerate = true;
                        break;
                    }
                    verts.push_back(vertexMap[s][local]);
                }
                if (degenerate) continue;
                // Reject faces with any duplicated vertex (would create
                // a zero-area corner). Cheap O(N²) check is fine for
                // typical N <= 8.
                for (size_t i = 0; i < verts.size() && !degenerate; ++i) {
                    for (size_t j = i + 1; j < verts.size(); ++j) {
                        if (verts[i] == verts[j]) { degenerate = true; break; }
                    }
                }
                if (degenerate) continue;

                int he0 = static_cast<int>(m_halfEdges.size());
                appendFace(verts, s);
                for (size_t i = 0; i < verts.size(); ++i) {
                    registerVertexHE(verts[i], he0 + static_cast<int>(i));
                }
            }
        } else {
            for (const auto& tri : sub.triangles) {
                int v0 = vertexMap[s][tri.indices[0]];
                int v1 = vertexMap[s][tri.indices[1]];
                int v2 = vertexMap[s][tri.indices[2]];

                if (v0 == v1 || v1 == v2 || v0 == v2) continue;

                int he0 = static_cast<int>(m_halfEdges.size());
                appendTriangle(v0, v1, v2, s);

                int verts[3] = {v0, v1, v2};
                for (int i = 0; i < 3; ++i) {
                    registerVertexHE(verts[i], he0 + i);
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
    return appendFace({v0, v1, v2}, subMeshIndex);
}

int HalfEdgeMesh::appendFace(const std::vector<int>& vertices, int subMeshIndex)
{
    const int n = static_cast<int>(vertices.size());
    if (n < 3) return -1;

    const int fIdx = static_cast<int>(m_faces.size());
    HEFace f;
    f.subMeshIndex = subMeshIndex;
    m_faces.push_back(f);

    const int he0 = static_cast<int>(m_halfEdges.size());
    for (int i = 0; i < n; ++i) {
        HalfEdge he;
        he.vertex = vertices[(i + 1) % n]; // points TO the next vertex in the loop
        he.face = fIdx;
        he.next = he0 + (i + 1) % n;
        he.prev = he0 + (i + n - 1) % n;
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

    // Track whether any submesh ended up with a non-triangle face. If so,
    // we mirror the face list into `EditableSubMesh::faces` so downstream
    // n-gon-aware code (chunks 2+) sees the polygon structure. If every
    // face is a triangle, we leave `faces` empty to keep the legacy path
    // canonical.
    std::vector<bool> hasNonTriangleFace(m_subMeshCount, false);

    for (int f = 0; f < static_cast<int>(m_faces.size()); ++f) {
        const auto& heFace = m_faces[f];
        int subIdx = heFace.subMeshIndex;
        auto& outSub = outSubMeshes[subIdx];

        auto verts = faceVertices(f);
        // Accept any face with 3+ vertices. n-gons (4+) get fan-
        // triangulated into the legacy `triangles` array AND mirrored
        // into `faces` so the n-gon structure survives the round-trip.
        // Faces with < 3 vertices are degenerate — silently skip.
        if (verts.size() < 3) continue;

        // Map every face vertex into the submesh's local vertex array,
        // collecting the local indices in `localIdxs` for later.
        std::vector<unsigned int> localIdxs;
        localIdxs.reserve(verts.size());
        for (int heVertIdx : verts) {
            auto it = vertexRemap[subIdx].find(heVertIdx);
            int localIdx;
            if (it == vertexRemap[subIdx].end()) {
                localIdx = static_cast<int>(outSub.vertices.size());
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
            } else {
                localIdx = it->second;
            }
            localIdxs.push_back(static_cast<unsigned int>(localIdx));
        }

        // Fan-triangulate the face into `triangles` (legacy storage):
        //   tris = (v0, v_i, v_{i+1}) for i in [1, N-1)
        for (size_t i = 1; i + 1 < localIdxs.size(); ++i) {
            EditableTriangle tri;
            tri.indices[0] = localIdxs[0];
            tri.indices[1] = localIdxs[i];
            tri.indices[2] = localIdxs[i + 1];
            outSub.triangles.push_back(tri);
        }

        // Always record the face in `EditableSubMesh::faces` so the n-gon
        // round-trip is information-preserving. We finalise below by
        // clearing `faces` on submeshes that turned out to be all
        // triangles (legacy invariant: faces is non-empty only when
        // there's actually polygonal information to preserve).
        EditableFace face;
        face.indices = std::move(localIdxs);
        if (face.indices.size() > 3) hasNonTriangleFace[subIdx] = true;
        outSub.faces.push_back(std::move(face));
    }

    // Finalise: drop `faces` on triangle-only submeshes. This keeps the
    // legacy invariant that `faces` is empty unless the submesh really
    // contains n-gons, so downstream code that doesn't yet understand
    // n-gons (every consumer of EditableSubMesh today) keeps working.
    for (int s = 0; s < m_subMeshCount; ++s) {
        if (!hasNonTriangleFace[s]) outSubMeshes[s].faces.clear();
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

// Matches the UI SpinBox upper bound in PropertiesPanel.qml. Large values
// cost O(segments) allocation per beveled endpoint and can overflow the
// hole-filler's 64-vertex loop cap; clamping here keeps the public API
// safe even if a caller bypasses the UI.
static constexpr int kMaxBevelSegments = 16;

// The Phase 1-7 bevel topology below has high cognitive complexity that
// predates this PR; splitting it into phase-sized helpers is tracked as
// a separate refactor. This PR only adds the optional profilePoints
// parameter and uses it inside buildSegmentVerts.
std::vector<int> HalfEdgeMesh::bevelEdges(const std::vector<int>& edgeIndices, // NOSONAR(cpp:S3776)
                                           float width,
                                           int segments,
                                           float profile,
                                           const std::vector<float>& profilePointsIn)
{
    std::vector<int> newVertices;
    if (edgeIndices.empty() || width <= 0.0f)
        return newVertices;
    segments = std::clamp(segments, 1, kMaxBevelSegments);
    profile = std::clamp(profile, 0.0f, 1.0f);

    // Build the per-interior-point profile vector. If the caller supplied
    // an explicit one of the right size, clamp and use it directly; other-
    // wise synthesize from the scalar profile using a sin envelope so a
    // single UI number still produces the classic bulge shape.
    std::vector<float> profilePoints;
    if (segments > 1) {
        profilePoints.resize(segments - 1, 0.5f);
        if (profilePointsIn.size() == static_cast<size_t>(segments - 1)) {
            for (size_t i = 0; i < profilePoints.size(); ++i)
                profilePoints[i] = std::clamp(profilePointsIn[i], 0.0f, 1.0f);
        } else {
            const float kPi = 3.14159265358979323846f;
            const float amp = profile - 0.5f;
            for (int i = 1; i < segments; ++i) {
                const float t = static_cast<float>(i)
                              / static_cast<float>(segments);
                profilePoints[i - 1] = 0.5f + amp * std::sin(kPi * t);
            }
        }
    }

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

    // Track which original vertices were touched by a bevel so the post-
    // pass hole filler can limit itself to those regions. Endpoints of each
    // beveled edge plus the opposite vertices of the two beveled faces are
    // all candidate "repair zone" vertices (any boundary edge incident to
    // these or to newVertices is a bevel-introduced gap).
    std::unordered_set<int> bevelTouchedVerts;

    // Ear-clip a simple (possibly concave) polygon lying roughly in the
    // plane of `refNormal`. `poly` is a list of HE vertex indices in
    // boundary order matching the source face (CCW from outside). Emits
    // each triangle via `emit` using the refNormal's orientation for
    // winding. Handles concave polygons produced by splicing chain
    // intermediates into neighbor-face polygons — a plain fan from poly[0]
    // would create an artificial diagonal across the concave arc.
    auto triangulatePolygon = [&](const std::vector<int>& poly,
                                  const Ogre::Vector3& refNormal,
                                  const std::function<void(int,int,int)>& emit) {
        if (poly.size() < 3) return;
        if (poly.size() == 3) { emit(poly[0], poly[1], poly[2]); return; }
        Ogre::Vector3 nrm = refNormal;
        if (nrm.length() < 1e-8f) nrm = Ogre::Vector3::UNIT_Y;
        else nrm.normalise();
        Ogre::Vector3 u = std::abs(nrm.x) < 0.9f
            ? Ogre::Vector3::UNIT_X.crossProduct(nrm)
            : Ogre::Vector3::UNIT_Y.crossProduct(nrm);
        u.normalise();
        Ogre::Vector3 v2 = nrm.crossProduct(u);
        auto project = [&](const Ogre::Vector3& p) {
            return Ogre::Vector2(p.dotProduct(u), p.dotProduct(v2));
        };
        std::vector<Ogre::Vector2> pts2D;
        pts2D.reserve(poly.size());
        for (int idx : poly) pts2D.push_back(project(m_vertices[idx].position));
        float signedArea = 0.0f;
        for (size_t i = 0; i < pts2D.size(); ++i) {
            const auto& a = pts2D[i];
            const auto& b = pts2D[(i + 1) % pts2D.size()];
            signedArea += a.x * b.y - b.x * a.y;
        }
        std::vector<int> p = poly;
        std::vector<Ogre::Vector2> pxy = pts2D;
        bool reversed = false;
        if (signedArea < 0.0f) {
            std::reverse(p.begin(), p.end());
            std::reverse(pxy.begin(), pxy.end());
            reversed = true;
        }
        auto cross2 = [](const Ogre::Vector2& a,
                         const Ogre::Vector2& b,
                         const Ogre::Vector2& c) {
            return (b.x - a.x) * (c.y - a.y)
                 - (b.y - a.y) * (c.x - a.x);
        };
        auto pointInTri = [&](const Ogre::Vector2& q,
                              const Ogre::Vector2& a,
                              const Ogre::Vector2& b,
                              const Ogre::Vector2& c) {
            float d1 = cross2(q, a, b);
            float d2 = cross2(q, b, c);
            float d3 = cross2(q, c, a);
            bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
            bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
            return !(hasNeg && hasPos);
        };
        std::vector<bool> removed(p.size(), false);
        size_t remaining = p.size();
        size_t cur = 0;
        int guard = 0;
        const int guardLimit = static_cast<int>(p.size() * p.size() + 10);
        // When the 2D orientation was flipped to CCW, emit back in the
        // caller's original orientation by swapping the last two indices.
        auto emitKeep = [&](int a, int b, int c) {
            if (reversed) emit(a, c, b);
            else          emit(a, b, c);
        };
        while (remaining > 3 && guard++ < guardLimit) {
            while (removed[cur]) cur = (cur + 1) % p.size();
            size_t prev = (cur + p.size() - 1) % p.size();
            while (removed[prev]) prev = (prev + p.size() - 1) % p.size();
            size_t next = (cur + 1) % p.size();
            while (removed[next]) next = (next + 1) % p.size();
            const auto& A = pxy[prev];
            const auto& B = pxy[cur];
            const auto& C = pxy[next];
            float conv = cross2(A, B, C);
            if (conv > 0.0f) {
                bool isEar = true;
                for (size_t i = 0; i < p.size(); ++i) {
                    if (removed[i] || i == prev || i == cur || i == next) continue;
                    if (pointInTri(pxy[i], A, B, C)) { isEar = false; break; }
                }
                if (isEar) {
                    emitKeep(p[prev], p[cur], p[next]);
                    removed[cur] = true;
                    --remaining;
                    cur = prev;
                    continue;
                }
            }
            cur = next;
        }
        if (remaining == 3) {
            size_t idx[3]; size_t ii = 0;
            for (size_t i = 0; i < p.size(); ++i)
                if (!removed[i]) idx[ii++] = i;
            if (ii == 3) emitKeep(p[idx[0]], p[idx[1]], p[idx[2]]);
        }
    };

    for (const auto& info : clean) {
        float w = effectiveWidth(info);
        if (w < 1e-5f) continue;
        bevelTouchedVerts.insert(info.v1);
        bevelTouchedVerts.insert(info.v2);
        bevelTouchedVerts.insert(info.f1Opposite);
        bevelTouchedVerts.insert(info.f2Opposite);

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

        // A face that is coplanar with either beveled face (same logical
        // polygon, split along a triangulation diagonal) acts like part of
        // the beveled side for bevel-boundary detection. Without this, a
        // cube-face diagonal separating f1 from its sibling is treated as
        // an interior ring step, and the crease edge between the sibling
        // and the neighboring cube face is missed.
        auto isEffectivelyBeveled = [&](int f) {
            if (isBeveledFace(f)) return true;
            // Only "coplanar with beveled" counts; real neighbors with
            // crease edges to the beveled face must stay non-beveled.
            auto faceNormal = [&](int fi) {
                auto verts = faceVertices(fi);
                if (verts.size() != 3) return Ogre::Vector3::ZERO;
                auto n = (m_vertices[verts[1]].position - m_vertices[verts[0]].position)
                    .crossProduct(m_vertices[verts[2]].position - m_vertices[verts[0]].position);
                if (n.length() < 1e-8f) return Ogre::Vector3::ZERO;
                n.normalise();
                return n;
            };
            auto n = faceNormal(f);
            auto nF1 = faceNormal(info.f1);
            auto nF2 = faceNormal(info.f2);
            if (n.length() < 0.5f) return false;
            if (nF1.length() > 0.5f && n.dotProduct(nF1) > 0.999f) return true;
            if (nF2.length() > 0.5f && n.dotProduct(nF2) > 0.999f) return true;
            return false;
        };

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
                // Bevel boundary: exactly one side is effectively beveled
                // (real beveled face OR coplanar sibling in the same logical
                // polygon), AND the two faces form a real crease. Coplanar
                // siblings are treated as beveled so the crease edge between
                // the sibling and the neighboring cube face is caught — this
                // is what lets neighbor-face corners get properly trimmed.
                bool oneSideBeveled =
                    (isEffectivelyBeveled(fA) != isEffectivelyBeveled(fB));
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
            // Preferred sharing: if (v, faceOpposite) is itself a crease edge,
            // reuse that on-edge offset as the inner vertex.
            int u = onEdgeOffset(v, faceOpposite);
            if (u >= 0) return u;

            // Compute the perpendicular-in-face offset position. Before
            // creating a new vertex, check whether an existing on-edge offset
            // at v coincides with that position AND belongs to an edge that
            // lies in the same logical (coplanar) polygon as this face — e.g.,
            // a crease edge at the opposite corner of the coplanar sibling.
            // Without the coplanarity check, a perpendicular direction that
            // happens to align with an unrelated crease edge (through
            // geometric coincidence) would wrongly collapse onto it.
            Ogre::Vector3 dir = faceInwardDirection(faceIdx, v, other);
            if (dir.length() < 0.5f) return -1;
            Ogre::Vector3 pos = m_vertices[v].position + dir * w;

            // Collect faces that share the plane of faceIdx and contain v.
            Ogre::Vector3 facePlaneNormal;
            {
                auto fv = faceVertices(faceIdx);
                if (fv.size() == 3) {
                    Ogre::Vector3 p0 = m_vertices[fv[0]].position;
                    Ogre::Vector3 p1 = m_vertices[fv[1]].position;
                    Ogre::Vector3 p2 = m_vertices[fv[2]].position;
                    facePlaneNormal = (p1 - p0).crossProduct(p2 - p0);
                    if (facePlaneNormal.length() > 1e-8f) facePlaneNormal.normalise();
                }
            }
            // Only reuse an on-edge offset if the edge belongs to the same
            // logical (coplanar) polygon as faceIdx — i.e., the edge is an
            // edge of some face coplanar with faceIdx. An edge that merely
            // lies in the same plane (through geometric coincidence) does
            // NOT qualify.
            auto edgeInCoplanarGroup = [&](int endpoint) {
                for (int f = 0; f < static_cast<int>(m_faces.size()); ++f) {
                    if (f == faceIdx) continue;
                    if (m_faces[f].halfEdge < 0) continue;
                    auto fv = faceVertices(f);
                    if (fv.size() != 3) continue;
                    bool hasV = false, hasE = false;
                    for (int x : fv) { if (x == v) hasV = true; if (x == endpoint) hasE = true; }
                    if (!hasV || !hasE) continue;
                    // Check coplanarity with faceIdx.
                    Ogre::Vector3 p0 = m_vertices[fv[0]].position;
                    Ogre::Vector3 p1 = m_vertices[fv[1]].position;
                    Ogre::Vector3 p2 = m_vertices[fv[2]].position;
                    Ogre::Vector3 n = (p1 - p0).crossProduct(p2 - p0);
                    if (n.length() < 1e-8f) continue;
                    n.normalise();
                    if (std::abs(n.dotProduct(facePlaneNormal)) > 0.999f) return true;
                }
                // Also: the edge might be an edge of faceIdx itself.
                auto fv = faceVertices(faceIdx);
                bool hasE = false;
                for (int x : fv) if (x == endpoint) hasE = true;
                return hasE;
            };
            for (const auto& kv : edgeOffsetAtV) {
                int a = kv.first.first, b = kv.first.second;
                if (a != v && b != v) continue;
                int otherEnd = (a == v) ? b : a;
                const auto& p = m_vertices[kv.second].position;
                if (p.squaredDistance(pos) >= 1e-10f) continue;
                if (!edgeInCoplanarGroup(otherEnd)) continue;
                return kv.second;
            }

            HEVertex nv = m_vertices[v];
            nv.position = pos;
            nv.halfEdge = -1;
            int idx = static_cast<int>(m_vertices.size());
            m_vertices.push_back(nv);
            newVertices.push_back(idx);
            return idx;
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

        // Track edges emitted during this edge's bevel (Phases 4-5). The
        // cap emission in buildCorner consults this — if (v1a, v1b) is
        // already an edge of some emitted tri, emitting a cap would
        // overlap, so skip. This one mechanism handles both the cube-
        // sibling case (edge covered by a merged polygon's fan) AND the
        // case where a neighbor face's "both on-edge offsets" branch
        // emits a tri with that edge.
        std::set<std::pair<int, int>> emittedEdges;
        auto sortedPair = [](int a, int b) {
            return std::make_pair(std::min(a, b), std::max(a, b));
        };
        auto recordTriEdges = [&](int a, int b, int c) {
            emittedEdges.insert(sortedPair(a, b));
            emittedEdges.insert(sortedPair(b, c));
            emittedEdges.insert(sortedPair(c, a));
        };
        auto hasEmittedEdge = [&](int a, int b) {
            return emittedEdges.count(sortedPair(a, b)) > 0;
        };

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
            auto tri = [&](int x, int y, int z) {
                appendTriangle(x, y, z, subIdx);
                recordTriEdges(x, y, z);
            };

            // NOTE: the real emission happens below in the polygon/fan
            // construction — uA/uB are used there.

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

            // Corner triangles bridge A/B to C via the non-beveled v-edge
            // of this face (the diagonal, when the adjacent face is a
            // coplanar sibling). Skip the corner tri when that adjacent
            // face is a coplanar sibling — its retriangulation removed the
            // shared diagonal, so the corner tri would be a spike.
            auto adjacentAcrossEdge = [&](int va, int vb) -> int {
                // Return the face sharing edge (va, vb) with the current
                // beveled face. The current face contains all three of
                // {A, B, C} — the adjacent face contains the two vertices
                // of the shared edge but not the third corner of the
                // beveled face.
                int currentThird = -1;
                if (va == A && vb == C) currentThird = B;
                else if (va == B && vb == C) currentThird = A;
                else if (va == C && vb == A) currentThird = B;
                else if (va == C && vb == B) currentThird = A;
                for (int f = 0; f < static_cast<int>(m_faces.size()); ++f) {
                    if (m_faces[f].halfEdge < 0) continue;
                    auto fv = faceVertices(f);
                    if (fv.size() != 3) continue;
                    bool hasA = false, hasB = false, hasThird = false;
                    for (int x : fv) {
                        if (x == va) hasA = true;
                        if (x == vb) hasB = true;
                        if (x == currentThird) hasThird = true;
                    }
                    if (hasA && hasB && !hasThird) return f;
                }
                return -1;
            };
            auto isCoplanarSiblingAcross = [&](int va, int vb) {
                int fAdj = adjacentAcrossEdge(va, vb);
                if (fAdj < 0) return false;
                // Is fAdj coplanar with one of the beveled faces (f1 or f2)?
                auto normal = [&](int f) {
                    auto fv = faceVertices(f);
                    if (fv.size() != 3) return Ogre::Vector3::ZERO;
                    Ogre::Vector3 p0 = m_vertices[fv[0]].position;
                    Ogre::Vector3 p1 = m_vertices[fv[1]].position;
                    Ogre::Vector3 p2 = m_vertices[fv[2]].position;
                    Ogre::Vector3 n = (p1 - p0).crossProduct(p2 - p0);
                    if (n.length() < 1e-8f) return Ogre::Vector3::ZERO;
                    n.normalise();
                    return n;
                };
                auto nAdj = normal(fAdj);
                auto nF1 = normal(info.f1);
                auto nF2 = normal(info.f2);
                if (nAdj.length() < 0.5f) return false;
                if (nF1.length() > 0.5f && nAdj.dotProduct(nF1) > 0.999f) return true;
                if (nF2.length() > 0.5f && nAdj.dotProduct(nF2) > 0.999f) return true;
                return false;
            };

            if (uA < 0 && !isCoplanarSiblingAcross(A, C)) {
                if (aToB) tri(A, C, Aprime);
                else      tri(A, Aprime, C);
            }
            if (uB < 0 && !isCoplanarSiblingAcross(B, C)) {
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
        // Build chamfer-chain intermediates here (before Phase 5) so the
        // neighbor-face retriangulation below can splice them into the cut
        // polyline when the profile is concave/convex. Phase 6 reuses the
        // same chains to emit the chamfer-strip tris.
        //
        // vChain = [innerA, interm_1, ..., interm_{N-1}, innerB]
        //        = [f1-side, ..., f2-side] at endpoint v.
        // Each intermediate offset: (profilePoints[i-1] - 0.5) * w along
        // `outward` (chord-midpoint toward v). Linear, no sin attenuation.
        // =====================================================================
        auto computeOutward = [&](const Ogre::Vector3& pA,
                                  const Ogre::Vector3& pB,
                                  const Ogre::Vector3& pV) {
            const auto chord = pB - pA;
            auto outward = pV - (pA + pB) * 0.5f;
            if (const float chordLen2 = chord.squaredLength(); chordLen2 > 1e-12f)
                outward -= chord * (outward.dotProduct(chord) / chordLen2);
            if (outward.length() > 1e-6f) outward.normalise();
            else                          outward = Ogre::Vector3::ZERO;
            return outward;
        };
        auto appendChamferVertex = [&](int v, const Ogre::Vector3& pos) {
            HEVertex nv = m_vertices[v];
            nv.position = pos;
            nv.halfEdge = -1;
            const auto idx = static_cast<int>(m_vertices.size());
            m_vertices.push_back(nv);
            newVertices.push_back(idx);
            return idx;
        };
        auto buildSegmentVerts = [&](int v, int innerA, int innerB) {
            std::vector<int> chain;
            chain.reserve(segments + 1);
            chain.push_back(innerA);
            if (segments > 1) {
                const auto pA = m_vertices[innerA].position;
                const auto pB = m_vertices[innerB].position;
                const auto chord = pB - pA;
                const auto outward = computeOutward(pA, pB, m_vertices[v].position);
                for (int i = 1; i < segments; ++i) {
                    const float t = static_cast<float>(i)
                                  / static_cast<float>(segments);
                    const float pt = profilePoints[i - 1];
                    const auto pos = pA + chord * t + outward * ((pt - 0.5f) * w);
                    chain.push_back(appendChamferVertex(v, pos));
                }
            }
            chain.push_back(innerB);
            return chain;
        };
        const std::vector<int> v1Chain = buildSegmentVerts(info.v1, v1a, v1b);
        const std::vector<int> v2Chain = buildSegmentVerts(info.v2, v2a, v2b);

        // A chain is "shaped" when any interior vertex is non-collinear
        // with its endpoints. For flat chamfers the chain is collinear and
        // we keep the plain fan (manifold guarantees hold automatically).
        auto chainIsShaped = [&](const std::vector<int>& chain) {
            if (chain.size() <= 2) return false;
            const auto& pA = m_vertices[chain.front()].position;
            const auto& pB = m_vertices[chain.back()].position;
            const auto ab = pB - pA;
            const float abLen2 = ab.squaredLength();
            if (abLen2 < 1e-12f) return false;
            for (size_t i = 1; i + 1 < chain.size(); ++i) {
                const auto& p = m_vertices[chain[i]].position;
                const auto d = p - pA;
                const auto proj = ab * (d.dotProduct(ab) / abLen2);
                if ((d - proj).squaredLength() > 1e-8f) return true;
            }
            return false;
        };
        const bool v1Shaped = chainIsShaped(v1Chain);
        const bool v2Shaped = chainIsShaped(v2Chain);

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

            // Coplanar-sibling fix: if this face is a coplanar sibling of a
            // beveled face AND only one v-edge has an on-edge offset, the
            // other v-edge is the diagonal within the shared logical face,
            // where the beveled face's inner offset sits at the same
            // position as the (missing) diagonal offset would. Fully sever v
            // from this face using the single crease offset as both ends of
            // the cut — emit a single triangle (uCrease, outX, inX) which,
            // combined with the beveled face's retriangulation, tiles the
            // logical polygon without overlap and without v.
            bool coplanarSibling =
                !isBeveledFace(fIdx) && isEffectivelyBeveled(fIdx);
            if (coplanarSibling && ((uOut >= 0) != (uIn >= 0))) {
                int uCrease = (uOut >= 0) ? uOut : uIn;
                appendTriangle(uCrease, outX, inX,
                               m_faces[fIdx].subMeshIndex);
                recordTriEdges(uCrease, outX, inX);
                facesToRemove.push_back(fIdx);
                return;
            }

            // Remove the old face; emit new tris with correct winding.
            // Winding: always v → outX → inX → v (triangle's CCW order).
            auto tri = [&](int x, int y, int z) {
                appendTriangle(x, y, z, m_faces[fIdx].subMeshIndex);
                recordTriEdges(x, y, z);
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
                // Base polygon walks v's corner: uOut → outX → inX → uIn,
                // closing back to uOut. For shaped profiles, insert chain
                // intermediates between uIn and the close-back-to-uOut so
                // the cut polyline follows the chamfer curve. The direction
                // of intermediate insertion is chosen so each chain edge
                // is emitted in the OPPOSITE direction from the strip's
                // same-edge emission — yielding a manifold join.
                std::vector<int> polygon = {uOut, outX, inX, uIn};
                const bool shaped = (v == info.v1) ? v1Shaped : v2Shaped;
                if (shaped) {
                    const auto& chain = (v == info.v1) ? v1Chain : v2Chain;
                    const int cFront = chain.front();
                    const int cBack  = chain.back();
                    if (cFront == uIn && cBack == uOut) {
                        // Chain runs uIn → ... → uOut. To close the polygon
                        // uIn → chain[1] → chain[2] → ... → uOut, push
                        // chain's interior in FORWARD order.
                        for (size_t i = 1; i + 1 < chain.size(); ++i)
                            polygon.push_back(chain[i]);
                    } else if (cFront == uOut && cBack == uIn) {
                        // Chain runs uOut → ... → uIn. To close uIn → ...
                        // → uOut, push chain's interior in REVERSE order.
                        for (size_t i = chain.size() - 2; i >= 1; --i)
                            polygon.push_back(chain[i]);
                    }
                }
                Ogre::Vector3 refNrm = Ogre::Vector3::ZERO;
                {
                    const auto fv = faceVertices(fIdx);
                    if (fv.size() == 3) {
                        const auto& p0 = m_vertices[fv[0]].position;
                        const auto& p1 = m_vertices[fv[1]].position;
                        const auto& p2 = m_vertices[fv[2]].position;
                        refNrm = (p1 - p0).crossProduct(p2 - p0);
                    }
                }
                triangulatePolygon(polygon, refNrm,
                                   [&](int a, int b, int c) { tri(a, b, c); });
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

        // Group the non-beveled ring faces at v into coplanar components —
        // each group is a run of ring faces bounded by crease edges. If a
        // coplanar group has BOTH of its outer boundaries (left and right
        // ring-neighbors) on the bevel-boundary with on-edge offsets, v is
        // fully severed from that group and the group becomes a single
        // polygon (no v). Otherwise each face in the group gets the
        // standard single-edge-cut via processNeighborFace.
        auto processRingNeighbors = [&](int v, const std::vector<RingStep>& ring) {
            if (ring.empty()) return;
            size_t k = ring.size();

            // Find the slice(s) of non-beveled ring faces (ignoring the
            // beveled arc from f1 to f2). Within each slice, split further
            // into coplanar groups.
            std::vector<bool> visited(k, false);
            for (size_t start = 0; start < k; ++start) {
                if (visited[start]) continue;
                if (isBeveledFace(ring[start].face)) { visited[start] = true; continue; }

                // Expand [lo..hi] to the maximal contiguous coplanar run
                // around v. Coplanar: the shared edge between consecutive
                // ring faces is not a crease.
                int lo = static_cast<int>(start);
                int hi = static_cast<int>(start);
                // Extend backward.
                while (true) {
                    int prev = (lo - 1 + static_cast<int>(k)) % static_cast<int>(k);
                    if (prev == hi) break;
                    if (isBeveledFace(ring[prev].face)) break;
                    // shared edge between ring[prev] and ring[lo] =
                    // ring[prev].sharedWithNext
                    if (facesFormCrease(ring[prev].face, ring[lo].face)) break;
                    lo = prev;
                    if (lo == static_cast<int>(start)) break; // wrapped
                }
                // Extend forward.
                while (true) {
                    int nxt = (hi + 1) % static_cast<int>(k);
                    if (nxt == lo) break;
                    if (isBeveledFace(ring[nxt].face)) break;
                    if (facesFormCrease(ring[hi].face, ring[nxt].face)) break;
                    hi = nxt;
                }

                // Collect group indices in order lo .. hi (with wrap).
                std::vector<int> group;
                int idx = lo;
                while (true) {
                    group.push_back(idx);
                    visited[idx] = true;
                    if (idx == hi) break;
                    idx = (idx + 1) % static_cast<int>(k);
                }

                // Outer boundary edges (shared with the neighbor outside the
                // group). Left boundary: between ring[lo-1] and ring[lo],
                // shared vertex = ring[lo-1].sharedWithNext. Right boundary:
                // between ring[hi] and ring[hi+1], shared vertex =
                // ring[hi].sharedWithNext.
                int loPrev = (lo - 1 + static_cast<int>(k)) % static_cast<int>(k);
                int leftBoundaryX = ring[loPrev].sharedWithNext;
                int rightBoundaryX = ring[hi].sharedWithNext;
                int uLeft = onEdgeOffset(v, leftBoundaryX);
                int uRight = onEdgeOffset(v, rightBoundaryX);

                if (uLeft >= 0 && uRight >= 0 && group.size() >= 2) {
                    // Merged polygon: walk the group's outer perimeter (edges
                    // of the group faces not incident to v). For a coplanar
                    // triangle group at v, the outer perimeter is one polyline
                    // that starts at ring[lo].outgoingX (first face's far
                    // corner on the uLeft side) and ends at
                    // ring[hi].sharedWithNext (last face's far corner on the
                    // uRight side). But uLeft sits on the v→outgoingX[lo]
                    // edge, so outgoingX[lo] is still a polygon corner; and
                    // uRight sits on the v→sharedWithNext[hi] edge, so
                    // sharedWithNext[hi] is also a corner.
                    //
                    // Perimeter order:
                    //   uLeft
                    //   ring[lo].outgoingX
                    //   ring[lo].sharedWithNext  (== ring[lo+1].outgoingX
                    //                              when that face is in-group)
                    //   ring[lo+1].sharedWithNext
                    //   ...
                    //   ring[hi].sharedWithNext
                    //   uRight
                    // where consecutive duplicates collapse.

                    std::vector<int> polygon;
                    polygon.push_back(uLeft);
                    polygon.push_back(ring[group[0]].outgoingX);
                    for (int gi : group) {
                        polygon.push_back(ring[gi].sharedWithNext);
                    }
                    polygon.push_back(uRight);

                    // Splice chain intermediates between uRight and the
                    // implicit close-back-to-uLeft so the neighbor face's
                    // cut polyline follows the chamfer's concave/convex
                    // curve instead of a straight diagonal.
                    const bool shaped = (v == info.v1) ? v1Shaped : v2Shaped;
                    if (shaped) {
                        const auto& chain = (v == info.v1) ? v1Chain : v2Chain;
                        const int cFront = chain.front();
                        const int cBack  = chain.back();
                        if (cFront == uLeft && cBack == uRight) {
                            for (size_t i = chain.size() - 2; i >= 1; --i)
                                polygon.push_back(chain[i]);
                        } else if (cFront == uRight && cBack == uLeft) {
                            for (size_t i = 1; i + 1 < chain.size(); ++i)
                                polygon.push_back(chain[i]);
                        }
                    }

                    // Dedupe consecutive duplicates and positional matches.
                    std::vector<int> dedup;
                    dedup.reserve(polygon.size());
                    for (int pIdx : polygon) {
                        if (!dedup.empty()) {
                            if (dedup.back() == pIdx) continue;
                            const auto& a = m_vertices[dedup.back()].position;
                            const auto& b = m_vertices[pIdx].position;
                            if (a.squaredDistance(b) < 1e-10f) continue;
                        }
                        dedup.push_back(pIdx);
                    }
                    if (dedup.size() < 3) {
                        // Fallback: process individually.
                        for (int gi : group) {
                            processNeighborFace(v, ring[gi].face, ring);
                        }
                        continue;
                    }

                    // Triangulate via ear-clipping. Shaped profiles make
                    // the polygon concave (intermediates bulge inward), and
                    // a plain fan from dedup[0] would emit a tri spanning
                    // the straight diagonal dedup[0]↔dedup[last], hiding
                    // the concave arc. Ear-clipping handles concave simple
                    // polygons correctly.
                    int subIdx = m_faces[ring[group[0]].face].subMeshIndex;
                    Ogre::Vector3 refNrm = Ogre::Vector3::ZERO;
                    {
                        const auto fv = faceVertices(ring[group[0]].face);
                        if (fv.size() == 3) {
                            const auto& p0 = m_vertices[fv[0]].position;
                            const auto& p1 = m_vertices[fv[1]].position;
                            const auto& p2 = m_vertices[fv[2]].position;
                            refNrm = (p1 - p0).crossProduct(p2 - p0);
                        }
                    }
                    triangulatePolygon(dedup, refNrm, [&](int a, int b, int c) {
                        appendTriangle(a, b, c, subIdx);
                        recordTriEdges(a, b, c);
                    });
                    for (int gi : group) {
                        facesToRemove.push_back(ring[gi].face);
                    }
                } else {
                    // Partial cut or no cut — fall back to per-face logic.
                    for (int gi : group) {
                        processNeighborFace(v, ring[gi].face, ring);
                    }
                }
            }
        };

        if (fullRingV1) processRingNeighbors(info.v1, ringV1);
        if (fullRingV2) processRingNeighbors(info.v2, ringV2);

        // =====================================================================
        // Phase 6: chamfer strip between f1's and f2's inner offsets.
        // Chain intermediates were built before Phase 5 (see above).
        // =====================================================================
        // Emit two triangles per strip i: bridges chain[i]->chain[i+1] on each
        // endpoint. Original (segments=1) winding is preserved when N=1.
        for (int i = 0; i < segments; ++i) {
            const int a = v1Chain[i];      // f1-side at v1, step i
            const int b = v1Chain[i + 1];  // f2-side at v1, step i (toward f2)
            const int c = v2Chain[i + 1];  // f2-side at v2, step i (toward f2)
            const int d = v2Chain[i];      // f1-side at v2, step i
            if (f1WalksAB) {
                appendTriangle(b, c, d, info.subMeshIndex);
                appendTriangle(b, d, a, info.subMeshIndex);
            } else {
                appendTriangle(c, b, a, info.subMeshIndex);
                appendTriangle(c, a, d, info.subMeshIndex);
            }
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
                // No ring (boundary or non-manifold): no neighbor-face
                // retriangulation ran, so chain edges aren't already
                // shared with an adjacent face. Cap the open chamfer end
                // ourselves. For shaped profiles we fan v across the full
                // chain polyline (one tri per chain edge); for flat, one
                // triangle (v, innerA, innerB) suffices.
                const bool shaped = isV1 ? v1Shaped : v2Shaped;
                auto tri = [&](int x, int y, int z) {
                    appendTriangle(x, y, z, info.subMeshIndex);
                };
                if (shaped) {
                    const auto& chain = isV1 ? v1Chain : v2Chain;
                    const bool flip = isV1 ? !fWalksAB : fWalksAB;
                    for (size_t i = 0; i + 1 < chain.size(); ++i) {
                        int a = chain[i], b = chain[i + 1];
                        if (flip) std::swap(a, b);
                        tri(v, a, b);
                    }
                    return;
                }
                int vf1 = innerA, vf2 = innerB;
                if (isV1) {
                    if (fWalksAB) tri(v, vf2, vf1);
                    else          tri(v, vf1, vf2);
                } else {
                    if (fWalksAB) tri(v, vf1, vf2);
                    else          tri(v, vf2, vf1);
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
            bool pushedAnyRingEdgeOffset = false;
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
                bool oneBeveled =
                    (isEffectivelyBeveled(fA) != isEffectivelyBeveled(fB));
                bool crease = facesFormCrease(fA, fB);
                if (oneBeveled && crease) {
                    int u = onEdgeOffset(v, edgeOutX);
                    if (u >= 0) { pushUnique(u); pushedAnyRingEdgeOffset = true; }
                }
                // Non-crease segments between non-beveled neighbors are
                // covered by the merged-group retriangulation (see
                // processRingNeighbors) — don't push v here.
                if (ring[nextPos].face == info.f2) break;
                pos = nextPos;
            }
            pushUnique(innerB);

            // If no on-edge offsets were picked up AND the polygon reduces
            // to [innerA, innerB], the chamfer's end hangs open — insert v
            // so we emit a single cap triangle (innerA, v, innerB). When
            // offsets WERE collected but got deduped away (because the
            // chamfer-end offsets coincide with the crease on-edge offsets),
            // the chamfer's end shares an edge with the neighbor face
            // retriangulation and no cap triangle is needed.
            // Cap handling when polygon reduces to [innerA, innerB]:
            //   - If some non-chamfer tri emitted during Phases 4-5 already
            //     has the (innerA, innerB) edge (e.g., a merged polygon's
            //     fan, or a neighbor face's "both on-edge offsets" branch),
            //     the cap is covered — emitting one would overlap. Skip.
            //   - Otherwise (smooth-mesh or no-crease case), emit a single
            //     cap triangle (v, innerA, innerB) with the empty-ring
            //     winding convention (NOT via the fan path below, which
            //     produces flipped winding for this case). Return early.
            // If any INNER polygon edge (between consecutive pushed
            // offsets) isn't already an edge of an emitted tri, the cap
            // fan would introduce boundary edges. In that case, collapse
            // the polygon to the simple [innerA, v, innerB] cap.
            bool polygonInteriorCovered = true;
            for (size_t i = 0; i + 1 < polygon.size(); ++i) {
                if (!hasEmittedEdge(polygon[i], polygon[i + 1])) {
                    // Acceptable if this is the final edge and it equals
                    // the chamfer-end edge (innerA-innerB, wrap-around).
                    if (i == 0 && polygon.size() == 2) continue;
                    polygonInteriorCovered = false;
                    break;
                }
            }
            if (polygon.size() > 2 && !polygonInteriorCovered) {
                polygon.clear();
                polygon.push_back(innerA);
                polygon.push_back(innerB);
            }
            if (polygon.size() == 2) {
                if (hasEmittedEdge(polygon[0], polygon[1])) return;

                auto tri = [&](int x, int y, int z) {
                    appendTriangle(x, y, z, info.subMeshIndex);
                };

                // For shaped profiles the single-edge cap (v, innerA,
                // innerB) would stretch across the concave arc and hide
                // the intermediate vertices beneath a flat triangle.
                // Check whether the chain's interior edges are already
                // covered by the neighbor-face splice — if so, the
                // chamfer's v-end is already closed and no cap is needed.
                // If not (e.g. a smooth/no-crease mesh where
                // processRingNeighbors never fires for this endpoint), we
                // still need a cap: fan v across the chain polyline so
                // the cap follows the arc instead of crossing it.
                const bool shaped = isV1 ? v1Shaped : v2Shaped;
                if (shaped) {
                    const auto& chain = isV1 ? v1Chain : v2Chain;
                    bool chainClosed = true;
                    for (size_t i = 0; i + 1 < chain.size(); ++i) {
                        if (!hasEmittedEdge(chain[i], chain[i + 1])) {
                            chainClosed = false; break;
                        }
                    }
                    if (chainClosed) return;
                    const bool flip = isV1 ? !fWalksAB : fWalksAB;
                    for (size_t i = 0; i + 1 < chain.size(); ++i) {
                        int a = chain[i], b = chain[i + 1];
                        if (flip) std::swap(a, b);
                        tri(v, a, b);
                    }
                    return;
                }
                // Winding: innerA is on the f1 side, innerB on f2 side.
                // For v1 (the first endpoint) with fWalksAB, the beveled
                // edge goes v1→v2 inside f1 — so the cap's outward normal
                // points the same direction as f1+f2 averaged. Correct
                // winding at v1 (isV1=true, fWalksAB=true) is
                // (v, innerB, innerA): going CCW from the far side, we see
                // v→innerB→innerA. The other 3 cases are derived by
                // swapping innerA↔innerB depending on direction.
                int vf1 = innerA, vf2 = innerB;
                if (isV1) {
                    if (fWalksAB) tri(v, vf2, vf1);
                    else          tri(v, vf1, vf2);
                } else {
                    if (fWalksAB) tri(v, vf1, vf2);
                    else          tri(v, vf2, vf1);
                }
                return;
            }

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
            bool flip = (isV1 ? !fWalksAB : fWalksAB);

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

    // =========================================================================
    // Post-pass: hole filler for bevel-introduced boundaries.
    //
    // Scan the still-alive triangles for boundary edges (edges used once
    // in one direction, with no reverse-direction partner). Restrict the
    // scan to edges whose BOTH endpoints are either bevel-created
    // vertices (in newVertices) OR bevel-touched vertices (v1, v2, f1Opp,
    // f2Opp). This avoids touching the mesh's pre-existing legitimate
    // boundaries (submesh seams on character meshes, etc.).
    //
    // Boundary edges are collected into closed loops and each loop is
    // fan-triangulated from loop[0]. Winding is chosen so each emitted
    // tri's middle edge (loop[i]→loop[i+1]) partners the boundary edge
    // loop[i+1]→loop[i] that we found.
    {
        std::unordered_set<int> repairZone;
        for (int v : newVertices) repairZone.insert(v);
        for (int v : bevelTouchedVerts) repairZone.insert(v);

        // Build directed edge usage map from still-alive faces.
        struct EdgeRef { int subMesh; };
        std::map<std::pair<int,int>, int> dirCount;
        std::map<std::pair<int,int>, int> dirSub;
        for (int f = 0; f < static_cast<int>(m_faces.size()); ++f) {
            if (m_faces[f].halfEdge < 0) continue;
            auto fv = faceVertices(f);
            if (fv.size() != 3) continue;
            int sm = m_faces[f].subMeshIndex;
            for (int k = 0; k < 3; ++k) {
                int a = fv[k], b = fv[(k + 1) % 3];
                dirCount[{a, b}]++;
                dirSub[{a, b}] = sm;
            }
        }

        // Expand the repair zone by position-coincident duplicates:
        // multi-submesh meshes keep separate vertex indices for seams, so
        // V_A moved by a bevel on submesh A leaves coincident V_B in
        // submesh B untouched, producing a crack. Including V_B lets the
        // filler close the seam.
        const float tol2 = 1e-10f;
        {
            std::vector<Ogre::Vector3> zonePositions;
            zonePositions.reserve(repairZone.size());
            for (int v : repairZone) {
                if (v >= 0 && v < (int)m_vertices.size()) {
                    zonePositions.push_back(m_vertices[v].position);
                }
            }
            std::unordered_set<int> coincident;
            for (int v = 0; v < (int)m_vertices.size(); ++v) {
                if (repairZone.count(v)) continue;
                const Ogre::Vector3& p = m_vertices[v].position;
                for (const Ogre::Vector3& zp : zonePositions) {
                    if ((p - zp).squaredLength() < tol2) {
                        coincident.insert(v);
                        break;
                    }
                }
            }
            for (int v : coincident) repairZone.insert(v);
        }

        // Promote half-in-zone seam vertices: when exactly one endpoint of
        // an unpartnered directed edge is in the zone, promote the other
        // IF it's a seam (another vertex shares its position). On
        // single-submesh patches like a fan, seam vertices don't exist,
        // so legitimate open-perimeter vertices stay out of the zone.
        {
            auto isSeamVertex = [&](int v) {
                const Ogre::Vector3& p = m_vertices[v].position;
                for (int u = 0; u < (int)m_vertices.size(); ++u) {
                    if (u == v) continue;
                    if ((m_vertices[u].position - p).squaredLength() < tol2)
                        return true;
                }
                return false;
            };
            std::unordered_set<int> promote;
            for (const auto& [dir, cnt] : dirCount) {
                int a = dir.first, b = dir.second;
                if (cnt != 1) continue;
                auto it = dirCount.find({b, a});
                if (it != dirCount.end() && it->second > 0) continue;
                bool aIn = repairZone.count(a) > 0;
                bool bIn = repairZone.count(b) > 0;
                if (aIn && !bIn && isSeamVertex(b)) promote.insert(b);
                else if (bIn && !aIn && isSeamVertex(a)) promote.insert(a);
            }
            for (int v : promote) repairZone.insert(v);
        }
        // Collect unpartnered directed edges whose BOTH endpoints are in
        // the zone. For each (a, b) boundary, the missing partner would
        // travel b→a, which is what we want to emit.
        std::map<int, std::vector<std::pair<int,int>>> zoneBoundaryBySub;
        for (const auto& [dir, cnt] : dirCount) {
            int a = dir.first, b = dir.second;
            int rev = 0;
            auto it = dirCount.find({b, a});
            if (it != dirCount.end()) rev = it->second;
            if (cnt == 1 && rev == 0) {
                if (!repairZone.count(a) || !repairZone.count(b)) continue;
                zoneBoundaryBySub[dirSub[{a, b}]].push_back({b, a});
            }
        }
        for (const auto& [sm, edges] : zoneBoundaryBySub) {
            // Build adjacency: each needed (start→end) edge
            // One vertex may have MULTIPLE outgoing needed edges (if two
            // separate holes touch the same vertex). Build a multi-map
            // and consume edges as we walk — each edge used once. When
            // we revisit an already-seen vertex during a walk, we've
            // found a sub-loop — extract it and continue from there.
            std::unordered_map<int, std::vector<int>> nextVerts;
            for (const auto& [a, b] : edges) nextVerts[a].push_back(b);
            std::set<std::pair<int,int>> consumed;

            std::vector<std::vector<int>> loopsToProcess;

            for (const auto& [startA, startB] : edges) {
                if (consumed.count({startA, startB})) continue;
                std::vector<int> walk;
                std::unordered_map<int, int> posInWalk;
                int cur = startA;
                int nextCandidate = startB;
                while (true) {
                    if (!consumed.insert({cur, nextCandidate}).second) break;
                    // If cur is already in walk, split out the sub-loop.
                    auto posIt = posInWalk.find(cur);
                    if (posIt != posInWalk.end()) {
                        int startIdx = posIt->second;
                        std::vector<int> subLoop(walk.begin() + startIdx, walk.end());
                        if (subLoop.size() >= 3) {
                            loopsToProcess.push_back(subLoop);
                        }
                        for (size_t j = startIdx; j < walk.size(); ++j) {
                            posInWalk.erase(walk[j]);
                        }
                        walk.resize(startIdx);
                        // After the subloop is removed, the pre-subloop
                        // prefix + cur may itself form a closed outer
                        // loop if cur's just-consumed edge closes back to
                        // walk[0]. Push it immediately so we don't lose
                        // it when the walker breaks below.
                        if (!walk.empty() && walk[0] == nextCandidate
                            && walk.size() >= 2) {
                            std::vector<int> outer = walk;
                            outer.push_back(cur);
                            if (outer.size() >= 3) {
                                loopsToProcess.push_back(outer);
                            }
                        }
                        auto it = nextVerts.find(cur);
                        if (it == nextVerts.end()) break;
                        bool found = false;
                        for (int candidate : it->second) {
                            if (!consumed.count({cur, candidate})) {
                                nextCandidate = candidate;
                                found = true;
                                break;
                            }
                        }
                        if (!found) break;
                        continue;
                    }
                    posInWalk[cur] = static_cast<int>(walk.size());
                    walk.push_back(cur);
                    cur = nextCandidate;
                    auto it = nextVerts.find(cur);
                    if (it == nextVerts.end()) break;
                    bool found = false;
                    for (int candidate : it->second) {
                        if (!consumed.count({cur, candidate})) {
                            nextCandidate = candidate;
                            found = true;
                            break;
                        }
                    }
                    if (!found) break;
                }
                if (!walk.empty()) {
                    auto posIt = posInWalk.find(cur);
                    if (posIt != posInWalk.end()) {
                        int startIdx = posIt->second;
                        std::vector<int> subLoop(walk.begin() + startIdx, walk.end());
                        if (subLoop.size() >= 3) {
                            loopsToProcess.push_back(subLoop);
                        }
                    } else if (!walk.empty()) {
                        // Open walk: salvage only if start and end are
                        // position-coincident (implicit closing edge
                        // crosses a submesh seam).
                        int chainStart = walk[0];
                        int chainEnd = cur;
                        const Ogre::Vector3& pStart =
                            m_vertices[chainStart].position;
                        const Ogre::Vector3& pEnd =
                            m_vertices[chainEnd].position;
                        if ((pStart - pEnd).squaredLength() < 1e-10f
                            && chainStart != chainEnd) {
                            std::vector<int> chain = walk;
                            chain.push_back(cur);
                            if (chain.size() >= 3) {
                                loopsToProcess.push_back(chain);
                            }
                        }
                    }
                }
            }

            for (auto& loop : loopsToProcess) {
                if (loop.size() < 3) continue;
                // Hard cap to avoid filling bizarrely large loops (which
                // almost certainly indicate a non-simple polygon the
                // fan-triangulation would bungle). Scales with segment
                // count: a multi-segment bevel can legitimately produce
                // loops of ~2*(segments+1) verts around the chamfer strip
                // perimeter, so the old hard 8 was too tight for
                // segments>=4. Use a generous but bounded ceiling.
                if (loop.size() > 64) continue;
                // Only fill loops that contain at least one bevel-created
                // offset vertex (in newVertices). Loops made entirely of
                // pre-existing perimeter vertices are legitimate open
                // edges (e.g., quad bevel test's mesh perimeter) — don't
                // close them.
                bool hasNewVert = false;
                for (int lv : loop) {
                    if (std::find(newVertices.begin(), newVertices.end(),
                                  lv) != newVertices.end()) {
                        hasNewVert = true;
                        break;
                    }
                }
                if (!hasNewVert) continue;
                // Winding decision via per-option "boundary closure"
                // scoring. For each candidate winding, count how many of
                // the loop's directed boundary edges would be PARTNERED
                // (existing tri has the opposite direction) by the fan,
                // and subtract how many would be duplicated (same
                // direction). Pick the winding with the best score.
                auto scoreWinding = [&](bool flip) {
                    int partnered = 0;
                    int duplicates = 0;
                    for (size_t i = 1; i + 1 < loop.size(); ++i) {
                        int a = loop[0], b = loop[i], c = loop[i + 1];
                        if (flip) std::swap(b, c);
                        for (auto [u, v] : std::vector<std::pair<int,int>>{
                                 {a, b}, {b, c}, {c, a}}) {
                            auto fwd = dirCount.find({u, v});
                            auto rev = dirCount.find({v, u});
                            int fwdN = (fwd == dirCount.end()) ? 0 : fwd->second;
                            int revN = (rev == dirCount.end()) ? 0 : rev->second;
                            if (fwdN > 0) duplicates += fwdN;
                            if (revN > 0) partnered += revN;
                        }
                    }
                    return partnered - 2 * duplicates;
                };
                int noFlipScore = scoreWinding(false);
                int flipScore = scoreWinding(true);
                int noFlipConflicts = -noFlipScore;
                int flipConflicts = -flipScore;

                // Reference normal from tris that share a loop edge
                // (undirected).
                Ogre::Vector3 refNormal = Ogre::Vector3::ZERO;
                std::set<std::pair<int,int>> loopEdges;
                for (size_t i = 0; i + 1 < loop.size(); ++i) {
                    loopEdges.insert({std::min(loop[i], loop[i + 1]),
                                      std::max(loop[i], loop[i + 1])});
                }
                loopEdges.insert({std::min(loop.back(), loop.front()),
                                  std::max(loop.back(), loop.front())});
                for (int f = 0; f < static_cast<int>(m_faces.size()); ++f) {
                    if (m_faces[f].halfEdge < 0) continue;
                    auto fv = faceVertices(f);
                    if (fv.size() != 3) continue;
                    bool shares = false;
                    for (int k = 0; k < 3; ++k) {
                        int ea = fv[k], eb = fv[(k + 1) % 3];
                        if (loopEdges.count({std::min(ea, eb),
                                             std::max(ea, eb)})) {
                            shares = true; break;
                        }
                    }
                    if (!shares) continue;
                    Ogre::Vector3 p0 = m_vertices[fv[0]].position;
                    Ogre::Vector3 p1 = m_vertices[fv[1]].position;
                    Ogre::Vector3 p2 = m_vertices[fv[2]].position;
                    Ogre::Vector3 n = (p1 - p0).crossProduct(p2 - p0);
                    if (n.length() > 1e-8f) {
                        n.normalise();
                        refNormal += n;
                    }
                }
                // Topology-first winding: prefer the winding that partners
                // the most directed boundary edges with existing tris and
                // duplicates the fewest. This is robust against curved
                // fill surfaces (e.g., concave multi-segment bevels) where
                // the geometric face-normal check can flip sign based on
                // profile even though the correct topological winding is
                // unchanged.
                //
                // Fall back to the geometric refNormal check only when the
                // topology scores are tied (ambiguous), which happens when
                // the loop's boundary edges don't meaningfully overlap the
                // existing mesh — then visual appearance is the only cue.
                bool flipWinding;
                if (flipScore != noFlipScore) {
                    flipWinding = flipScore > noFlipScore;
                } else if (refNormal.length() > 0.1f && loop.size() >= 3) {
                    Ogre::Vector3 p0 = m_vertices[loop[0]].position;
                    Ogre::Vector3 p1 = m_vertices[loop[1]].position;
                    Ogre::Vector3 p2 = m_vertices[loop[2]].position;
                    Ogre::Vector3 noFlipN = (p1 - p0).crossProduct(p2 - p0);
                    if (noFlipN.length() > 1e-6f) {
                        noFlipN.normalise();
                        flipWinding = refNormal.dotProduct(noFlipN) < 0;
                    } else {
                        flipWinding = false;
                    }
                } else {
                    flipWinding = false;
                }
                (void)noFlipConflicts;
                (void)flipConflicts;
                for (size_t i = 1; i + 1 < loop.size(); ++i) {
                    int a = loop[0], b = loop[i], c = loop[i + 1];
                    if (flipWinding) std::swap(b, c);
                    appendTriangle(a, b, c, sm);
                }
            }
        }
    }

    rebuildEdgesAndTwins();
    compactBoundaryHalfEdges();
    buildBoundaryHalfEdges();
    fixVertexHalfEdges();

    return newVertices;
}

// ===========================================================================
// bevelVertices — corner cut at each selected vertex.
// ===========================================================================
//
// For each vertex v of valence N >= 3:
//   1. Walk v's face ring, collecting the ring-ordered sequence of incident
//      faces and their half-edges. Skip boundary verts for the MVP.
//   2. For each outgoing edge v->x_i (i = 0..N-1), create a new vertex o_i
//      at v + (x_i - v) * offset / |x_i - v|. offset is clamped per-vertex
//      to half the shortest incident edge so neighboring vertex bevels
//      don't overlap.
//   3. For each face f that contained v: replace v with the two edge
//      offsets on that face's two v-edges. The old triangle (v, a, b)
//      becomes a quad (o_a, a, b, o_b), triangulated as two tris.
//   4. Emit a new "cap" face using all o_i in ring order. Valence 3 gives
//      a single triangle, higher valence an N-gon fanned from o_0.
//
// This MVP emits a flat cap (segments=1). Shaped profiles and segments>1
// are TODO (rounded dome). Unused params are accepted for forward
// compatibility with the edge-bevel API.
std::vector<int> HalfEdgeMesh::bevelVertices(
    const std::vector<int>& vertexIndices,
    float width,
    int segments,
    float profile,
    const std::vector<float>& profilePointsIn)
{
    std::vector<int> newVertices;
    if (vertexIndices.empty() || width <= 0.0f) return newVertices;

    segments = std::clamp(segments, 1, kMaxBevelSegments);
    profile = std::clamp(profile, 0.0f, 1.0f);

    // Build the per-interior-point profile values, same scheme as
    // edge-bevel: an explicit vector of the right size is used directly;
    // otherwise a sin-envelope fills in from the scalar `profile`.
    std::vector<float> profilePoints;
    if (segments > 1) {
        profilePoints.resize(segments - 1, 0.5f);
        if (profilePointsIn.size() == static_cast<size_t>(segments - 1)) {
            for (size_t i = 0; i < profilePoints.size(); ++i)
                profilePoints[i] = std::clamp(profilePointsIn[i], 0.0f, 1.0f);
        } else {
            const float kPi = 3.14159265358979323846f;
            const float amp = profile - 0.5f;
            for (int i = 1; i < segments; ++i) {
                const float t = static_cast<float>(i)
                              / static_cast<float>(segments);
                profilePoints[i - 1] = 0.5f + amp * std::sin(kPi * t);
            }
        }
    }

    // Multi-vertex support: when multiple vertices are passed, we bevel
    // them sequentially — one per internal iteration. Each iteration
    // rebuilds the half-edge adjacency before the next vertex is
    // processed, so the next bevel sees the updated topology from the
    // previous one. This sidesteps the shared-edge-gap problem that
    // arises when two adjacent vertices are processed in the same pass.
    //
    // When two selected vertices share an edge, each side should claim
    // HALF of that edge. Without an up-front budget, the first vertex
    // to run clamps to edge_length/2 and the second clamps against the
    // now-shortened edge, producing asymmetric offsets that "push" each
    // other. Precompute a per-vertex width from the PRISTINE mesh so
    // both sides converge to the midpoint when they meet.
    if (vertexIndices.size() > 1) {
        std::unordered_set<int> selected(vertexIndices.begin(), vertexIndices.end());
        std::vector<float> perVertexWidth(vertexIndices.size(), width);
        for (size_t i = 0; i < vertexIndices.size(); ++i) {
            int v = vertexIndices[i];
            if (v < 0 || v >= static_cast<int>(m_vertices.size())) continue;
            int startHE = m_vertices[v].halfEdge;
            if (startHE < 0) continue;
            int he = startHE;
            float minBudget = std::numeric_limits<float>::max();
            for (int guard = 0; guard < 1024; ++guard) {
                if (he < 0) break;
                int n = m_halfEdges[he].vertex;
                const float edgeLen =
                    m_vertices[v].position.distance(m_vertices[n].position);
                // Shared edges (both endpoints selected) split 50/50 with
                // a near-full ceiling — each side reaches ~0.4995 × edgeLen,
                // meeting the neighbour's bevel near the midpoint. Unshared
                // edges use the same 0.499 × edgeLen safety clamp the
                // single-vertex path would apply, so disconnected multi-
                // vertex selections don't over-reach when
                // m_skipVertexBevelClamp (set below) bypasses the single-
                // vertex re-clamp.
                float budget = selected.count(n)
                               ? edgeLen * 0.999f * 0.5f
                               : edgeLen * 0.499f;
                if (budget < minBudget) minBudget = budget;
                int prev = m_halfEdges[he].prev;
                int twin = m_halfEdges[prev].twin;
                if (twin < 0) break;
                he = twin;
                if (he == startHE) break;
            }
            if (minBudget < width) perVertexWidth[i] = minBudget;
        }
        // Tell the single-vertex path to honor our pre-budgeted widths
        // verbatim. Otherwise the single-vertex clamp re-clamps against
        // the mutated mesh's edge lengths (prior bevels have shortened
        // the shared edges), which produces asymmetric offsets. Flag is
        // reset after the loop so a later top-level bevelVertices call
        // starts with the usual safety clamp enabled.
        m_skipVertexBevelClamp = true;
        for (size_t i = 0; i < vertexIndices.size(); ++i) {
            auto added = bevelVertices({vertexIndices[i]}, perVertexWidth[i],
                                       segments, profile, profilePointsIn);
            newVertices.insert(newVertices.end(), added.begin(), added.end());
        }
        m_skipVertexBevelClamp = false;
        return newVertices;
    }

    // Walk v's outgoing half-edges in ring order. Returns one step per
    // incident face. For our mesh representation an interior "diagonal"
    // edge inside a logical polygon (two coplanar tris sharing it) still
    // shows up as a ring step — the MVP accepts that and just cuts each
    // tri corner. A future improvement could merge coplanar siblings so
    // a cube-corner bevel always produces N = topological valence.
    struct RingStep {
        int face;
        int outHE;       // outgoing half-edge from v in this face
        int neighbor;    // far endpoint of this outgoing edge (a v-neighbor)
        int vEdgeNext;   // the face's third corner from v's perspective
                         // (target of m_halfEdges[outHE].next)
    };
    auto buildRing = [&](int v) -> std::vector<RingStep> {
        std::vector<RingStep> ring;
        if (v < 0 || v >= static_cast<int>(m_vertices.size())) return ring;
        int startHE = m_vertices[v].halfEdge;
        if (startHE < 0) return ring;
        int he = startHE;
        for (int guard = 0; guard < 1024; ++guard) {
            if (he < 0) return {};
            int face = m_halfEdges[he].face;
            if (face < 0) return {}; // boundary — bail
            RingStep step;
            step.face = face;
            step.outHE = he;
            step.neighbor = m_halfEdges[he].vertex;
            step.vEdgeNext = m_halfEdges[m_halfEdges[he].next].vertex;
            ring.push_back(step);
            int prev = m_halfEdges[he].prev;
            int twin = m_halfEdges[prev].twin;
            if (twin < 0) return {}; // boundary
            he = twin;
            if (he == startHE) return ring;
        }
        return {}; // guard tripped
    };

    // True if the edge between two ring-adjacent faces is a geometric
    // crease (non-coplanar). A "diagonal" inside a planar polygon
    // (two coplanar tris sharing a diagonal) is NOT a crease.
    auto facesFormCrease = [&](int fA, int fB) {
        auto faceNormal = [&](int f) {
            Ogre::Vector3 n = Ogre::Vector3::ZERO;
            auto fv = faceVertices(f);
            if (fv.size() != 3) return n;
            const auto& p0 = m_vertices[fv[0]].position;
            const auto& p1 = m_vertices[fv[1]].position;
            const auto& p2 = m_vertices[fv[2]].position;
            n = (p1 - p0).crossProduct(p2 - p0);
            if (n.length() > 1e-8f) n.normalise();
            return n;
        };
        auto nA = faceNormal(fA), nB = faceNormal(fB);
        if (nA.length() < 0.5f || nB.length() < 0.5f) return true;
        return nA.dotProduct(nB) < 0.999f;
    };

    // Pre-compute each vertex's ring + clamped offset + new edge-offset
    // vertex indices. We defer face removal / re-emission to a second
    // pass so ring-walking isn't disturbed mid-loop.
    struct VertBevelPlan {
        int v;
        std::vector<RingStep> ring;
        std::vector<int> edgeOffsets;    // one new vertex per CREASE
                                         // (topological edge at v)
        std::vector<int> stepToCrease;   // interleaved creaseL/creaseR
        std::vector<int> creaseTargets;  // ring-ordered crease neighbors
        std::vector<char> isCrease;      // per ring step
        // For segments > 1: chordChains[c] holds segments-1 intermediate
        // vertex indices along the chord from crease c's offset to
        // crease (c+1 % M)'s offset. Empty when segments == 1.
        std::vector<std::vector<int>> chordChains;
        int subMeshIndex;                // for the new cap face
    };
    std::vector<VertBevelPlan> plans;
    plans.reserve(vertexIndices.size());
    std::unordered_set<int> beveledVerts(vertexIndices.begin(), vertexIndices.end());
    std::unordered_set<int> facesToRemove;

    for (int v : vertexIndices) {
        if (v < 0 || v >= static_cast<int>(m_vertices.size())) continue;
        auto ring = buildRing(v);
        if (ring.empty()) continue;
        const int N = static_cast<int>(ring.size());

        // Identify creases: ring step i's outgoing edge (v→neighbor_i)
        // is a crease iff the edge's two faces (prev step's face and
        // step i's face) are non-coplanar. isCreaseAtStep[i] marks this.
        std::vector<bool> isCreaseAtStep(N, false);
        for (int i = 0; i < N; ++i) {
            int prev = (i - 1 + N) % N;
            isCreaseAtStep[i] = facesFormCrease(ring[prev].face, ring[i].face);
        }
        // Topological valence = number of creases.
        int topoValence = 0;
        for (bool b : isCreaseAtStep) if (b) ++topoValence;
        if (topoValence < 3) continue;

        // For each ring step i, stepToCrease[i] = crease index (in
        // creaseOrder) of the crease at step i's OUTGOING edge if it's
        // a crease, else the crease to the RIGHT (the next crease when
        // walking forward — equivalently the crease at the start of the
        // coplanar run this step belongs to).
        //
        // We actually want two mappings per step:
        //   creaseL[i] = crease at the outgoing edge (v→neighbor_i)
        //                or, if not a crease, the crease at the run's
        //                starting edge.
        //   creaseR[i] = crease at the next-step's outgoing edge
        //                (v→ring[(i+1)%N].neighbor) or, if not a crease,
        //                the crease at the end of the run.
        // Within a coplanar run, creaseL == creaseR — all interior tris
        // of the run share the same two offsets at v's corner.
        std::vector<int> creaseOrder;      // ordered list of crease step indices
        std::vector<int> creaseL(N, -1);
        std::vector<int> creaseR(N, -1);
        for (int i = 0; i < N; ++i) {
            if (isCreaseAtStep[i]) creaseOrder.push_back(i);
        }
        // creaseL[i] = index in creaseOrder of the most-recent crease
        // at-or-before step i (walking backward). This is the crease on
        // step i's outgoing edge (v→neighbor_i) when step i IS a crease,
        // or the crease at the start of its coplanar run otherwise.
        //
        // creaseR[i] = index in creaseOrder of the NEXT crease strictly
        // AFTER step i (walking forward, wrapping around). This is the
        // crease on step (i+1)'s outgoing edge if that's a crease, or
        // the next crease further along.
        {
            int startStep = creaseOrder[0];
            int curCreaseIdx = -1;
            for (int k = 0; k < N; ++k) {
                int i = (startStep + k) % N;
                if (isCreaseAtStep[i]) {
                    curCreaseIdx = static_cast<int>(
                        std::find(creaseOrder.begin(), creaseOrder.end(), i)
                        - creaseOrder.begin());
                }
                creaseL[i] = curCreaseIdx;
            }
            // creaseR: walk forward looking for strict-next crease.
            for (int i = 0; i < N; ++i) {
                int cur = -1;
                for (int k = 1; k <= N; ++k) {
                    int j = (i + k) % N;
                    if (isCreaseAtStep[j]) {
                        cur = static_cast<int>(
                            std::find(creaseOrder.begin(), creaseOrder.end(), j)
                            - creaseOrder.begin());
                        break;
                    }
                }
                creaseR[i] = cur;
            }
        }

        // The target vertex of crease c is ring[creaseOrder[c]].neighbor
        // (the far endpoint of the v→neighbor edge that crease sits on).
        std::vector<int> creaseTargets(creaseOrder.size());
        for (size_t c = 0; c < creaseOrder.size(); ++c) {
            creaseTargets[c] = ring[creaseOrder[c]].neighbor;
        }

        // Shortest crease edge from v.
        float minEdgeLen = std::numeric_limits<float>::max();
        for (int tgt : creaseTargets) {
            float d = m_vertices[v].position.distance(m_vertices[tgt].position);
            if (d < minEdgeLen) minEdgeLen = d;
        }
        const float offset = m_skipVertexBevelClamp
                             ? width
                             : std::min(width, 0.499f * minEdgeLen);
        if (offset <= 1e-6f) continue;

        VertBevelPlan plan;
        plan.v = v;
        plan.ring = ring;
        plan.subMeshIndex = m_faces[ring[0].face].subMeshIndex;
        plan.edgeOffsets.resize(creaseTargets.size());
        plan.creaseTargets = creaseTargets;
        // Pack creaseL and creaseR into a single vector: even indices
        // are creaseL, odd are creaseR.
        plan.stepToCrease.resize(N * 2);
        for (int i = 0; i < N; ++i) {
            plan.stepToCrease[2 * i + 0] = creaseL[i];
            plan.stepToCrease[2 * i + 1] = creaseR[i];
        }
        plan.isCrease.assign(isCreaseAtStep.begin(), isCreaseAtStep.end());

        // Snapshot pV by value — m_vertices grows inside the loop so a
        // reference would dangle after the first push_back reallocation.
        const Ogre::Vector3 pV = m_vertices[v].position;
        HEVertex vProto = m_vertices[v];
        vProto.halfEdge = -1;
        for (size_t c = 0; c < creaseTargets.size(); ++c) {
            const Ogre::Vector3 pN = m_vertices[creaseTargets[c]].position;
            Ogre::Vector3 dir = pN - pV;
            const float len = dir.length();
            if (len < 1e-8f) { plan.edgeOffsets[c] = -1; continue; }
            dir /= len;
            HEVertex nv = vProto;
            nv.position = pV + dir * offset;
            int idx = static_cast<int>(m_vertices.size());
            m_vertices.push_back(nv);
            newVertices.push_back(idx);
            plan.edgeOffsets[c] = idx;
        }

        // Build chord intermediates between each adjacent pair of
        // crease offsets. For segments=S we add S-1 intermediates per
        // chord bulging toward v (positive profile) or away from v
        // (negative). Linear along the chord; outward offset along the
        // cap-plane normal (computed per-chord from its endpoints and
        // v's position).
        if (segments > 1 && plan.edgeOffsets.size() >= 3) {
            const int M = static_cast<int>(plan.edgeOffsets.size());
            plan.chordChains.resize(M);
            // Scale the bulge so fully-convex lands at v (profile=1 →
            // offset of half chord length toward v); anything less
            // scales linearly. Keeps the look consistent with edge
            // bevel where the scale is `w`.
            for (int c = 0; c < M; ++c) {
                int oA = plan.edgeOffsets[c];
                int oB = plan.edgeOffsets[(c + 1) % M];
                if (oA < 0 || oB < 0) continue;
                const Ogre::Vector3 pA = m_vertices[oA].position;
                const Ogre::Vector3 pB = m_vertices[oB].position;
                const Ogre::Vector3 chord = pB - pA;
                const float chordLen = chord.length();
                // outward = from chord midpoint toward v, projected
                // onto the perpendicular-to-chord direction (so the
                // bulge is perpendicular to the chord, not along it).
                Ogre::Vector3 outward = pV - (pA + pB) * 0.5f;
                if (chordLen > 1e-6f) {
                    const Ogre::Vector3 chordN = chord / chordLen;
                    outward -= chordN * outward.dotProduct(chordN);
                }
                if (outward.length() > 1e-6f) outward.normalise();
                else outward = Ogre::Vector3::ZERO;
                // Magnitude: use the offset distance (same as edge-bevel's
                // `w`), so the bulge is commensurate with the cut depth.
                const float bulgeScale = offset;
                plan.chordChains[c].reserve(segments - 1);
                for (int i = 1; i < segments; ++i) {
                    const float t = static_cast<float>(i)
                                  / static_cast<float>(segments);
                    const float pt = profilePoints[i - 1];
                    const Ogre::Vector3 pos =
                        pA + chord * t + outward * ((pt - 0.5f) * bulgeScale);
                    HEVertex nvI = vProto;
                    nvI.position = pos;
                    int iIdx = static_cast<int>(m_vertices.size());
                    m_vertices.push_back(nvI);
                    newVertices.push_back(iIdx);
                    plan.chordChains[c].push_back(iIdx);
                }
            }
        }

        plans.push_back(std::move(plan));
    }

    if (plans.empty()) return newVertices;

    // Reusable ear-clipping helper (used for both adjacent-face polygon
    // retriangulation and cap-polygon emission). Polygon verts are HE
    // vertex indices; refNrm gives the desired face-outward direction
    // for winding.
    auto earClipPoly = [&](const std::vector<int>& p,
                           const Ogre::Vector3& refNrm,
                           int subIdx) {
        if (p.size() < 3) return;
        if (p.size() == 3) {
            appendTriangle(p[0], p[1], p[2], subIdx);
            return;
        }
        Ogre::Vector3 nrm = refNrm;
        if (nrm.length() < 1e-8f) nrm = Ogre::Vector3::UNIT_Y;
        else nrm.normalise();
        Ogre::Vector3 uu = std::abs(nrm.x) < 0.9f
            ? Ogre::Vector3::UNIT_X.crossProduct(nrm)
            : Ogre::Vector3::UNIT_Y.crossProduct(nrm);
        uu.normalise();
        Ogre::Vector3 vv = nrm.crossProduct(uu);
        std::vector<Ogre::Vector2> pts2D;
        pts2D.reserve(p.size());
        for (int idx : p) {
            const auto& pos = m_vertices[idx].position;
            pts2D.emplace_back(pos.dotProduct(uu), pos.dotProduct(vv));
        }
        float area = 0.0f;
        for (size_t i = 0; i < pts2D.size(); ++i) {
            const auto& a = pts2D[i];
            const auto& b = pts2D[(i + 1) % pts2D.size()];
            area += a.x * b.y - b.x * a.y;
        }
        std::vector<int> poly = p;
        std::vector<Ogre::Vector2> pxy = pts2D;
        bool reversed = false;
        if (area < 0.0f) {
            std::reverse(poly.begin(), poly.end());
            std::reverse(pxy.begin(), pxy.end());
            reversed = true;
        }
        auto cross2 = [](const Ogre::Vector2& a,
                         const Ogre::Vector2& b,
                         const Ogre::Vector2& c) {
            return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        };
        auto inTri = [&](const Ogre::Vector2& q,
                         const Ogre::Vector2& a,
                         const Ogre::Vector2& b,
                         const Ogre::Vector2& c) {
            float d1 = cross2(q, a, b);
            float d2 = cross2(q, b, c);
            float d3 = cross2(q, c, a);
            bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
            bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
            return !(neg && pos);
        };
        std::vector<bool> removed(poly.size(), false);
        size_t remaining = poly.size();
        size_t cur = 0;
        int guard = 0;
        const int lim = static_cast<int>(poly.size() * poly.size() + 10);
        auto emit = [&](int a, int b, int c) {
            if (reversed) appendTriangle(a, c, b, subIdx);
            else          appendTriangle(a, b, c, subIdx);
        };
        while (remaining > 3 && guard++ < lim) {
            while (removed[cur]) cur = (cur + 1) % poly.size();
            size_t prev = (cur + poly.size() - 1) % poly.size();
            while (removed[prev]) prev = (prev + poly.size() - 1) % poly.size();
            size_t nxt = (cur + 1) % poly.size();
            while (removed[nxt]) nxt = (nxt + 1) % poly.size();
            const auto& A = pxy[prev];
            const auto& B = pxy[cur];
            const auto& C = pxy[nxt];
            if (cross2(A, B, C) > 0.0f) {
                bool isEar = true;
                for (size_t i = 0; i < poly.size(); ++i) {
                    if (removed[i] || i == prev || i == cur || i == nxt) continue;
                    if (inTri(pxy[i], A, B, C)) { isEar = false; break; }
                }
                if (isEar) {
                    emit(poly[prev], poly[cur], poly[nxt]);
                    removed[cur] = true;
                    --remaining;
                    cur = prev;
                    continue;
                }
            }
            cur = nxt;
        }
        if (remaining == 3) {
            size_t id[3]; size_t k = 0;
            for (size_t i = 0; i < poly.size(); ++i)
                if (!removed[i]) id[k++] = i;
            if (k == 3) emit(poly[id[0]], poly[id[1]], poly[id[2]]);
        }
    };

    // Build a per-edge "offset-at-v-toward-t" map. For each plan, plan's
    // vertex v has offsets[c] along the edge v→creaseTargets[c]. If the
    // neighbor t is ALSO beveled, t's own plan has an offset along t→v
    // that we want to substitute for t in this plan's face retri.
    //
    // Key: (vertex, targetVertex). Value: offset HE index.
    std::unordered_map<long long, int> offsetAtVTowardT;
    auto edgeKey = [](int a, int b) {
        return static_cast<long long>(a) * 1000000LL + b;
    };
    for (const auto& p : plans) {
        for (size_t c = 0; c < p.creaseTargets.size(); ++c) {
            if (p.edgeOffsets[c] < 0) continue;
            offsetAtVTowardT[edgeKey(p.v, p.creaseTargets[c])] = p.edgeOffsets[c];
        }
    }
    // Helper: substitute neighbor by the neighbor's own offset-toward-v
    // if the neighbor is also beveled.
    auto substituteIfBeveled = [&](int planV, int neighbor) {
        auto it = offsetAtVTowardT.find(edgeKey(neighbor, planV));
        return (it != offsetAtVTowardT.end()) ? it->second : neighbor;
    };

    // Pass 2: for each planned vertex, retriangulate each face to
    // replace v with the two crease offsets bracketing its run; emit
    // the cap N-gon (one tri per crease fanned from crease 0).
    for (const auto& plan : plans) {
        (void)beveledVerts;

        // Retriangulate coplanar runs. A run is a maximal contiguous
        // block of ring steps whose outgoing edges (except the first)
        // are all non-creases — i.e., the same logical polygon
        // triangulated along its diagonals.
        //
        // Each run's polygon is:
        //   [o_L, n_0', vEdgeNext_0', vEdgeNext_1', ..., vEdgeNext_last', o_R]
        // where primes mean "substitute a beveled neighbor by its own
        // offset toward this plan's v". Fan from o_L.
        //
        // Ownership rule for multi-vertex bevels: a run might include
        // vertices that are themselves beveled. To avoid another plan
        // also retriangulating the same tri, we only retriangulate
        // faces whose lowest-index corner in {v, any other beveled
        // vertex in the tri} equals this plan's v. I.e., the
        // lowest-index plan among those touching the tri owns it.
        // A run is "owned" by this plan when plan.v is the lowest-index
        // beveled vertex among all vertices appearing in the run's
        // faces. Ensures each logical face is retriangulated exactly
        // once regardless of how many of its corners are beveled.
        auto runOwnedByThisPlan = [&](const std::vector<int>& runSteps) {
            int lowestBeveled = plan.v;
            for (int s : runSteps) {
                auto fv = faceVertices(plan.ring[s].face);
                for (int fvIdx : fv) {
                    if (beveledVerts.count(fvIdx) && fvIdx < lowestBeveled)
                        lowestBeveled = fvIdx;
                }
            }
            return lowestBeveled == plan.v;
        };

        auto isCrease = [&](int i) -> bool { return plan.isCrease[i] != 0; };
        const int N = static_cast<int>(plan.ring.size());
        int runStart = -1;
        for (int i = 0; i < N; ++i) {
            if (isCrease(i)) { runStart = i; break; }
        }
        if (runStart < 0) continue;

        int cursor = runStart;
        for (int visited = 0; visited < N; ) {
            std::vector<int> runSteps;
            runSteps.push_back(cursor);
            ++visited;
            int nextStep = (cursor + 1) % N;
            while (visited < N && !isCrease(nextStep)) {
                runSteps.push_back(nextStep);
                ++visited;
                nextStep = (nextStep + 1) % N;
            }

            // Ownership check: only the lowest-index beveled vertex
            // that appears in ANY face of the run retriangulates it.
            // Other beveled verts in the run just leave the tri alone
            // here — the owner's polygon (with substitutions for those
            // other beveled corners) covers it.
            if (!runOwnedByThisPlan(runSteps)) {
                // Even when not owning, we still need to mark the
                // member faces for removal — they'll be re-added by
                // the owner's polygon emission below.
                cursor = nextStep;
                continue;
            }

            const int cL = plan.stepToCrease[2 * runSteps.front() + 0];
            const int cR = plan.stepToCrease[2 * runSteps.back() + 1];
            if (cL < 0 || cR < 0) { cursor = nextStep; continue; }
            const int o_L = plan.edgeOffsets[cL];
            const int o_R = plan.edgeOffsets[cR];

            std::vector<int> polygon;
            polygon.push_back(o_L);
            polygon.push_back(substituteIfBeveled(
                plan.v, plan.ring[runSteps.front()].neighbor));
            for (int s : runSteps) {
                polygon.push_back(substituteIfBeveled(
                    plan.v, plan.ring[s].vEdgeNext));
            }
            polygon.push_back(o_R);

            // Splice chord intermediates between o_R and the implicit
            // close-back-to-o_L, so the adjacent face's notch boundary
            // follows the cap curve instead of a straight diagonal.
            // Skip when flat (profile 0.5 collinear with chord — the
            // same "shaped" test edge-bevel uses) to preserve manifold
            // in the single-segment case.
            const int M = static_cast<int>(plan.edgeOffsets.size());
            // chordChains[cL] walks o_L → ... → o_R if cR == (cL+1)%M.
            // Otherwise walks backward (o_R → ... → o_L) through
            // chordChains[cR]. We want the closing path o_R → o_L, so
            // insert intermediates in the direction that bridges that.
            if (!plan.chordChains.empty()
                && cL >= 0 && cR >= 0 && cL != cR) {
                auto isShaped = [&](const std::vector<int>& chain,
                                    int oL, int oR) {
                    if (chain.empty()) return false;
                    const auto& pL = m_vertices[oL].position;
                    const auto& pR = m_vertices[oR].position;
                    const auto lr = pR - pL;
                    const float lrLen2 = lr.squaredLength();
                    if (lrLen2 < 1e-12f) return false;
                    for (int idx : chain) {
                        const auto& p = m_vertices[idx].position;
                        const auto d = p - pL;
                        const auto proj = lr * (d.dotProduct(lr) / lrLen2);
                        if ((d - proj).squaredLength() > 1e-8f) return true;
                    }
                    return false;
                };
                if ((cL + 1) % M == cR) {
                    // chain runs o_L → ... → o_R. Close back via reverse.
                    const auto& chain = plan.chordChains[cL];
                    if (isShaped(chain, o_L, o_R)) {
                        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                            polygon.push_back(*it);
                    }
                } else if ((cR + 1) % M == cL) {
                    // chain runs o_R → ... → o_L. Close-back walks it
                    // in forward order.
                    const auto& chain = plan.chordChains[cR];
                    if (isShaped(chain, o_R, o_L)) {
                        for (int idx : chain) polygon.push_back(idx);
                    }
                }
            }

            // Dedupe consecutive duplicates and positional matches.
            std::vector<int> dedup;
            for (int idx : polygon) {
                if (!dedup.empty() && dedup.back() == idx) continue;
                if (!dedup.empty() &&
                    m_vertices[dedup.back()].position.squaredDistance(
                        m_vertices[idx].position) < 1e-10f) continue;
                dedup.push_back(idx);
            }
            if (dedup.size() >= 2 && dedup.front() == dedup.back())
                dedup.pop_back();
            if (dedup.size() < 3) { cursor = nextStep; continue; }

            const int sub = m_faces[plan.ring[runSteps[0]].face].subMeshIndex;
            // When the profile is shaped (chord intermediates spliced
            // in), the polygon is concave and a plain fan from dedup[0]
            // would draw a straight diagonal across the arc. Use
            // ear-clipping against the face's normal so concave polys
            // triangulate without artificial diagonals.
            Ogre::Vector3 refNrm = Ogre::Vector3::ZERO;
            {
                auto fv = faceVertices(plan.ring[runSteps[0]].face);
                if (fv.size() == 3) {
                    const auto& p0 = m_vertices[fv[0]].position;
                    const auto& p1 = m_vertices[fv[1]].position;
                    const auto& p2 = m_vertices[fv[2]].position;
                    refNrm = (p1 - p0).crossProduct(p2 - p0);
                }
            }
            earClipPoly(dedup, refNrm, sub);
            for (int s : runSteps) {
                facesToRemove.insert(plan.ring[s].face);
            }

            cursor = nextStep;
        }

        // Cap polygon: walk ring-ordered offsets, inserting each chord's
        // intermediates between consecutive offsets. For segments=1 the
        // chains are empty and this reduces to the plain N-gon fan.
        // Flat-profile chains are collinear with their chord — skip
        // them so the cap stays a simple N-gon with no degenerate ear
        // triangles.
        const int Mcap = static_cast<int>(plan.edgeOffsets.size());
        auto chordIsShaped = [&](int c) {
            if (c < 0 || c >= static_cast<int>(plan.chordChains.size())) return false;
            const auto& chain = plan.chordChains[c];
            if (chain.empty()) return false;
            int oA = plan.edgeOffsets[c];
            int oB = plan.edgeOffsets[(c + 1) % Mcap];
            if (oA < 0 || oB < 0) return false;
            const auto& pA = m_vertices[oA].position;
            const auto& pB = m_vertices[oB].position;
            const auto ab = pB - pA;
            const float lenSq = ab.squaredLength();
            if (lenSq < 1e-12f) return false;
            for (int idx : chain) {
                const auto& p = m_vertices[idx].position;
                const auto d = p - pA;
                const auto proj = ab * (d.dotProduct(ab) / lenSq);
                if ((d - proj).squaredLength() > 1e-8f) return true;
            }
            return false;
        };
        std::vector<int> capPoly;
        capPoly.reserve(Mcap * segments);
        for (int c = 0; c < Mcap; ++c) {
            capPoly.push_back(plan.edgeOffsets[c]);
            if (segments > 1 && chordIsShaped(c)) {
                for (int idx : plan.chordChains[c])
                    capPoly.push_back(idx);
            }
        }
        if (capPoly.size() >= 3) {
            // Cap normal: approximate as the average of the ring steps'
            // outward directions toward v (robust for any valence).
            Ogre::Vector3 capNrm = Ogre::Vector3::ZERO;
            for (int c = 0; c + 1 < Mcap; ++c) {
                const auto& pA = m_vertices[plan.edgeOffsets[0]].position;
                const auto& pB = m_vertices[plan.edgeOffsets[c + 1]].position;
                const auto& pC = m_vertices[plan.edgeOffsets[
                    (c + 1) % Mcap == 0 ? 0 : (c + 2) % Mcap]].position;
                capNrm += (pB - pA).crossProduct(pC - pA);
            }
            earClipPoly(capPoly, capNrm, plan.subMeshIndex);
        }
    }

    // Remove the old faces.
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

namespace {

// Interpolate a new vertex between two existing ones at parameter t.
// Position / normal / UV / color / tangent / bone weights are lerped by t,
// with an eps safety margin so an exactly-on-endpoint t doesn't collapse
// the source face into degeneracy.
HEVertex interpolateVertex(const HEVertex& a, const HEVertex& b, float t)
{
    HEVertex r;
    r.position = a.position * (1.0f - t) + b.position * t;
    r.normal   = a.normal   * (1.0f - t) + b.normal   * t;
    if (r.normal.squaredLength() > 1e-12f) r.normal.normalise();
    r.uv       = a.uv       * (1.0f - t) + b.uv       * t;
    r.color    = a.color    * (1.0f - t) + b.color    * t;
    r.tangent  = a.tangent  * (1.0f - t) + b.tangent  * t;
    r.hasNormal  = a.hasNormal && b.hasNormal;
    r.hasUV      = a.hasUV && b.hasUV;
    r.hasColor   = a.hasColor && b.hasColor;
    r.hasTangent = a.hasTangent && b.hasTangent;

    // Union of bone influences by index, weights blended by t. Skip zero-
    // weight entries so the list stays short on typical rigs.
    auto addOrBlend = [&](unsigned short idx, float w) {
        if (w <= 1e-6f) return;
        for (auto& ba : r.boneAssignments) {
            if (ba.first == idx) { ba.second += w; return; }
        }
        r.boneAssignments.emplace_back(idx, w);
    };
    for (const auto& ba : a.boneAssignments) addOrBlend(ba.first, ba.second * (1.0f - t));
    for (const auto& ba : b.boneAssignments) addOrBlend(ba.first, ba.second * t);
    return r;
}

} // namespace

int HalfEdgeMesh::splitEdge(int edgeIdx, float t)
{
    if (edgeIdx < 0 || edgeIdx >= static_cast<int>(m_edges.size())) return -1;
    const int heA = m_edges[edgeIdx].halfEdge;
    if (heA < 0 || heA >= static_cast<int>(m_halfEdges.size())) return -1;

    // Clamp t off the endpoints so neither new face degenerates to a sliver.
    constexpr float kEps = 1e-4f;
    t = std::clamp(t, kEps, 1.0f - kEps);

    // Collect the two adjacent triangle descriptions while the old
    // topology is still intact. Both faces must be triangles for this
    // MVP; n-gon splitting needs ear-clip-aware rewiring.
    struct TriFace {
        int face;           // face index (for submesh lookup)
        int subMeshIndex;
        int vFrom;          // edge endpoint (start)
        int vTo;            // edge endpoint (end)
        int vOpp;           // third triangle vertex
        int heFrom, heTo, heOpp; // the three half-edges of the triangle
    };

    auto describeFace = [&](int he, TriFace& out) -> bool {
        if (he < 0 || he >= static_cast<int>(m_halfEdges.size())) return false;
        if (m_halfEdges[he].face < 0) return false;
        const int fIdx = m_halfEdges[he].face;
        const auto verts = faceVertices(fIdx);
        if (verts.size() != 3) return false;

        // Identify which HE of the triangle is the one carrying `edgeIdx`.
        // Traverse the face loop starting at the face's HE; the HE pointing
        // "to" heVertex of `he` is the one sharing edgeIdx. Fill TriFace
        // in face-loop order so we keep winding.
        const int startHE = m_faces[fIdx].halfEdge;
        int he0 = startHE;
        int he1 = m_halfEdges[he0].next;
        int he2 = m_halfEdges[he1].next;
        int heIndices[3] = {he0, he1, he2};

        int shareIdx = -1;
        for (int i = 0; i < 3; ++i) {
            if (heIndices[i] == he) { shareIdx = i; break; }
        }
        if (shareIdx < 0) return false;

        out.face = fIdx;
        out.subMeshIndex = m_faces[fIdx].subMeshIndex;
        out.heFrom = heIndices[shareIdx];
        out.heTo   = heIndices[(shareIdx + 1) % 3];
        out.heOpp  = heIndices[(shareIdx + 2) % 3];

        // Vertex layout: heFrom.prev.vertex -> heFrom.vertex -> heTo.vertex.
        // The shared edge goes vFrom -> vTo. The third vertex is heTo.vertex.
        out.vFrom = m_halfEdges[out.heOpp].vertex;
        out.vTo   = m_halfEdges[out.heFrom].vertex;
        out.vOpp  = m_halfEdges[out.heTo].vertex;
        return true;
    };

    TriFace fA{}, fB{};
    const bool hasA = describeFace(heA, fA);
    const int heB = m_halfEdges[heA].twin;
    const bool hasValidB = (heB >= 0);
    const bool hasB = hasValidB && describeFace(heB, fB);

    if (!hasA && !hasB) return -1;

    // splitEdge is a triangle-only MVP: if either adjacent face exists
    // and isn't a triangle, bail before we mutate. Partially splitting
    // would desync the other side's half-edge pointers against a face
    // that's still an n-gon, producing silent topology corruption.
    // (A face "exists" here means its twin is non-boundary. Boundary
    // half-edges — face == -1 — are fine.)
    if (!hasA && m_halfEdges[heA].face >= 0) return -1;
    if (!hasB && hasValidB && m_halfEdges[heB].face >= 0) return -1;

    // Edge direction is fA.vFrom -> fA.vTo (with t measured from vFrom).
    // If only fB exists (the edge sits on the boundary on the A side),
    // fB.vFrom / fB.vTo run in the opposite direction, so re-measure t.
    int vFrom = hasA ? fA.vFrom : fB.vTo;
    int vTo   = hasA ? fA.vTo   : fB.vFrom;

    // Create the midpoint vertex.
    HEVertex mid = interpolateVertex(m_vertices[vFrom], m_vertices[vTo], t);
    mid.halfEdge = -1;
    const int vMid = static_cast<int>(m_vertices.size());
    m_vertices.push_back(std::move(mid));

    // Retire a face: set each of its half-edges' face to -1 and clear the
    // face slot's halfEdge pointer so the four-call cleanup drops them.
    auto retireFace = [&](const TriFace& tf) {
        m_halfEdges[tf.heFrom].face = -1;
        m_halfEdges[tf.heTo].face = -1;
        m_halfEdges[tf.heOpp].face = -1;
        m_faces[tf.face].halfEdge = -1;
    };

    if (hasA) {
        retireFace(fA);
        // Old triangle [vFrom, vTo, vOpp] wound as
        // heOpp: vOpp -> vFrom, heFrom: vFrom -> vTo, heTo: vTo -> vOpp.
        // New triangles share the vMid → vOpp diagonal and keep the same winding.
        appendFace({fA.vFrom, vMid, fA.vOpp}, fA.subMeshIndex);
        appendFace({vMid, fA.vTo, fA.vOpp}, fA.subMeshIndex);
    }
    if (hasB) {
        retireFace(fB);
        // fB's winding is reversed relative to fA (opposite twin). Using
        // fB's own vFrom/vTo keeps its orientation intact.
        appendFace({fB.vFrom, vMid, fB.vOpp}, fB.subMeshIndex);
        appendFace({vMid, fB.vTo, fB.vOpp}, fB.subMeshIndex);
    }

    rebuildEdgesAndTwins();
    compactBoundaryHalfEdges();
    buildBoundaryHalfEdges();
    fixVertexHalfEdges();
    return vMid;
}

bool HalfEdgeMesh::splitFace(int faceIdx, int vA, int vB)
{
    if (faceIdx < 0 || faceIdx >= static_cast<int>(m_faces.size())) return false;
    if (vA == vB) return false;
    if (vA < 0 || vB < 0) return false;
    if (vA >= static_cast<int>(m_vertices.size())) return false;
    if (vB >= static_cast<int>(m_vertices.size())) return false;
    if (m_faces[faceIdx].halfEdge < 0) return false;

    const auto verts = faceVertices(faceIdx);
    if (verts.size() < 3 || verts.size() > 4) return false;

    // Require both vA and vB to be on the boundary loop.
    int posA = -1;
    int posB = -1;
    for (int i = 0; i < static_cast<int>(verts.size()); ++i) {
        if (verts[i] == vA) posA = i;
        if (verts[i] == vB) posB = i;
    }
    if (posA < 0 || posB < 0) return false;

    // Reject splits that don't actually cut the face: adjacent boundary
    // vertices are already connected by an edge, so a "diagonal" between
    // them would be a duplicate.
    const int n = static_cast<int>(verts.size());
    const int gap = std::abs(posA - posB);
    if (gap == 1 || gap == n - 1) return false;

    const int subMeshIndex = m_faces[faceIdx].subMeshIndex;

    // Retire the old face.
    {
        const int startHE = m_faces[faceIdx].halfEdge;
        int he = startHE;
        do {
            const int next = m_halfEdges[he].next;
            m_halfEdges[he].face = -1;
            he = next;
        } while (he != startHE);
        m_faces[faceIdx].halfEdge = -1;
    }

    // Walk the old loop from vA to vB to collect one side, then from vB
    // to vA for the other. appendFace needs at least 3 vertices, so a
    // degenerate side (empty) bails the whole operation.
    std::vector<int> sideAB;
    for (int i = posA; ; i = (i + 1) % n) {
        sideAB.push_back(verts[i]);
        if (i == posB) break;
    }
    std::vector<int> sideBA;
    for (int i = posB; ; i = (i + 1) % n) {
        sideBA.push_back(verts[i]);
        if (i == posA) break;
    }
    if (sideAB.size() < 3 || sideBA.size() < 3) return false;

    appendFace(sideAB, subMeshIndex);
    appendFace(sideBA, subMeshIndex);

    rebuildEdgesAndTwins();
    compactBoundaryHalfEdges();
    buildBoundaryHalfEdges();
    fixVertexHalfEdges();
    return true;
}

namespace {

// Intersect two line segments in 3D, parameterised as
//   A(sA) = a0 + sA * (a1 - a0),
//   B(sB) = b0 + sB * (b1 - b0),
// minimising |A(sA) - B(sB)|². Returns the parameter pair plus the
// squared separation; callers decide whether that separation is small
// enough to call the segments "coincident" for cut purposes.
struct SegmentSegmentClosest {
    float sA = 0.0f;
    float sB = 0.0f;
    float squaredSeparation = 0.0f;
    bool degenerate = false; // true if either segment has ~zero length
};

SegmentSegmentClosest closestOnSegments(const Ogre::Vector3& a0, const Ogre::Vector3& a1,
                                        const Ogre::Vector3& b0, const Ogre::Vector3& b1)
{
    SegmentSegmentClosest r;
    const Ogre::Vector3 dA = a1 - a0;
    const Ogre::Vector3 dB = b1 - b0;
    const Ogre::Vector3 w0 = a0 - b0;
    const float aa = dA.dotProduct(dA);
    const float bb = dB.dotProduct(dB);
    const float ab = dA.dotProduct(dB);
    const float aw = dA.dotProduct(w0);
    const float bw = dB.dotProduct(w0);
    const float det = aa * bb - ab * ab;
    if (aa < 1e-12f || bb < 1e-12f) { r.degenerate = true; return r; }
    if (det < 1e-12f) {
        // Parallel — pick the middle of the overlap on A.
        r.sA = 0.5f;
        r.sB = std::clamp((r.sA * ab + bw) / bb, 0.0f, 1.0f);
    } else {
        r.sA = (ab * bw - bb * aw) / det;
        r.sB = (aa * bw - ab * aw) / det;
    }
    r.sA = std::clamp(r.sA, 0.0f, 1.0f);
    r.sB = std::clamp(r.sB, 0.0f, 1.0f);
    const Ogre::Vector3 pA = a0 + dA * r.sA;
    const Ogre::Vector3 pB = b0 + dB * r.sB;
    r.squaredSeparation = (pA - pB).squaredLength();
    return r;
}

} // namespace

std::vector<int> HalfEdgeMesh::cutPath(const std::vector<CutPoint>& points)
{
    std::vector<int> newVertices;
    if (points.size() < 2) return newVertices;

    // Resolve every click's edge to a stable (vA, vB) vertex pair BEFORE
    // touching the mesh. Edge indices aren't stable across splitEdge
    // (rebuildEdgesAndTwins reorders them) but vertex indices are append-
    // only, so we can re-find the edge each time by its endpoints.
    std::vector<std::pair<int, int>> clickEdgeVerts(points.size(), {-1, -1});
    std::vector<float> clickT(points.size(), 0.0f);
    for (size_t i = 0; i < points.size(); ++i) {
        const int eIdx = points[i].edgeIndex;
        if (eIdx < 0 || eIdx >= static_cast<int>(m_edges.size())) return newVertices;
        clickEdgeVerts[i] = edgeVertices(eIdx);
        clickT[i] = points[i].t;
        if (clickEdgeVerts[i].first < 0 || clickEdgeVerts[i].second < 0) return newVertices;
    }

    // Helper: re-find the HE edge index whose endpoints are {va, vb}
    // against the current mesh. Returns -1 if either vertex has been
    // removed or the pair is no longer a logical edge.
    auto findEdgeByVerts = [this](int va, int vb) -> int {
        for (size_t e = 0; e < m_edges.size(); ++e) {
            const auto [a, b] = edgeVertices(static_cast<int>(e));
            if ((a == va && b == vb) || (a == vb && b == va))
                return static_cast<int>(e);
        }
        return -1;
    };

    // Snapshot the whole HE representation so partial failures can roll
    // back cleanly (e.g. two CutPoints referencing the same underlying
    // edge: the first split removes it, the second findEdgeByVerts
    // returns -1). Without this the caller sees a half-mutated mesh.
    const auto snapHalfEdges = m_halfEdges;
    const auto snapVertices = m_vertices;
    const auto snapFaces = m_faces;
    const auto snapEdges = m_edges;
    auto restore = [&]() {
        m_halfEdges = snapHalfEdges;
        m_vertices = snapVertices;
        m_faces = snapFaces;
        m_edges = snapEdges;
    };

    // Step 1: insert vertices at every click endpoint, re-resolving the
    // edge each time because the previous split may have renumbered them.
    std::vector<int> clickVerts(points.size(), -1);
    for (size_t i = 0; i < points.size(); ++i) {
        const int resolvedEdge = findEdgeByVerts(clickEdgeVerts[i].first,
                                                 clickEdgeVerts[i].second);
        if (resolvedEdge < 0) { restore(); newVertices.clear(); return newVertices; }
        const int v = splitEdge(resolvedEdge, clickT[i]);
        if (v < 0) { restore(); newVertices.clear(); return newVertices; }
        clickVerts[i] = v;
        newVertices.push_back(v);
    }

    // Step 2: for each consecutive pair, walk from one click vertex to the
    // next through the tris between them. Every edge the straight-line
    // cut crosses gets splitEdge'd so the cut materialises as real mesh
    // edges.
    constexpr int kMaxWalkSteps = 256;    // safety: real cuts visit <~10 tris
    constexpr float kNearZero = 1e-5f;    // close-to-vertex tolerance
    constexpr float kMinStep = 1e-4f;     // reject crossings that don't advance

    for (size_t i = 0; i + 1 < clickVerts.size(); ++i) {
        int vA = clickVerts[i];
        const int vTarget = clickVerts[i + 1];
        if (vA < 0 || vTarget < 0 || vA == vTarget) continue;

        for (int step = 0; step < kMaxWalkSteps; ++step) {
            if (vA == vTarget) break;

            // Does vA already share a triangle with vTarget? Then the
            // existing splitEdge semantics (two click splits on one tri
            // produce the connecting edge automatically) already created
            // the final cut edge — nothing more to do for this pair.
            const auto facesAtA = facesAroundVertex(vA);
            bool shareFace = false;
            for (int f : facesAtA) {
                const auto fv = faceVertices(f);
                if (std::find(fv.begin(), fv.end(), vTarget) != fv.end()) {
                    shareFace = true; break;
                }
            }
            if (shareFace) break;

            const Ogre::Vector3 pA = m_vertices[vA].position;
            const Ogre::Vector3 pT = m_vertices[vTarget].position;

            // Pick the face adjacent to vA whose straight-line to vTarget
            // exits through an interior edge. For each candidate face,
            // consider only edges not incident to vA: if the closest-point
            // pair sits strictly inside the edge and advances us toward
            // vTarget (sCut > kMinStep), that's an exit candidate. Among
            // candidates, choose the smallest sCut (closest crossing).
            int bestFace = -1;
            int bestEdge = -1;
            float bestT = 0.0f;
            float bestSA = std::numeric_limits<float>::infinity();

            for (int f : facesAtA) {
                const auto fe = faceEdges(f);
                for (int eIdx : fe) {
                    const auto [ev0, ev1] = edgeVertices(eIdx);
                    if (ev0 == vA || ev1 == vA) continue; // entry edge
                    const Ogre::Vector3& b0 = m_vertices[ev0].position;
                    const Ogre::Vector3& b1 = m_vertices[ev1].position;
                    const auto hit = closestOnSegments(pA, pT, b0, b1);
                    if (hit.degenerate) continue;
                    if (hit.squaredSeparation > 1e-4f) continue;
                    // Must actually advance from vA toward vTarget.
                    if (hit.sA < kMinStep) continue;
                    // Exit edge parameter must be strictly inside the
                    // segment (endpoint-snap is a TODO).
                    if (hit.sB < kNearZero || hit.sB > 1.0f - kNearZero) continue;
                    if (hit.sA < bestSA) {
                        bestSA = hit.sA;
                        bestEdge = eIdx;
                        bestT = hit.sB;
                        bestFace = f;
                    }
                }
            }

            if (bestEdge < 0) {
                // No valid forward exit found. Either the cut's ray went
                // off the surface (click span not on a coplanar region)
                // or the target sits on a face we don't share — give up
                // on this pair and move on.
                break;
            }
            (void)bestFace;

            // Split the exit edge; the new vertex becomes the entry point
            // for the next triangle. Because the previous iteration's vA
            // and the new vertex now both live on the same triangle, the
            // splitEdge pass already leaves the segment between them as a
            // real mesh edge.
            const int vMid = splitEdge(bestEdge, bestT);
            if (vMid < 0) break;
            newVertices.push_back(vMid);
            vA = vMid;
        }
    }

    return newVertices;
}

int HalfEdgeMesh::mergeVertices(const std::vector<int>& vertexIndices,
                                const Ogre::Vector3& targetPos)
{
    // 1. Pick a survivor and build the doomed set, filtering out invalid /
    //    out-of-range indices and dedup'ing along the way. Anything < 2
    //    valid distinct verts is a no-op.
    int survivor = -1;
    std::set<int> doomed;
    for (int v : vertexIndices) {
        if (v < 0 || v >= static_cast<int>(m_vertices.size())) continue;
        if (m_vertices[v].halfEdge < 0) continue; // already retired
        if (survivor == -1) {
            survivor = v;
            continue;
        }
        if (v == survivor) continue;
        doomed.insert(v);
    }
    if (survivor == -1 || doomed.empty()) return 0;

    // 2. Refuse cross-submesh merges. The HE build pipeline keeps a
    //    distinct HEVertex per (submesh, localVert) pair so UV seams and
    //    material boundaries survive; collapsing across that boundary
    //    would silently fuse them. Cheap check: every face touching the
    //    merge set must agree on subMeshIndex with the survivor's faces.
    auto collectSubmeshes = [&](int v, std::set<int>& out) {
        const auto faces = facesAroundVertex(v);
        for (int f : faces) {
            if (f >= 0 && f < static_cast<int>(m_faces.size())
                && m_faces[f].halfEdge >= 0) {
                out.insert(m_faces[f].subMeshIndex);
            }
        }
    };
    std::set<int> mergeSubs;
    collectSubmeshes(survivor, mergeSubs);
    for (int v : doomed) collectSubmeshes(v, mergeSubs);
    if (mergeSubs.size() > 1) return 0;

    // 3. Move the survivor to targetPos and re-point every half-edge
    //    that pointed TO a doomed vertex so it now points to the survivor.
    m_vertices[survivor].position = targetPos;

    for (auto& he : m_halfEdges) {
        if (he.face < 0) continue;            // skip retired / boundary HEs
        if (doomed.count(he.vertex))
            he.vertex = survivor;
    }

    // 4. Retire every triangle that is now degenerate (any two of its
    //    three vertices identical) OR a duplicate of an earlier surviving
    //    triangle. Mergers commonly produce duplicates: e.g. two
    //    coincident-vertex pairs each collapse to a single vert and the
    //    two triangles that used to differ only by those two indices end
    //    up with identical vertex sets. We compare unordered vertex sets
    //    (sub-mesh agnostic — same trio in the same submesh is a dup).
    auto retireFace = [&](int f) {
        int startHE = m_faces[f].halfEdge;
        if (startHE < 0) return;
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
    };

    auto canonicalKey = [&](const std::vector<int>& verts, int subIdx)
        -> std::tuple<int, int, int, int> {
        std::array<int, 3> sorted = {verts[0], verts[1], verts[2]};
        std::sort(sorted.begin(), sorted.end());
        return {subIdx, sorted[0], sorted[1], sorted[2]};
    };

    int retiredFaces = 0;
    std::set<std::tuple<int, int, int, int>> seenFaces;
    for (int f = 0; f < static_cast<int>(m_faces.size()); ++f) {
        int startHE = m_faces[f].halfEdge;
        if (startHE < 0) continue;

        const auto verts = faceVertices(f);
        if (verts.size() != 3) continue;
        const bool degenerate = (verts[0] == verts[1])
                             || (verts[1] == verts[2])
                             || (verts[0] == verts[2]);
        if (degenerate) {
            retireFace(f);
            ++retiredFaces;
            continue;
        }
        // Duplicate-of-earlier check: triangle with same {subMesh, sorted-verts}
        // collapses into the first occurrence.
        const auto key = canonicalKey(verts, m_faces[f].subMeshIndex);
        if (!seenFaces.insert(key).second) {
            retireFace(f);
            ++retiredFaces;
        }
    }

    // 5. Mark the doomed vertex slots as retired (halfEdge = -1) so
    //    later operations skip them. Their physical entries stay in
    //    m_vertices but become inert.
    for (int v : doomed) {
        m_vertices[v].halfEdge = -1;
    }

    // 6. Rebuild edge tables. After the rewrite, two formerly-distinct
    //    edges may now share endpoints, and twin links need to be
    //    recomputed. The standard trio mirrors splitFace / extrude.
    rebuildEdgesAndTwins();
    compactBoundaryHalfEdges();
    buildBoundaryHalfEdges();
    fixVertexHalfEdges();

    return static_cast<int>(doomed.size());
}

int HalfEdgeMesh::mergeVerticesByDistance(const std::vector<int>& vertexIndices,
                                          float threshold)
{
    if (vertexIndices.size() < 2 || threshold <= 0.0f) return 0;

    // Defensive clamp on the squared-distance comparand: if a caller
    // passes a huge threshold (or one mis-converted from world to mesh
    // units), `threshold * threshold` overflows to +∞ and every pair
    // satisfies the comparison, collapsing the entire selection into one
    // cluster. Cap at sqrt(FLT_MAX/2) — well past any realistic mesh
    // diagonal and still leaves headroom in the squared form.
    constexpr float kMaxThreshold = 1.0e18f;
    if (threshold > kMaxThreshold) threshold = kMaxThreshold;

    // Filter to live vertices, keep original ordering — the first vertex
    // of each cluster becomes the survivor (matches Blender's "Merge By
    // Distance" deterministic-survivor behavior).
    std::vector<int> alive;
    alive.reserve(vertexIndices.size());
    for (int v : vertexIndices) {
        if (v >= 0 && v < static_cast<int>(m_vertices.size())
            && m_vertices[v].halfEdge >= 0) {
            alive.push_back(v);
        }
    }
    if (alive.size() < 2) return 0;

    // Compute each candidate vertex's submesh up front so the union step
    // only fuses pairs in the same submesh. mergeVertices() refuses
    // cross-submesh inputs anyway, so a mixed cluster would silently
    // drop everything; pre-partitioning preserves the same-submesh
    // pairs the user actually wanted to fuse. (Codex P2)
    auto vertexSubmesh = [&](int v) -> int {
        const auto faces = facesAroundVertex(v);
        if (faces.empty()) return -1;
        return m_faces[faces.front()].subMeshIndex;
    };

    // Union-find over alive[]: pair-scan in O(N²). N is the selected-
    // vertex count, which is small in practice (hundreds at most). If
    // this becomes a bottleneck, swap in a uniform spatial hash.
    std::vector<int> parent(alive.size());
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = static_cast<int>(i);
    std::function<int(int)> find = [&](int i) {
        while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
        return i;
    };
    auto unite = [&](int a, int b) {
        int ra = find(a), rb = find(b);
        if (ra == rb) return;
        // Lower index wins so cluster.front() == oldest selection slot.
        if (ra < rb) parent[rb] = ra; else parent[ra] = rb;
    };

    std::vector<int> aliveSubs;
    aliveSubs.reserve(alive.size());
    for (int v : alive) aliveSubs.push_back(vertexSubmesh(v));

    const float t2 = threshold * threshold;
    for (size_t i = 0; i < alive.size(); ++i) {
        for (size_t j = i + 1; j < alive.size(); ++j) {
            // Skip cross-submesh pairs entirely — UV seams / material
            // boundaries must survive.
            if (aliveSubs[i] != aliveSubs[j]) continue;
            const auto& pa = m_vertices[alive[i]].position;
            const auto& pb = m_vertices[alive[j]].position;
            if (pa.squaredDistance(pb) <= t2)
                unite(static_cast<int>(i), static_cast<int>(j));
        }
    }

    // Group cluster members and dispatch one mergeVertices() per cluster
    // with cluster centroid as the target. Clusters of size 1 are skipped.
    std::map<int, std::vector<int>> clusters;
    for (size_t i = 0; i < alive.size(); ++i)
        clusters[find(static_cast<int>(i))].push_back(alive[i]);

    int totalRetired = 0;
    for (auto& [root, members] : clusters) {
        if (members.size() < 2) continue;
        Ogre::Vector3 centroid = Ogre::Vector3::ZERO;
        for (int v : members) centroid += m_vertices[v].position;
        centroid /= static_cast<float>(members.size());
        totalRetired += mergeVertices(members, centroid);
    }
    return totalRetired;
}

// ---------------------------------------------------------------------------
// Delete / Dissolve
//
// Deletion is the simplest topology op: zero out the half-edges of the
// targeted faces, mark the face slot retired (halfEdge == -1), and let the
// rebuild trio re-derive edges, twins, and boundary loops. Vertex variants
// fan out from the vertex's incident faces, edge variants from the edge's
// 1-or-2 adjacent faces.
//
// Dissolve is delete + retriangulate: the targeted element is removed but
// the *region* it bounded survives as a fan-triangulated patch, so the
// surface stays watertight.
// ---------------------------------------------------------------------------

namespace {
// Retire a face by index: blank every half-edge in its loop, then null the
// face's halfEdge pointer. Mirrors the lambda inside mergeVertices but
// pulled out so the delete/dissolve paths share it.
void retireFaceImpl(std::vector<HalfEdge>& halfEdges,
                    std::vector<HEFace>& faces,
                    int faceIdx)
{
    if (faceIdx < 0 || faceIdx >= static_cast<int>(faces.size())) return;
    int startHE = faces[faceIdx].halfEdge;
    if (startHE < 0) return;
    int he = startHE;
    do {
        int next = halfEdges[he].next;
        halfEdges[he].face = -1;
        halfEdges[he].twin = -1;
        halfEdges[he].next = -1;
        halfEdges[he].prev = -1;
        he = next;
    } while (he != startHE && he >= 0);
    faces[faceIdx].halfEdge = -1;
}
} // namespace

int HalfEdgeMesh::deleteFaces(const std::vector<int>& faceIndices)
{
    if (faceIndices.empty()) return 0;

    // 1. Collect the unique set of currently-live target faces, plus the
    //    set of vertices touched by them. After the retire pass we'll
    //    drop any vertex that had ALL its incident faces deleted.
    std::set<int> doomedFaces;
    std::set<int> touchedVerts;
    for (int f : faceIndices) {
        if (f < 0 || f >= static_cast<int>(m_faces.size())) continue;
        if (m_faces[f].halfEdge < 0) continue;
        if (!doomedFaces.insert(f).second) continue;
        for (int v : faceVertices(f)) touchedVerts.insert(v);
    }
    if (doomedFaces.empty()) return 0;

    // 2. Retire the target faces.
    for (int f : doomedFaces) retireFaceImpl(m_halfEdges, m_faces, f);

    // 3. Drop vertices whose entire incident-face set was retired. We
    //    re-query facesAroundVertex (which respects halfEdge < 0 retire
    //    flags) — a vertex with zero live faces becomes inert.
    for (int v : touchedVerts) {
        // Vertex's halfEdge may now point at a retired HE; refresh first.
        // facesAroundVertex returns empty either way once everything is gone,
        // so checking it is enough.
        bool anyAlive = false;
        for (int he = 0; he < static_cast<int>(m_halfEdges.size()); ++he) {
            if (m_halfEdges[he].face < 0) continue;
            if (m_halfEdges[he].vertex == v) { anyAlive = true; break; }
            // Also check the "from" side: prev->vertex == v
            int prev = m_halfEdges[he].prev;
            if (prev >= 0 && m_halfEdges[prev].vertex == v) {
                anyAlive = true;
                break;
            }
        }
        if (!anyAlive) m_vertices[v].halfEdge = -1;
    }

    // 4. Standard rebuild trio: edges, boundary, vertex pointers. Same
    //    sequence used by mergeVertices.
    rebuildEdgesAndTwins();
    compactBoundaryHalfEdges();
    buildBoundaryHalfEdges();
    fixVertexHalfEdges();

    return static_cast<int>(doomedFaces.size());
}

int HalfEdgeMesh::deleteEdges(const std::vector<int>& edgeIndices)
{
    if (edgeIndices.empty()) return 0;

    // Resolve each edge to its 0-2 adjacent faces. We feed the union set
    // into deleteFaces so the rebuild + orphan-vertex pass run once.
    std::set<int> doomedFaces;
    for (int e : edgeIndices) {
        if (e < 0 || e >= static_cast<int>(m_edges.size())) continue;
        if (m_edges[e].halfEdge < 0) continue;
        auto [fA, fB] = edgeFaces(e);
        if (fA >= 0) doomedFaces.insert(fA);
        if (fB >= 0) doomedFaces.insert(fB);
    }
    if (doomedFaces.empty()) return 0;

    std::vector<int> faces(doomedFaces.begin(), doomedFaces.end());
    return deleteFaces(faces);
}

int HalfEdgeMesh::deleteVertices(const std::vector<int>& vertexIndices)
{
    if (vertexIndices.empty()) return 0;

    // Collect the union of faces incident to any targeted vertex.
    std::set<int> doomedVerts;
    std::set<int> doomedFaces;
    for (int v : vertexIndices) {
        if (v < 0 || v >= static_cast<int>(m_vertices.size())) continue;
        if (m_vertices[v].halfEdge < 0) continue;
        if (!doomedVerts.insert(v).second) continue;
        for (int f : facesAroundVertex(v)) doomedFaces.insert(f);
    }
    if (doomedVerts.empty()) return 0;

    if (!doomedFaces.empty()) {
        std::vector<int> faces(doomedFaces.begin(), doomedFaces.end());
        deleteFaces(faces);
    }

    // Mark the vertex slots retired even if they had no live faces (a
    // vertex with no faces is unreachable from toEditableMesh anyway, but
    // explicitly retiring keeps subsequent operations honest).
    for (int v : doomedVerts) m_vertices[v].halfEdge = -1;

    return static_cast<int>(doomedVerts.size());
}

int HalfEdgeMesh::dissolveEdges(const std::vector<int>& edgeIndices)
{
    if (edgeIndices.empty()) return 0;

    // Edge slots are reordered by rebuildEdgesAndTwins() at the end of
    // each iteration. Snapshot endpoint vertex pairs (vertex slots are
    // append-only and stable across rebuilds) and re-resolve the edge
    // index by pair on every iteration. Mirrors cutPath's pattern.
    // Found by Codex P1 + CodeRabbit Major.
    std::vector<std::pair<int,int>> targetVerts;
    targetVerts.reserve(edgeIndices.size());
    {
        std::set<std::pair<int,int>> seenPairs;
        for (int e : edgeIndices) {
            if (e < 0 || e >= static_cast<int>(m_edges.size())) continue;
            if (m_edges[e].halfEdge < 0) continue;
            auto [a, b] = edgeVertices(e);
            if (a < 0 || b < 0) continue;
            auto key = std::make_pair(std::min(a, b), std::max(a, b));
            if (!seenPairs.insert(key).second) continue;
            targetVerts.push_back(key);
        }
    }

    auto findEdgeByVerts = [this](int va, int vb) -> int {
        for (size_t e = 0; e < m_edges.size(); ++e) {
            if (m_edges[e].halfEdge < 0) continue;
            auto [a, b] = edgeVertices(static_cast<int>(e));
            if ((a == va && b == vb) || (a == vb && b == va))
                return static_cast<int>(e);
        }
        return -1;
    };

    int dissolved = 0;
    for (const auto& [va, vb] : targetVerts) {
        const int e = findEdgeByVerts(va, vb);
        if (e < 0) continue;                  // earlier dissolve removed this edge
        if (m_edges[e].halfEdge < 0) continue;

        auto [fA, fB] = edgeFaces(e);
        if (fA < 0 || fB < 0) continue;                     // boundary
        if (m_faces[fA].halfEdge < 0 || m_faces[fB].halfEdge < 0) continue;
        if (m_faces[fA].subMeshIndex != m_faces[fB].subMeshIndex) continue;

        const auto vA = faceVertices(fA);
        const auto vB = faceVertices(fB);
        if (vA.size() != 3 || vB.size() != 3) continue; // MVP: triangle pairs

        const auto [eu, ev] = edgeVertices(e);
        // Find the "third" vertex of each face — the one not on the shared edge.
        auto thirdVert = [eu, ev](const std::vector<int>& v) {
            for (int x : v) if (x != eu && x != ev) return x;
            return -1;
        };
        const int oppA = thirdVert(vA);
        const int oppB = thirdVert(vB);
        if (oppA < 0 || oppB < 0 || oppA == oppB) continue;

        // Dissolving (eu, ev) means re-triangulating the {eu, oppA, ev, oppB}
        // quad using the *other* diagonal (oppA, oppB). We need to preserve
        // the original winding of fA so the outward normal stays sane: if
        // fA traverses (eu -> ev -> oppA), the new triangles are
        // (eu, oppB, oppA) and (ev, oppA, oppB) (winding flipped by the
        // shared diagonal direction). Easier in practice: pull the boundary
        // loop directly off fA and fB.
        //
        // Walk fA from oppA and collect the three boundary verts in order.
        // Then concatenate fB's boundary starting at oppB. That gives a
        // 4-vertex loop in correct CCW order, which we fan-triangulate
        // (oppA, x, oppB) and (oppA, oppB, y) — but easier: just use
        // (oppA, vA_next_after_oppA, oppB) and (oppA, oppB, vB_next_after_oppB).
        auto faceLoopFrom = [this](int faceIdx, int startVert) -> std::vector<int> {
            std::vector<int> out;
            int startHE = m_faces[faceIdx].halfEdge;
            int he = startHE;
            // Find the HE whose `prev->vertex == startVert` (HE points away
            // from startVert).
            do {
                int prev = m_halfEdges[he].prev;
                if (prev >= 0 && m_halfEdges[prev].vertex == startVert) break;
                he = m_halfEdges[he].next;
            } while (he != startHE && he >= 0);
            int cur = he;
            for (int i = 0; i < 3 && cur >= 0; ++i) {
                int prev = m_halfEdges[cur].prev;
                if (prev < 0) break;
                out.push_back(m_halfEdges[prev].vertex);
                cur = m_halfEdges[cur].next;
            }
            return out;
        };

        const std::vector<int> loopA = faceLoopFrom(fA, oppA); // [oppA, ?, ?]
        const std::vector<int> loopB = faceLoopFrom(fB, oppB); // [oppB, ?, ?]
        if (loopA.size() != 3 || loopB.size() != 3) continue;

        const int subIdx = m_faces[fA].subMeshIndex;
        retireFaceImpl(m_halfEdges, m_faces, fA);
        retireFaceImpl(m_halfEdges, m_faces, fB);

        // The merged quad's CCW loop is loopA followed by the two non-oppB
        // vertices of loopB (which are eu/ev) — but those are already in
        // loopA, so loopA + [oppB] in the right slot is what we want. Since
        // we want the new diagonal (oppA, oppB), the two new triangles are
        // (loopA[0], loopA[1], oppB) and (loopA[0], oppB, loopA[2]) i.e.
        // (oppA, neighborA1, oppB) and (oppA, oppB, neighborA2).
        appendTriangle(loopA[0], loopA[1], oppB, subIdx);
        appendTriangle(loopA[0], oppB,    loopA[2], subIdx);

        // Rebuild incrementally per dissolve so the next edge in the input
        // sees a coherent topology. This is O(|HE|) per dissolve; fine for
        // interactive selections (tens to hundreds of edges).
        rebuildEdgesAndTwins();
        compactBoundaryHalfEdges();
        buildBoundaryHalfEdges();
        fixVertexHalfEdges();

        ++dissolved;
    }
    return dissolved;
}

int HalfEdgeMesh::dissolveVertices(const std::vector<int>& vertexIndices)
{
    if (vertexIndices.empty()) return 0;

    int dissolved = 0;
    std::set<int> processed;
    for (int v : vertexIndices) {
        if (!processed.insert(v).second) continue;
        if (v < 0 || v >= static_cast<int>(m_vertices.size())) continue;
        if (m_vertices[v].halfEdge < 0) continue;
        if (isVertexBoundary(v)) continue;        // MVP: skip boundary vertices

        const auto incident = facesAroundVertex(v);
        if (incident.size() < 3) continue;       // valence < 3 — nothing to dissolve

        // All incident faces must share a submesh; otherwise dissolving would
        // fuse material groups. Same constraint as mergeVertices.
        const int subIdx = m_faces[incident.front()].subMeshIndex;
        bool sameSub = true;
        for (int f : incident) {
            if (m_faces[f].subMeshIndex != subIdx) { sameSub = false; break; }
            if (faceVertices(f).size() != 3) { sameSub = false; break; }
        }
        if (!sameSub) continue;

        // Walk the boundary loop of the umbrella (the N-gon left after
        // removing the central vertex). The standard half-edge trick: pick
        // any outgoing HE from v, follow its next pointer (skipping v),
        // then jump across the next HE's twin to walk to the next umbrella
        // face. Each "next->vertex" along the way is one boundary vertex.
        //
        // We collect by walking the incident faces and pulling the two
        // non-v vertices in winding order. With a manifold umbrella those
        // pairs chain into a single closed loop.
        std::vector<int> loop;
        loop.reserve(incident.size());
        // Build a small adjacency: face -> (boundaryStart, boundaryEnd) where
        // the face's loop is (v -> boundaryStart -> boundaryEnd -> v).
        std::map<int, std::pair<int,int>> faceEdges;
        for (int f : incident) {
            const auto vs = faceVertices(f);
            int idxOfV = -1;
            for (int i = 0; i < 3; ++i) if (vs[i] == v) { idxOfV = i; break; }
            if (idxOfV < 0) { faceEdges.clear(); break; }
            int a = vs[(idxOfV + 1) % 3];
            int b = vs[(idxOfV + 2) % 3];
            faceEdges[f] = {a, b};
        }
        if (faceEdges.empty()) continue;

        // Chain the (start, end) pairs into one loop.
        // Pick a starting face arbitrarily; walk via shared boundary verts.
        std::set<int> remaining(incident.begin(), incident.end());
        int curFace = *remaining.begin();
        loop.push_back(faceEdges[curFace].first);
        loop.push_back(faceEdges[curFace].second);
        remaining.erase(curFace);

        bool ok = true;
        while (!remaining.empty()) {
            int needed = loop.back();
            int found = -1;
            for (int f : remaining) {
                if (faceEdges[f].first == needed) { found = f; break; }
            }
            if (found < 0) { ok = false; break; }
            // The new face contributes its `second` vertex.
            // (Its `first` matches the existing tail.)
            loop.push_back(faceEdges[found].second);
            remaining.erase(found);
        }
        if (!ok) continue;
        // The loop should close — last == first. Drop the duplicate.
        if (loop.size() < 4 || loop.front() != loop.back()) continue;
        loop.pop_back();
        if (loop.size() < 3) continue;

        // Sanity: no duplicated boundary verts (would mean a non-manifold
        // umbrella we can't safely fan-triangulate).
        std::set<int> uniqueLoop(loop.begin(), loop.end());
        if (uniqueLoop.size() != loop.size()) continue;

        // Retire the old faces, fan-triangulate from loop[0]. New triangles:
        //   (loop[0], loop[i], loop[i+1])  for i in [1, N-1)
        for (int f : incident) retireFaceImpl(m_halfEdges, m_faces, f);
        for (size_t i = 1; i + 1 < loop.size(); ++i) {
            appendTriangle(loop[0], loop[i], loop[i + 1], subIdx);
        }
        m_vertices[v].halfEdge = -1; // retire the dissolved center

        rebuildEdgesAndTwins();
        compactBoundaryHalfEdges();
        buildBoundaryHalfEdges();
        fixVertexHalfEdges();

        ++dissolved;
    }
    return dissolved;
}

std::vector<int> HalfEdgeMesh::subdivideFaces(const std::vector<int>& faceIndices)
{
    std::vector<int> newVertices;
    if (faceIndices.empty()) return newVertices;

    // 1. Collect the unique set of currently-live target faces. Triangles
    //    only — n-gons would need their own ear-clip handling and aren't in
    //    the current pipeline (matches splitEdge / splitFace MVP scope).
    std::set<int> selectedFaces;
    for (int f : faceIndices) {
        if (f < 0 || f >= static_cast<int>(m_faces.size())) continue;
        if (m_faces[f].halfEdge < 0) continue;
        if (faceVertices(f).size() != 3) continue;
        selectedFaces.insert(f);
    }
    if (selectedFaces.empty()) return newVertices;

    // 2. Build the set of edges that need a midpoint, keyed by undirected
    //    vertex pair. Adjacent selected faces share endpoints, so the same
    //    edge can be referenced twice — the map dedups it. Each edge maps
    //    to the midpoint vertex index (filled in step 3).
    std::map<std::pair<int,int>, int> midpointOf;
    auto edgeKey = [](int a, int b) {
        return std::make_pair(std::min(a, b), std::max(a, b));
    };
    for (int f : selectedFaces) {
        const auto verts = faceVertices(f);
        for (int i = 0; i < 3; ++i) {
            const int a = verts[i];
            const int b = verts[(i + 1) % 3];
            midpointOf.emplace(edgeKey(a, b), -1);
        }
    }

    // 3. Materialise a midpoint vertex for each unique edge. UVs / normals
    //    / bone weights / tangents are interpolated linearly with t=0.5
    //    via the existing `interpolateVertex` helper. Reuse the same
    //    midpoint for both adjacent faces so the tessellation is watertight.
    for (auto& [key, vMid] : midpointOf) {
        HEVertex mid = interpolateVertex(m_vertices[key.first],
                                         m_vertices[key.second], 0.5f);
        mid.halfEdge = -1;
        vMid = static_cast<int>(m_vertices.size());
        m_vertices.push_back(std::move(mid));
        newVertices.push_back(vMid);
    }

    // 4. Find every NON-selected face that has at least one of its edges
    //    split. Those need to be retriangulated to avoid T-junctions where
    //    one side of the edge has been refined and the other hasn't.
    //    Selected faces are always fully refined (1 → 4 split) so we
    //    handle them in the same loop.
    auto retireFace = [&](int faceIdx) {
        if (faceIdx < 0) return;
        const int startHE = m_faces[faceIdx].halfEdge;
        if (startHE < 0) return;
        int he = startHE;
        do {
            const int next = m_halfEdges[he].next;
            m_halfEdges[he].face = -1;
            he = next;
        } while (he != startHE && he >= 0);
        m_faces[faceIdx].halfEdge = -1;
    };

    // Snapshot the live face count BEFORE we start emitting new ones —
    // appendFace appends to m_faces and we must not iterate over its tail.
    const int origFaceCount = static_cast<int>(m_faces.size());
    for (int f = 0; f < origFaceCount; ++f) {
        if (m_faces[f].halfEdge < 0) continue;
        const auto verts = faceVertices(f);
        if (verts.size() != 3) continue; // skip non-triangles

        const int subIdx = m_faces[f].subMeshIndex;

        // For each edge in winding order, look up its midpoint (or -1 if
        // the edge wasn't split).
        const int v0 = verts[0], v1 = verts[1], v2 = verts[2];
        auto find = [&](int a, int b) {
            auto it = midpointOf.find(edgeKey(a, b));
            return (it != midpointOf.end()) ? it->second : -1;
        };
        const int m01 = find(v0, v1);
        const int m12 = find(v1, v2);
        const int m20 = find(v2, v0);
        const int splitMask = (m01 >= 0 ? 1 : 0)
                            | (m12 >= 0 ? 2 : 0)
                            | (m20 >= 0 ? 4 : 0);
        if (splitMask == 0) continue; // not adjacent to any subdivided edge

        retireFace(f);

        // Cases by which edges have midpoints. Winding order is preserved
        // from the original triangle so face normals stay consistent.
        switch (splitMask) {
        case 1: // only v0-v1 split
            appendTriangle(v0, m01, v2, subIdx);
            appendTriangle(m01, v1, v2, subIdx);
            break;
        case 2: // only v1-v2 split
            appendTriangle(v0, v1, m12, subIdx);
            appendTriangle(v0, m12, v2, subIdx);
            break;
        case 4: // only v2-v0 split
            appendTriangle(v0, v1, m20, subIdx);
            appendTriangle(m20, v1, v2, subIdx);
            break;
        case 3: // v0-v1 and v1-v2 split (corner v1 is the "split corner")
            appendTriangle(v0, m01, m12, subIdx);
            appendTriangle(m01, v1, m12, subIdx);
            appendTriangle(v0, m12, v2, subIdx);
            break;
        case 5: // v0-v1 and v2-v0 split (corner v0)
            appendTriangle(v0, m01, m20, subIdx);
            appendTriangle(m01, v1, m20, subIdx);
            appendTriangle(m20, v1, v2, subIdx);
            break;
        case 6: // v1-v2 and v2-v0 split (corner v2)
            appendTriangle(v0, v1, m20, subIdx);
            appendTriangle(m20, v1, m12, subIdx);
            appendTriangle(m20, m12, v2, subIdx);
            break;
        case 7: // all three split — full 1-to-4 subdivide
            appendTriangle(v0, m01, m20, subIdx);
            appendTriangle(m01, v1, m12, subIdx);
            appendTriangle(m20, m12, v2, subIdx);
            appendTriangle(m01, m12, m20, subIdx);
            break;
        default:
            // splitMask is 3 bits, covered above. Should be unreachable.
            break;
        }
    }

    rebuildEdgesAndTwins();
    compactBoundaryHalfEdges();
    buildBoundaryHalfEdges();
    fixVertexHalfEdges();

    return newVertices;
}

int HalfEdgeMesh::fillSelection(const std::vector<int>& vertexIndices)
{
    const int n = static_cast<int>(vertexIndices.size());
    if (n < 3) return 0;

    // 1. Validate inputs: every vertex must be in-range and unique. We do
    //    NOT require the vertex to have an incident face — orphans (e.g.
    //    left over after a deleteFaces) can legitimately be the corners
    //    of a hole the user wants to cap.
    std::set<int> seen;
    for (int v : vertexIndices) {
        if (v < 0 || v >= static_cast<int>(m_vertices.size())) return 0;
        if (!seen.insert(v).second) return 0; // duplicate vertex
    }

    // 2. All vertices must share a submesh — otherwise the new face would
    //    silently fuse material groups (same constraint as mergeVertices).
    //    For vertices with at least one incident face, read the submesh
    //    off that face. For orphaned vertices, fall back to a positional
    //    submesh inference: the submesh of the FIRST inputs that has an
    //    incident face. If every input is orphaned we have no way to
    //    pick a target submesh, so default to submesh 0.
    auto vertexSubMesh = [this](int v) -> int {
        const auto faces = facesAroundVertex(v);
        if (faces.empty()) return -1;
        return m_faces[faces.front()].subMeshIndex;
    };
    int subIdx = -1;
    for (int v : vertexIndices) {
        const int s = vertexSubMesh(v);
        if (s >= 0) { subIdx = s; break; }
    }
    if (subIdx < 0) subIdx = 0;
    for (int v : vertexIndices) {
        const int s = vertexSubMesh(v);
        if (s >= 0 && s != subIdx) return 0;
    }

    // 2. Reject inputs that would duplicate any triangle the fan would
    //    emit. For n=3 that's the single fan triangle; for larger N each
    //    of the N-2 fan triangles is checked against existing faces
    //    incident to the fan apex. Without this, the fan can recreate
    //    overlapping faces on top of an already-triangulated region
    //    (e.g. selecting all 4 verts of a closed quad). (CodeRabbit Major)
    auto triangleExists = [this](int va, int vb, int vc) {
        const std::set<int> target = {va, vb, vc};
        for (int f : facesAroundVertex(va)) {
            const auto fv = faceVertices(f);
            if (fv.size() != 3) continue;
            const std::set<int> tri(fv.begin(), fv.end());
            if (tri == target) return true;
        }
        return false;
    };
    for (int i = 1; i + 1 < n; ++i) {
        if (triangleExists(vertexIndices[0], vertexIndices[i],
                           vertexIndices[i + 1]))
            return 0;
    }

    // 3. Fan-triangulate from vertexIndices[0]. For n=3 emits one triangle;
    //    for n=4 emits two; for general N emits N-2.
    int triCount = 0;
    for (int i = 1; i + 1 < n; ++i) {
        appendTriangle(vertexIndices[0], vertexIndices[i],
                       vertexIndices[i + 1], subIdx);
        ++triCount;
    }
    if (triCount == 0) return 0;

    rebuildEdgesAndTwins();
    compactBoundaryHalfEdges();
    buildBoundaryHalfEdges();
    fixVertexHalfEdges();

    return triCount;
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

        // Polygonal faces have at least 3 half-edges. Larger N-gons
        // (quads etc.) are allowed by the data model — chunks 1+ of the
        // quad migration drop the triangle-only assumption.
        if (count < 3)
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
