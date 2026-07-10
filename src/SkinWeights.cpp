#include "SkinWeights.h"
#include "GeodesicVoxelBind.h"
#include "SkinTokensPredictor.h"
#include "SkinWeightsPost.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSubMesh.h>
#include <OgreSkeleton.h>
#include <OgreBone.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

namespace {

// ─── Geometry helpers ───────────────────────────────────────────────────────

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    double lengthSq() const { return x * x + y * y + z * z; }
};

// Distance from point P to segment AB. If A==B, falls through to
// point-point distance. Returns squared distance to avoid an
// unnecessary sqrt — comparisons and inverse-weighting work fine
// against the squared value (we apply `pow(d, -falloff/2)` at the
// inverse step).
double distSqPointSegment(const Vec3& p, const Vec3& a, const Vec3& b)
{
    const Vec3 ab = b - a;
    const double abLenSq = ab.lengthSq();
    if (abLenSq < 1e-18) {
        // A == B (leaf or single-point bone) → point distance.
        return (p - a).lengthSq();
    }
    const Vec3 ap = p - a;
    const double t = std::clamp(ap.dot(ab) / abLenSq, 0.0, 1.0);
    const Vec3 closest = { a.x + t * ab.x, a.y + t * ab.y, a.z + t * ab.z };
    return (p - closest).lengthSq();
}

// ─── Top-K-with-min-heap helper ─────────────────────────────────────────────

// Push (boneIdx, weight) into the per-vertex top-K array,
// maintaining a sorted-descending order so the smallest weight is
// always at index `K-1`. This is O(K) per push but K is tiny
// (typically 4) — overall O(V·B·K) which is fine for asset-time
// processing.
void pushTopK(SkinWeights::VertexWeights& vw, int maxK,
              int boneIdx, double weight)
{
    if (vw.count < maxK) {
        // Slot available — insert in sorted position.
        int i = vw.count;
        while (i > 0 && vw.weights[i - 1] < weight) {
            vw.weights[i]     = vw.weights[i - 1];
            vw.boneIndices[i] = vw.boneIndices[i - 1];
            --i;
        }
        vw.weights[i]     = weight;
        vw.boneIndices[i] = boneIdx;
        ++vw.count;
        return;
    }
    // Array full — replace smallest if our new weight beats it.
    if (weight <= vw.weights[maxK - 1]) return;
    int i = maxK - 1;
    while (i > 0 && vw.weights[i - 1] < weight) {
        vw.weights[i]     = vw.weights[i - 1];
        vw.boneIndices[i] = vw.boneIndices[i - 1];
        --i;
    }
    vw.weights[i]     = weight;
    vw.boneIndices[i] = boneIdx;
}

} // namespace

// ─── Public API ────────────────────────────────────────────────────────────

