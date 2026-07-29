#include "SubMeshOps.h"

#include "MeshSegmenter.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

std::vector<SubMeshOps::FaceGroup>
SubMeshOps::groupFacesByLabel(const std::vector<int>& faceLabels)
{
    std::map<int, FaceGroup> byLabel; // ordered by label so Unknown(0) is first
    for (uint32_t tri = 0; tri < faceLabels.size(); ++tri) {
        const int label = faceLabels[tri];
        auto it = byLabel.find(label);
        if (it == byLabel.end()) {
            FaceGroup g;
            g.label = label;
            g.name = MeshSegmenter::partName(label);
            it = byLabel.emplace(label, std::move(g)).first;
        }
        it->second.triangles.push_back(tri);
    }
    std::vector<FaceGroup> out;
    out.reserve(byLabel.size());
    for (auto& kv : byLabel)
        out.push_back(std::move(kv.second));
    return out;
}

size_t SubMeshOps::totalTriangleCount(const std::vector<EditableSubMesh>& subMeshes)
{
    size_t n = 0;
    for (const auto& sm : subMeshes)
        n += sm.triangles.size();
    return n;
}

namespace {

// Resolve a GLOBAL triangle index to (submesh, localTriangle). The global
// stream is submesh-then-local order — the same convention MeshSegmenter's
// faceLabels use (built by facesFromVertexLabels over the concatenated index
// buffer) and EditModeController::globalTriToLocal.
bool globalTriToLocal(const std::vector<EditableSubMesh>& subMeshes, uint32_t globalTri,
                      size_t& subOut, size_t& localOut)
{
    size_t base = 0;
    for (size_t s = 0; s < subMeshes.size(); ++s) {
        const size_t n = subMeshes[s].triangles.size();
        if (globalTri < base + n) {
            subOut = s;
            localOut = globalTri - base;
            return true;
        }
        base += n;
    }
    return false;
}

} // namespace

SubMeshOps::SplitResult
SubMeshOps::splitByFaceGroups(const std::vector<EditableSubMesh>& subMeshes,
                              const std::vector<int>& faceLabels,
                              const std::vector<FaceGroup>& groups)
{
    return splitByFaceGroups(subMeshes, faceLabels, groups, SplitOptions{});
}

