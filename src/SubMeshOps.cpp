#include "SubMeshOps.h"

#include "MeshSegmenter.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

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

    // One output submesh per accepted label. We accumulate into EditableSubMesh
    // copies that inherit the *first contributing* source submesh's material +
    // n-gon-ness; a vertex is duplicated into every group it touches so parts
    // are independent (the boundary-vertex-duplication requirement, #861).
    struct Builder {
        int label = 0;
        QString name;
        EditableSubMesh sub;
        bool sourceHadFaces = false;
        std::string sourceMaterial;
        // per source-submesh: source-local vertex -> new local index in `sub`
        std::vector<std::unordered_map<unsigned int, unsigned int>> remap;
    };

    // Deterministic builder order = accepted group order sorted by label.
    std::vector<int> orderedLabels;
    orderedLabels.reserve(accepted.size());
    for (const auto& kv : accepted)
        orderedLabels.push_back(kv.first);
    std::sort(orderedLabels.begin(), orderedLabels.end());

    std::unordered_map<int, size_t> builderOfLabel;
    std::vector<Builder> builders;
    builders.reserve(orderedLabels.size());
    for (int label : orderedLabels) {
        Builder b;
        b.label = label;
        b.name = accepted[label]->name;
        b.remap.resize(subMeshes.size());
        builderOfLabel[label] = builders.size();
        builders.push_back(std::move(b));
    }

    int duplicated = 0;
    // Track how many groups each (source submesh, source vertex) landed in, to
    // count boundary duplications (a vertex used by >1 group is duplicated).
    std::vector<std::unordered_map<unsigned int, int>> groupUseCount(subMeshes.size());

    for (uint32_t gtri = 0; gtri < faceLabels.size(); ++gtri) {
        const int label = faceLabels[gtri];
        auto bit = builderOfLabel.find(label);
        if (bit == builderOfLabel.end())
            continue; // excluded / no accepted group -> dropped

        size_t s = 0, localTri = 0;
        if (!globalTriToLocal(subMeshes, gtri, s, localTri))
            continue;
        const EditableSubMesh& src = subMeshes[s];
        const EditableTriangle& tri = src.triangles[localTri];

        Builder& b = builders[bit->second];
        if (b.sub.vertices.empty() && b.sub.triangles.empty()) {
            b.sourceMaterial = src.materialName;
            b.sub.materialName =
                opts.assignPartMaterials
                    ? (opts.namePrefix.isEmpty()
                           ? b.name.toStdString()
                           : (opts.namePrefix + QStringLiteral(".") + b.name).toStdString())
                    : src.materialName;
        }
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
                // more group. The 2nd+ group to claim it is a duplication.
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

    // Emit submeshes; rebuild n-gon faces where the source had them (promote
    // the triangle soup back to trivial faces — the exporter/edit layer expects
    // `faces` canonical when non-empty). Optional connected-component split.
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

        int pieceIdx = 0;
        for (EditableSubMesh& piece : pieces) {
            if (piece.triangles.empty())
                continue;
            if (b.sourceHadFaces)
                promoteTrianglesToFaces(piece); // keep n-gon storage canonical
            QString partName = b.name;
            if (pieces.size() > 1 && pieceIdx > 0)
                partName += QStringLiteral(".%1").arg(pieceIdx);
            result.subMeshes.push_back(std::move(piece));
            result.partNames.push_back(partName);
            ++pieceIdx;
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
                    v.tangent = Ogre::Vector4(t3.x, t3.y, t3.z, sv.tangent.w);
                }
                dst.vertices.push_back(v);
            }
            for (const EditableTriangle& t : src.triangles) {
                EditableTriangle nt;
                nt.indices[0] = t.indices[0] + base;
                nt.indices[1] = t.indices[1] + base;
                nt.indices[2] = t.indices[2] + base;
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
        const float a = 2.0f * float(M_PI) * float(i) / float(segments);
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
            const float a = 2.0f * float(M_PI) * float(i) / float(nPegs);
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
