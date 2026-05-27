#include "ExportOptimizer.h"

#include <meshoptimizer.h>

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreHardwareIndexBuffer.h>
#include <OgreHardwareVertexBuffer.h>
#include <OgreMesh.h>
#include <OgreSceneNode.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>

#include <QJsonArray>
#include <QLocale>
#include <QTextStream>
#include <algorithm>
#include <cstring>
#include <numeric>

namespace {

// Pull a tight uint32 index array out of an Ogre IndexData regardless
// of the underlying 16/32-bit storage.
std::vector<uint32_t> readIndices(Ogre::IndexData* idata)
{
    std::vector<uint32_t> out;
    if (!idata || !idata->indexBuffer || idata->indexCount == 0) return out;

    auto ibuf = idata->indexBuffer;
    const size_t n = idata->indexCount;
    out.resize(n);

    auto* src = static_cast<unsigned char*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    src += idata->indexStart * ibuf->getIndexSize();

    if (ibuf->getType() == Ogre::HardwareIndexBuffer::IT_16BIT) {
        const auto* p = reinterpret_cast<const uint16_t*>(src);
        for (size_t i = 0; i < n; ++i) out[i] = p[i];
    } else {
        std::memcpy(out.data(), src, n * sizeof(uint32_t));
    }
    ibuf->unlock();
    return out;
}

// Replace `idata`'s index buffer with `indices`. Picks 16-bit storage
// when the max index fits, otherwise 32-bit — same logic as
// MeshOptimizerLod::buildIndexData.
void writeIndices(Ogre::IndexData* idata, const std::vector<uint32_t>& indices)
{
    if (!idata) return;
    idata->indexCount = indices.size();
    idata->indexStart = 0;

    if (indices.empty()) {
        idata->indexBuffer.reset();
        return;
    }

    uint32_t maxIdx = 0;
    for (uint32_t i : indices) if (i > maxIdx) maxIdx = i;
    const bool use32 = (maxIdx > std::numeric_limits<uint16_t>::max());

    const auto itype = use32
        ? Ogre::HardwareIndexBuffer::IT_32BIT
        : Ogre::HardwareIndexBuffer::IT_16BIT;

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        itype, indices.size(), Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);

    auto* dst = ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD);
    if (use32) {
        std::memcpy(dst, indices.data(), indices.size() * sizeof(uint32_t));
    } else {
        auto* p = reinterpret_cast<uint16_t*>(dst);
        for (size_t i = 0; i < indices.size(); ++i)
            p[i] = static_cast<uint16_t>(indices[i]);
    }
    ibuf->unlock();

    idata->indexBuffer = ibuf;
}

// Pull a tight float3 position array (one vec3 per vertex) out of an
// Ogre VertexData. Returns empty when positions aren't present.
std::vector<float> readPositions(Ogre::VertexData* vdata)
{
    if (!vdata) return {};
    const auto* elem = vdata->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (!elem) return {};
    auto vbuf = vdata->vertexBufferBinding->getBuffer(elem->getSource());
    if (!vbuf) return {};

    const size_t n = vdata->vertexCount;
    std::vector<float> out(n * 3);
    auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    const size_t stride = vbuf->getVertexSize();
    for (size_t i = 0; i < n; ++i) {
        float* p = nullptr;
        elem->baseVertexPointerToElement(base + i * stride, &p);
        out[i * 3 + 0] = p[0];
        out[i * 3 + 1] = p[1];
        out[i * 3 + 2] = p[2];
    }
    vbuf->unlock();
    return out;
}

// Run meshopt_optimizeVertexFetchRemap + apply the remap to every
// vertex buffer bound on `vdata` and the supplied index list. Vertex
// buffers shared across submeshes can't be safely fetch-optimized in
// place (a remap from one submesh would invalidate the index lists of
// other submeshes), so the caller must only invoke this on
// per-submesh-private vertex data — see optimizeEntity below.
bool applyVertexFetchRemap(Ogre::VertexData* vdata, std::vector<uint32_t>& indices)
{
    if (!vdata || indices.empty()) return false;
    const size_t vertCount = vdata->vertexCount;

    std::vector<unsigned int> remap(vertCount);
    const size_t newVertCount = meshopt_optimizeVertexFetchRemap(
        remap.data(), indices.data(), indices.size(), vertCount);
    if (newVertCount == 0) return false;

    // Apply to the index buffer in place.
    meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());

    // Apply to every vertex buffer source bound on the declaration —
    // each binding gets its own remap pass since strides differ.
    auto* binding = vdata->vertexBufferBinding;
    const auto& bindings = binding->getBindings();
    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
        const auto source = it->first;
        auto vbuf = it->second;
        if (!vbuf) continue;

        const size_t stride = vbuf->getVertexSize();
        const size_t oldBytes = vertCount * stride;
        const size_t newBytes = newVertCount * stride;

        std::vector<unsigned char> tmp(oldBytes);
        auto* src = vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY);
        std::memcpy(tmp.data(), src, oldBytes);
        vbuf->unlock();

        std::vector<unsigned char> out(newBytes);
        meshopt_remapVertexBuffer(out.data(), tmp.data(), vertCount, stride, remap.data());

        // Replace the buffer with one sized to the new vertex count.
        auto newBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            stride, newVertCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        auto* dst = newBuf->lock(Ogre::HardwareBuffer::HBL_DISCARD);
        std::memcpy(dst, out.data(), newBytes);
        newBuf->unlock();

        binding->setBinding(source, newBuf);
    }
    vdata->vertexCount = newVertCount;
    return true;
}