SubMeshOps::SplitResult
SubMeshOps::splitByFaceGroups(const std::vector<EditableSubMesh>& subMeshes,
                              const std::vector<int>& faceLabels,
                              const std::vector<FaceGroup>& groups,
                              const SplitOptions& opts)
{
    SplitResult result;

    const size_t triTotal = totalTriangleCount(subMeshes);
    if (faceLabels.size() != triTotal) {
        result.error = QStringLiteral(
            "face label count (%1) does not match triangle count (%2)")
            .arg(faceLabels.size()).arg(triTotal);
        return result;
    }

    // Accepted groups only (skip excluded), label -> group meta.
    std::unordered_map<int, const FaceGroup*> accepted;
    for (const FaceGroup& g : groups) {
        if (!g.excluded)
            accepted.emplace(g.label, &g);
    }
    if (accepted.empty()) {
        result.error = QStringLiteral("no part groups accepted (all excluded or empty)");
        return result;
    }

    // One output submesh per (accepted label, source material). Keying on the
    // material too is required for multi-material assets: a single part label
    // (e.g. "torso") whose triangles come from two different source materials
    // must emit TWO submeshes so each keeps its own material — collapsing them
    // onto whichever triangle came first silently repaints the model (#859
    // review). When assignPartMaterials is set the part gets one generated
    // material regardless, so it keys on the label alone.
    struct Builder {
        int label = 0;
        QString name;
        std::string sourceMaterial;  // the key's material (empty when part-mat)
        EditableSubMesh sub;
        bool sourceHadFaces = false;
        int firstGlobalTri = 0;      // for deterministic output ordering
        std::vector<std::unordered_map<unsigned int, unsigned int>> remap;
    };

    // Builder key: label, plus source material unless we're assigning one
    // material per part. Kept ordered for determinism.
    using BuilderKey = std::pair<int, std::string>;
    std::map<BuilderKey, size_t> builderOfKey;
    std::vector<Builder> builders;

    int duplicated = 0;
    // Track how many builders each (source submesh, source vertex) landed in,
    // to count boundary duplications (a vertex used by >1 builder is dup'd).
    std::vector<std::unordered_map<unsigned int, int>> groupUseCount(subMeshes.size());

    for (uint32_t gtri = 0; gtri < faceLabels.size(); ++gtri) {
        const int label = faceLabels[gtri];
        auto ait = accepted.find(label);
        if (ait == accepted.end())
            continue; // excluded / no accepted group -> dropped

        size_t s = 0, localTri = 0;
        if (!globalTriToLocal(subMeshes, gtri, s, localTri))
            continue;
        const EditableSubMesh& src = subMeshes[s];
        const EditableTriangle& tri = src.triangles[localTri];

        // assignPartMaterials → one submesh per label (material is generated);
        // else split per source material so multi-material parts round-trip.
        const std::string keyMat = opts.assignPartMaterials ? std::string() : src.materialName;
        const BuilderKey key{label, keyMat};
        auto kit = builderOfKey.find(key);
        size_t bi;
        if (kit == builderOfKey.end()) {
            Builder nb;
            nb.label = label;
            nb.name = ait->second->name;
            nb.sourceMaterial = keyMat;
            nb.firstGlobalTri = static_cast<int>(gtri);
            nb.remap.resize(subMeshes.size());
            nb.sub.materialName =
                opts.assignPartMaterials
                    ? (opts.namePrefix.isEmpty()
                           ? nb.name.toStdString()
                           : (opts.namePrefix + QStringLiteral(".") + nb.name).toStdString())
                    : src.materialName;
            bi = builders.size();
            builderOfKey.emplace(key, bi);
            builders.push_back(std::move(nb));
        } else {
            bi = kit->second;
        }

        Builder& b = builders[bi];
        b.sourceHadFaces = b.sourceHadFaces || !src.faces.empty();

        EditableTriangle newTri;
        for (int k = 0; k < 3; ++k) {
            const unsigned int srcV = tri.indices[k];
            auto& map = b.remap[s];
            auto vit = map.find(srcV);
            unsigned int newV;
            if (vit == map.end()) {
                newV = static_cast<unsigned int>(b.sub.vertices.size());
                b.sub.vertices.push_back(src.vertices[srcV]);
                map.emplace(srcV, newV);
                // Boundary bookkeeping: this source vertex is now used by one
                // more builder. The 2nd+ builder to claim it is a duplication.
                int& uses = groupUseCount[s][srcV];
                if (uses >= 1)
                    ++duplicated;
                ++uses;
            } else {
                newV = vit->second;
            }
            newTri.indices[k] = newV;
        }
        b.sub.triangles.push_back(newTri);
    }

    // Deterministic emit order: by part label, then by first-contributing
    // triangle (so multi-material pieces of one part stay grouped + stable).
    std::sort(builders.begin(), builders.end(), [](const Builder& a, const Builder& b) {
        if (a.label != b.label)
            return a.label < b.label;
        return a.firstGlobalTri < b.firstGlobalTri;
    });

    // Emit submeshes; rebuild n-gon faces where the source had them (promote
    // the triangle soup back to trivial faces — the exporter/edit layer expects
    // `faces` canonical when non-empty). Optional connected-component split.
    //
    // A part label can now produce MORE than one submesh (multiple source
    // materials and/or disconnected islands), so names get a per-label running
    // suffix: `torso`, `torso.1`, `torso.2`, … The first piece of each label
    // keeps the bare name. Builders are label-sorted, so the counter is simply
    // reset when the label changes.
    std::unordered_map<int, int> nameCounterByLabel;
    for (Builder& b : builders) {
        if (b.sub.triangles.empty())
            continue;

        std::vector<EditableSubMesh> pieces;
        if (opts.splitDisconnected) {
            // Partition this builder's submesh into connected components.
            const int vcount = static_cast<int>(b.sub.vertices.size());
            std::vector<uint32_t> flat;
            flat.reserve(b.sub.triangles.size() * 3);
            for (const auto& t : b.sub.triangles) {
                flat.push_back(t.indices[0]);
                flat.push_back(t.indices[1]);
                flat.push_back(t.indices[2]);
            }
            std::vector<int> island;
            const int islands = MeshSegmenter::connectedComponents(
                vcount, flat.data(), static_cast<int>(flat.size()), island);
            if (islands <= 1) {
                pieces.push_back(std::move(b.sub));
            } else {
                pieces.resize(islands);
                std::vector<std::unordered_map<unsigned int, unsigned int>> pmap(islands);
                for (const auto& t : b.sub.triangles) {
                    const int isl = island[t.indices[0]];
                    EditableSubMesh& piece = pieces[isl];
                    if (piece.vertices.empty() && piece.triangles.empty())
                        piece.materialName = b.sub.materialName;
                    EditableTriangle nt;
                    for (int k = 0; k < 3; ++k) {
                        const unsigned int sv = t.indices[k];
                        auto& m = pmap[isl];
                        auto it = m.find(sv);
                        unsigned int nv;
                        if (it == m.end()) {
                            nv = static_cast<unsigned int>(piece.vertices.size());
                            piece.vertices.push_back(b.sub.vertices[sv]);
                            m.emplace(sv, nv);
                        } else {
                            nv = it->second;
                        }
                        nt.indices[k] = nv;
                    }
                    piece.triangles.push_back(nt);
                }
            }
        } else {
            pieces.push_back(std::move(b.sub));
        }

        for (EditableSubMesh& piece : pieces) {
            if (piece.triangles.empty())
                continue;
            if (b.sourceHadFaces)
                promoteTrianglesToFaces(piece); // keep n-gon storage canonical
            const int n = nameCounterByLabel[b.label]++;
            QString partName = b.name;
            if (n > 0)
                partName += QStringLiteral(".%1").arg(n);
            result.subMeshes.push_back(std::move(piece));
            result.partNames.push_back(partName);
        }
    }

    if (result.subMeshes.empty()) {
        result.error = QStringLiteral("split produced no geometry");
        return result;
    }

    // Close each part's OPEN cut face so every part is a watertight solid — a
    // split just separates geometry and leaves the seam hollow, so an exploded
    // part would show a see-through hole where it was cut from its neighbour.
    // On by default (opts.capParts); print-prep re-caps harmlessly (idempotent
    // once closed).
    if (opts.capParts)
        for (auto& part : result.subMeshes)
            capOpenBoundaries(part);

    result.duplicatedBoundaryVertices = duplicated;
    result.createdSubMeshes = static_cast<int>(result.subMeshes.size());
    result.ok = true;
    return result;
}

