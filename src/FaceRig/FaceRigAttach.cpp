#include "FaceRigAttach.h"

#include "ArkitTemplate.h"

#include "../commands/MorphCommands.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSubMesh.h>

#include <cstdint>
#include <map>

namespace FaceRig {

namespace {

// Read tight xyz floats from a VertexData POSITION element (mirrors
// SkinWeights.cpp's extractor). Returns false if there's no position stream.
bool extractPositions(Ogre::VertexData* vd, std::vector<float>& out)
{
    if (!vd) return false;
    const auto* posElem =
        vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (!posElem) return false;
    auto vbuf = vd->vertexBufferBinding->getBuffer(posElem->getSource());
    if (!vbuf || vd->vertexCount == 0) return false;
    out.resize(size_t(vd->vertexCount) * 3);
    const size_t stride = vbuf->getVertexSize();
    auto* base = static_cast<unsigned char*>(
        vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    for (size_t i = 0; i < vd->vertexCount; ++i) {
        float* fp = nullptr;
        posElem->baseVertexPointerToElement(base + i * stride, &fp);
        out[3*i+0] = fp[0]; out[3*i+1] = fp[1]; out[3*i+2] = fp[2];
    }
    vbuf->unlock();
    return true;
}

void appendIndices(Ogre::IndexData* id, std::uint32_t offset,
                   std::vector<int>& out)
{
    if (!id || !id->indexBuffer || id->indexCount == 0) return;
    auto ibuf = id->indexBuffer;
    const auto* base = static_cast<const unsigned char*>(
        ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    base += id->indexStart * ibuf->getIndexSize();
    out.reserve(out.size() + id->indexCount);
    if (ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT) {
        const auto* ip = reinterpret_cast<const std::uint32_t*>(base);
        for (size_t i = 0; i < id->indexCount; ++i) out.push_back(int(ip[i] + offset));
    } else {
        const auto* ip = reinterpret_cast<const std::uint16_t*>(base);
        for (size_t i = 0; i < id->indexCount; ++i) out.push_back(int(ip[i] + offset));
    }
    ibuf->unlock();
}

}  // namespace

FaceRigGeometry extractGeometry(Ogre::Entity* entity)
{
    FaceRigGeometry geo;
    if (!entity) return geo;
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return geo;

    // Collect every geometry owner into ONE combined vertex/index set so the
    // fit sees the whole face (a single-submesh head is the common case, but a
    // face split across submeshes still fits as one surface). Track each owner
    // so we can split the per-vertex deltas back onto the right pose handle.
    auto addOwner = [&](unsigned short handle, Ogre::VertexData* vd,
                        Ogre::IndexData* id) {
        std::vector<float> pos;
        if (!extractPositions(vd, pos)) return;
        const std::uint32_t base = std::uint32_t(geo.userV.size() / 3);
        geo.owners.push_back({handle, base, int(pos.size() / 3)});
        geo.userV.insert(geo.userV.end(), pos.begin(), pos.end());
        appendIndices(id, base, geo.userF);
    };

    if (mesh->sharedVertexData) {
        // shared pool → handle 0; its indices live per-submesh.
        std::vector<float> pos;
        if (extractPositions(mesh->sharedVertexData, pos)) {
            geo.owners.push_back({0, 0, int(pos.size() / 3)});
            geo.userV.insert(geo.userV.end(), pos.begin(), pos.end());
            for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
                Ogre::SubMesh* sm = mesh->getSubMesh(si);
                if (sm && sm->useSharedVertices)
                    appendIndices(sm->indexData, 0, geo.userF);
            }
        }
    }
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sm = mesh->getSubMesh(si);
        if (!sm || sm->useSharedVertices) continue;
        addOwner(static_cast<unsigned short>(si + 1), sm->vertexData, sm->indexData);
    }
    return geo;
}

void attachShapes(Ogre::Entity* entity, const FaceRigGeometry& geo,
                  const FaceRigResult& result, AttachReport& report)
{
    // Attach each shape as a pose + VAT_POSE clip, splitting the combined
    // deltas back onto each owner's handle. Reuse AddMorphTargetCommand's
    // redo() (the exact MorphCommands pose-build) so face capture drives these
    // with no new playback code; call redo() directly (headless-safe — no undo
    // stack required, GUI callers can wrap in UndoManager separately).
    for (const FaceRigShape& shape : result.shapes) {
        std::vector<MorphPoseSlice> slices;
        for (const GeometryOwner& o : geo.owners) {
            MorphPoseSlice slice;
            slice.submeshHandle = o.handle;
            for (int i = 0; i < o.count; ++i) {
                const std::uint32_t gv = o.base + std::uint32_t(i);
                if (size_t(gv) * 3 + 2 >= shape.userDeltas.size()) break;
                const float* d = &shape.userDeltas[size_t(gv) * 3];
                if (d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f) continue;
                slice.offsets[static_cast<unsigned int>(i)] =
                    Ogre::Vector3f(d[0], d[1], d[2]);
            }
            if (!slice.offsets.empty()) slices.push_back(std::move(slice));
        }
        if (slices.empty()) continue;   // shape moved nothing on this mesh
        AddMorphTargetCommand cmd(entity, shape.name, slices);
        cmd.redo();
        report.shapesAttached++;
    }
    report.ok = report.shapesAttached > 0;
    if (!report.ok)
        report.error = QStringLiteral("no blendshapes produced any vertex movement");
}

AttachReport attachFaceRig(Ogre::Entity* entity,
                           const ArkitTemplate& tmpl,
                           const FaceRigOptions& opts)
{
    AttachReport rep;
    if (!entity) { rep.error = QStringLiteral("no entity"); return rep; }
    if (!entity->getMesh()) { rep.error = QStringLiteral("entity has no mesh"); return rep; }
    if (!tmpl.valid()) { rep.error = QStringLiteral("ARKit template not loaded"); return rep; }

    const FaceRigGeometry geo = extractGeometry(entity);
    if (!geo.valid()) {
        rep.error = QStringLiteral("could not read mesh geometry");
        return rep;
    }

    const FaceRigResult res = buildFaceRig(geo.userV, geo.userF, tmpl, opts);
    rep.userVertexCount = res.userVertexCount;
    rep.fitMeanResidualPct = res.fitMeanResidualPct;
    rep.fitMaxResidualPct = res.fitMaxResidualPct;
    if (!res.ok) {
        rep.error = QString::fromStdString(res.error);
        return rep;
    }

    attachShapes(entity, geo, res, rep);
    return rep;
}

AttachReport attachFaceRigWithBundledTemplate(Ogre::Entity* entity,
                                               const FaceRigOptions& opts)
{
    AttachReport rep;
    const QString path = ArkitTemplate::ensureModelBlocking();
    if (path.isEmpty()) {
        rep.error = QStringLiteral(
            "ARKit template unavailable (offline and not downloaded, or the "
            "build has no face-rig model). Set QTMESH_FACERIG_MODEL_BASE_URL or "
            "download arkit_template.bin to ai_models/facerig/.");
        return rep;
    }
    ArkitTemplate tmpl;
    QString err;
    if (!tmpl.load(path, &err)) {
        rep.error = QStringLiteral("failed to load ARKit template: %1").arg(err);
        return rep;
    }
    return attachFaceRig(entity, tmpl, opts);
}

}  // namespace FaceRig
