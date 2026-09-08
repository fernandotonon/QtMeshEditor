#include "MeshWeldOps.h"

#include <QByteArray>

#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSubMesh.h>
#include <OgreHardwareBufferManager.h>

#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

namespace {

// One vertex-data owner: the mesh's shared block (submeshIndex -1) or a
// non-shared submesh. Global vertex ids are owner base + local index —
// the same convention SkinEvaluate::extract uses.
struct Owner {
    int submeshIndex = -1;               // -1 = shared
    Ogre::VertexData* vd = nullptr;
    int base = 0;                        // global index of local vertex 0
};

struct QuantKey {
    long long x, y, z;
    bool operator<(const QuantKey& o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
    }
};

// Full-record byte signature of one vertex (every element from every bound
// buffer, in declaration order). Byte-identical signature = true duplicate.
QByteArray vertexSignature(Ogre::VertexData* vd,
                           const std::vector<const unsigned char*>& locked,
                           size_t v)
{
    QByteArray sig;
    const auto& elems = vd->vertexDeclaration->getElements();
    for (const auto& el : elems) {
        const unsigned char* buf = locked[el.getSource()];
        if (!buf) continue;
        const size_t stride =
            vd->vertexBufferBinding->getBuffer(el.getSource())->getVertexSize();
        sig.append(reinterpret_cast<const char*>(
                       buf + v * stride + el.getOffset()),
                   static_cast<int>(el.getSize()));
    }
    return sig;
}

} // namespace

MeshWeldOps::Report MeshWeldOps::analyze(Ogre::Entity* entity, float epsilon)
{
    return run(entity, epsilon, /*mutate=*/false);
}

MeshWeldOps::Report MeshWeldOps::apply(Ogre::Entity* entity, float epsilon)
{
    return run(entity, epsilon, /*mutate=*/true);
}