bool SkinWeights::computeWeights(const float* vertexPositions,
                                  int vertexCount,
                                  const std::vector<BoneSegment>& bones,
                                  const SkinWeightsOptions& opts,
                                  std::vector<VertexWeights>& outWeights)
{
    outWeights.clear();
    if (!vertexPositions || vertexCount < 1 || bones.empty()) return false;

    const int maxK = std::clamp(opts.maxInfluencesPerVertex, 1, 8);
    const double falloffOver2 = std::max(0.5, opts.falloff) * 0.5;

    // Compute the bounding box for the distance-cap calculation.
    double minX = std::numeric_limits<double>::max(), minY = minX, minZ = minX;
    double maxX = -minX, maxY = -minX, maxZ = -minX;
    for (int i = 0; i < vertexCount; ++i) {
        const float* p = vertexPositions + 3 * i;
        minX = std::min<double>(minX, p[0]); maxX = std::max<double>(maxX, p[0]);
        minY = std::min<double>(minY, p[1]); maxY = std::max<double>(maxY, p[1]);
        minZ = std::min<double>(minZ, p[2]); maxZ = std::max<double>(maxZ, p[2]);
    }
    const double diag = std::sqrt((maxX - minX) * (maxX - minX) +
                                   (maxY - minY) * (maxY - minY) +
                                   (maxZ - minZ) * (maxZ - minZ));
    const double maxDistSq = (opts.maxInfluenceDistance > 0)
        ? std::pow(opts.maxInfluenceDistance * diag, 2.0)
        : std::numeric_limits<double>::infinity();

    outWeights.resize(vertexCount);

    for (int v = 0; v < vertexCount; ++v) {
        const float* p = vertexPositions + 3 * v;
        const Vec3 pos { p[0], p[1], p[2] };
        VertexWeights& vw = outWeights[v];

        for (size_t b = 0; b < bones.size(); ++b) {
            const auto& seg = bones[b];
            const Vec3 head { seg.headX, seg.headY, seg.headZ };
            const Vec3 tail { seg.tailX, seg.tailY, seg.tailZ };
            const double distSq = distSqPointSegment(pos, head, tail);
            if (distSq > maxDistSq) continue;
            // Inverse-distance weight: w_b = 1 / (distSq^(falloff/2))
            // == 1 / d^falloff. Add a tiny epsilon to dist to avoid
            // a divide-by-zero on vertices that sit exactly on a
            // bone segment.
            const double w = 1.0 / std::pow(distSq + 1e-12, falloffOver2);
            pushTopK(vw, maxK, static_cast<int>(b), w);
        }

        // Normalize so the kept weights sum to 1.
        double sum = 0.0;
        for (int i = 0; i < vw.count; ++i) sum += vw.weights[i];
        if (sum > 0.0) {
            for (int i = 0; i < vw.count; ++i) vw.weights[i] /= sum;
        } else if (!bones.empty()) {
            // Vertex outside every bone's influence radius — pin to
            // bone 0 with weight 1.0 so it still moves with the rig.
            vw.boneIndices[0] = 0;
            vw.weights[0]     = 1.0;
            vw.count          = 1;
        }
    }

    return true;
}