SubMeshOps::JoinResult SubMeshOps::joinParts(const std::vector<JoinPart>& parts)
{
    JoinResult result;
    if (parts.empty()) {
        result.error = QStringLiteral("no parts to join");
        return result;
    }

    // Output submeshes keyed by material name so parts sharing a material merge
    // into one submesh (matches the epic's "expected submeshes/materials").
    std::vector<EditableSubMesh> merged;
    std::unordered_map<std::string, size_t> byMaterial;

    for (const JoinPart& part : parts) {
        // Linear part of the transform for normals/tangents (inverse-transpose
        // for correctness under non-uniform scale; for the common rigid case
        // this equals the rotation).
        const Ogre::Matrix4& M = part.transform;
        Ogre::Matrix3 linear;
        M.extract3x3Matrix(linear);
        Ogre::Matrix3 normalMat = linear.Inverse().Transpose();
        // A negative-determinant (orientation-reversing) transform — e.g. a
        // negative scale on one axis — mirrors positions but leaves the
        // triangle index order and tangent handedness unchanged, which would
        // flip the effective winding and back-face-cull the joined part. Detect
        // it and reverse winding + flip tangent handedness (w) to compensate.
        const bool mirrored = linear.Determinant() < 0.0f;

        for (const EditableSubMesh& src : part.subMeshes) {
            auto mit = byMaterial.find(src.materialName);
            size_t dstIdx;
            if (mit == byMaterial.end()) {
                dstIdx = merged.size();
                merged.emplace_back();
                merged.back().materialName = src.materialName;
                byMaterial.emplace(src.materialName, dstIdx);
            } else {
                dstIdx = mit->second;
            }
            EditableSubMesh& dst = merged[dstIdx];
            const unsigned int base = static_cast<unsigned int>(dst.vertices.size());

            for (const EditableVertex& sv : src.vertices) {
                EditableVertex v = sv;
                v.position = M * sv.position;
                if (v.hasNormal)
                    v.normal = (normalMat * sv.normal).normalisedCopy();
                if (v.hasTangent) {
                    Ogre::Vector3 t3(sv.tangent.x, sv.tangent.y, sv.tangent.z);
                    t3 = (linear * t3).normalisedCopy();
                    // Flip handedness (w) under a mirror so normal mapping stays
                    // correct against the reversed winding.
                    const float w = mirrored ? -sv.tangent.w : sv.tangent.w;
                    v.tangent = Ogre::Vector4(t3.x, t3.y, t3.z, w);
                }
                dst.vertices.push_back(v);
            }
            for (const EditableTriangle& t : src.triangles) {
                EditableTriangle nt;
                if (mirrored) {
                    // Reverse winding (swap the last two corners) so front faces
                    // stay front-facing after the position mirror.
                    nt.indices[0] = t.indices[0] + base;
                    nt.indices[1] = t.indices[2] + base;
                    nt.indices[2] = t.indices[1] + base;
                } else {
                    nt.indices[0] = t.indices[0] + base;
                    nt.indices[1] = t.indices[1] + base;
                    nt.indices[2] = t.indices[2] + base;
                }
                dst.triangles.push_back(nt);
            }
        }
    }

    // Rebuild trivial n-gon faces so the merged submeshes stay consistent with
    // the editor's canonical-faces invariant when consumed downstream.
    result.subMeshes = std::move(merged);
    result.ok = true;
    return result;
}

