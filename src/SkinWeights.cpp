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
                                  const SkeletonHierarchy* hierarchy,
                                  const ProgressFn& progress)
{
    outWeights.clear();
    ComputeInfo localInfo;
    ComputeInfo& inf = info ? *info : localInfo;
    inf = {};
    if (!vertexPositions || vertexCount < 1 || bones.empty()) return false;

    if (algo == Algorithm::SkinTokens) {
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
            SkinTokensPredictor::ProgressFn mlProgress;
            if (progress)
                mlProgress = [&progress](int done, int total) {
                    return progress(done, total);
                };
            const auto ml = SkinTokensPredictor::predict(
                vertexPositions, vertexCount, indices, indexCount,
                joints, mlOpts, mlProgress);
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


// ─── Threaded pipeline: prepare (main) → run (worker) → commit (main) ───────

bool SkinWeights::prepareJob(Ogre::Entity* entity,
                             const SkinWeightsOptions& opts,
                             Algorithm algo,
                             ComputeJob& out,
                             QString* error)
{
    auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        return false;
    };
    out = {};
    if (!entity) return fail(QStringLiteral("null entity"));
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return fail(QStringLiteral("entity has no mesh"));
    Ogre::Skeleton* skel = mesh->getSkeleton().get();
    if (!skel) return fail(QStringLiteral("mesh has no skeleton attached"));

    out.meshName     = QString::fromStdString(mesh->getName());
    out.skeletonName = QString::fromStdString(skel->getName());

    // Bind pose: Ogre's "initial state" — reset so _getDerivedPosition
    // returns bind-pose positions, not the current animation frame.
    skel->reset(true);

    const unsigned short numBones = skel->getNumBones();
    out.numBones = numBones;

    // Optionally collect the set of already-weighted bones to filter
    // helper bones (Mixamo's `mixamorig:HeadTop_End` etc.).
    std::vector<char> boneInUse(numBones, 0);
    if (opts.skipUnweightedBones) {
        for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
            const auto& ba = mesh->getSubMesh(si)->getBoneAssignments();
            for (const auto& kv : ba)
                if (kv.second.boneIndex < numBones)
                    boneInUse[kv.second.boneIndex] = 1;
        }
        for (const auto& kv : mesh->getBoneAssignments())
            if (kv.second.boneIndex < numBones)
                boneInUse[kv.second.boneIndex] = 1;
    }

    out.bones.reserve(numBones);
    out.boneIdxToHandle.reserve(numBones);
    for (unsigned short bi = 0; bi < numBones; ++bi) {
        if (opts.skipUnweightedBones && !boneInUse[bi]) continue;
        Ogre::Bone* bone = skel->getBone(bi);
        if (!bone) continue;
        const Ogre::Vector3 head = bone->_getDerivedPosition();
        // Average child position as the "tail"; leaf bones fall back
        // to head==tail (point distance).
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
        BoneSegment seg;
        seg.headX = head.x; seg.headY = head.y; seg.headZ = head.z;
        seg.tailX = tail.x; seg.tailY = tail.y; seg.tailZ = tail.z;
        out.bones.push_back(seg);
        out.boneIdxToHandle.push_back(bi);
        out.boneNames << QString::fromStdString(bone->getName());
    }
    if (out.bones.empty())
        return fail(QStringLiteral("skeleton has no usable bones"));

    out.handleToIdx.assign(numBones, -1);
    for (size_t i = 0; i < out.boneIdxToHandle.size(); ++i)
        out.handleToIdx[out.boneIdxToHandle[i]] = static_cast<int>(i);

    // Joint hierarchy for the ML (SkinTokens) path — aligned with
    // bones[]. Parent = the nearest ANCESTOR that survived the
    // skipUnweightedBones filter.
    if (algo == Algorithm::SkinTokens) {
        out.hierarchy.nodes.reserve(out.bones.size());
        for (size_t i = 0; i < out.boneIdxToHandle.size(); ++i) {
            Ogre::Bone* bone = skel->getBone(out.boneIdxToHandle[i]);
            SkeletonHierarchy::Node n;
            n.x = out.bones[i].headX;
            n.y = out.bones[i].headY;
            n.z = out.bones[i].headZ;
            n.parent = -1;
            const Ogre::Node* p = bone ? bone->getParent() : nullptr;
            while (p) {
                const auto* pb = dynamic_cast<const Ogre::Bone*>(p);
                if (pb && pb->getHandle() < numBones
                    && out.handleToIdx[pb->getHandle()] >= 0) {
                    n.parent = out.handleToIdx[pb->getHandle()];
                    break;
                }
                p = p->getParent();
            }
            out.hierarchy.nodes.push_back(n);
        }
    }

    // Helper: read tight xyz floats out of a VertexData's POSITION
    // element.
    auto extractPositions = [](Ogre::VertexData* vd,
                               std::vector<float>& outPos) -> bool {
        const auto* posElem = vd->vertexDeclaration->findElementBySemantic(
            Ogre::VES_POSITION);
        if (!posElem) return false;
        auto vbuf = vd->vertexBufferBinding->getBuffer(posElem->getSource());
        if (!vbuf || vd->vertexCount == 0) return false;
        outPos.resize(static_cast<size_t>(vd->vertexCount) * 3);
        const size_t stride = vbuf->getVertexSize();
        auto* base = static_cast<unsigned char*>(
            vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (size_t i = 0; i < vd->vertexCount; ++i) {
            float* fp = nullptr;
            posElem->baseVertexPointerToElement(base + i * stride, &fp);
            outPos[3 * i + 0] = fp[0];
            outPos[3 * i + 1] = fp[1];
            outPos[3 * i + 2] = fp[2];
        }
        vbuf->unlock();
        return true;
    };

    // Helper: append one owner's index data as flat uint32 triangle
    // indices, offset by the owner's base vertex.
    auto appendIndices = [](Ogre::IndexData* id,
                            std::uint32_t vertexOffset,
                            std::vector<std::uint32_t>& outIdx) {
        if (!id || !id->indexBuffer || id->indexCount == 0) return;
        auto ibuf = id->indexBuffer;
        const auto* base = static_cast<const unsigned char*>(
            ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        base += id->indexStart * ibuf->getIndexSize();
        outIdx.reserve(outIdx.size() + id->indexCount);
        if (ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT) {
            const auto* ip = reinterpret_cast<const std::uint32_t*>(base);
            for (size_t i = 0; i < id->indexCount; ++i)
                outIdx.push_back(ip[i] + vertexOffset);
        } else {
            const auto* ip = reinterpret_cast<const std::uint16_t*>(base);
            for (size_t i = 0; i < id->indexCount; ++i)
                outIdx.push_back(ip[i] + vertexOffset);
        }
        ibuf->unlock();
    };

    auto copyExisting = [](const Ogre::Mesh::VertexBoneAssignmentList& list,
                           std::vector<ComputeJob::Assign>& outList) {
        outList.reserve(list.size());
        for (const auto& kv : list) {
            ComputeJob::Assign a;
            a.vertexIndex = kv.second.vertexIndex;
            a.boneIndex   = kv.second.boneIndex;
            a.weight      = kv.second.weight;
            outList.push_back(a);
        }
    };

    // ── Collect every assignment owner into ONE combined vertex/
    // index set (the whole mesh computes in a single pass — the
    // paper voxelizes the whole character; per-submesh grids strand
    // bones outside accessory AABBs). Shared-vertex bone assignments
    // live on the Mesh, not the SubMesh (Codex review on PR #699).
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
            ComputeJob::Owner o;
            o.submeshIndex = -1;
            o.baseVertex   = std::uint32_t(out.positions.size() / 3);
            o.vertexCount  = std::uint32_t(mesh->sharedVertexData->vertexCount);
            copyExisting(mesh->getBoneAssignments(), o.existing);
            for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
                Ogre::SubMesh* sub = mesh->getSubMesh(si);
                if (sub && sub->useSharedVertices)
                    appendIndices(sub->indexData, o.baseVertex, out.indices);
            }
            out.positions.insert(out.positions.end(), ownerPositions.begin(),
                                 ownerPositions.end());
            out.owners.push_back(std::move(o));
        }
    }
    const unsigned short numSubs = mesh->getNumSubMeshes();
    for (unsigned short si = 0; si < numSubs; ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub) continue;
        if (sub->useSharedVertices) continue;
        if (!sub->vertexData) continue;
        std::vector<float> ownerPositions;
        if (!extractPositions(sub->vertexData, ownerPositions)) continue;
        ComputeJob::Owner o;
        o.submeshIndex = si;
        o.baseVertex   = std::uint32_t(out.positions.size() / 3);
        o.vertexCount  = std::uint32_t(sub->vertexData->vertexCount);
        copyExisting(sub->getBoneAssignments(), o.existing);
        appendIndices(sub->indexData, o.baseVertex, out.indices);
        out.positions.insert(out.positions.end(), ownerPositions.begin(),
                             ownerPositions.end());
        out.owners.push_back(std::move(o));
    }

    if (out.owners.empty())
        return fail(QStringLiteral("mesh has no readable vertex data"));
    return true;
}