bool SkinWeights::computeWeights(const float* vertexPositions,
                                  int vertexCount,
                                  const std::uint32_t* indices,
                                  std::size_t indexCount,
                                  const std::vector<BoneSegment>& bones,
                                  const SkinWeightsOptions& opts,
                                  Algorithm algo,
                                  std::vector<VertexWeights>& outWeights,
                                  ComputeInfo* info,
                                  const SkeletonHierarchy* hierarchy)
{
    outWeights.clear();
    ComputeInfo localInfo;
    ComputeInfo& inf = info ? *info : localInfo;
    inf = {};
    if (!vertexPositions || vertexCount < 1 || bones.empty()) return false;

    if (algo == Algorithm::UniRigML) {
        // ML path: SkinTokens (see THIRD_PARTY_AI_MODELS.md — it
        // replaced UniRig's spconv-blocked skin head). Needs the
        // joint hierarchy for tokenization, an ONNX build, and the
        // downloaded models; anything missing falls back to
        // geodesic-voxel, its designed fallback.
        QString mlWhy;
        if (!SkinTokensPredictor::isAvailable()) {
            mlWhy = QStringLiteral("built without ONNX");
        } else if (!hierarchy || hierarchy->nodes.size() != bones.size()) {
            mlWhy = QStringLiteral("no skeleton hierarchy available");
        } else if (!indices || indexCount < 3) {
            mlWhy = QStringLiteral("no triangle indices");
        } else if (SkinTokensPredictor::ensureModelBlocking().isEmpty()) {
            mlWhy = QStringLiteral("SkinTokens models unavailable");
        } else {
            std::vector<SkinTokensPredictor::Joint> joints;
            joints.reserve(hierarchy->nodes.size());
            for (const auto& n : hierarchy->nodes) {
                SkinTokensPredictor::Joint j;
                j.pos = { n.x, n.y, n.z };
                j.parent = n.parent;
                joints.push_back(j);
            }
            SkinTokensPredictor::Options mlOpts;
            mlOpts.maxInfluencesPerVertex = opts.maxInfluencesPerVertex;
            const auto ml = SkinTokensPredictor::predict(
                vertexPositions, vertexCount, indices, indexCount,
                joints, mlOpts);
            if (ml.ok && int(ml.weights.size()) == vertexCount) {
                outWeights.resize(std::size_t(vertexCount));
                for (int v = 0; v < vertexCount; ++v) {
                    const auto& src = ml.weights[std::size_t(v)];
                    VertexWeights& dst = outWeights[std::size_t(v)];
                    dst.count = std::min<int>(src.count, 8);
                    for (int k = 0; k < dst.count; ++k) {
                        dst.boneIndices[k] = src.jointIndices[k];
                        dst.weights[k]     = src.weights[k];
                    }
                }
                // Geodesic localisation pass: SkinTokens' RAW weights
                // are diffuse — the upstream demo post-processes them
                // with a voxel-visibility mask by default. Our
                // geodesic field is the stronger version of the same
                // idea: keep only geodesically-local bones per vertex
                // and renormalise; vertices left empty (or meshes with
                // no volume) take the geodesic weights instead.
                std::vector<VertexWeights> gvb;
                std::vector<std::vector<int>> allowed;
                const GeodesicVoxelBind::Result gres =
                    GeodesicVoxelBind::compute(
                        vertexPositions, vertexCount, indices, indexCount,
                        bones, opts, gvb, &allowed);
                if (gres.ok && int(allowed.size()) == vertexCount) {
                    for (int v = 0; v < vertexCount; ++v) {
                        VertexWeights& vw = outWeights[std::size_t(v)];
                        const auto& ok = allowed[std::size_t(v)];
                        VertexWeights kept;
                        double sum = 0.0;
                        for (int k = 0; k < vw.count; ++k) {
                            if (std::find(ok.begin(), ok.end(),
                                          vw.boneIndices[k]) == ok.end())
                                continue;
                            kept.boneIndices[kept.count] = vw.boneIndices[k];
                            kept.weights[kept.count]     = vw.weights[k];
                            sum += vw.weights[k];
                            ++kept.count;
                        }
                        if (kept.count > 0 && sum > 0.0) {
                            for (int k = 0; k < kept.count; ++k)
                                kept.weights[k] /= sum;
                            vw = kept;
                        } else if (std::size_t(v) < gvb.size()
                                   && gvb[std::size_t(v)].count > 0) {
                            vw = gvb[std::size_t(v)];
                        }
                    }
                    inf.allowedBones = std::move(allowed);
                }
                inf.algorithmUsed = QStringLiteral("skintokens");
                return true;
            }
            mlWhy = ml.error.isEmpty()
                ? QStringLiteral("prediction failed") : ml.error;
        }
        inf.fallbackReason = QStringLiteral(
            "SkinTokens ML skinning unavailable (%1) — used geodesic-voxel")
            .arg(mlWhy);
        algo = Algorithm::GeodesicVoxel;
    }

    if (algo == Algorithm::GeodesicVoxel) {
        std::vector<VertexWeights> gvb;
        const GeodesicVoxelBind::Result r = GeodesicVoxelBind::compute(
            vertexPositions, vertexCount, indices, indexCount,
            bones, opts, gvb, &inf.allowedBones);
        if (r.ok) {
            inf.algorithmUsed     = QStringLiteral("geodesic-voxel");
            inf.bonesWithoutSeeds = r.bonesWithoutSeeds;
            if (r.verticesWithoutGeodesicWeights > 0) {
                // Vertices unreachable from every bone seed (floating
                // island with no bone inside): fill them with
                // inverse-distance weights so they still move with
                // the rig instead of staying pinned in place.
                std::vector<VertexWeights> id;
                if (computeWeights(vertexPositions, vertexCount,
                                   bones, opts, id)) {
                    for (int v = 0; v < vertexCount; ++v)
                        if (gvb[v].count == 0) gvb[v] = id[v];
                }
            }
            outWeights = std::move(gvb);
            return true;
        }
        if (!inf.fallbackReason.isEmpty())
            inf.fallbackReason += QStringLiteral("; ");
        inf.fallbackReason += QStringLiteral(
            "geodesic-voxel unavailable (%1) — used inverse-distance")
            .arg(r.error);
        inf.allowedBones.clear();
    }

    inf.algorithmUsed = QStringLiteral("inverse-distance");
    return computeWeights(vertexPositions, vertexCount, bones, opts,
                          outWeights);
}

