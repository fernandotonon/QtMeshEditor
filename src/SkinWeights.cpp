#include "SkinWeights.h"

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

namespace {

// Walk every submesh of the entity, gather flat vertex positions
// transformed into the entity-local space the skeleton is
// expressed in, build the bone segment list from the skeleton's
// bind pose, run `computeWeights`, then commit the result via
// `SubMesh::addBoneAssignment`/`_compileBoneAssignments`.
SkinWeightsReport applyToEntity(Ogre::Entity* entity,
                                 const SkinWeightsOptions& opts)
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

    // Helper: compute + commit weights against one assignment owner
    // (either an Ogre::SubMesh* or the Ogre::Mesh* for shared
    // vertices). `getList` / `clear` / `add` / `compile` are the
    // four operations that differ between the two owners; everything
    // else is shared. `mesh-local space` matches the skeleton's
    // bind-pose world space for the same entity, so no transform is
    // applied to the positions here.
    auto computeAndCommit =
        [&](Ogre::VertexData* vd,
            const Ogre::Mesh::VertexBoneAssignmentList& existing,
            const std::function<void()>& clearFn,
            const std::function<void(const Ogre::VertexBoneAssignment&)>& addFn,
            const std::function<void()>& compileFn,
            SkinWeightsSubmeshReport& subReport) -> bool
    {
        std::vector<float> positions;
        if (!extractPositions(vd, positions)) return false;

        std::vector<SkinWeights::VertexWeights> weights;
        SkinWeights::computeWeights(positions.data(),
                                     static_cast<int>(vd->vertexCount),
                                     bones, opts, weights);

        // Merge mode (`replaceExisting=false`): keep existing
        // weights and only fill vertices that have NONE. Build the
        // set of already-weighted vertex indices from the existing
        // assignment list BEFORE we touch anything. In replace mode
        // we clear the list outright.
        std::vector<char> alreadyWeighted;
        if (opts.replaceExisting) {
            clearFn();
        } else {
            alreadyWeighted.assign(weights.size(), 0);
            for (const auto& kv : existing) {
                if (kv.second.vertexIndex < alreadyWeighted.size())
                    alreadyWeighted[kv.second.vertexIndex] = 1;
            }
        }

        for (size_t v = 0; v < weights.size(); ++v) {
            // In merge mode, skip vertices that already had weights —
            // don't append a second normalized set on top of theirs.
            if (!opts.replaceExisting && v < alreadyWeighted.size()
                && alreadyWeighted[v])
                continue;
            const auto& vw = weights[v];
            if (static_cast<size_t>(vw.count) == maxK)
                ++subReport.verticesWithMaxInfluences;
            for (int k = 0; k < vw.count; ++k) {
                Ogre::VertexBoneAssignment vba;
                vba.vertexIndex = static_cast<unsigned int>(v);
                vba.boneIndex   = boneIdxToHandle[vw.boneIndices[k]];
                vba.weight      = static_cast<float>(vw.weights[k]);
                addFn(vba);
            }
        }
        compileFn();
        subReport.verticesProcessed = static_cast<int>(weights.size());
        return true;
    };

    // Process the shared vertex data once (if any submesh uses it).
    // Ogre stores shared-vertex bone assignments on the Mesh, not
    // the SubMesh — the FBX/glTF exporters read
    // `Mesh::getBoneAssignments()` for shared geometry, so routing
    // them to the submesh list would leave the export with stale /
    // missing weights (Codex review on PR #699).
    bool anySharedProcessed = false;
    if (mesh->sharedVertexData) {
        bool anySubUsesShared = false;
        for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
            if (mesh->getSubMesh(si) && mesh->getSubMesh(si)->useSharedVertices) {
                anySubUsesShared = true;
                break;
            }
        }
        if (anySubUsesShared) {
            SkinWeightsSubmeshReport subReport;
            subReport.submeshIndex = -1;   // -1 == mesh-level shared data
            subReport.boneAssignmentsBefore =
                static_cast<int>(mesh->getBoneAssignments().size());
            const auto existingShared = mesh->getBoneAssignments();
            if (computeAndCommit(
                    mesh->sharedVertexData,
                    existingShared,
                    [&]() { mesh->clearBoneAssignments(); },
                    [&](const Ogre::VertexBoneAssignment& vba) {
                        mesh->addBoneAssignment(vba);
                    },
                    [&]() { mesh->_compileBoneAssignments(); },
                    subReport)) {
                subReport.boneAssignmentsAfter =
                    static_cast<int>(mesh->getBoneAssignments().size());
                report.submeshes.push_back(subReport);
                report.totalVerticesProcessed += subReport.verticesProcessed;
                report.totalAssignmentsBefore += subReport.boneAssignmentsBefore;
                report.totalAssignmentsAfter  += subReport.boneAssignmentsAfter;
                anySharedProcessed = true;
            }
        }
    }

    // Process every submesh that owns its own (non-shared) vertex
    // data.
    const unsigned short numSubs = mesh->getNumSubMeshes();
    for (unsigned short si = 0; si < numSubs; ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub) continue;
        if (sub->useSharedVertices) continue;  // handled above
        if (!sub->vertexData) continue;

        SkinWeightsSubmeshReport subReport;
        subReport.submeshIndex = si;
        subReport.boneAssignmentsBefore =
            static_cast<int>(sub->getBoneAssignments().size());
        const auto existingSub = sub->getBoneAssignments();
        if (computeAndCommit(
                sub->vertexData,
                existingSub,
                [&]() { sub->clearBoneAssignments(); },
                [&](const Ogre::VertexBoneAssignment& vba) {
                    sub->addBoneAssignment(vba);
                },
                [&]() { sub->_compileBoneAssignments(); },
                subReport)) {
            subReport.boneAssignmentsAfter =
                static_cast<int>(sub->getBoneAssignments().size());
            report.submeshes.push_back(subReport);
            report.totalVerticesProcessed += subReport.verticesProcessed;
            report.totalAssignmentsBefore += subReport.boneAssignmentsBefore;
            report.totalAssignmentsAfter  += subReport.boneAssignmentsAfter;
        }
    }

    Q_UNUSED(anySharedProcessed);
    report.applied = true;
    return report;
}

} // namespace

SkinWeightsReport SkinWeights::computeAndApply(Ogre::Entity* entity,
                                                const SkinWeightsOptions& opts,
                                                Algorithm algo)
{
    SkinWeightsReport report;
    if (algo != Algorithm::InverseDistance) {
        report.error = QStringLiteral("only InverseDistance backend is implemented");
        return report;
    }
    return applyToEntity(entity, opts);
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
    if (!report.error.isEmpty()) s << "Error: " << report.error << "\n";
    return out;
}

QString SkinWeights::algorithmToString(Algorithm algo)
{
    Q_UNUSED(algo);
    return QStringLiteral("inverse-distance");
}

SkinWeights::Algorithm SkinWeights::algorithmFromString(const QString& s)
{
    // Only `InverseDistance` is implemented today. The
    // recognized-string set widens here naturally when libigl
    // BBW lands behind `-DENABLE_LIBIGL_BBW`.
    Q_UNUSED(s);
    return Algorithm::InverseDistance;
}