SkinWeights::JobResult SkinWeights::runJob(const ComputeJob& job,
                                           const SkinWeightsOptions& opts,
                                           Algorithm algo,
                                           const ProgressFn& progress)
{
    JobResult res;
    const int totalVerts = static_cast<int>(job.positions.size() / 3);
    if (totalVerts < 1 || job.bones.empty()) {
        res.error = QStringLiteral("empty job");
        return res;
    }

    if (!computeWeights(job.positions.data(), totalVerts,
                        job.indices.empty() ? nullptr : job.indices.data(),
                        job.indices.size(),
                        job.bones, opts, algo, res.weights, &res.info,
                        job.hierarchy.nodes.empty() ? nullptr : &job.hierarchy,
                        progress)) {
        res.error = QStringLiteral("weight computation failed");
        return res;
    }
    if (res.info.fallbackReason.contains(QStringLiteral("cancelled"))) {
        res.error = QStringLiteral("cancelled");
        return res;
    }

    // Merge mode: locked vertices keep their existing weights and act
    // as Dirichlet constraints for the smoothing.
    if (!opts.replaceExisting) {
        res.locked.assign(res.weights.size(), 0);
        for (const ComputeJob::Owner& o : job.owners) {
            for (const ComputeJob::Assign& a : o.existing) {
                const size_t v = o.baseVertex + a.vertexIndex;
                if (v >= res.weights.size()) continue;
                if (!res.locked[v]) {
                    res.locked[v]  = 1;
                    res.weights[v] = {};
                }
                const int bi = (a.boneIndex < job.numBones)
                    ? job.handleToIdx[a.boneIndex] : -1;
                if (bi >= 0 && res.weights[v].count < 8) {
                    res.weights[v].boneIndices[res.weights[v].count] = bi;
                    res.weights[v].weights[res.weights[v].count] = a.weight;
                    ++res.weights[v].count;
                }
            }
        }
    }

    // Slice-B post-passes.
    if (opts.smoothIterations > 0 && !job.indices.empty()) {
        const auto adjacency = SkinWeightsPost::buildAdjacency(
            totalVerts, job.indices.data(), job.indices.size());
        SkinWeightsPost::laplacianSmooth(
            res.weights, adjacency, opts.smoothIterations, res.locked);
    }
    SkinWeightsPost::pruneAndRenormalize(res.weights,
                                         opts.maxInfluencesPerVertex);

    if (!res.info.allowedBones.empty()) {
        const double f = SkinWeightsPost::bleedFraction(res.weights,
                                                        res.info.allowedBones);
        if (f >= 0.0) res.bleedFraction = f;
    }
    res.ok = true;
    return res;
}