namespace {

// Walk every submesh of the entity, gather flat vertex positions
// transformed into the entity-local space the skeleton is
// expressed in, build the bone segment list from the skeleton's
// bind pose, run `computeWeights`, then commit the result via
// `SubMesh::addBoneAssignment`/`_compileBoneAssignments`.
SkinWeightsReport applyToEntity(Ogre::Entity* entity,
                                 const SkinWeightsOptions& opts,
                                 SkinWeights::Algorithm algo)
{
    SkinWeightsReport report;
    if (!entity) { report.error = QStringLiteral("null entity"); return report; }
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) { report.error = QStringLiteral("entity has no mesh"); return report; }
    Ogre::Skeleton* skel = mesh->getSkeleton().get();
    if (!skel) {
        report.error = QStringLiteral("mesh has no skeleton attached");
        return report;
    }

    report.meshName     = QString::fromStdString(mesh->getName());
    report.skeletonName = QString::fromStdString(skel->getName());

    // Build the bone-segment list in bind pose. The bind pose is
    // Ogre's "initial state" — `Bone::setBindingPose` was called
    // by the mesh loader and `Bone::reset()` returns to it. We
    // call reset() temporarily so `_getDerivedPosition` returns
    // the bind-pose world position rather than whatever animation
    // frame is currently active.
    skel->reset(true);

    const unsigned short numBones = skel->getNumBones();
    report.totalBones = numBones;

