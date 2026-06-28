#include "UvUnwrap.h"
#include "MeshImporterExporter.h"

#include <xatlas.h>

#include <QFileInfo>

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
#include <array>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

uint64_t seamEdgeKey(unsigned int a, unsigned int b)
{
    if (a > b)
        std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
}

const UvUnwrapOptions::FaceMask* faceMaskForSub(const UvUnwrapOptions& opts, unsigned si)
{
    for (const auto& mask : opts.faceMasks) {
        if (mask.subMeshIndex == si)
            return &mask;
    }
    return nullptr;
}

std::vector<uint32_t> buildFaceMaterials(const std::vector<uint32_t>& indices,
                                         const std::vector<bool>& includeTri,
                                         const std::vector<uint64_t>& seamEdges)
{
    const size_t triCount = indices.size() / 3;
    std::vector<uint32_t> triChart(triCount, 0);
    if (triCount == 0)
        return triChart;

    std::unordered_set<uint64_t> seamSet(seamEdges.begin(), seamEdges.end());

    auto triIncluded = [&](size_t t) {
        return includeTri.empty() || (t < includeTri.size() && includeTri[t]);
    };

    std::vector<std::vector<size_t>> triNeighbors(triCount);
    for (size_t t0 = 0; t0 < triCount; ++t0) {
        if (!triIncluded(t0))
            continue;
        const uint32_t v0 = indices[t0 * 3 + 0];
        const uint32_t v1 = indices[t0 * 3 + 1];
        const uint32_t v2 = indices[t0 * 3 + 2];
        const std::array<std::pair<uint32_t, uint32_t>, 3> edges{{{v0, v1}, {v1, v2}, {v2, v0}}};
        for (size_t t1 = t0 + 1; t1 < triCount; ++t1) {
            if (!triIncluded(t1))
                continue;
            const uint32_t u0 = indices[t1 * 3 + 0];
            const uint32_t u1 = indices[t1 * 3 + 1];
            const uint32_t u2 = indices[t1 * 3 + 2];
            const std::array<std::pair<uint32_t, uint32_t>, 3> edgesB{{{u0, u1}, {u1, u2}, {u2, u0}}};
            bool adjacent = false;
            for (const auto& eA : edges) {
                for (const auto& eB : edgesB) {
                    if (eA.first == eB.second && eA.second == eB.first
                        && seamSet.count(seamEdgeKey(eA.first, eA.second)) == 0) {
                        adjacent = true;
                        break;
                    }
                }
                if (adjacent)
                    break;
            }
            if (adjacent) {
                triNeighbors[t0].push_back(t1);
                triNeighbors[t1].push_back(t0);
            }
        }
    }

    uint32_t nextChart = 1;
    for (size_t start = 0; start < triCount; ++start) {
        if (!triIncluded(start) || triChart[start] != 0)
            continue;
        triChart[start] = nextChart;
        std::vector<size_t> stack{start};
        while (!stack.empty()) {
            const size_t t = stack.back();
            stack.pop_back();
            for (size_t n : triNeighbors[t]) {
                if (triChart[n] != 0)
                    continue;
                triChart[n] = nextChart;
                stack.push_back(n);
            }
        }
        ++nextChart;
    }
    return triChart;
}

std::vector<uint8_t> buildFaceIgnore(const std::vector<uint32_t>& indices,
                                     const UvUnwrapOptions::FaceMask* mask)
{
    const size_t triCount = indices.size() / 3;
    if (!mask || mask->includeTriangle.empty())
        return {};
    std::vector<uint8_t> ignore(triCount, 1);
    for (size_t i = 0; i < triCount && i < mask->includeTriangle.size(); ++i)
        ignore[i] = mask->includeTriangle[i] ? 0 : 1;
    return ignore;
}

bool readSourceUv(Ogre::VertexData* src, int channel, unsigned vertIdx, Ogre::Vector2& out)
{
    if (!src || vertIdx >= src->vertexCount)
        return false;
    const auto* uvElem = src->vertexDeclaration->findElementBySemantic(
        Ogre::VES_TEXTURE_COORDINATES, static_cast<unsigned short>(channel));
    if (!uvElem)
        return false;
    auto vbuf = src->vertexBufferBinding->getBuffer(uvElem->getSource());
    if (!vbuf)
        return false;
    auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    float* uv = nullptr;
    uvElem->baseVertexPointerToElement(base + vertIdx * vbuf->getVertexSize(), &uv);
    out.x = uv[0];
    out.y = uv[1];
    vbuf->unlock();
    return true;
}

