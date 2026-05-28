#include "UvUnwrap.h"

#include <xatlas.h>

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreHardwareIndexBuffer.h>
#include <OgreHardwareVertexBuffer.h>
#include <OgreMesh.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QTextStream>

#include <algorithm>
#include <cstring>
#include <vector>

// ── Pure-data helpers ────────────────────────────────────────────────────────

namespace {

// Pull `(positions, indices)` out of a submesh in the layout xatlas
// expects: tight float3 positions (one per vertex) and a uint32
// triangle list. Returns false if the submesh has no usable geometry.
struct SubmeshGeometry {
    std::vector<float>    positions;     // tight xyz per vertex
    std::vector<uint32_t> indices;       // triangle list
    Ogre::VertexData*     sourceVertexData = nullptr;
    bool                  ownsVertexData   = false; // true if `vertexData` is per-submesh, false if shared
};

SubmeshGeometry extractGeometry(Ogre::Mesh* mesh, unsigned si)
{
    SubmeshGeometry out;
    Ogre::SubMesh* sub = mesh->getSubMesh(si);
    if (!sub) return out;

    Ogre::VertexData* vd = sub->useSharedVertices ? mesh->sharedVertexData : sub->vertexData;
    if (!vd) return out;

    const auto* posElem = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (!posElem) return out;

    auto vbuf = vd->vertexBufferBinding->getBuffer(posElem->getSource());
    if (!vbuf || vd->vertexCount == 0) return out;

    out.positions.resize(static_cast<size_t>(vd->vertexCount) * 3);
    const size_t stride = vbuf->getVertexSize();
    auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    for (size_t i = 0; i < vd->vertexCount; ++i) {
        float* p = nullptr;
        posElem->baseVertexPointerToElement(base + i * stride, &p);
        out.positions[i * 3 + 0] = p[0];
        out.positions[i * 3 + 1] = p[1];
        out.positions[i * 3 + 2] = p[2];
    }
    vbuf->unlock();

    // Index buffer → uint32.
    if (!sub->indexData || !sub->indexData->indexBuffer || sub->indexData->indexCount == 0)
        return out;
    auto ibuf = sub->indexData->indexBuffer;
    const size_t n = sub->indexData->indexCount;
    out.indices.resize(n);
    auto* isrc = static_cast<unsigned char*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    isrc += sub->indexData->indexStart * ibuf->getIndexSize();
    if (ibuf->getType() == Ogre::HardwareIndexBuffer::IT_16BIT) {
        const auto* p = reinterpret_cast<const uint16_t*>(isrc);
        for (size_t i = 0; i < n; ++i) out.indices[i] = p[i];
    } else {
        std::memcpy(out.indices.data(), isrc, n * sizeof(uint32_t));
    }
    ibuf->unlock();

    out.sourceVertexData = vd;
    out.ownsVertexData = !sub->useSharedVertices;
    return out;
}

// Build a new VertexData for a submesh that holds the post-unwrap
// vertices. Layout: source positions/normals/colors/etc copied via
// `xref` (input vertex id), and the UV channel `channel` set to the
// xatlas output. Returns the new VertexData; caller takes ownership.
//
// `srcUvBackup` non-empty → write that into UV{channel + 1} (the
// "preserve original UVs as backup" knob).
struct UnwrappedSubmesh {
    Ogre::VertexData* vdata = nullptr;
    Ogre::IndexData*  idata = nullptr;
    std::vector<uint32_t> xrefs; // index = new vertex, value = source vertex
};

UnwrappedSubmesh buildUnwrappedSubmesh(const xatlas::Mesh& xmesh,
                                       Ogre::VertexData* src,
                                       float invAtlasW,
                                       float invAtlasH,
                                       int   uvChannel,
                                       bool  preserveBackup)
{
    UnwrappedSubmesh out;

    // 1. Clone the source vertex declaration. If the target UV
    // channel doesn't exist on the source, append it. Same for the
    // backup channel.
    auto* newVd = new Ogre::VertexData();
    auto* srcDecl = src->vertexDeclaration;
    auto* newDecl = newVd->vertexDeclaration;

    // We pack everything into a single binding (source 0) for the
    // new buffer — simpler than cloning the multi-source layout.
    size_t offset = 0;
    struct ElemCopy {
        Ogre::VertexElementSemantic semantic;
        Ogre::VertexElementType     type;
        unsigned short              srcSource;
        size_t                      srcOffset;
        size_t                      newOffset;
        unsigned short              index;        // for VES_TEXTURE_COORDINATES
    };
    std::vector<ElemCopy> copies;
    for (const auto& e : srcDecl->getElements()) {
        ElemCopy c{};
        c.semantic = e.getSemantic();
        c.type      = e.getType();
        c.srcSource = e.getSource();
        c.srcOffset = e.getOffset();
        c.newOffset = offset;
        c.index     = e.getIndex();
        newDecl->addElement(0, offset, c.type, c.semantic, c.index);
        offset += Ogre::VertexElement::getTypeSize(c.type);
        copies.push_back(c);
    }

    // Ensure the target UV channel exists. If the source already
    // has a TEXCOORDS element at `uvChannel`, we'll overwrite it.
    const auto* existingTargetUv = srcDecl->findElementBySemantic(
        Ogre::VES_TEXTURE_COORDINATES, static_cast<unsigned short>(uvChannel));
    size_t targetUvOffset = 0;
    if (!existingTargetUv) {
        targetUvOffset = offset;
        newDecl->addElement(0, offset, Ogre::VET_FLOAT2,
                            Ogre::VES_TEXTURE_COORDINATES,
                            static_cast<unsigned short>(uvChannel));
        offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT2);
    } else {
        // Find the offset we assigned for it during the copy above.
        for (const auto& c : copies) {
            if (c.semantic == Ogre::VES_TEXTURE_COORDINATES && c.index == uvChannel) {
                targetUvOffset = c.newOffset;
                break;
            }
        }
    }