    // Optionally collect the set of already-weighted bones to
    // filter helper bones (Mixamo's `mixamorig:HeadTop_End` etc.
    // ship with zero weights). Build the set by scanning every
    // submesh's existing bone assignments.
    std::vector<char> boneInUse(numBones, 0);
    if (opts.skipUnweightedBones) {
        for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
            const auto& ba = mesh->getSubMesh(si)->getBoneAssignments();
            for (const auto& kv : ba)
                if (kv.second.boneIndex < numBones)
                    boneInUse[kv.second.boneIndex] = 1;
        }
        // Also consider mesh-level (shared) bone assignments.
        for (const auto& kv : mesh->getBoneAssignments())
            if (kv.second.boneIndex < numBones)
                boneInUse[kv.second.boneIndex] = 1;
    }

    std::vector<SkinWeights::BoneSegment> bones;
    bones.reserve(numBones);
    // Map from `bones[]` index → Ogre bone handle. When skipping
    // unweighted bones the lists are sparse, so we need to
    // translate back at commit time.
    std::vector<unsigned short> boneIdxToHandle;
    boneIdxToHandle.reserve(numBones);
    for (unsigned short bi = 0; bi < numBones; ++bi) {
        if (opts.skipUnweightedBones && !boneInUse[bi]) continue;
        Ogre::Bone* bone = skel->getBone(bi);
        if (!bone) continue;
        const Ogre::Vector3 head = bone->_getDerivedPosition();
        // Use the average child position as the "tail" — gives the
        // bone a real segment for distance computation. Leaf bones
        // fall back to head==tail (point distance).
        Ogre::Vector3 tail = head;
        const unsigned short numChildren = bone->numChildren();
        if (numChildren > 0) {
            Ogre::Vector3 sum(0, 0, 0);
            int kept = 0;
            for (unsigned short c = 0; c < numChildren; ++c) {
                auto* child = dynamic_cast<Ogre::Bone*>(bone->getChild(c));
                if (!child) continue;
                sum += child->_getDerivedPosition();
                ++kept;
            }
            if (kept > 0) tail = sum / static_cast<Ogre::Real>(kept);
        }
        SkinWeights::BoneSegment seg;
        seg.headX = head.x; seg.headY = head.y; seg.headZ = head.z;
        seg.tailX = tail.x; seg.tailY = tail.y; seg.tailZ = tail.z;
        bones.push_back(seg);
        boneIdxToHandle.push_back(bi);
    }

    if (bones.empty()) {
        report.error = QStringLiteral("skeleton has no usable bones");
        return report;
    }

    const size_t maxK = static_cast<size_t>(
        std::clamp(opts.maxInfluencesPerVertex, 1, 8));

    // Reverse map: Ogre bone handle → `bones[]` index (for merge-
    // mode constraint rows and bones-without-seeds names).
    std::vector<int> handleToIdx(numBones, -1);
    for (size_t i = 0; i < boneIdxToHandle.size(); ++i)
        handleToIdx[boneIdxToHandle[i]] = static_cast<int>(i);

    // Joint hierarchy for the ML (SkinTokens) path — aligned with
    // bones[]. Parent = the nearest ANCESTOR that survived the
    // skipUnweightedBones filter. Ogre bone handles are creation-
    // ordered (parent-first for every importer we ship), which is
    // exactly the parent-before-child ordering the tokenizer needs;
    // if an exotic skeleton violates it, the predictor rejects and
    // the geodesic fallback runs.
    SkinWeights::SkeletonHierarchy hierarchy;
    if (algo == SkinWeights::Algorithm::UniRigML) {
        hierarchy.nodes.reserve(bones.size());
        for (size_t i = 0; i < boneIdxToHandle.size(); ++i) {
            Ogre::Bone* bone = skel->getBone(boneIdxToHandle[i]);
            SkinWeights::SkeletonHierarchy::Node n;
            n.x = bones[i].headX;
            n.y = bones[i].headY;
            n.z = bones[i].headZ;
            n.parent = -1;
            const Ogre::Node* p = bone ? bone->getParent() : nullptr;
            while (p) {
                const auto* pb = dynamic_cast<const Ogre::Bone*>(p);
                if (pb && pb->getHandle() < numBones
                    && handleToIdx[pb->getHandle()] >= 0) {
                    n.parent = handleToIdx[pb->getHandle()];
                    break;
                }
                p = p->getParent();
            }
            hierarchy.nodes.push_back(n);
        }
    }

    // Helper: append one owner's index data (16- or 32-bit) as flat
    // uint32 triangle indices, offset by the owner's base vertex in
    // the combined arrays. GeodesicVoxel needs the surface; the
    // Slice-B smoothing needs the adjacency.
    auto appendIndices = [](Ogre::IndexData* id,
                            std::uint32_t vertexOffset,
                            std::vector<std::uint32_t>& out) {
        if (!id || !id->indexBuffer || id->indexCount == 0) return;
        auto ibuf = id->indexBuffer;
        const auto* base = static_cast<const unsigned char*>(
            ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        base += id->indexStart * ibuf->getIndexSize();
        out.reserve(out.size() + id->indexCount);
        if (ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT) {
            const auto* p = reinterpret_cast<const std::uint32_t*>(base);
            for (size_t i = 0; i < id->indexCount; ++i)
                out.push_back(p[i] + vertexOffset);
        } else {
            const auto* p = reinterpret_cast<const std::uint16_t*>(base);
            for (size_t i = 0; i < id->indexCount; ++i)
                out.push_back(p[i] + vertexOffset);
        }
        ibuf->unlock();
    };

    // Helper: read tight xyz floats out of a VertexData's POSITION
    // element. Returns false if the buffer is unusable.
    auto extractPositions = [](Ogre::VertexData* vd,
                               std::vector<float>& out) -> bool {
        const auto* posElem = vd->vertexDeclaration->findElementBySemantic(
            Ogre::VES_POSITION);
        if (!posElem) return false;
        auto vbuf = vd->vertexBufferBinding->getBuffer(posElem->getSource());
        if (!vbuf || vd->vertexCount == 0) return false;
        out.resize(static_cast<size_t>(vd->vertexCount) * 3);
        const size_t stride = vbuf->getVertexSize();
        auto* base = static_cast<unsigned char*>(
            vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (size_t i = 0; i < vd->vertexCount; ++i) {
            float* p = nullptr;
            posElem->baseVertexPointerToElement(base + i * stride, &p);
            out[3 * i + 0] = p[0];
            out[3 * i + 1] = p[1];
            out[3 * i + 2] = p[2];
        }
        vbuf->unlock();
        return true;
    };

    // ── Collect every assignment owner into ONE combined vertex/
    // index set. The whole mesh is computed in a single pass — the
    // paper voxelizes the whole character, so accessories (hats,
    // props) bind through the body's solid, the bone field is
    // seeded once, and per-submesh grids can't strand bones outside
    // their AABB. Ogre stores shared-vertex bone assignments on the
    // Mesh, not the SubMesh — the FBX/glTF exporters read
    // `Mesh::getBoneAssignments()` for shared geometry, so routing
    // them to the submesh list would leave the export with stale /
    // missing weights (Codex review on PR #699). `mesh-local space`
    // matches the skeleton's bind-pose space for the same entity,
    // so no transform is applied to the positions here.
    struct Owner {
        Ogre::VertexData* vd = nullptr;
        int submeshIndex = 0;             // -1 == mesh-level shared data
        std::uint32_t baseVertex = 0;     // offset in the combined arrays
        Ogre::Mesh::VertexBoneAssignmentList existing;
        std::function<void()> clearFn;
        std::function<void(const Ogre::VertexBoneAssignment&)> addFn;
        std::function<void()> compileFn;
        std::function<int()>  countFn;
    };
    std::vector<Owner> owners;
    std::vector<float>         positions;   // combined xyz
    std::vector<std::uint32_t> indices;     // combined, owner-offset

    if (mesh->sharedVertexData) {
        bool anySubUsesShared = false;
        for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
            if (mesh->getSubMesh(si) && mesh->getSubMesh(si)->useSharedVertices) {
                anySubUsesShared = true;
                break;
            }
        }
        std::vector<float> ownerPositions;
        if (anySubUsesShared
            && extractPositions(mesh->sharedVertexData, ownerPositions)) {
            Owner o;
            o.vd           = mesh->sharedVertexData;
            o.submeshIndex = -1;
            o.baseVertex   = std::uint32_t(positions.size() / 3);
            o.existing     = mesh->getBoneAssignments();
            o.clearFn      = [mesh]() { mesh->clearBoneAssignments(); };
            o.addFn        = [mesh](const Ogre::VertexBoneAssignment& vba) {
                mesh->addBoneAssignment(vba);
            };
            o.compileFn    = [mesh]() { mesh->_compileBoneAssignments(); };
            o.countFn      = [mesh]() {
                return static_cast<int>(mesh->getBoneAssignments().size());
            };
            // Shared geometry: the surface is the union of every
            // shared-vertex submesh's triangles.
            for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
                Ogre::SubMesh* sub = mesh->getSubMesh(si);
                if (sub && sub->useSharedVertices)
                    appendIndices(sub->indexData, o.baseVertex, indices);
            }
            positions.insert(positions.end(), ownerPositions.begin(),
                             ownerPositions.end());
            owners.push_back(std::move(o));
        }
    }

    const unsigned short numSubs = mesh->getNumSubMeshes();
    for (unsigned short si = 0; si < numSubs; ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub) continue;
        if (sub->useSharedVertices) continue;  // handled above
        if (!sub->vertexData) continue;
        std::vector<float> ownerPositions;
        if (!extractPositions(sub->vertexData, ownerPositions)) continue;
        Owner o;
        o.vd           = sub->vertexData;
        o.submeshIndex = si;
        o.baseVertex   = std::uint32_t(positions.size() / 3);
        o.existing     = sub->getBoneAssignments();
        o.clearFn      = [sub]() { sub->clearBoneAssignments(); };
        o.addFn        = [sub](const Ogre::VertexBoneAssignment& vba) {
            sub->addBoneAssignment(vba);
        };
        o.compileFn    = [sub]() { sub->_compileBoneAssignments(); };
        o.countFn      = [sub]() {
            return static_cast<int>(sub->getBoneAssignments().size());
        };
        appendIndices(sub->indexData, o.baseVertex, indices);
        positions.insert(positions.end(), ownerPositions.begin(),
                         ownerPositions.end());
        owners.push_back(std::move(o));
    }

    if (owners.empty()) {
        report.error = QStringLiteral("mesh has no readable vertex data");
        return report;
    }
    const int totalVerts = static_cast<int>(positions.size() / 3);

    // ── One compute over the whole mesh ────────────────────────────
    std::vector<SkinWeights::VertexWeights> weights;
    SkinWeights::ComputeInfo info;
    if (!SkinWeights::computeWeights(positions.data(), totalVerts,
                                      indices.empty() ? nullptr : indices.data(),
                                      indices.size(),
                                      bones, opts, algo, weights, &info,
                                      hierarchy.nodes.empty() ? nullptr
                                                              : &hierarchy)) {
        report.error = QStringLiteral("weight computation failed");
        return report;
    }

    report.algorithmUsed  = info.algorithmUsed;
    report.fallbackReason = info.fallbackReason;
    for (const int b : info.bonesWithoutSeeds) {
        if (b < 0 || static_cast<size_t>(b) >= boneIdxToHandle.size())
            continue;
        Ogre::Bone* bone = skel->getBone(boneIdxToHandle[b]);
        if (!bone) continue;
        const QString name = QString::fromStdString(bone->getName());
        if (!report.bonesWithoutSeeds.contains(name))
            report.bonesWithoutSeeds.push_back(name);
    }

    // Merge mode (`replaceExisting=false`): keep existing weights
    // and only fill vertices that have NONE. Locked vertices double
    // as Dirichlet constraints for the Slice-B smoothing — seed
    // their rows from the existing assignments (translated to
    // `bones[]` index space) so manual weights shape the blend at
    // the merge boundary.
    std::vector<std::uint8_t> locked;
    if (!opts.replaceExisting) {
        locked.assign(weights.size(), 0);
        for (const Owner& o : owners) {
            for (const auto& kv : o.existing) {
                const size_t v = o.baseVertex + kv.second.vertexIndex;
                if (v >= weights.size()) continue;
                if (!locked[v]) {
                    locked[v]  = 1;
                    weights[v] = {};
                }
                const int bi = (kv.second.boneIndex < numBones)
                    ? handleToIdx[kv.second.boneIndex] : -1;
                if (bi >= 0 && weights[v].count < 8) {
                    weights[v].boneIndices[weights[v].count] = bi;
                    weights[v].weights[weights[v].count] = kv.second.weight;
                    ++weights[v].count;
                }
            }
        }
    }

    // ── Slice-B post-passes ────────────────────────────────────────
    if (opts.smoothIterations > 0 && !indices.empty()) {
        const auto adjacency = SkinWeightsPost::buildAdjacency(
            totalVerts, indices.data(), indices.size());
        SkinWeightsPost::laplacianSmooth(
            weights, adjacency, opts.smoothIterations, locked);
    }
    SkinWeightsPost::pruneAndRenormalize(weights, opts.maxInfluencesPerVertex);

    // Bleed metric (report only) — needs the geodesic field.
    if (!info.allowedBones.empty()) {
        const double f = SkinWeightsPost::bleedFraction(weights,
                                                        info.allowedBones);
        if (f >= 0.0) report.bleedFraction = f;
    }

    // ── Commit per owner ───────────────────────────────────────────
    for (const Owner& o : owners) {
        SkinWeightsSubmeshReport subReport;
        subReport.submeshIndex          = o.submeshIndex;
        subReport.boneAssignmentsBefore = static_cast<int>(o.existing.size());

        if (opts.replaceExisting) o.clearFn();
        const size_t ownerVerts = o.vd->vertexCount;
        for (size_t v = 0; v < ownerVerts; ++v) {
            const size_t gv = o.baseVertex + v;
            if (gv >= weights.size()) break;
            // In merge mode, skip vertices that already had weights —
            // don't append a second normalized set on top of theirs.
            if (gv < locked.size() && locked[gv])
                continue;
            const auto& vw = weights[gv];
            if (static_cast<size_t>(vw.count) == maxK)
                ++subReport.verticesWithMaxInfluences;
            for (int k = 0; k < vw.count; ++k) {
                Ogre::VertexBoneAssignment vba;
                vba.vertexIndex = static_cast<unsigned int>(v);
                vba.boneIndex   = boneIdxToHandle[vw.boneIndices[k]];
                vba.weight      = static_cast<float>(vw.weights[k]);
                o.addFn(vba);
            }
        }
        o.compileFn();
        subReport.verticesProcessed    = static_cast<int>(ownerVerts);
        subReport.boneAssignmentsAfter = o.countFn();
        report.submeshes.push_back(subReport);
        report.totalVerticesProcessed += subReport.verticesProcessed;
        report.totalAssignmentsBefore += subReport.boneAssignmentsBefore;
        report.totalAssignmentsAfter  += subReport.boneAssignmentsAfter;
    }

    report.applied = true;
    return report;
}

} // namespace