bool writeOutputUv(Ogre::VertexData* vd, int channel, unsigned vertIdx, const Ogre::Vector2& uv)
{
    if (!vd || vertIdx >= vd->vertexCount)
        return false;
    const auto* uvElem = vd->vertexDeclaration->findElementBySemantic(
        Ogre::VES_TEXTURE_COORDINATES, static_cast<unsigned short>(channel));
    if (!uvElem)
        return false;
    auto vbuf = vd->vertexBufferBinding->getBuffer(uvElem->getSource());
    if (!vbuf)
        return false;
    auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_NORMAL));
    float* out = nullptr;
    uvElem->baseVertexPointerToElement(base + vertIdx * vbuf->getVertexSize(), &out);
    out[0] = uv.x;
    out[1] = uv.y;
    vbuf->unlock();
    return true;
}

void restoreIgnoredTriangleSourceUvs(Ogre::VertexData* newVd,
                                     const xatlas::Mesh& xmesh,
                                     Ogre::VertexData* srcVd,
                                     int uvChannel,
                                     const UvUnwrapOptions::FaceMask* mask)
{
    if (!newVd || !srcVd || !mask || mask->includeTriangle.empty())
        return;
    const size_t triCount = xmesh.indexCount / 3;
    for (size_t t = 0; t < triCount && t < mask->includeTriangle.size(); ++t) {
        if (mask->includeTriangle[t])
            continue;
        for (int k = 0; k < 3; ++k) {
            const uint32_t outV = xmesh.indexArray[t * 3 + k];
            if (outV >= xmesh.vertexCount)
                continue;
            const uint32_t srcV = xmesh.vertexArray[outV].xref;
            Ogre::Vector2 orig;
            if (readSourceUv(srcVd, uvChannel, srcV, orig))
                writeOutputUv(newVd, uvChannel, outV, orig);
        }
    }
}

} // namespace

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
                                       bool  preserveBackup,
                                       const std::unordered_map<unsigned int, Ogre::Vector2>* pinnedUvs,
                                       const UvUnwrapOptions::FaceMask* partialMask)
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
        // SKIP BLEND_INDICES / BLEND_WEIGHTS — those are derived
        // from the SubMesh's bone-assignment list and the per-mesh
        // `blendIndexToBoneIndexMap`. After we call
        // `remapBoneAssignments + _compileBoneAssignments` below,
        // Ogre re-adds these elements to the declaration AND
        // populates them in the buffer against the new vertex
        // ordering. Copying the source bytes here produces stale
        // packed indices that point at the wrong slot of the
        // rebuilt `blendIndexToBoneIndexMap` — the symptom is a
        // shattered mesh on first render (the same failure mode we
        // hit with the earlier meshopt vertex-fetch experiment).
        if (e.getSemantic() == Ogre::VES_BLEND_INDICES ||
            e.getSemantic() == Ogre::VES_BLEND_WEIGHTS) {
            continue;
        }
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
        if (pinnedUvs) {
            const auto it = pinnedUvs->find(xv.xref);
            if (it != pinnedUvs->end()) {
                uvOut[0] = it->second.x;
                uvOut[1] = it->second.y;
            }
        }
    }
    newBuf->unlock();

    for (size_t i = 0; i < srcLocks.size(); ++i) {
        auto kv = std::find_if(bindings.begin(), bindings.end(),
                               [&](const auto& p){ return p.first == bindingIndex[i]; });
        if (kv != bindings.end()) kv->second->unlock();
    }

    newVd->vertexBufferBinding->setBinding(0, newBuf);

    if (partialMask)
        restoreIgnoredTriangleSourceUvs(newVd, xmesh, src, uvChannel, partialMask);

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