    // Optional backup channel.
    size_t backupUvOffset = 0;
    bool   hasBackup      = false;
    if (preserveBackup && existingTargetUv) {
        const auto backupIdx = static_cast<unsigned short>(uvChannel + 1);
        // Only meaningful if there isn't already something at that channel.
        if (!srcDecl->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES, backupIdx)) {
            backupUvOffset = offset;
            newDecl->addElement(0, offset, Ogre::VET_FLOAT2,
                                Ogre::VES_TEXTURE_COORDINATES, backupIdx);
            offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT2);
            hasBackup = true;
        }
    }

    const size_t stride = offset;
    newVd->vertexCount = xmesh.vertexCount;

    // 2. Lock every source binding once so we can read its bytes
    // for each xref.
    std::vector<unsigned char*> srcLocks;
    std::vector<size_t> srcStrides;
    auto* srcBinding = src->vertexBufferBinding;
    const auto& bindings = srcBinding->getBindings();
    std::vector<unsigned short> bindingIndex;  // map iter pos → binding source idx
    for (const auto& kv : bindings) {
        srcLocks.push_back(static_cast<unsigned char*>(
            kv.second->lock(Ogre::HardwareBuffer::HBL_READ_ONLY)));
        srcStrides.push_back(kv.second->getVertexSize());
        bindingIndex.push_back(kv.first);
    }
    auto findSrcLock = [&](unsigned short source) -> std::pair<unsigned char*, size_t> {
        for (size_t i = 0; i < bindingIndex.size(); ++i) {
            if (bindingIndex[i] == source) return { srcLocks[i], srcStrides[i] };
        }
        return { nullptr, 0 };
    };

    // 3. Allocate the new buffer and write one vertex at a time.
    auto newBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        stride, xmesh.vertexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    auto* dst = static_cast<unsigned char*>(newBuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));

    out.xrefs.resize(xmesh.vertexCount);
    for (uint32_t i = 0; i < xmesh.vertexCount; ++i) {
        const auto& xv = xmesh.vertexArray[i];
        out.xrefs[i] = xv.xref;
        unsigned char* row = dst + i * stride;

        // Copy every source element from xref.
        for (const auto& c : copies) {
            auto [srcBase, srcStride] = findSrcLock(c.srcSource);
            if (!srcBase) continue;
            const size_t typeBytes = Ogre::VertexElement::getTypeSize(c.type);
            std::memcpy(row + c.newOffset,
                        srcBase + xv.xref * srcStride + c.srcOffset,
                        typeBytes);
        }

        // Backup channel: copy the original UV{channel} bytes from
        // xref into the new backup slot. We have to do this BEFORE
        // we overwrite the target UV slot below.
        if (hasBackup && existingTargetUv) {
            auto [srcBase, srcStride] = findSrcLock(existingTargetUv->getSource());
            if (srcBase) {
                std::memcpy(row + backupUvOffset,
                            srcBase + xv.xref * srcStride + existingTargetUv->getOffset(),
                            sizeof(float) * 2);
            }
        }

        // Target UV: xatlas's uv is in texels (atlas-pixel coords),
        // normalize to [0, 1].
        auto* uvOut = reinterpret_cast<float*>(row + targetUvOffset);
        uvOut[0] = xv.uv[0] * invAtlasW;
        uvOut[1] = xv.uv[1] * invAtlasH;
    }
    newBuf->unlock();

    for (size_t i = 0; i < srcLocks.size(); ++i) {
        auto kv = std::find_if(bindings.begin(), bindings.end(),
                               [&](const auto& p){ return p.first == bindingIndex[i]; });
        if (kv != bindings.end()) kv->second->unlock();
    }

    newVd->vertexBufferBinding->setBinding(0, newBuf);

    // 4. Build the new IndexData.
    auto* newId = new Ogre::IndexData();
    newId->indexCount = xmesh.indexCount;
    newId->indexStart = 0;

    uint32_t maxIdx = 0;
    for (uint32_t i = 0; i < xmesh.indexCount; ++i)
        if (xmesh.indexArray[i] > maxIdx) maxIdx = xmesh.indexArray[i];
    const bool use32 = maxIdx > std::numeric_limits<uint16_t>::max();
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        use32 ? Ogre::HardwareIndexBuffer::IT_32BIT : Ogre::HardwareIndexBuffer::IT_16BIT,
        xmesh.indexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    auto* idst = ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD);
    if (use32) {
        std::memcpy(idst, xmesh.indexArray, xmesh.indexCount * sizeof(uint32_t));
    } else {
        auto* p = reinterpret_cast<uint16_t*>(idst);
        for (uint32_t i = 0; i < xmesh.indexCount; ++i)
            p[i] = static_cast<uint16_t>(xmesh.indexArray[i]);
    }
    ibuf->unlock();
    newId->indexBuffer = ibuf;

    out.vdata = newVd;
    out.idata = newId;
    return out;
}