SkinWeightsReport SkinWeights::computeAndApply(Ogre::Entity* entity,
                                                const SkinWeightsOptions& opts,
                                                Algorithm algo)
{
    return applyToEntity(entity, opts, algo);
}

QJsonObject SkinWeights::reportToJson(const SkinWeightsReport& report)
{
    QJsonObject root;
    root["meshName"]                = report.meshName;
    root["skeletonName"]            = report.skeletonName;
    root["applied"]                 = report.applied;
    root["totalBones"]              = report.totalBones;
    root["totalVerticesProcessed"]  = report.totalVerticesProcessed;
    root["totalAssignmentsBefore"]  = report.totalAssignmentsBefore;
    root["totalAssignmentsAfter"]   = report.totalAssignmentsAfter;
    if (!report.error.isEmpty()) root["error"] = report.error;
    if (!report.algorithmUsed.isEmpty())
        root["algorithmUsed"] = report.algorithmUsed;
    if (!report.fallbackReason.isEmpty())
        root["fallbackReason"] = report.fallbackReason;
    if (report.bleedFraction >= 0.0)
        root["bleedFraction"] = report.bleedFraction;
    if (!report.bonesWithoutSeeds.isEmpty())
        root["bonesWithoutSeeds"] = QJsonArray::fromStringList(report.bonesWithoutSeeds);

    QJsonArray subs;
    for (const auto& s : report.submeshes) {
        QJsonObject obj;
        obj["submeshIndex"]              = s.submeshIndex;
        obj["verticesProcessed"]         = s.verticesProcessed;
        obj["boneAssignmentsBefore"]     = s.boneAssignmentsBefore;
        obj["boneAssignmentsAfter"]      = s.boneAssignmentsAfter;
        obj["verticesWithMaxInfluences"] = s.verticesWithMaxInfluences;
        subs.push_back(obj);
    }
    root["submeshes"] = subs;
    return root;
}

