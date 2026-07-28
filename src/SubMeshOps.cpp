#include "SubMeshOps.h"

#include "MeshSegmenter.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include <manifold/manifold.h>

namespace {

// A vertex key that survives cross-submesh comparison for boundary welding:
// quantised position (sufficient for coincident split-seam verts). Position is
// the only reliable identity across independently-rebuilt part submeshes.
struct PosKey {
    int64_t x, y, z;
    bool operator==(const PosKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct PosKeyHash {
    size_t operator()(const PosKey& k) const
    {
        uint64_t h = 1469598103934665603ULL;
        for (int64_t c : {k.x, k.y, k.z}) {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ULL;
        }
        return static_cast<size_t>(h);
    }
};
PosKey posKeyOf(const Ogre::Vector3& p, double scale)
{
    return PosKey{static_cast<int64_t>(std::llround(double(p.x) * scale)),
                  static_cast<int64_t>(std::llround(double(p.y) * scale)),
                  static_cast<int64_t>(std::llround(double(p.z) * scale))};
}

} // namespace

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
        centre.hasNormal = false; // recomputed after (or by createNewMesh)
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

SubMeshOps::BoundaryPlane
SubMeshOps::estimateBoundaryPlane(const std::vector<EditableSubMesh>& partA,
                                  const std::vector<EditableSubMesh>& partB, float weldTol)
{
    BoundaryPlane plane;

    // Boundary = vertices of A that are coincident (within weldTol) with a
    // vertex of B — the seam the split duplicated.
    const double scale = weldTol > 0.0f ? 1.0 / static_cast<double>(weldTol) : 1e4;
    std::unordered_map<PosKey, int, PosKeyHash> bKeys;
    for (const auto& sm : partB)
        for (const auto& v : sm.vertices)
            bKeys[posKeyOf(v.position, scale)] += 1;

    std::vector<Ogre::Vector3> pts;
    for (const auto& sm : partA)
        for (const auto& v : sm.vertices) {
            if (bKeys.count(posKeyOf(v.position, scale)))
                pts.push_back(v.position);
        }

    if (pts.size() < 8) {
        plane.reason = QStringLiteral("boundary too small (%1 shared verts, need >= 8)")
                           .arg(pts.size());
        return plane;
    }

    // Centroid + covariance -> best-fit plane via smallest-eigenvalue direction.
    Ogre::Vector3 c = Ogre::Vector3::ZERO;
    for (const auto& p : pts)
        c += p;
    c /= static_cast<float>(pts.size());

    double cov[6] = {0, 0, 0, 0, 0, 0}; // xx xy xz yy yz zz
    for (const auto& p : pts) {
        const double dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
        cov[0] += dx * dx; cov[1] += dx * dy; cov[2] += dx * dz;
        cov[3] += dy * dy; cov[4] += dy * dz; cov[5] += dz * dz;
    }
    const double inv = 1.0 / static_cast<double>(pts.size());
    for (double& v : cov)
        v *= inv;

    Ogre::Matrix3 C(cov[0], cov[1], cov[2],
                    cov[1], cov[3], cov[4],
                    cov[2], cov[4], cov[5]);
    Ogre::Vector3 eigvec[3];
    Ogre::Real eigval[3];
    C.EigenSolveSymmetric(eigval, eigvec);

    // Smallest eigenvalue → plane normal. Ogre returns ascending eigenvalues.
    int smallest = 0;
    for (int i = 1; i < 3; ++i)
        if (eigval[i] < eigval[smallest])
            smallest = i;
    int largest = 0;
    for (int i = 1; i < 3; ++i)
        if (eigval[i] > eigval[largest])
            largest = i;

    // Stability: the boundary must look planar (small normal-direction spread
    // relative to in-plane spread). If the smallest eigenvalue isn't clearly
    // separated from the largest, it's a blob, not a seam.
    const double lnMin = std::max(0.0, static_cast<double>(eigval[smallest]));
    const double lnMax = std::max(1e-12, static_cast<double>(eigval[largest]));
    const double flatness = lnMin / lnMax;

    plane.center = c;
    plane.normal = eigvec[smallest].normalisedCopy();
    // In-plane radius: RMS distance to centroid projected off the normal.
    double r2 = 0.0;
    for (const auto& p : pts) {
        Ogre::Vector3 d = p - c;
        Ogre::Vector3 inPlane = d - plane.normal * d.dotProduct(plane.normal);
        r2 += inPlane.squaredLength();
    }
    plane.radius = std::sqrt(r2 / static_cast<double>(pts.size()));

    if (flatness > 0.15) {
        plane.reason = QStringLiteral("boundary not planar enough (flatness %1)")
                           .arg(flatness, 0, 'g', 3);
        return plane;
    }
    if (!(plane.radius > 0.0f)) {
        plane.reason = QStringLiteral("degenerate boundary radius");
        return plane;
    }
    plane.stable = true;
    return plane;
}

namespace {

// Append a closed cylinder (both caps) to `sub`, axis = `axis` (unit),
// centered at `base` and extending `depth` along +axis. Radius `r`,
// `segments` around. Adds vertices + triangles; leaves faces triangle-only.
void appendCylinder(EditableSubMesh& sub, const Ogre::Vector3& base,
                    const Ogre::Vector3& axis, float r, float depth, int segments)
{
    // Build an orthonormal frame around the axis.
    Ogre::Vector3 up = std::fabs(axis.y) < 0.9f ? Ogre::Vector3::UNIT_Y : Ogre::Vector3::UNIT_X;
    Ogre::Vector3 u = axis.crossProduct(up).normalisedCopy();
    Ogre::Vector3 w = axis.crossProduct(u).normalisedCopy();
    const Ogre::Vector3 top = base + axis * depth;

    const unsigned int startV = static_cast<unsigned int>(sub.vertices.size());
    auto addVert = [&](const Ogre::Vector3& p, const Ogre::Vector3& n) {
        EditableVertex v;
        v.position = p;
        v.normal = n;
        v.hasNormal = true;
        sub.vertices.push_back(v);
        return static_cast<unsigned int>(sub.vertices.size() - 1);
    };
    auto addTri = [&](unsigned int a, unsigned int b, unsigned int c) {
        EditableTriangle t;
        t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
        sub.triangles.push_back(t);
    };

    std::vector<unsigned int> ringBase(segments), ringTop(segments);
    for (int i = 0; i < segments; ++i) {
        const float a = 2.0f * Ogre::Math::PI * float(i) / float(segments);
        const Ogre::Vector3 radial = (u * std::cos(a) + w * std::sin(a));
        ringBase[i] = addVert(base + radial * r, radial);
        ringTop[i] = addVert(top + radial * r, radial);
    }
    for (int i = 0; i < segments; ++i) {
        const int j = (i + 1) % segments;
        addTri(ringBase[i], ringBase[j], ringTop[j]);
        addTri(ringBase[i], ringTop[j], ringTop[i]);
    }
    // Caps.
    const unsigned int cBase = addVert(base, -axis);
    const unsigned int cTop = addVert(top, axis);
    for (int i = 0; i < segments; ++i) {
        const int j = (i + 1) % segments;
        addTri(cBase, ringBase[j], ringBase[i]);
        addTri(cTop, ringTop[i], ringTop[j]);
    }
    (void)startV;
}

} // namespace

int SubMeshOps::buildAlignmentPegs(const BoundaryPlane& plane, const PegOptions& opts,
                                   EditableSubMesh& outMale, EditableSubMesh& outSocket)
{
    if (!plane.stable)
        return 0;
    if (opts.maxPegsPerBoundary <= 0 || !(opts.pegRadius > 0.0f))
        return 0;

    // How many pegs actually fit inside the boundary ring without overlapping.
    // Place them on a ring at ~half the boundary radius.
    const float placeRadius = plane.radius * 0.5f;
    const float pegR = opts.pegRadius;
    const float socketR = opts.pegRadius + opts.clearance;
    int nPegs = opts.maxPegsPerBoundary;
    if (placeRadius < pegR * 1.5f)
        nPegs = 1; // boundary too small for a ring; one central peg

    // In-plane frame.
    Ogre::Vector3 up = std::fabs(plane.normal.y) < 0.9f ? Ogre::Vector3::UNIT_Y
                                                        : Ogre::Vector3::UNIT_X;
    Ogre::Vector3 u = plane.normal.crossProduct(up).normalisedCopy();
    Ogre::Vector3 w = plane.normal.crossProduct(u).normalisedCopy();

    int made = 0;
    for (int i = 0; i < nPegs; ++i) {
        Ogre::Vector3 center = plane.center;
        if (nPegs > 1) {
            const float a = 2.0f * Ogre::Math::PI * float(i) / float(nPegs);
            center += (u * std::cos(a) + w * std::sin(a)) * placeRadius;
        }
        // Male peg protrudes from the boundary along +normal; socket cutter
        // sinks along the same axis but starts slightly behind the plane so it
        // fully overlaps the mating solid.
        appendCylinder(outMale, center, plane.normal, pegR, opts.pegDepth, opts.radialSegments);
        appendCylinder(outSocket, center - plane.normal * (opts.clearance),
                       plane.normal, socketR, opts.pegDepth + opts.clearance, opts.radialSegments);
        ++made;
    }

    if (made > 0) {
        outMale.materialName = "connector_male";
        outSocket.materialName = "connector_socket";
    }
    return made;
}

namespace {
// Append `src`'s vertices + triangles onto `dst` (offsetting the indices by
// dst's current vertex count). Used to merge a peg cylinder into a part.
void appendGeometry(EditableSubMesh& dst, const EditableSubMesh& src)
{
    const unsigned int base = static_cast<unsigned int>(dst.vertices.size());
    dst.vertices.insert(dst.vertices.end(), src.vertices.begin(), src.vertices.end());
    for (const EditableTriangle& t : src.triangles) {
        EditableTriangle nt;
        nt.indices[0] = t.indices[0] + base;
        nt.indices[1] = t.indices[1] + base;
        nt.indices[2] = t.indices[2] + base;
        dst.triangles.push_back(nt);
    }
}

// Give every vertex in `sub` that lacks bone weights the bone assignments of its
// nearest vertex in `source` — so connector geometry (peg / socket collar) on a
// SKINNED part rigidly follows the part it attaches to instead of collapsing to
// the skeleton origin (a vertex with no weights binds to bone 0 at weight 0).
// No-op when the source part has no weights (static mesh).
void inheritNearestBoneWeights(EditableSubMesh& sub, const EditableSubMesh& source)
{
    bool sourceSkinned = false;
    for (const EditableVertex& v : source.vertices)
        if (!v.boneAssignments.empty()) { sourceSkinned = true; break; }
    if (!sourceSkinned || source.vertices.empty())
        return;
    for (EditableVertex& v : sub.vertices) {
        if (!v.boneAssignments.empty())
            continue;
        const EditableVertex* best = nullptr;
        float bestD = std::numeric_limits<float>::max();
        for (const EditableVertex& sv : source.vertices) {
            if (sv.boneAssignments.empty())
                continue;
            const float d = sv.position.squaredDistance(v.position);
            if (d < bestD) { bestD = d; best = &sv; }
        }
        if (best)
            v.boneAssignments = best->boneAssignments;
    }
}

// Append a short, thick RING (annular collar) at a socket mouth to `sub`,
// centered at `center`, in the plane normal to `axis`. Inner radius = `r`
// (the socket bore), outer = 1.35·r, thickness `t` along +axis. Purely a
// visible red marker for the female side; renders as a flat washer.
void appendSocketCollar(EditableSubMesh& sub, const Ogre::Vector3& center,
                        const Ogre::Vector3& axis, float r, int segments)
{
    const Ogre::Vector3 n = axis.normalisedCopy();
    const float rOuter = r * 1.35f;
    const float t = r * 0.12f;  // shallow washer thickness
    Ogre::Vector3 up = std::fabs(n.y) < 0.9f ? Ogre::Vector3::UNIT_Y : Ogre::Vector3::UNIT_X;
    Ogre::Vector3 u = n.crossProduct(up).normalisedCopy();
    Ogre::Vector3 w = n.crossProduct(u).normalisedCopy();
    const Ogre::Vector3 front = center + n * (t * 0.5f);
    const Ogre::Vector3 back = center - n * (t * 0.5f);

    auto addVert = [&](const Ogre::Vector3& p, const Ogre::Vector3& nrm) {
        EditableVertex v; v.position = p; v.normal = nrm; v.hasNormal = true;
        sub.vertices.push_back(v);
        return static_cast<unsigned int>(sub.vertices.size() - 1);
    };
    auto addTri = [&](unsigned int a, unsigned int b, unsigned int c) {
        EditableTriangle tri; tri.indices[0] = a; tri.indices[1] = b; tri.indices[2] = c;
        sub.triangles.push_back(tri);
    };
    std::vector<unsigned int> fi(segments), fo(segments), bi(segments), bo(segments);
    for (int i = 0; i < segments; ++i) {
        const float a = 2.0f * Ogre::Math::PI * float(i) / float(segments);
        const Ogre::Vector3 rad = (u * std::cos(a) + w * std::sin(a));
        fi[i] = addVert(front + rad * r, n);
        fo[i] = addVert(front + rad * rOuter, n);
        bi[i] = addVert(back + rad * r, -n);
        bo[i] = addVert(back + rad * rOuter, -n);
    }
    for (int i = 0; i < segments; ++i) {
        const int j = (i + 1) % segments;
        // front face (facing +n)
        addTri(fi[i], fo[i], fo[j]); addTri(fi[i], fo[j], fi[j]);
        // back face (facing -n)
        addTri(bi[i], bo[j], bo[i]); addTri(bi[i], bi[j], bo[j]);
        // outer wall
        addTri(fo[i], bo[i], bo[j]); addTri(fo[i], bo[j], fo[j]);
        // inner wall
        addTri(fi[i], bi[j], bi[i]); addTri(fi[i], fi[j], bi[j]);
    }
}

// Reproduce the exact peg-ring centers that buildAlignmentPegs() places, so the
// SOCKET boolean cutters line up 1:1 with the male pegs. `made` is the peg count
// buildAlignmentPegs actually produced (it clamps a too-small boundary to 1).
std::vector<Ogre::Vector3> pegRingCenters(const SubMeshOps::BoundaryPlane& plane,
                                          const SubMeshOps::PegOptions& opts, int made)
{
    std::vector<Ogre::Vector3> centers;
    if (made <= 0)
        return centers;
    const float placeRadius = plane.radius * 0.5f;
    Ogre::Vector3 up = std::fabs(plane.normal.y) < 0.9f ? Ogre::Vector3::UNIT_Y
                                                        : Ogre::Vector3::UNIT_X;
    Ogre::Vector3 u = plane.normal.crossProduct(up).normalisedCopy();
    Ogre::Vector3 w = plane.normal.crossProduct(u).normalisedCopy();
    for (int i = 0; i < made; ++i) {
        Ogre::Vector3 center = plane.center;
        if (made > 1) {
            const float a = 2.0f * Ogre::Math::PI * float(i) / float(made);
            center += (u * std::cos(a) + w * std::sin(a)) * placeRadius;
        }
        centers.push_back(center);
    }
    return centers;
}

// Convert an EditableSubMesh's triangle soup into a Manifold solid. Position-only
// (Manifold does its own vertex welding by geometric position), which is all the
// boolean needs — attributes are re-derived after by nearest-source lookup.
manifold::Manifold toManifold(const EditableSubMesh& sub)
{
    manifold::MeshGL m;
    m.numProp = 3;
    m.vertProperties.reserve(sub.vertices.size() * 3);
    for (const EditableVertex& v : sub.vertices) {
        m.vertProperties.push_back(v.position.x);
        m.vertProperties.push_back(v.position.y);
        m.vertProperties.push_back(v.position.z);
    }
    m.triVerts.reserve(sub.triangles.size() * 3);
    for (const EditableTriangle& t : sub.triangles) {
        m.triVerts.push_back(t.indices[0]);
        m.triVerts.push_back(t.indices[1]);
        m.triVerts.push_back(t.indices[2]);
    }
    return manifold::Manifold(m);
}

// Rebuild an EditableSubMesh from a Manifold result, re-deriving per-vertex
// attributes (normal/uv/colour/bone weights) from the ORIGINAL sub by nearest
// source vertex — so verts the boolean left untouched keep their exact data and
// newly-created socket-wall verts inherit their closest neighbour's attributes.
void fromManifold(const manifold::Manifold& man, const EditableSubMesh& original,
                  EditableSubMesh& out)
{
    manifold::MeshGL result = man.GetMeshGL();
    out.vertices.clear();
    out.triangles.clear();
    out.vertices.reserve(result.NumVert());

    // Brute-force nearest source vertex (part vertex counts are small — a few
    // thousand at most — and this runs once per socket cut).
    auto nearestSource = [&](const Ogre::Vector3& p) -> const EditableVertex* {
        const EditableVertex* best = nullptr;
        float bestD = std::numeric_limits<float>::max();
        for (const EditableVertex& sv : original.vertices) {
            const float d = sv.position.squaredDistance(p);
            if (d < bestD) { bestD = d; best = &sv; }
        }
        return best;
    };

    const uint32_t stride = result.numProp;
    for (uint32_t i = 0; i < result.NumVert(); ++i) {
        EditableVertex v;
        v.position = Ogre::Vector3(result.vertProperties[i * stride + 0],
                                   result.vertProperties[i * stride + 1],
                                   result.vertProperties[i * stride + 2]);
        if (const EditableVertex* src = nearestSource(v.position)) {
            EditableVertex copy = *src;
            copy.position = v.position; // keep the boolean's exact position
            out.vertices.push_back(copy);
        } else {
            out.vertices.push_back(v);
        }
    }
    for (size_t i = 0; i + 2 < result.triVerts.size(); i += 3) {
        EditableTriangle t;
        t.indices[0] = result.triVerts[i + 0];
        t.indices[1] = result.triVerts[i + 1];
        t.indices[2] = result.triVerts[i + 2];
        out.triangles.push_back(t);
    }
    out.materialName = original.materialName;
}

// Cut real cylindrical socket cavities into `part` — one per peg center — via a
// robust mesh boolean. Each cutter is a cylinder of radius `r`, length `depth`,
// axis `-axis` (into the part), starting slightly proud of the seam so it fully
// overlaps the solid. Falls back to leaving `part` untouched if the boolean
// throws (degenerate input) — the male peg still guides assembly.
void subtractSockets(EditableSubMesh& part, const std::vector<Ogre::Vector3>& centers,
                     const Ogre::Vector3& axis, float r, float depth, int segments)
{
    if (centers.empty() || part.triangles.empty())
        return;
    try {
        manifold::Manifold solid = toManifold(part);
        if (solid.IsEmpty())
            return;
        const Ogre::Vector3 unit = axis.normalisedCopy();
        // Manifold::Cylinder is built along +Z from the origin; rotate/translate
        // each cutter so its +Z maps to -unit (into the part) starting proud of
        // the seam. We approximate the transform with Manifold's own helpers by
        // building the cylinder then applying a 4x4.
        for (const Ogre::Vector3& c : centers) {
            // A cylinder from the origin along +Z, height `depth`, radius r.
            manifold::Manifold cutter =
                manifold::Manifold::Cylinder(depth, r, r, segments, false);
            // Orient +Z -> -unit. Build a rotation matrix from basis vectors.
            const Ogre::Vector3 zdir = -unit;
            Ogre::Vector3 upv = std::fabs(zdir.y) < 0.9f ? Ogre::Vector3::UNIT_Y
                                                         : Ogre::Vector3::UNIT_X;
            Ogre::Vector3 xdir = upv.crossProduct(zdir).normalisedCopy();
            Ogre::Vector3 ydir = zdir.crossProduct(xdir).normalisedCopy();
            // Cutter starts barely proud of the seam (along +unit) so it fully
            // spans into the part along -unit.
            const Ogre::Vector3 base = c + unit * 0.001f;
            // Column-major 3x4 affine for Manifold::Transform (mat3x4).
            manifold::mat3x4 tf;
            tf[0] = manifold::vec3(xdir.x, xdir.y, xdir.z);
            tf[1] = manifold::vec3(ydir.x, ydir.y, ydir.z);
            tf[2] = manifold::vec3(zdir.x, zdir.y, zdir.z);
            tf[3] = manifold::vec3(base.x, base.y, base.z);
            cutter = cutter.Transform(tf);
            solid = solid - cutter;
        }
        if (solid.IsEmpty())
            return;
        EditableSubMesh cut;
        fromManifold(solid, part, cut);
        if (!cut.triangles.empty())
            part = std::move(cut);
    } catch (const std::exception&) {
        // Boolean failed on degenerate input — leave the part unchanged.
    }
}

} // namespace

SubMeshOps::PrintPrepResult
SubMeshOps::preparePrintPegs(const std::vector<EditableSubMesh>& subMeshes,
                             const PegOptions& opts, const std::vector<QString>& partNames)
{
    PrintPrepResult out;
    if (subMeshes.size() < 2) {
        out.error = QStringLiteral("need at least two parts to add alignment pegs");
        return out;
    }
    out.subMeshes = subMeshes;   // start from the parts; merge pegs in below.
    out.partNames = partNames;
    out.partNames.resize(subMeshes.size());

    // Each connector is merged directly INTO the part it belongs to (male peg →
    // its source part, female socket cavity + collar → the mating part) so every
    // part stays ONE self-contained printable mesh in its own material — no
    // separate connector submeshes.

    // Close each part's OPEN cut face first (a split leaves it hollow) so every
    // part is a watertight printable solid and the pegs attach to a real
    // surface. Boundary planes are still estimated from the ORIGINAL (uncapped)
    // submeshes below, so the coincident-seam detection is unaffected by the cap.
    for (auto& part : out.subMeshes)
        out.cappedParts += (capOpenBoundaries(part) > 0) ? 1 : 0;
    auto nameOf = [&](int i) -> QString {
        return (i >= 0 && i < static_cast<int>(out.partNames.size()) && !out.partNames[i].isEmpty())
                   ? out.partNames[i] : QStringLiteral("part%1").arg(i);
    };

    // For every unordered pair of parts, estimate the shared boundary; where it
    // is stable, build a male peg (→ partA) + socket (→ partB) and merge each
    // into its part as extra geometry. `estimateBoundaryPlane` works on submesh
    // VECTORS, so wrap each part in a one-element vector.
    const int n = static_cast<int>(subMeshes.size());
    for (int a = 0; a < n; ++a) {
        for (int b = a + 1; b < n; ++b) {
            PegBoundary rec;
            rec.partA = a; rec.partB = b;
            rec.nameA = nameOf(a); rec.nameB = nameOf(b);

            BoundaryPlane plane = estimateBoundaryPlane({ subMeshes[a] }, { subMeshes[b] });
            if (!plane.stable) {
                rec.reason = plane.reason.isEmpty()
                    ? QStringLiteral("no stable shared boundary") : plane.reason;
                out.boundaries.push_back(rec);
                continue;
            }
            // The best-fit normal's SIGN is arbitrary (an eigenvector), but the
            // male peg extrudes along +normal — so orient it from the MALE part
            // (A) toward the FEMALE part (B), using their body centroids, or the
            // peg would protrude into the wrong part (CodeRabbit/Codex).
            {
                Ogre::Vector3 cA = Ogre::Vector3::ZERO, cB = Ogre::Vector3::ZERO;
                size_t na = 0, nb = 0;
                for (const auto& sm : { subMeshes[a] }) for (const auto& v : sm.vertices) { cA += v.position; ++na; }
                for (const auto& sm : { subMeshes[b] }) for (const auto& v : sm.vertices) { cB += v.position; ++nb; }
                if (na && nb) {
                    cA /= float(na); cB /= float(nb);
                    if (plane.normal.dotProduct(cB - cA) < 0.0f)
                        plane.normal = -plane.normal;
                }
            }

            // Adapt the peg size to THIS boundary so it always fits, regardless
            // of the model's unit scale (the issue's fixed radius=1.5 is 80% of a
            // unit-normalised character's diagonal — a giant blob). A peg radius
            // is capped at 30% of the boundary ring radius, and the socket
            // clearance / peg depth scale down with it (keeping their ratios to
            // the user's request). The user's values are treated as an UPPER
            // bound — a big model with a big boundary keeps them as-is.
            PegOptions boundaryOpts = opts;
            const float maxPegR = 0.30f * plane.radius;
            if (maxPegR > 1e-4f && boundaryOpts.pegRadius > maxPegR) {
                const float scale = maxPegR / boundaryOpts.pegRadius;
                boundaryOpts.pegRadius = maxPegR;
                boundaryOpts.pegDepth *= scale;
                boundaryOpts.clearance *= scale;
            }

            // Bound the socket DEPTH so it never punches through the thinner of
            // the two mating parts. Measure each part's extent ALONG the peg axis
            // and cap depth at 35% of the smaller — otherwise a deep default peg
            // (pegDepth=4) bores clean through a thin torso/limb, showing as a
            // dark tunnel. The socket sinks pegDepth+clearance, so bound on that.
            {
                auto extentAlong = [&](int idx) {
                    float mn = 1e30f, mx = -1e30f;
                    for (const auto& v : subMeshes[idx].vertices) {
                        const float d = v.position.dotProduct(plane.normal);
                        mn = std::min(mn, d); mx = std::max(mx, d);
                    }
                    return (mx > mn) ? (mx - mn) : 0.0f;
                };
                const float thin = std::min(extentAlong(a), extentAlong(b));
                if (thin > 1e-4f) {
                    const float maxSink = 0.35f * thin;      // socket total sink
                    const float sink = boundaryOpts.pegDepth + boundaryOpts.clearance;
                    if (sink > maxSink) {
                        const float ds = maxSink / sink;
                        boundaryOpts.pegDepth *= ds;
                        boundaryOpts.clearance *= ds;
                    }
                }
            }

            // Keep the peg count modest — a single centered peg unless the
            // boundary ring is clearly big enough for a spaced pair/trio (each
            // extra peg is another pit in the part). This avoids the torso
            // sprouting three large sockets around one joint.
            {
                const float ringToPeg = boundaryOpts.pegRadius > 1e-5f
                    ? plane.radius / boundaryOpts.pegRadius : 0.0f;
                if (ringToPeg < 6.0f) boundaryOpts.maxPegsPerBoundary =
                    std::min(boundaryOpts.maxPegsPerBoundary, 1);
                else if (ringToPeg < 10.0f) boundaryOpts.maxPegsPerBoundary =
                    std::min(boundaryOpts.maxPegsPerBoundary, 2);
            }

            EditableSubMesh male, socketUnused;
            const int made = buildAlignmentPegs(plane, boundaryOpts, male, socketUnused);
            if (made <= 0) {
                rec.reason = QStringLiteral("boundary too small for a peg");
                out.boundaries.push_back(rec);
                continue;
            }
            // Merge the male peg directly INTO its source part (A) so each part
            // is one self-contained printable object that carries its own peg —
            // it renders in the part's own material, not a separate connector
            // submesh. On a skinned mesh the peg inherits part A's nearest bone
            // weights so it moves with that part (not the skeleton origin).
            inheritNearestBoneWeights(male, subMeshes[a]);
            appendGeometry(out.subMeshes[a], male);
            // Cut a real cylindrical SOCKET CAVITY into partB for each peg via a
            // robust mesh boolean (Manifold), so the male peg actually inserts —
            // not a solid cylinder added as fake geometry. The socket is the peg
            // + clearance, sunk slightly behind the seam so it fully overlaps B.
            const std::vector<Ogre::Vector3> pegCenters =
                pegRingCenters(plane, boundaryOpts, made);
            const float socketR = boundaryOpts.pegRadius + boundaryOpts.clearance;
            subtractSockets(out.subMeshes[b], pegCenters, plane.normal, socketR,
                            boundaryOpts.pegDepth + boundaryOpts.clearance,
                            boundaryOpts.radialSegments);
            // A shallow collar ring at each socket mouth (a raised lip around the
            // bore). Merged INTO part B so the female side is one solid too;
            // inherits part B's bone weights. Built into a temp first for that.
            EditableSubMesh collars;
            for (const Ogre::Vector3& pc : pegCenters)
                appendSocketCollar(collars, pc, plane.normal, socketR,
                                   boundaryOpts.radialSegments);
            inheritNearestBoneWeights(collars, subMeshes[b]);
            appendGeometry(out.subMeshes[b], collars);
            rec.pegged = true;
            rec.pegCount = made;
            out.boundaries.push_back(rec);
            ++out.peggedBoundaries;
            out.totalPegs += made;
        }
    }

    out.ok = true;
    if (out.peggedBoundaries == 0)
        out.error = QStringLiteral("no stable boundary found — no pegs added");
    return out;
}