// Rebuild the bone-assignment list for a submesh against the new
// vertex IDs. xatlas adds new verts at seam splits; each new vert
// inherits the source's bone weights via xref.
void remapBoneAssignments(Ogre::SubMesh* sub,
                          const std::vector<uint32_t>& xrefs)
{
    // Snapshot the per-source-vertex bone-assignment list, then
    // emit new entries against the post-unwrap vertex ids.
    using BAList = std::multimap<size_t, Ogre::VertexBoneAssignment>;
    BAList snapshot;
    for (const auto& kv : sub->getBoneAssignments())
        snapshot.insert(kv);

    sub->clearBoneAssignments();
    for (uint32_t newIdx = 0; newIdx < xrefs.size(); ++newIdx) {
        const uint32_t src = xrefs[newIdx];
        auto range = snapshot.equal_range(src);
        for (auto it = range.first; it != range.second; ++it) {
            Ogre::VertexBoneAssignment a = it->second;
            a.vertexIndex = newIdx;
            sub->addBoneAssignment(a);
        }
    }
    sub->_compileBoneAssignments();
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────────────

UvUnwrapReport UvUnwrap::unwrapEntity(Ogre::Entity* entity,
                                      const UvUnwrapOptions& opts)
{
    UvUnwrapReport report;
    if (!entity) { report.error = QStringLiteral("null entity"); return report; }
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh)   { report.error = QStringLiteral("entity has no mesh"); return report; }

    report.meshName = QString::fromStdString(mesh->getName());
    report.submeshCount = static_cast<int>(mesh->getNumSubMeshes());

    // 1. Extract geometry, addMesh per submesh.
    xatlas::Atlas* atlas = xatlas::Create();
    std::vector<SubmeshGeometry> geoms(mesh->getNumSubMeshes());
    std::vector<bool>            addedToAtlas(mesh->getNumSubMeshes(), false);

    for (unsigned si = 0; si < mesh->getNumSubMeshes(); ++si) {
        geoms[si] = extractGeometry(mesh.get(), si);
        if (geoms[si].positions.empty() || geoms[si].indices.empty()) continue;

        const size_t vertCount = geoms[si].positions.size() / 3;
        report.verticesBefore     += static_cast<int>(vertCount);
        report.trianglesProcessed += static_cast<int>(geoms[si].indices.size() / 3);

        xatlas::MeshDecl decl;
        decl.vertexCount          = static_cast<uint32_t>(vertCount);
        decl.vertexPositionData   = geoms[si].positions.data();
        decl.vertexPositionStride = sizeof(float) * 3;
        decl.indexCount           = static_cast<uint32_t>(geoms[si].indices.size());
        decl.indexData            = geoms[si].indices.data();
        decl.indexFormat          = xatlas::IndexFormat::UInt32;

        const auto err = xatlas::AddMesh(atlas, decl);
        if (err != xatlas::AddMeshError::Success) {
            xatlas::Destroy(atlas);
            report.error = QStringLiteral("xatlas::AddMesh failed: %1")
                .arg(QString::fromLatin1(xatlas::StringForEnum(err)));
            return report;
        }
        addedToAtlas[si] = true;
    }
    xatlas::AddMeshJoin(atlas);

    // `atlas->meshCount` is populated only after Generate; until then
    // gate on the number of AddMesh calls we made ourselves.
    const int submittedMeshes = std::count(addedToAtlas.begin(), addedToAtlas.end(), true);
    if (submittedMeshes == 0) {
        xatlas::Destroy(atlas);
        report.error = QStringLiteral("no submeshes had usable geometry");
        return report;
    }

    // 2. Generate. xatlas estimates texelsPerUnit to roughly hit
    // `resolution` when texelsPerUnit is 0.
    xatlas::PackOptions pack;
    pack.resolution = static_cast<uint32_t>(std::max(64, opts.resolution));
    pack.padding    = static_cast<uint32_t>(std::max(0, opts.padding));
    pack.bilinear   = true;
    xatlas::Generate(atlas, /*chartOptions=*/{}, pack);

    report.atlasWidth  = static_cast<int>(atlas->width);
    report.atlasHeight = static_cast<int>(atlas->height);
    report.chartCount  = static_cast<int>(atlas->chartCount);
    if (atlas->utilization && atlas->atlasCount > 0) {
        // Weighted average of per-sub-atlas utilization.
        double acc = 0.0;
        for (uint32_t a = 0; a < atlas->atlasCount; ++a) acc += atlas->utilization[a];
        report.utilization = acc / static_cast<double>(atlas->atlasCount);
    }

    const float invAtlasW = atlas->width  > 0 ? 1.0f / static_cast<float>(atlas->width)  : 1.0f;
    const float invAtlasH = atlas->height > 0 ? 1.0f / static_cast<float>(atlas->height) : 1.0f;

    // 3. Commit results back to each submesh.
    uint32_t atlasMeshIdx = 0;
    for (unsigned si = 0; si < mesh->getNumSubMeshes(); ++si) {
        if (!addedToAtlas[si]) continue;
        const xatlas::Mesh& xmesh = atlas->meshes[atlasMeshIdx++];

        Ogre::SubMesh* sub = mesh->getSubMesh(si);

        // Build the unwrapped vertex + index data against the
        // source vertex layout.
        auto built = buildUnwrappedSubmesh(xmesh, geoms[si].sourceVertexData,
                                           invAtlasW, invAtlasH,
                                           opts.channel,
                                           opts.preserveOriginalAsBackup);

        // The unwrap can split shared vertices into per-submesh
        // copies. Each submesh now owns its own vertex data — flip
        // the flag and free the old per-submesh buffer.
        if (sub->vertexData && !sub->useSharedVertices) {
            delete sub->vertexData;
            sub->vertexData = nullptr;
        }
        sub->useSharedVertices = false;
        sub->vertexData = built.vdata;

        // Replace index data.
        if (sub->indexData) {
            sub->indexData->indexBuffer.reset();
            delete sub->indexData;
        }
        sub->indexData = built.idata;

        // Remap skin weights.
        remapBoneAssignments(sub, built.xrefs);

        report.verticesAfter += static_cast<int>(xmesh.vertexCount);

        // The `qtme.faces.<i>` n-gon binding is keyed on source
        // vertex indices and is now stale — drop it so the exporter
        // walks the new index buffer instead.
        mesh->getUserObjectBindings().eraseUserAny(
            std::string("qtme.faces.") + std::to_string(si));
    }

    // If we replaced per-submesh vertex data, the mesh's shared
    // vertex data is no longer referenced by any submesh — leave it
    // alone since other consumers may still hold pointers.

    xatlas::Destroy(atlas);
    report.applied = true;
    return report;
}