QString SkinWeights::reportToText(const SkinWeightsReport& report)
{
    QString out;
    QTextStream s(&out);
    s << "Skin Weights\n";
    s << "============\n";
    s << "Mesh:               " << report.meshName << "\n";
    s << "Skeleton:           " << report.skeletonName << "\n";
    s << "Bones:              " << report.totalBones << "\n";
    s << "Vertices processed: " << report.totalVerticesProcessed << "\n";
    s << "Assignments:        " << report.totalAssignmentsBefore
      << " → " << report.totalAssignmentsAfter << "\n";
    if (!report.algorithmUsed.isEmpty())
        s << "Algorithm:          " << report.algorithmUsed << "\n";
    if (!report.fallbackReason.isEmpty())
        s << "Fallback:           " << report.fallbackReason << "\n";
    if (report.bleedFraction >= 0.0)
        s << "Bleed fraction:     "
          << QString::number(report.bleedFraction, 'f', 4) << "\n";
    if (!report.bonesWithoutSeeds.isEmpty())
        s << "Bones w/o seeds:    "
          << report.bonesWithoutSeeds.join(QStringLiteral(", ")) << "\n";
    if (!report.error.isEmpty()) s << "Error: " << report.error << "\n";
    return out;
}

QString SkinWeights::algorithmToString(Algorithm algo)
{
    switch (algo) {
    case Algorithm::InverseDistance: return QStringLiteral("inverse-distance");
    case Algorithm::GeodesicVoxel:   return QStringLiteral("geodesic-voxel");
    case Algorithm::UniRigML:        return QStringLiteral("unirig");
    }
    return QStringLiteral("geodesic-voxel");
}

SkinWeights::Algorithm SkinWeights::algorithmFromString(const QString& s)
{
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("inverse-distance")
        || v == QLatin1String("inverse_distance")
        || v == QLatin1String("id"))
        return Algorithm::InverseDistance;
    if (v == QLatin1String("unirig") || v == QLatin1String("unirig-ml")
        || v == QLatin1String("unirigml"))
        return Algorithm::UniRigML;
    // "geodesic-voxel" / "geodesic" / "gvb" / anything else → the
    // default.
    return Algorithm::GeodesicVoxel;
}
