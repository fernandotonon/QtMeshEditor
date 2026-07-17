#include "FaceRigAttach.h"

#include "ArkitTemplate.h"
#include "FaceRigLandmarks.h"

#include "../AutoRig.h"
#include "../MeshSegmenter.h"
#include "../commands/MorphCommands.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSubMesh.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

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

    // ── HEAD ISOLATION (the fix for full-body characters) ────────────────────
    // The ARKit template is a FACE. Fitting it against a whole dancing body
    // smears the face over the body (mouth shapes end up on an arm). So we
    // isolate the head region and only fit / deform those vertices. Preference:
    //   1) rig-prior — if the mesh is SKINNED, label each vertex by the body
    //      region of the bone it's most weighted to (EXACT; handles the fox's
    //      snout/ears and Mixamo's exaggerated proportions the coordinate model
    //      can't). This is AutoRig::rigPriorPartLabels, ordered to match our
    //      combined gather.
    //   2) geometric fallback — MeshSegmenter's spatial head/torso/limb split.
    // If neither finds a plausible head (e.g. the mesh really IS just a face),
    // leave headMask empty and fit the whole thing (previous behaviour).
    const int nv = int(geo.userV.size() / 3);
    const int headPart = int(MeshSegmenter::Part::Head);
    std::vector<int> labels;
    if (mesh->hasSkeleton()) {
        int resolved = 0;
        labels = AutoRig::rigPriorPartLabels(entity, nv, &resolved);
        // Require the rig prior to resolve a decent share; else fall through.
        if (resolved < nv / 2) labels.clear();
    }
    if (labels.empty() && geo.userF.size() >= 3) {
        std::vector<std::uint32_t> idx(geo.userF.begin(), geo.userF.end());
        MeshSegmenter::Result seg = MeshSegmenter::segmentGeometric(
            geo.userV.data(), nv, idx.data(), int(idx.size()));
        if (seg.ok) labels = seg.vertexLabels;
    }
    if (int(labels.size()) == nv) {
        int headCount = 0;
        geo.headMask.assign(size_t(nv), 0);
        for (int v = 0; v < nv; ++v)
            if (labels[size_t(v)] == headPart) { geo.headMask[size_t(v)] = 1; ++headCount; }

        // GEOMETRIC expansion: eyes / teeth / tongue / lashes are often
        // separate submeshes skinned to non-body-region bones (eye bones),
        // which the rig-prior can't label — they'd be silently EXCLUDED from
        // the face rig and never blink or look around (field-reported: 414-vert
        // eye submesh with 0 masked verts). Anything inside the labeled head's
        // slightly-expanded AABB belongs to the face.
        if (headCount >= 50) {
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (int v = 0; v < nv; ++v) {
                if (!geo.headMask[size_t(v)]) continue;
                for (int a = 0; a < 3; ++a) {
                    lo[a] = std::min(lo[a], geo.userV[size_t(v)*3 + a]);
                    hi[a] = std::max(hi[a], geo.userV[size_t(v)*3 + a]);
                }
            }
            float pad[3];
            for (int a = 0; a < 3; ++a) pad[a] = 0.05f * (hi[a] - lo[a]);
            for (int v = 0; v < nv; ++v) {
                if (geo.headMask[size_t(v)]) continue;
                bool inside = true;
                for (int a = 0; a < 3; ++a) {
                    const float p = geo.userV[size_t(v)*3 + a];
                    if (p < lo[a] - pad[a] || p > hi[a] + pad[a]) { inside = false; break; }
                }
                if (inside) { geo.headMask[size_t(v)] = 1; ++headCount; }
            }
        }

        // Only isolate when the head is a real, minority region of the mesh —
        // i.e. this looks like a full body, not a bare face. A head that IS
        // most of the mesh means it's already a face crop; fit it whole.
        if (headCount >= 50 && headCount < nv * 3 / 4)
            geo.headVertexCount = headCount;
        else
            geo.headMask.clear();   // treat as a face crop
    }
    return geo;
}

void headSubmesh(const FaceRigGeometry& geo,
                 std::vector<float>& outV, std::vector<int>& outF)
{
    outV.clear();
    outF.clear();
    const int nv = int(geo.userV.size() / 3);
    if (int(geo.headMask.size()) != nv) {
        // no head isolation — the whole mesh IS the face crop.
        outV = geo.userV;
        outF = geo.userF;
        return;
    }
    std::vector<int> fullToSub(size_t(nv), -1);
    for (int v = 0; v < nv; ++v) {
        if (!geo.headMask[size_t(v)]) continue;
        fullToSub[size_t(v)] = int(outV.size() / 3);
        outV.insert(outV.end(), {geo.userV[size_t(v)*3],
                                 geo.userV[size_t(v)*3+1],
                                 geo.userV[size_t(v)*3+2]});
    }
    for (size_t f = 0; f + 2 < geo.userF.size(); f += 3) {
        const int a = geo.userF[f], b = geo.userF[f+1], c = geo.userF[f+2];
        if (a < 0 || b < 0 || c < 0) continue;
        const int sa = fullToSub[size_t(a)], sb = fullToSub[size_t(b)],
                  sc = fullToSub[size_t(c)];
        if (sa >= 0 && sb >= 0 && sc >= 0)
            outF.insert(outF.end(), {sa, sb, sc});
    }
    if (outV.size() < 9 || outF.size() < 3) { outV = geo.userV; outF = geo.userF; }
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
        report.shapeNames.push_back(shape.name);
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

    // Facial-landmark anchors (render + detect on template AND user, pair by
    // MediaPipe index). Frame/raycast the HEAD sub-mesh so the face fills the
    // detector's frame. Empty when ONNX/model/face-detection unavailable — the
    // fit then runs unanchored (previous behaviour).
    std::vector<float> headV; std::vector<int> headF;
    headSubmesh(geo, headV, headF);
    const std::vector<NricpLandmark> anchors =
        buildLandmarkAnchors(entity, headV, headF, tmpl);

    const FaceRigResult res = buildFaceRig(geo.userV, geo.userF, tmpl, opts,
                                           geo.headMask, anchors);
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

bool writeArkitSidecar(const QString& meshPath,
                       const std::vector<QString>& shapeNames)
{
    if (meshPath.isEmpty() || shapeNames.empty()) return false;
    QJsonArray names;
    for (const QString& n : shapeNames) names.append(n);
    QJsonObject root;
    root["schema"] = QStringLiteral("qtmesh-arkit-blendshapes-v1");
    root["count"] = int(shapeNames.size());
    root["names"] = names;   // ordered, matches the mesh's morph targets

    // <mesh>.arkit.json alongside the exported mesh — recovers the ARKit names
    // that Assimp 6.0's glTF exporter drops (it doesn't emit targetNames even
    // though we set aiAnimMesh::mName). Face capture (#869) and re-import can
    // read this to rebind the mocap-52 vocabulary by index.
    QFile f(meshPath + QStringLiteral(".arkit.json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

}  // namespace FaceRig
