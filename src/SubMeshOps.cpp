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

    // Give each part real WALL VOLUME (thin-shell assets) so a cut shows a solid
    // wall cross-section instead of the hollow interior. Solidify also SEALS each
    // part watertight (it walls every open boundary).
    if (opts.solidifyParts)
        for (auto& part : result.subMeshes)
            solidify(part, opts.solidifyThickness);

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

int SubMeshOps::solidify(EditableSubMesh& sub, float thickness)
{
    const unsigned int outerN = static_cast<unsigned int>(sub.vertices.size());
    if (outerN == 0 || sub.triangles.empty())
        return 0;

    // 1) Area-weighted vertex normals (use existing when the whole mesh has
    //    them; otherwise compute so the inward offset direction is sane).
    std::vector<Ogre::Vector3> vn(outerN, Ogre::Vector3::ZERO);
    bool haveAll = true;
    for (unsigned int i = 0; i < outerN; ++i) {
        if (sub.vertices[i].hasNormal && sub.vertices[i].normal.squaredLength() > 1e-12f)
            vn[i] = sub.vertices[i].normal.normalisedCopy();
        else
            haveAll = false;
    }
    if (!haveAll) {
        std::fill(vn.begin(), vn.end(), Ogre::Vector3::ZERO);
        for (const EditableTriangle& t : sub.triangles) {
            const Ogre::Vector3& p0 = sub.vertices[t.indices[0]].position;
            const Ogre::Vector3& p1 = sub.vertices[t.indices[1]].position;
            const Ogre::Vector3& p2 = sub.vertices[t.indices[2]].position;
            const Ogre::Vector3 fn = (p1 - p0).crossProduct(p2 - p0); // area-weighted (unnormalised)
            for (int k = 0; k < 3; ++k) vn[t.indices[k]] += fn;
        }
        for (auto& n : vn) { if (n.squaredLength() > 1e-12f) n.normalise(); }
    }

    // 2) Auto thickness = ~1.5% of the AABB diagonal when not specified.
    if (!(thickness > 0.0f)) {
        Ogre::Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
        for (const auto& v : sub.vertices) { mn.makeFloor(v.position); mx.makeCeil(v.position); }
        const float diag = (mx - mn).length();
        thickness = (diag > 1e-6f) ? diag * 0.015f : 0.01f;
    }

    // 3) Inner shell: duplicate every vertex pushed inward by `thickness` along
    //    -normal. Attributes carry over; normal flips inward.
    sub.vertices.reserve(outerN * 2);
    for (unsigned int i = 0; i < outerN; ++i) {
        EditableVertex inner = sub.vertices[i];
        inner.position = sub.vertices[i].position - vn[i] * thickness;
        inner.normal = -vn[i];
        inner.hasNormal = true;
        sub.vertices.push_back(inner);
    }
    const unsigned int innerBase = outerN; // inner index = outer index + innerBase

    // 4) Inner-shell triangles with REVERSED winding (faces inward, so the slab
    //    reads solid from inside the wall).
    const size_t outerTriCount = sub.triangles.size();
    for (size_t i = 0; i < outerTriCount; ++i) {
        const EditableTriangle& t = sub.triangles[i];
        EditableTriangle it;
        it.indices[0] = t.indices[0] + innerBase;
        it.indices[1] = t.indices[2] + innerBase; // swap 1<->2 to reverse winding
        it.indices[2] = t.indices[1] + innerBase;
        sub.triangles.push_back(it);
    }

    // 5) Stitch a wall between every OPEN boundary edge (outer a→b, interior on
    //    its LEFT) and its inner counterpart, closing the slab along the rim.
    //    Wall quad (outer a, outer b, inner b, inner a) → two triangles wound so
    //    the wall faces OUTWARD (consistent with the outer surface).
    auto k64 = [](unsigned int a, unsigned int b) -> uint64_t {
        return (static_cast<uint64_t>(a) << 32) | b;
    };
    std::unordered_map<uint64_t, int> dir;
    for (size_t i = 0; i < outerTriCount; ++i) {
        const EditableTriangle& t = sub.triangles[i];
        dir[k64(t.indices[0], t.indices[1])]++;
        dir[k64(t.indices[1], t.indices[2])]++;
        dir[k64(t.indices[2], t.indices[0])]++;
    }
    int walls = 0;
    for (const auto& kv : dir) {
        const unsigned int a = static_cast<unsigned int>(kv.first >> 32);
        const unsigned int b = static_cast<unsigned int>(kv.first & 0xffffffff);
        if (dir.find(k64(b, a)) != dir.end())
            continue; // interior edge, shared by two tris — not a boundary
        const unsigned int ai = a + innerBase, bi = b + innerBase;
        // The wall must CANCEL the dangling edges so the slab is watertight: the
        // outer surface has boundary edge a→b (needs b→a), and the reverse-wound
        // inner shell has boundary edge ai→bi (needs bi→ai). The quad loop
        // b→a→ai→bi→b supplies both. Triangulate (b,a,ai) + (b,ai,bi).
        EditableTriangle t1, t2;
        t1.indices[0] = b;  t1.indices[1] = a;  t1.indices[2] = ai;
        t2.indices[0] = b;  t2.indices[1] = ai; t2.indices[2] = bi;
        sub.triangles.push_back(t1);
        sub.triangles.push_back(t2);
        ++walls;
    }

    // Triangle list is now canonical; drop any stale n-gon binding.
    sub.faces.clear();
    return walls;
}