// Whether `sub` owns its vertex buffer or shares it with other
// submeshes (which means we can't safely run vertex-fetch on it —
// remapping the shared verts would scramble every other submesh's
// indices).
bool subOwnsItsVertexData(Ogre::Mesh* mesh, Ogre::SubMesh* sub)
{
    if (!sub || sub->useSharedVertices) return false;
    // sub->vertexData is non-shared if we got here.
    return mesh != nullptr;  // mesh is just for completeness
}

ExportOptimizeSubMeshReport optimizeSubMesh(Ogre::Mesh* mesh, unsigned si,
                                            OptimizeFlags flags)
{
    ExportOptimizeSubMeshReport sr;
    sr.meshName = QString::fromStdString(mesh->getName());
    sr.submeshIndex = static_cast<int>(si);

    Ogre::SubMesh* sub = mesh->getSubMesh(si);
    if (!sub || !sub->indexData) return sr;

    Ogre::VertexData* vdata = sub->useSharedVertices
        ? mesh->sharedVertexData
        : sub->vertexData;
    if (!vdata) return sr;

    auto indices = readIndices(sub->indexData);
    if (indices.empty()) return sr;

    sr.vertexCount   = static_cast<int>(vdata->vertexCount);
    sr.triangleCount = static_cast<int>(indices.size() / 3);
    sr.acmrBefore    = ExportOptimizer::computeAcmr(indices, vdata->vertexCount);

    // 1. Vertex-cache reorder (Forsyth).
    if (any(flags & OptimizeFlags::VertexCache)) {
        meshopt_optimizeVertexCache(indices.data(), indices.data(),
                                    indices.size(), vdata->vertexCount);
        sr.vertexCacheRun = true;
    }

    // 2. Overdraw optimization. Needs positions; skip if absent.
    if (any(flags & OptimizeFlags::Overdraw)) {
        const auto positions = readPositions(vdata);
        if (positions.size() == vdata->vertexCount * 3) {
            meshopt_optimizeOverdraw(indices.data(), indices.data(), indices.size(),
                                     positions.data(), vdata->vertexCount, sizeof(float) * 3,
                                     ExportOptimizer::kOverdrawThreshold);
            sr.overdrawRun = true;
        }
    }

    // 3. Vertex-fetch reorder. Only safe when this submesh owns its
    // vertex data — running on shared verts would scramble the index
    // buffers of every other submesh.
    if (any(flags & OptimizeFlags::VertexFetch) &&
        !sub->useSharedVertices && subOwnsItsVertexData(mesh, sub)) {
        if (applyVertexFetchRemap(vdata, indices))
            sr.vertexFetchRun = true;
    }

    // Commit the reordered indices regardless of which optimizers ran;
    // the array is unchanged if everything was skipped.
    writeIndices(sub->indexData, indices);

    sr.acmrAfter = ExportOptimizer::computeAcmr(indices, vdata->vertexCount);
    return sr;
}

} // namespace

double ExportOptimizer::computeAcmr(const std::vector<uint32_t>& indices,
                                    uint32_t vertexCount)
{
    if (indices.empty() || vertexCount == 0) return 0.0;
    // meshopt_analyzeVertexCache returns a struct; ACMR = misses / tris.
    // Cache size 32 matches Forsyth's paper / VertexCacheOptimizer.
    const meshopt_VertexCacheStatistics stats = meshopt_analyzeVertexCache(
        indices.data(), indices.size(), vertexCount,
        kCacheSize, /*warp_size=*/0, /*primgroup_size=*/0);
    return stats.acmr;
}