SkinWeightsReport SkinWeights::commitJob(Ogre::Entity* entity,
                                         const ComputeJob& job,
                                         const JobResult& result,
                                         const SkinWeightsOptions& opts)
{
    SkinWeightsReport report;
    report.meshName     = job.meshName;
    report.skeletonName = job.skeletonName;
    report.totalBones   = job.numBones;
    if (!result.ok) {
        report.error = result.error.isEmpty()
            ? QStringLiteral("weight computation failed") : result.error;
        return report;
    }
    if (!entity || !entity->getMesh()) {
        report.error = QStringLiteral("entity no longer valid");
        return report;
    }
    Ogre::Mesh* mesh = entity->getMesh().get();

    report.algorithmUsed  = result.info.algorithmUsed;
    report.fallbackReason = result.info.fallbackReason;
    report.bleedFraction  = result.bleedFraction;
    for (const int b : result.info.bonesWithoutSeeds) {
        if (b >= 0 && b < job.boneNames.size()
            && !report.bonesWithoutSeeds.contains(job.boneNames[b]))
            report.bonesWithoutSeeds.push_back(job.boneNames[b]);
    }

    const size_t maxK = static_cast<size_t>(
        std::clamp(opts.maxInfluencesPerVertex, 1, 8));

    for (const ComputeJob::Owner& o : job.owners) {
        // Resolve the assignment owner. The mesh may have changed
        // since prepareJob (redo after edits) — bounds-check.
        Ogre::SubMesh* sub = nullptr;
        if (o.submeshIndex >= 0) {
            if (o.submeshIndex >= mesh->getNumSubMeshes()) continue;
            sub = mesh->getSubMesh(
                static_cast<unsigned short>(o.submeshIndex));
            if (!sub) continue;
        }

        SkinWeightsSubmeshReport subReport;
        subReport.submeshIndex          = o.submeshIndex;
        subReport.boneAssignmentsBefore = static_cast<int>(o.existing.size());

        if (opts.replaceExisting) {
            if (sub) sub->clearBoneAssignments();
            else     mesh->clearBoneAssignments();
        }
        for (std::uint32_t v = 0; v < o.vertexCount; ++v) {
            const size_t gv = o.baseVertex + v;
            if (gv >= result.weights.size()) break;
            if (gv < result.locked.size() && result.locked[gv])
                continue;   // merge mode keeps the existing weights
            const auto& vw = result.weights[gv];
            if (static_cast<size_t>(vw.count) == maxK)
                ++subReport.verticesWithMaxInfluences;
            for (int k = 0; k < vw.count; ++k) {
                Ogre::VertexBoneAssignment vba;
                vba.vertexIndex = v;
                vba.boneIndex   = job.boneIdxToHandle[vw.boneIndices[k]];
                vba.weight      = static_cast<float>(vw.weights[k]);
                if (sub) sub->addBoneAssignment(vba);
                else     mesh->addBoneAssignment(vba);
            }
        }
        if (sub) sub->_compileBoneAssignments();
        else     mesh->_compileBoneAssignments();

        subReport.verticesProcessed = static_cast<int>(o.vertexCount);
        subReport.boneAssignmentsAfter = static_cast<int>(
            sub ? sub->getBoneAssignments().size()
                : mesh->getBoneAssignments().size());
        report.submeshes.push_back(subReport);
        report.totalVerticesProcessed += subReport.verticesProcessed;
        report.totalAssignmentsBefore += subReport.boneAssignmentsBefore;
        report.totalAssignmentsAfter  += subReport.boneAssignmentsAfter;
    }

    report.applied = true;
    return report;
}

SkinWeightsReport SkinWeights::computeAndApply(Ogre::Entity* entity,
                                                const SkinWeightsOptions& opts,
                                                Algorithm algo)
{
    ComputeJob job;
    QString err;
    if (!prepareJob(entity, opts, algo, job, &err)) {
        SkinWeightsReport report;
        report.error = err;
        return report;
    }
    const JobResult result = runJob(job, opts, algo);
    return commitJob(entity, job, result, opts);
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
    case Algorithm::SkinTokens:      return QStringLiteral("skintokens");
    }
    return QStringLiteral("skintokens");
}

SkinWeights::Algorithm SkinWeights::algorithmFromString(const QString& s)
{
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("inverse-distance")
        || v == QLatin1String("inverse_distance")
        || v == QLatin1String("id"))
        return Algorithm::InverseDistance;
    if (v == QLatin1String("geodesic-voxel") || v == QLatin1String("geodesic")
        || v == QLatin1String("gvb"))
        return Algorithm::GeodesicVoxel;
    // "skintokens" / the deprecated "unirig" aliases / anything else
    // → the default (SkinTokens ML).
    return Algorithm::SkinTokens;
}