// Rebuild the bone-assignment list against the new vertex IDs.
// `sourceAssignments` is the snapshot of the original mesh's or
// submesh's `VertexBoneAssignmentList` keyed by SOURCE vertex
// index. `xrefs` maps new vertex ID → source vertex ID (xatlas
// populates this as `Vertex::xref`). Each new vert inherits the
// source's weights.
//
// Splitting bone assignments by submesh: when the source mesh had
// `sharedVertexData`, every submesh originally read from the
// mesh-level `Mesh::getBoneAssignments()` list. After unwrap each
// submesh owns its own vertex data, so we need to re-emit the
// assignments per submesh — only the verts actually referenced by
// THIS submesh's new vertex buffer get assignments here, which is
// the natural consequence of looping over `xrefs` (one entry per
// new vert in THIS submesh's new buffer).
void remapBoneAssignments(
    Ogre::SubMesh* sub,
    const std::multimap<size_t, Ogre::VertexBoneAssignment>& sourceAssignments,
    const std::vector<uint32_t>& xrefs)
{
    sub->clearBoneAssignments();
    for (uint32_t newIdx = 0; newIdx < xrefs.size(); ++newIdx) {
        const uint32_t src = xrefs[newIdx];
        auto range = sourceAssignments.equal_range(src);
        for (auto it = range.first; it != range.second; ++it) {
            Ogre::VertexBoneAssignment a = it->second;
            a.vertexIndex = newIdx;
            sub->addBoneAssignment(a);
        }
    }
    sub->_compileBoneAssignments();
}

} // namespace