ExportOptimizeReport ExportOptimizer::optimizeEntity(Ogre::Entity* entity,
                                                     OptimizeFlags flags)
{
    ExportOptimizeReport report;
    if (!entity) return report;
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return report;
    if (!any(flags)) return report;

    for (unsigned si = 0; si < mesh->getNumSubMeshes(); ++si) {
        auto sr = optimizeSubMesh(mesh.get(), si, flags);
        if (sr.triangleCount == 0) continue;
        if (sr.vertexCacheRun || sr.overdrawRun || sr.vertexFetchRun)
            ++report.submeshesOptimized;
        report.submeshes.append(sr);
        report.totalTriangles    += sr.triangleCount;
        report.weightedAcmrBefore += sr.acmrBefore * sr.triangleCount;
        report.weightedAcmrAfter  += sr.acmrAfter  * sr.triangleCount;
    }
    if (report.totalTriangles > 0) {
        report.weightedAcmrBefore /= report.totalTriangles;
        report.weightedAcmrAfter  /= report.totalTriangles;
    }
    return report;
}

namespace {
// Walk a scene node tree and call `visit(entity)` for each attached Entity.
void visitEntities(const Ogre::SceneNode* node,
                   const std::function<void(Ogre::Entity*)>& visit)
{
    if (!node) return;
    for (unsigned i = 0; i < node->numAttachedObjects(); ++i) {
        auto* obj = node->getAttachedObject(i);
        if (obj && obj->getMovableType() == "Entity")
            visit(static_cast<Ogre::Entity*>(obj));
    }
    for (unsigned i = 0; i < node->numChildren(); ++i) {
        if (auto* child = dynamic_cast<const Ogre::SceneNode*>(node->getChild(i)))
            visitEntities(child, visit);
    }
}
} // namespace

ExportOptimizeReport ExportOptimizer::optimizeSceneNode(const Ogre::SceneNode* node,
                                                        OptimizeFlags flags)
{
    ExportOptimizeReport report;
    if (!node || !any(flags)) return report;
    visitEntities(node, [&](Ogre::Entity* e) {
        auto partial = optimizeEntity(e, flags);
        for (const auto& sr : partial.submeshes) {
            report.submeshes.append(sr);
            if (sr.vertexCacheRun || sr.overdrawRun || sr.vertexFetchRun)
                ++report.submeshesOptimized;
            report.totalTriangles += sr.triangleCount;
            report.weightedAcmrBefore += sr.acmrBefore * sr.triangleCount;
            report.weightedAcmrAfter  += sr.acmrAfter  * sr.triangleCount;
        }
    });
    if (report.totalTriangles > 0) {
        report.weightedAcmrBefore /= report.totalTriangles;
        report.weightedAcmrAfter  /= report.totalTriangles;
    }
    return report;
}

QJsonObject ExportOptimizer::toJson(const ExportOptimizeReport& report)
{
    QJsonObject obj;
    QJsonArray submeshes;
    for (const auto& sr : report.submeshes) {
        QJsonObject so;
        so["mesh"]           = sr.meshName;
        so["submeshIndex"]   = sr.submeshIndex;
        so["triangleCount"]  = sr.triangleCount;
        so["vertexCount"]    = sr.vertexCount;
        so["acmrBefore"]     = sr.acmrBefore;
        so["acmrAfter"]      = sr.acmrAfter;
        so["vertexCacheRun"] = sr.vertexCacheRun;
        so["overdrawRun"]    = sr.overdrawRun;
        so["vertexFetchRun"] = sr.vertexFetchRun;
        submeshes.append(so);
    }
    obj["submeshes"] = submeshes;
    obj["weightedAcmrBefore"] = report.weightedAcmrBefore;
    obj["weightedAcmrAfter"]  = report.weightedAcmrAfter;
    obj["totalTriangles"]     = report.totalTriangles;
    obj["submeshesOptimized"] = report.submeshesOptimized;
    obj["improvementPct"]     = report.improvementPct();
    return obj;
}

QString ExportOptimizer::toText(const ExportOptimizeReport& report)
{
    QString out;
    QTextStream s(&out);
    QLocale locale;
    s << "Export Optimization\n";
    s << "===================\n";
    if (report.submeshes.isEmpty()) {
        s << "(no submeshes to optimize)\n";
        return out;
    }
    s << QString("Optimized %1 of %2 submesh(es), ACMR %3 → %4 (%5%% improvement)\n")
            .arg(report.submeshesOptimized)
            .arg(report.submeshes.size())
            .arg(report.weightedAcmrBefore, 0, 'f', 3)
            .arg(report.weightedAcmrAfter,  0, 'f', 3)
            .arg(report.improvementPct(),   0, 'f', 1);
    return out;
}