std::vector<Ogre::Vector3>
SubMeshOps::explodeOffsets(const std::vector<Ogre::Vector3>& partCentroids,
                           const Ogre::AxisAlignedBox& assemblyBounds, float distance)
{
    std::vector<Ogre::Vector3> offsets(partCentroids.size(), Ogre::Vector3::ZERO);
    if (partCentroids.empty())
        return offsets;

    Ogre::Vector3 center = Ogre::Vector3::ZERO;
    for (const auto& c : partCentroids)
        center += c;
    center /= static_cast<float>(partCentroids.size());

    float diag = 1.0f;
    if (!assemblyBounds.isNull() && !assemblyBounds.isInfinite())
        diag = assemblyBounds.getSize().length();
    if (!(diag > 0.0f))
        diag = 1.0f;

    for (size_t i = 0; i < partCentroids.size(); ++i) {
        Ogre::Vector3 dir = partCentroids[i] - center;
        const float len = dir.length();
        if (len > 1e-6f)
            offsets[i] = (dir / len) * (distance * diag);
    }
    return offsets;
}

int SubMeshOps::capOpenBoundaries(EditableSubMesh& sub)
{
    const size_t triCount = sub.triangles.size();
    if (triCount == 0 || sub.vertices.empty())
        return 0;

    // 1) Boundary edges = directed edges whose REVERSE is not also present. In a
    //    closed manifold every edge appears once in each direction; an open cut
    //    face leaves its rim edges with no opposite. Key by the ordered vertex
    //    pair so we can find the unmatched ones, and remember the directed edge
    //    (a→b) so the cap can be wound consistently with the source triangles.
    auto key = [](unsigned int a, unsigned int b) -> uint64_t {
        return (static_cast<uint64_t>(a) << 32) | b;
    };
    std::unordered_map<uint64_t, int> dirCount; // directed edge → count
    for (const EditableTriangle& t : sub.triangles) {
        dirCount[key(t.indices[0], t.indices[1])]++;
        dirCount[key(t.indices[1], t.indices[2])]++;
        dirCount[key(t.indices[2], t.indices[0])]++;
    }
    // A directed edge a→b is a boundary edge when b→a is absent. A rim vertex can
    // have MORE than one outgoing boundary edge (a figure-eight / pinched cut, or
    // two separate rim loops touching a shared vertex — common at shoulders/hips),
    // so keep a LIST of successors per vertex and CONSUME them as we walk. A
    // single-successor map silently drops the extra edges and leaves those loops
    // uncapped (the "gap not closed on all joints" bug).
    std::unordered_map<unsigned int, std::vector<unsigned int>> succ;
    size_t boundaryEdges = 0;
    for (const auto& kv : dirCount) {
        const unsigned int a = static_cast<unsigned int>(kv.first >> 32);
        const unsigned int b = static_cast<unsigned int>(kv.first & 0xffffffff);
        if (dirCount.find(key(b, a)) == dirCount.end()) {
            succ[a].push_back(b);   // boundary edge a→b (interior on its left)
            ++boundaryEdges;
        }
    }
    if (boundaryEdges == 0)
        return 0; // already closed

    // 2) Walk each boundary loop by consuming edges from `succ`. Every boundary
    //    edge is used exactly once, so ALL rim loops get capped — not just the
    //    first one reachable from each vertex.
    auto popSucc = [&](unsigned int a, bool& ok) -> unsigned int {
        auto it = succ.find(a);
        if (it == succ.end() || it->second.empty()) { ok = false; return 0; }
        unsigned int b = it->second.back();
        it->second.pop_back();
        if (it->second.empty()) succ.erase(it);
        ok = true;
        return b;
    };
    int caps = 0;
    size_t consumed = 0;
    while (consumed < boundaryEdges) {
        // Find any vertex that still has an unused outgoing boundary edge.
        unsigned int start = 0; bool found = false;
        for (const auto& kv : succ) { if (!kv.second.empty()) { start = kv.first; found = true; break; } }
        if (!found)
            break;
        // Record the actual DIRECTED boundary edges (a→b) we consume, in order.
        std::vector<std::pair<unsigned int, unsigned int>> edges;
        std::vector<unsigned int> loopVerts;
        unsigned int cur = start;
        // Follow successors, consuming each edge, until we return to start or hit
        // a vertex with no remaining successor (open chain — still fan it).
        for (;;) {
            bool ok = false;
            unsigned int nxt = popSucc(cur, ok);
            if (!ok) break;
            ++consumed;
            edges.emplace_back(cur, nxt);
            loopVerts.push_back(cur);
            cur = nxt;
            if (cur == start) break;   // closed loop
        }
        if (edges.size() < 3)
            continue;

        // 3) Centroid-fan fill. New centre vertex copies a rim vertex's
        //    attributes (material/uv space) with the averaged position.
        Ogre::Vector3 c = Ogre::Vector3::ZERO;
        for (unsigned int vi : loopVerts) c += sub.vertices[vi].position;
        c /= static_cast<float>(loopVerts.size());
        EditableVertex centre = sub.vertices[loopVerts[0]];
        centre.position = c;
        // Give the centre vertex the cap's averaged geometric normal so it shades
        // correctly even when the mesh is built with recomputeNormals=false (the
        // split path preserves authored normals) — otherwise the cap centre is
        // normal-less and renders black. Cap face (centre,b,a) normal =
        // (b-c)×(a-c).
        Ogre::Vector3 capN = Ogre::Vector3::ZERO;
        for (const auto& e : edges) {
            const Ogre::Vector3& pb = sub.vertices[e.second].position;
            const Ogre::Vector3& pa = sub.vertices[e.first].position;
            capN += (pb - c).crossProduct(pa - c);
        }
        if (capN.squaredLength() > 1e-12f) {
            centre.normal = capN.normalisedCopy();
            centre.hasNormal = true;
        } else {
            centre.hasNormal = false;
        }
        const unsigned int cIdx = static_cast<unsigned int>(sub.vertices.size());
        sub.vertices.push_back(centre);

        // Winding — the ONLY watertight choice: a boundary edge a→b has the part
        // interior on its LEFT, so the cap triangle must contain the REVERSE edge
        // b→a to cancel it. Emit (centre, b, a) for every consumed edge. This is
        // exact for any loop shape and both ends of a tube, unlike a global
        // centroid-normal heuristic (which flips the wrong end and left the rim
        // open — the "gap not closed on all joints" bug).
        for (const auto& e : edges) {
            EditableTriangle t;
            t.indices[0] = cIdx;
            t.indices[1] = e.second; // b
            t.indices[2] = e.first;  // a
            sub.triangles.push_back(t);
        }
        ++caps;
    }

    // Cap triangles were appended; drop any stale n-gon `faces` binding so the
    // triangle list is authoritative downstream (buildSubMeshBuffers rebuilds).
    if (caps > 0)
        sub.faces.clear();
    return caps;
}