QList<UvUnwrap::UvInfo> UvUnwrap::infoForEntity(const Ogre::Entity* entity)
{
    QList<UvInfo> out;
    if (!entity || !entity->getMesh()) return out;
    const Ogre::MeshPtr& mesh = entity->getMesh();
    for (unsigned si = 0; si < mesh->getNumSubMeshes(); ++si) {
        const Ogre::SubMesh* sub = mesh->getSubMesh(si);
        const Ogre::VertexData* vd = sub->useSharedVertices ? mesh->sharedVertexData : sub->vertexData;
        UvInfo info;
        info.submeshIndex = static_cast<int>(si);
        if (vd) info.vertexCount = static_cast<int>(vd->vertexCount);
        if (sub->indexData) info.triangleCount = static_cast<int>(sub->indexData->indexCount / 3);

        if (vd) {
            for (const auto& e : vd->vertexDeclaration->getElements()) {
                if (e.getSemantic() == Ogre::VES_TEXTURE_COORDINATES) {
                    ++info.uvChannelCount;
                    if (e.getIndex() == 0) info.hasUv0 = true;
                }
            }

            // UV0 coverage: bbox of UV0 in [0, 1].
            const auto* uv0 = vd->vertexDeclaration->findElementBySemantic(
                Ogre::VES_TEXTURE_COORDINATES, 0);
            if (uv0 && vd->vertexCount > 0) {
                auto vbuf = vd->vertexBufferBinding->getBuffer(uv0->getSource());
                if (vbuf) {
                    const size_t stride = vbuf->getVertexSize();
                    auto* base = static_cast<unsigned char*>(
                        vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                    float umin = 1e9f, vmin = 1e9f, umax = -1e9f, vmax = -1e9f;
                    for (size_t i = 0; i < vd->vertexCount; ++i) {
                        float* p = nullptr;
                        uv0->baseVertexPointerToElement(base + i * stride, &p);
                        umin = std::min(umin, p[0]); umax = std::max(umax, p[0]);
                        vmin = std::min(vmin, p[1]); vmax = std::max(vmax, p[1]);
                    }
                    vbuf->unlock();
                    const float w = std::max(0.0f, std::min(1.0f, umax) - std::max(0.0f, umin));
                    const float h = std::max(0.0f, std::min(1.0f, vmax) - std::max(0.0f, vmin));
                    info.uv0Coverage = static_cast<double>(w) * static_cast<double>(h);
                }
            }
        }
        out.append(info);
    }
    return out;
}

QJsonObject UvUnwrap::reportToJson(const UvUnwrapReport& report)
{
    QJsonObject obj;
    obj["mesh"]               = report.meshName;
    obj["applied"]            = report.applied;
    obj["submeshCount"]       = report.submeshCount;
    obj["verticesBefore"]     = report.verticesBefore;
    obj["verticesAfter"]      = report.verticesAfter;
    obj["trianglesProcessed"] = report.trianglesProcessed;
    obj["atlasWidth"]         = report.atlasWidth;
    obj["atlasHeight"]        = report.atlasHeight;
    obj["chartCount"]         = report.chartCount;
    obj["utilization"]        = report.utilization;
    if (!report.error.isEmpty()) obj["error"] = report.error;
    return obj;
}

QString UvUnwrap::reportToText(const UvUnwrapReport& report)
{
    QString out;
    QTextStream s(&out);
    if (!report.applied) {
        s << "UV unwrap failed: " << (report.error.isEmpty() ? "(unknown)" : report.error) << "\n";
        return out;
    }
    QLocale locale;
    s << "UV Unwrap\n";
    s << "=========\n";
    s << "Mesh:          " << report.meshName << "\n";
    s << "Submeshes:     " << report.submeshCount << "\n";
    s << "Vertices:      " << locale.toString(report.verticesBefore)
      << " → " << locale.toString(report.verticesAfter)
      << "  (+" << locale.toString(report.verticesAfter - report.verticesBefore)
      << " from seam splits)\n";
    s << "Triangles:     " << locale.toString(report.trianglesProcessed) << "\n";
    s << "Atlas:         " << report.atlasWidth << "×" << report.atlasHeight << "\n";
    s << "Charts:        " << report.chartCount << "\n";
    s << "Utilization:   " << QString::number(report.utilization * 100.0, 'f', 1) << "%\n";
    return out;
}

QJsonObject UvUnwrap::infoToJson(const QString& fileName, const QList<UvInfo>& info)
{
    QJsonObject obj;
    obj["file"] = fileName;
    QJsonArray arr;
    for (const auto& i : info) {
        QJsonObject e;
        e["submeshIndex"]   = i.submeshIndex;
        e["vertexCount"]    = i.vertexCount;
        e["triangleCount"]  = i.triangleCount;
        e["uvChannelCount"] = i.uvChannelCount;
        e["hasUv0"]         = i.hasUv0;
        e["uv0Coverage"]    = i.uv0Coverage;
        arr.append(e);
    }
    obj["submeshes"] = arr;
    return obj;
}

QString UvUnwrap::infoToText(const QString& fileName, const QList<UvInfo>& info)
{
    QString out;
    QTextStream s(&out);
    s << "UV info: " << fileName << "\n";
    if (info.isEmpty()) { s << "  (no submeshes)\n"; return out; }
    for (const auto& i : info) {
        s << QStringLiteral("  [%1] verts=%2 tris=%3 uvChannels=%4 uv0=%5 coverage=%6%\n")
            .arg(i.submeshIndex)
            .arg(i.vertexCount)
            .arg(i.triangleCount)
            .arg(i.uvChannelCount)
            .arg(i.hasUv0 ? "yes" : "no")
            .arg(QString::number(i.uv0Coverage * 100.0, 'f', 1));
    }
    return out;
}