namespace {

// Snapshot of an Ogre::SubMesh's mutable per-submesh state, taken
// before `UvUnwrap::unwrapEntity` mutates everything in place. Used
// by `unwrapEntityToFile` to restore the live entity after export so
// the user's on-screen mesh isn't destroyed by the unwrap.
//
// We do NOT clone the underlying `Ogre::VertexData*` / `IndexData*`
// pointers — we steal them. The unwrap allocates fresh ones; on
// restore we delete the unwrap's allocations and reinstall the
// originals.
struct SubMeshSnapshot {
    Ogre::VertexData* vertexData = nullptr;
    Ogre::IndexData*  indexData  = nullptr;
    bool              useSharedVertices = true;
    std::multimap<size_t, Ogre::VertexBoneAssignment> boneAssignments;
    std::vector<unsigned short> blendIndexToBoneIndexMap;
};

struct MeshSnapshot {
    std::vector<SubMeshSnapshot> subs;
    std::multimap<size_t, Ogre::VertexBoneAssignment> meshBoneAssignments;
    std::vector<unsigned short> sharedBlendIndexToBoneIndexMap;
};

MeshSnapshot snapshotMesh(Ogre::Mesh* mesh)
{
    MeshSnapshot snap;
    if (!mesh) return snap;
    const unsigned int n = mesh->getNumSubMeshes();
    snap.subs.resize(n);
    for (unsigned int si = 0; si < n; ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        SubMeshSnapshot& s = snap.subs[si];
        // Keep the original pointers — `unwrapEntity` reads from
        // them and (when called via `unwrapEntityToFile`, with the
        // `keepOriginalBuffers` flag set) leaves them in place
        // rather than deleting them. We just install the unwrap's
        // new pointers temporarily for the export, then point the
        // submesh back at these originals.
        s.vertexData        = sub->vertexData;
        s.indexData         = sub->indexData;
        s.useSharedVertices = sub->useSharedVertices;
        for (const auto& kv : sub->getBoneAssignments())
            s.boneAssignments.insert(kv);
        s.blendIndexToBoneIndexMap = sub->blendIndexToBoneIndexMap;
    }
    for (const auto& kv : mesh->getBoneAssignments())
        snap.meshBoneAssignments.insert(kv);
    snap.sharedBlendIndexToBoneIndexMap = mesh->sharedBlendIndexToBoneIndexMap;
    return snap;
}

void restoreMesh(Ogre::Mesh* mesh, MeshSnapshot&& snap)
{
    if (!mesh) return;
    const unsigned int n = mesh->getNumSubMeshes();
    for (unsigned int si = 0; si < n && si < snap.subs.size(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        // The unwrap path (`unwrapEntity` called with
        // `keepOriginalBuffers=true`) leaks its own newly allocated
        // VertexData/IndexData rather than freeing them — so the
        // snapshot's original pointers are still valid. We delete
        // the unwrap's allocation here (it's the current pointer if
        // it's different from the snapshot's), then reinstall the
        // original.
        if (sub->vertexData && sub->vertexData != snap.subs[si].vertexData)
            delete sub->vertexData;
        if (sub->indexData && sub->indexData != snap.subs[si].indexData) {
            sub->indexData->indexBuffer.reset();
            delete sub->indexData;
        }
        sub->vertexData        = snap.subs[si].vertexData;
        sub->indexData         = snap.subs[si].indexData;
        sub->useSharedVertices = snap.subs[si].useSharedVertices;

        // Restore bone assignments BUT do NOT call
        // `_compileBoneAssignments`. The original vertex buffers
        // already hold the original packed BLEND_INDICES /
        // BLEND_WEIGHTS — we never touched the source buffers.
        // Re-compiling would re-pack those bytes against the
        // restored `mBoneAssignments` list and rebuild the
        // `blendIndexToBoneIndexMap`, which on the live entity
        // races with the active SkeletonInstance's cached state
        // and shatters the mesh on next frame. Just snap the
        // multimap back and leave the buffer untouched, then
        // restore the precomputed index map directly.
        sub->clearBoneAssignments();
        for (const auto& kv : snap.subs[si].boneAssignments)
            sub->addBoneAssignment(kv.second);
        sub->blendIndexToBoneIndexMap = snap.subs[si].blendIndexToBoneIndexMap;
    }
    mesh->clearBoneAssignments();
    for (const auto& kv : snap.meshBoneAssignments)
        mesh->addBoneAssignment(kv.second);
    mesh->sharedBlendIndexToBoneIndexMap = snap.sharedBlendIndexToBoneIndexMap;
    // Same reasoning as above — the mesh-level shared vertex
    // buffer (when applicable) holds intact original packed bone
    // data. We mustn't recompile.
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────────────

namespace {

// Body of the unwrap shared between the destructive and the
// keep-originals entry points. When `keepOriginalBuffers` is true,
// the old `VertexData` / `IndexData` pointers are NOT deleted when
// they're replaced — the caller is expected to retain them via a
// `MeshSnapshot` and free them later (or leak them intentionally
// when the unwrap output is the only thing that ends up exported).
UvUnwrapReport runUnwrap(Ogre::Entity* entity,
                         const UvUnwrapOptions& opts,
                         bool keepOriginalBuffers)
{
    UvUnwrapReport report;
    if (!entity) { report.error = QStringLiteral("null entity"); return report; }
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh)   { report.error = QStringLiteral("entity has no mesh"); return report; }

    report.meshName = QString::fromStdString(mesh->getName());
    report.submeshCount = static_cast<int>(mesh->getNumSubMeshes());

    // Snapshot the source bone-assignment lists BEFORE we mutate
    // anything. Shared-vertex meshes keep their bone assignments on
    // `Mesh::getBoneAssignments()`; per-submesh meshes keep them on
    // `SubMesh::getBoneAssignments()`. Either way we'll need to
    // re-emit them per submesh against the new vertex IDs because
    // every submesh ends up with its own (non-shared) vertex data
    // after the unwrap. Hip Hop Dancing is the canonical bug
    // repro: 11 submeshes all reading from sharedVertexData with the
    // weights on `Mesh::mBoneAssignments` — without snapshotting
    // here the upper-half submeshes ended up with zero weights and
    // rendered as if every vert was rigidly attached to bone 0.
    using BAList = std::multimap<size_t, Ogre::VertexBoneAssignment>;
    BAList sharedBoneAssignments;
    for (const auto& kv : mesh->getBoneAssignments())
        sharedBoneAssignments.insert(kv);

    // 1. Extract geometry, addMesh per submesh.
    xatlas::Atlas* atlas = xatlas::Create();
    std::vector<SubmeshGeometry> geoms(mesh->getNumSubMeshes());
    std::vector<bool>            addedToAtlas(mesh->getNumSubMeshes(), false);
    std::vector<BAList>          perSubBoneAssignments(mesh->getNumSubMeshes());
    for (unsigned si = 0; si < mesh->getNumSubMeshes(); ++si) {
        for (const auto& kv : mesh->getSubMesh(si)->getBoneAssignments())
            perSubBoneAssignments[si].insert(kv);
    }

    for (unsigned si = 0; si < mesh->getNumSubMeshes(); ++si) {
        geoms[si] = extractGeometry(mesh.get(), si);
        if (geoms[si].positions.empty() || geoms[si].indices.empty()) continue;

        const size_t vertCount = geoms[si].positions.size() / 3;
        const size_t triCount = geoms[si].indices.size() / 3;

        const UvUnwrapOptions::FaceMask* mask = faceMaskForSub(opts, si);
        std::vector<uint8_t> faceIgnore = buildFaceIgnore(geoms[si].indices, mask);
        if (mask && !faceIgnore.empty()
            && std::all_of(faceIgnore.begin(), faceIgnore.end(), [](uint8_t v) { return v != 0; })) {
            continue;
        }

        std::vector<bool> includeTri;
        if (mask)
            includeTri = mask->includeTriangle;
        const std::vector<uint64_t> emptySeams;
        const std::vector<uint64_t>& seams =
            si < opts.seamEdgeKeys.size() ? opts.seamEdgeKeys[si] : emptySeams;
        std::vector<uint32_t> faceMaterials;
        if (!seams.empty())
            faceMaterials = buildFaceMaterials(geoms[si].indices, includeTri, seams);

        report.verticesBefore     += static_cast<int>(vertCount);
        report.trianglesProcessed += static_cast<int>(triCount);

        xatlas::MeshDecl decl;
        decl.vertexCount          = static_cast<uint32_t>(vertCount);
        decl.vertexPositionData   = geoms[si].positions.data();
        decl.vertexPositionStride = sizeof(float) * 3;
        decl.indexCount           = static_cast<uint32_t>(geoms[si].indices.size());
        decl.indexData            = geoms[si].indices.data();
        decl.indexFormat          = xatlas::IndexFormat::UInt32;
        if (!faceIgnore.empty())
            decl.faceIgnoreData = reinterpret_cast<const bool*>(faceIgnore.data());
        if (!faceMaterials.empty())
            decl.faceMaterialData = faceMaterials.data();

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
        const UvUnwrapOptions::FaceMask* mask = faceMaskForSub(opts, si);
        const std::unordered_map<unsigned int, Ogre::Vector2>* pinned = nullptr;
        if (si < opts.pinnedUvs.size() && !opts.pinnedUvs[si].empty())
            pinned = &opts.pinnedUvs[si];
        auto built = buildUnwrappedSubmesh(xmesh, geoms[si].sourceVertexData,
                                           invAtlasW, invAtlasH,
                                           opts.channel,
                                           opts.preserveOriginalAsBackup,
                                           pinned,
                                           mask);

        // The unwrap can split shared vertices into per-submesh
        // copies. Each submesh now owns its own vertex data — flip
        // the flag and free the old per-submesh buffer (unless the
        // caller asked us to keep originals — they own freeing).
        if (sub->vertexData && !sub->useSharedVertices && !keepOriginalBuffers) {
            delete sub->vertexData;
        }
        sub->useSharedVertices = false;
        sub->vertexData = built.vdata;

        // Replace index data.
        if (sub->indexData && !keepOriginalBuffers) {
            sub->indexData->indexBuffer.reset();
            delete sub->indexData;
        }
        sub->indexData = built.idata;

        // Remap skin weights. If the source had shared verts, the
        // assignments lived on the Mesh; otherwise on the SubMesh.
        // Either way we re-emit them per submesh against the new
        // vertex IDs since the unwrap forces non-shared verts.
        const BAList& srcAssignments = geoms[si].ownsVertexData
            ? perSubBoneAssignments[si]
            : sharedBoneAssignments;
        remapBoneAssignments(sub, srcAssignments, built.xrefs);

        report.verticesAfter += static_cast<int>(xmesh.vertexCount);

        // The `qtme.faces.<i>` n-gon binding is keyed on source
        // vertex indices and is now stale — drop it so the exporter
        // walks the new index buffer instead.
        mesh->getUserObjectBindings().eraseUserAny(
            std::string("qtme.faces.") + std::to_string(si));
    }

    // Clear the mesh-level bone assignments — they reference the
    // OLD shared vertex IDs and are now stale. Skinning is driven
    // entirely from the per-submesh assignments we just emitted via
    // `addBoneAssignment`/`_compileBoneAssignments`. Without this,
    // the next time `Mesh::_compileBoneAssignments` runs (or
    // anything else walking the mesh-level list) it'd re-build
    // `sharedBlendIndexToBoneIndexMap` from positions that no
    // longer exist.
    mesh->clearBoneAssignments();

    // If we replaced per-submesh vertex data, the mesh's shared
    // vertex data is no longer referenced by any submesh — leave it
    // alone since other consumers may still hold pointers.

    xatlas::Destroy(atlas);
    report.applied = true;
    return report;
}

} // namespace

UvUnwrapReport UvUnwrap::unwrapEntity(Ogre::Entity* entity,
                                      const UvUnwrapOptions& opts)
{
    return runUnwrap(entity, opts, /*keepOriginalBuffers=*/false);
}

UvUnwrapReport UvUnwrap::unwrapEntityKeepingOriginals(Ogre::Entity* entity,
                                                      const UvUnwrapOptions& opts)
{
    return runUnwrap(entity, opts, /*keepOriginalBuffers=*/true);
}

UvUnwrapReport UvUnwrap::unwrapEntityToFile(Ogre::Entity* entity,
                                            const QString& outputPath,
                                            const UvUnwrapOptions& opts)
{
    UvUnwrapReport report;
    if (!entity || !entity->getMesh()) {
        report.error = QStringLiteral("null entity / no mesh"); return report;
    }
    if (outputPath.isEmpty()) {
        report.error = QStringLiteral("output path required"); return report;
    }

    Ogre::MeshPtr mesh = entity->getMesh();

    // Snapshot, unwrap, export, restore. If anything in the middle
    // throws or returns failure, the catch restores the snapshot so
    // the live entity is bit-identical to its pre-unwrap state.
    MeshSnapshot snap = snapshotMesh(mesh.get());
    bool unwrapDone = false;

    try {
        // Use the non-destructive entry point — the originals
        // captured in `snap` must remain valid until `restoreMesh`
        // reinstalls them. `unwrapEntityKeepingOriginals` leaks its
        // own freshly-allocated VertexData/IndexData on every
        // submesh; `restoreMesh` later deletes them and reinstalls
        // the snapshot's originals.
        report = unwrapEntityKeepingOriginals(entity, opts);
        unwrapDone = report.applied;
        if (!unwrapDone) {
            restoreMesh(mesh.get(), std::move(snap));
            return report;
        }

        auto* node = entity->getParentSceneNode();
        const QString fmt = MeshImporterExporter::formatFileURI(outputPath,
            QStringLiteral(""));  // not used here; we pass full path + ext
        // Re-derive the format string from extension via the same
        // mapping the CLI uses.
        const QFileInfo fi(outputPath);
        const QString ext = fi.suffix().toLower();
        QString fmtFilter;
        if      (ext == "mesh")    fmtFilter = QStringLiteral("Ogre Mesh (*.mesh)");
        else if (ext == "fbx")     fmtFilter = QStringLiteral("FBX Binary (*.fbx)");
        else if (ext == "gltf" || ext == "gltf2") fmtFilter = QStringLiteral("glTF 2.0 (*.gltf2)");
        else if (ext == "glb"  || ext == "glb2")  fmtFilter = QStringLiteral("glTF 2.0 Binary (*.glb2)");
        else if (ext == "obj")     fmtFilter = QStringLiteral("OBJ (*.obj)");
        else if (ext == "dae")     fmtFilter = QStringLiteral("Collada (*.dae)");
        else if (ext == "stl")     fmtFilter = QStringLiteral("STL (*.stl)");
        else if (ext == "ply")     fmtFilter = QStringLiteral("PLY (*.ply)");
        else                       fmtFilter = QStringLiteral("Ogre Mesh (*.mesh)");

        if (MeshImporterExporter::exporter(node, fi.absoluteFilePath(), fmtFilter) != 0) {
            report.applied = false;
            report.error   = QStringLiteral("export failed");
        }
    } catch (const Ogre::Exception& e) {
        report.applied = false;
        report.error   = QString::fromStdString(e.getFullDescription());
    } catch (const std::exception& e) {
        report.applied = false;
        report.error   = QString::fromUtf8(e.what());
    }

    // Always restore — the live entity must look unchanged regardless
    // of whether the export succeeded.
    restoreMesh(mesh.get(), std::move(snap));

    // Reload the mesh in Ogre so any cached state in the active
    // SkeletonInstance / hardware blend buffer is rebuilt against
    // the restored buffers on the next frame.
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