MeshWeldOps::Report MeshWeldOps::run(Ogre::Entity* entity, float epsilon,
                                     bool mutate)
{
    Report rep;
    if (!entity || !entity->getMesh()) {
        rep.error = QStringLiteral("No entity/mesh");
        return rep;
    }
    Ogre::MeshPtr mesh = entity->getMesh();
    rep.skinned = mesh->hasSkeleton();

    if (epsilon <= 0.0f) {
        const Ogre::Vector3 size = mesh->getBounds().getSize();
        epsilon = std::max(1e-9f, 1e-5f * size.length());
    }

    // ---- gather owners --------------------------------------------------
    std::vector<Owner> owners;
    int total = 0;
    if (mesh->sharedVertexData) {
        owners.push_back({-1, mesh->sharedVertexData, total});
        total += static_cast<int>(mesh->sharedVertexData->vertexCount);
    }
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (sub && !sub->useSharedVertices && sub->vertexData) {
            owners.push_back({si, sub->vertexData, total});
            total += static_cast<int>(sub->vertexData->vertexCount);
        }
    }
    if (total == 0) {
        rep.error = QStringLiteral("Mesh has no vertex data");
        return rep;
    }

    // ---- read positions + signatures (global indexing) -------------------
    std::vector<Ogre::Vector3> positions(static_cast<size_t>(total));
    std::vector<QByteArray> signatures(static_cast<size_t>(total));
    for (const Owner& ow : owners) {
        const auto* posElem = ow.vd->vertexDeclaration->findElementBySemantic(
            Ogre::VES_POSITION);
        if (!posElem) continue;
        // Lock every distinct source buffer once.
        const size_t nSrc = ow.vd->vertexBufferBinding->getBufferCount();
        std::vector<const unsigned char*> locked(
            ow.vd->vertexBufferBinding->getLastBoundIndex() + 1, nullptr);
        std::vector<Ogre::HardwareVertexBufferSharedPtr> bufs;
        (void)nSrc;
        for (const auto& [src, buf] : ow.vd->vertexBufferBinding->getBindings()) {
            bufs.push_back(buf);
            locked[src] = static_cast<const unsigned char*>(
                buf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        }
        const size_t posStride =
            ow.vd->vertexBufferBinding->getBuffer(posElem->getSource())
                ->getVertexSize();
        const unsigned char* posBuf = locked[posElem->getSource()];
        for (size_t v = 0; v < ow.vd->vertexCount; ++v) {
            const float* p;
            posElem->baseVertexPointerToElement(
                const_cast<unsigned char*>(posBuf + v * posStride),
                const_cast<float**>(&p));
            positions[ow.base + v] = Ogre::Vector3(p[0], p[1], p[2]);
            signatures[ow.base + v] = vertexSignature(ow.vd, locked, v);
        }
        for (auto& b : bufs) b->unlock();
    }

    // ---- cluster by quantized position (global, across owners) -----------
    std::map<QuantKey, std::vector<int>> clusters;
    for (int v = 0; v < total; ++v) {
        const Ogre::Vector3& p = positions[static_cast<size_t>(v)];
        clusters[{static_cast<long long>(std::llround(p.x / epsilon)),
                  static_cast<long long>(std::llround(p.y / epsilon)),
                  static_cast<long long>(std::llround(p.z / epsilon))}]
            .push_back(v);
    }

    // ---- current skin weights per global vertex ---------------------------
    // Owner-local assignment lists; vba.vertexIndex is OWNER-LOCAL.
    using WeightMap = std::map<unsigned short, float>;
    std::vector<WeightMap> weights(static_cast<size_t>(total));
    if (rep.skinned) {
        auto collect = [&](const Ogre::Mesh::VertexBoneAssignmentList& list,
                           int base) {
            for (const auto& [vi, vba] : list)
                weights[base + vba.vertexIndex][vba.boneIndex] += vba.weight;
        };
        for (const Owner& ow : owners) {
            if (ow.submeshIndex < 0)
                collect(mesh->getBoneAssignments(), ow.base);
            else
                collect(mesh->getSubMesh(
                            static_cast<unsigned short>(ow.submeshIndex))
                            ->getBoneAssignments(), ow.base);
        }
    }

    auto weightsDiffer = [](const WeightMap& a, const WeightMap& b) {
        if (a.size() != b.size()) return true;
        auto ia = a.begin();
        auto ib = b.begin();
        for (; ia != a.end(); ++ia, ++ib) {
            if (ia->first != ib->first) return true;
            if (std::fabs(ia->second - ib->second) > 1e-4f) return true;
        }
        return false;
    };

    // ---- analyze clusters -------------------------------------------------
    // weldRemap: OWNER-LOCAL index remap per owner (only within one owner —
    // index buffers can only reference their own vertex data).
    std::unordered_map<int, int> globalRemap;   // global dup -> global rep
    std::vector<int> unifyTargets;              // globals needing weight rewrite
    for (auto& [key, verts] : clusters) {
        if (verts.size() < 2) continue;
        ++rep.duplicateClusters;
        rep.duplicateVertices += static_cast<int>(verts.size());

        // Weld: byte-identical twins within the SAME owner collapse onto the
        // first such twin.
        auto ownerOf = [&](int g) {
            for (size_t i = owners.size(); i-- > 0;)
                if (g >= owners[i].base) return static_cast<int>(i);
            return 0;
        };
        for (size_t i = 0; i < verts.size(); ++i) {
            for (size_t j = 0; j < i; ++j) {
                if (ownerOf(verts[i]) != ownerOf(verts[j])) continue;
                if (signatures[verts[i]] == signatures[verts[j]]
                    && !globalRemap.count(verts[i])
                    && !globalRemap.count(verts[j])) {
                    globalRemap[verts[i]] = verts[j];
                    ++rep.weldableVertices;
                    break;
                }
            }
        }

        // Weight mismatch across the cluster?
        if (rep.skinned) {
            bool differ = false;
            for (size_t i = 1; i < verts.size() && !differ; ++i)
                differ = weightsDiffer(weights[verts[0]], weights[verts[i]]);
            if (differ) {
                ++rep.weightMismatchClusters;
                for (size_t i = 1; i < verts.size(); ++i)
                    if (weightsDiffer(weights[verts[0]], weights[verts[i]]))
                        unifyTargets.push_back(verts[i]);
                // Representative = verts[0]; remember the source for each.
                for (size_t i = 1; i < verts.size(); ++i)
                    weights[verts[i]] = weights[verts[0]];
            }
        }
    }

    if (!mutate) {
        rep.ok = true;
        return rep;
    }

    // ---- apply: index remap (weld) ---------------------------------------
    if (!globalRemap.empty()) {
        for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
            Ogre::SubMesh* sub = mesh->getSubMesh(si);
            if (!sub || !sub->indexData || !sub->indexData->indexBuffer)
                continue;
            // Which owner does this submesh's index buffer reference?
            int base = -1;
            if (sub->useSharedVertices) {
                for (const Owner& ow : owners)
                    if (ow.submeshIndex == -1) base = ow.base;
            } else {
                for (const Owner& ow : owners)
                    if (ow.submeshIndex == si) base = ow.base;
            }
            if (base < 0) continue;

            Ogre::IndexData* id = sub->indexData;
            const bool use16 = (id->indexBuffer->getType()
                                == Ogre::HardwareIndexBuffer::IT_16BIT);
            void* idata = id->indexBuffer->lock(
                Ogre::HardwareBuffer::HBL_NORMAL);
            auto* idx16 = static_cast<uint16_t*>(idata);
            auto* idx32 = static_cast<uint32_t*>(idata);
            for (size_t k = 0; k < id->indexCount; ++k) {
                const size_t pos = id->indexStart + k;
                const int global = base + static_cast<int>(
                    use16 ? idx16[pos] : idx32[pos]);
                const auto it = globalRemap.find(global);
                if (it == globalRemap.end()) continue;
                const int local = it->second - base;
                if (use16) idx16[pos] = static_cast<uint16_t>(local);
                else       idx32[pos] = static_cast<uint32_t>(local);
                ++rep.weldedVertices;
            }
            id->indexBuffer->unlock();
        }
    }

    // ---- apply: unified skin weights --------------------------------------
    if (rep.skinned && !unifyTargets.empty()) {
        rep.weightsUnified = static_cast<int>(unifyTargets.size());
        // Rewrite the owner assignment lists in place from the (already
        // unified) `weights` array — the SkinWeightController write path:
        // clear + add + _compileBoneAssignments per owner, NO _initialise
        // (swapping VertexData under a live SkeletonInstance shatters the
        // on-screen mesh).
        for (const Owner& ow : owners) {
            const int count = static_cast<int>(ow.vd->vertexCount);
            if (ow.submeshIndex < 0) {
                mesh->clearBoneAssignments();
                for (int v = 0; v < count; ++v)
                    for (const auto& [bone, w] : weights[ow.base + v])
                        mesh->addBoneAssignment(
                            {static_cast<unsigned>(v), bone, w});
                mesh->_compileBoneAssignments();
            } else {
                Ogre::SubMesh* sub = mesh->getSubMesh(
                    static_cast<unsigned short>(ow.submeshIndex));
                sub->clearBoneAssignments();
                for (int v = 0; v < count; ++v)
                    for (const auto& [bone, w] : weights[ow.base + v])
                        sub->addBoneAssignment(
                            {static_cast<unsigned>(v), bone, w});
                sub->_compileBoneAssignments();
            }
        }
    }

    rep.ok = true;
    return rep;
}
