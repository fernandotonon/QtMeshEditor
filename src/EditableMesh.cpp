/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

#include "EditableMesh.h"
#include "SubMeshTransform.h"
#include <OgreSubEntity.h>
#include <algorithm>
#include <iterator>
#include <limits>
#include <cmath>
#include <cstring>

// Assimp re-import path (loadFromAssimpFile) — drops aiProcess_Triangulate
// so source n-gons survive into EditableSubMesh::faces. Quad migration #326,
// chunk 3.
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

void triangulateFaces(EditableSubMesh& sub)
{
    sub.triangles.clear();
    if (sub.faces.empty()) return;

    // Best-effort capacity hint: every face emits N - 2 triangles, so
    // for a quad-dominant mesh the average is ~2× the face count. The
    // caller may have any mix of polygon sizes, so we don't try to be
    // exact here.
    sub.triangles.reserve(sub.faces.size() * 2);

    for (const auto& face : sub.faces) {
        if (!face.isValid()) continue;
        const auto& idx = face.indices;
        for (size_t i = 1; i + 1 < idx.size(); ++i) {
            EditableTriangle t;
            t.indices[0] = idx[0];
            t.indices[1] = idx[i];
            t.indices[2] = idx[i + 1];
            sub.triangles.push_back(t);
        }
    }
}

void promoteTrianglesToFaces(EditableSubMesh& sub)
{
    sub.faces.clear();
    sub.faces.reserve(sub.triangles.size());
    for (const auto& tri : sub.triangles) {
        EditableFace f;
        f.indices = {tri.indices[0], tri.indices[1], tri.indices[2]};
        sub.faces.push_back(std::move(f));
    }
    // Note: we deliberately do NOT call triangulateFaces() here. The
    // existing `triangles` already mirrors what `faces` would generate
    // (each face is a single triangle), so the canonical-faces invariant
    // already holds.
}

bool EditableMesh::loadFromEntity(Ogre::Entity* entity)
{
    if (!entity)
        return false;
    Ogre::MeshPtr meshPtr = entity->getMesh();
    if (!meshPtr)
        return false;
    m_sourceEntity = entity;
    return loadFromMesh(meshPtr);
}

bool EditableMesh::loadFromMesh(const Ogre::MeshPtr& meshPtr)
{
    if (!meshPtr)
        return false;

    m_subMeshes.clear();

    Ogre::Mesh* mesh = meshPtr.get();

    // Read shared vertex data once if any submesh uses it
    std::vector<EditableVertex> sharedVertices;
    bool hasSharedVertexData = (mesh->sharedVertexData != nullptr);
    if (hasSharedVertexData) {
        readVertexData(mesh->sharedVertexData, sharedVertices);
    }

    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
        Ogre::SubMesh* subMesh = mesh->getSubMesh(i);
        EditableSubMesh editSub;

        editSub.usesSharedVertices = subMesh->useSharedVertices;
        editSub.materialName = subMesh->getMaterialName();

        if (subMesh->useSharedVertices) {
            editSub.vertices = sharedVertices;
        } else {
            readVertexData(subMesh->vertexData, editSub.vertices);
        }

        if (mesh->hasSkeleton()) {
            const Ogre::SubMesh::VertexBoneAssignmentList& boneAssignments =
                subMesh->useSharedVertices ? mesh->getBoneAssignments() : subMesh->getBoneAssignments();

            for (auto it = boneAssignments.begin(); it != boneAssignments.end(); ++it) {
                const Ogre::VertexBoneAssignment& vba = it->second;
                if (vba.vertexIndex < editSub.vertices.size()) {
                    EditableBoneAssignment eba;
                    eba.boneIndex = vba.boneIndex;
                    eba.weight = vba.weight;
                    editSub.vertices[vba.vertexIndex].boneAssignments.push_back(eba);
                }
            }
        }

        if (subMesh->indexData) {
            readIndexData(subMesh->indexData, editSub.triangles);
        }

        m_subMeshes.push_back(std::move(editSub));
    }

    return true;
}

bool EditableMesh::loadFromAssimpFile(const std::string& path,
                                      bool convertToLeftHanded)
{
    if (path.empty()) return false;

    // Spin up an independent Assimp::Importer so we don't disturb the
    // existing AssimpToOgreImporter pipeline. Drop aiProcess_Triangulate
    // so source quads survive into aiMesh::mFaces. Keep the rest of the
    // post-processing aligned with the rendering importer so vertex
    // attributes don't drift between the two views — including the
    // ConvertToLeftHanded flip that AssimpToOgreImporter applies to
    // every non-.x asset. Without matching that flag here, the
    // editable mesh would end up mirrored (X flipped) relative to the
    // rendered Ogre mesh, and the vertex/edge/face overlays would draw
    // on the wrong side of the on-screen geometry. (Chunk 4 fix.)
    // Tangent handling: we deliberately do NOT request
    // aiProcess_CalcTangentSpace here because that flag implicitly
    // triangulates the mesh in Assimp, defeating the whole point of
    // this n-gon-aware path. Instead, when tangents aren't already
    // in the source file, the post-edit GPU upload pipeline rebuilds
    // them via Ogre::Mesh::buildTangentVectors (run from
    // applyNormalMapsToEntity / rewriteEntityAfterTopologyChange),
    // which operates on the fan-triangulated index buffer and produces
    // correct per-vertex tangents without disturbing `faces`.
    Assimp::Importer importer;
    unsigned int flags =
        aiProcess_JoinIdenticalVertices |
        aiProcess_GenSmoothNormals |
        aiProcess_ValidateDataStructure |
        aiProcess_LimitBoneWeights |
        aiProcess_GlobalScale;
    if (convertToLeftHanded) flags |= aiProcess_ConvertToLeftHanded;
    const aiScene* scene = importer.ReadFile(path, flags);
    if (!scene || !scene->mRootNode || scene->mNumMeshes == 0) {
        Ogre::LogManager::getSingleton().logMessage(
            "EditableMesh::loadFromAssimpFile: re-import failed for '" + path
            + "' — " + std::string(importer.GetErrorString()));
        return false;
    }

    m_subMeshes.clear();
    m_subMeshes.reserve(scene->mNumMeshes);

    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* aim = scene->mMeshes[m];
        if (!aim || aim->mNumVertices == 0) continue;

        EditableSubMesh sub;
        sub.usesSharedVertices = false;
        // Material name is left empty here — the live Ogre::Mesh in the
        // scene already carries the right material per submesh, and
        // EditModeController doesn't write material assignments back
        // through this path.
        sub.materialName.clear();

        // Vertices.
        sub.vertices.resize(aim->mNumVertices);
        const bool hasNormals = aim->HasNormals();
        const bool hasUVs = aim->HasTextureCoords(0);
        const bool hasColors = aim->HasVertexColors(0);
        const bool hasTangents = aim->HasTangentsAndBitangents();
        for (unsigned i = 0; i < aim->mNumVertices; ++i) {
            EditableVertex& ev = sub.vertices[i];
            const aiVector3D& p = aim->mVertices[i];
            ev.position = Ogre::Vector3(p.x, p.y, p.z);
            if (hasNormals) {
                const aiVector3D& n = aim->mNormals[i];
                ev.normal = Ogre::Vector3(n.x, n.y, n.z);
                ev.hasNormal = true;
            }
            if (hasUVs) {
                const aiVector3D& t = aim->mTextureCoords[0][i];
                ev.uv = Ogre::Vector2(t.x, t.y);
                ev.hasUV = true;
            }
            if (hasColors) {
                const aiColor4D& c = aim->mColors[0][i];
                ev.color = Ogre::ColourValue(c.r, c.g, c.b, c.a);
                ev.hasColor = true;
            }
            if (hasTangents) {
                const aiVector3D& t = aim->mTangents[i];
                // RTSS expects FLOAT4 tangents with handedness in w.
                // Compute parity from the input bitangent: if cross
                // (normal × tangent) aligns with bitangent, parity =
                // +1, else -1. Same convention MeshProcessor /
                // applyNormalMapsToEntity use.
                const aiVector3D& bt = aim->mBitangents[i];
                Ogre::Vector3 normalV = ev.hasNormal ? ev.normal : Ogre::Vector3::UNIT_Z;
                Ogre::Vector3 tangentV(t.x, t.y, t.z);
                Ogre::Vector3 expectedBT = normalV.crossProduct(tangentV);
                float parity = expectedBT.dotProduct(
                    Ogre::Vector3(bt.x, bt.y, bt.z)) >= 0.0f ? 1.0f : -1.0f;
                ev.tangent = Ogre::Vector4(t.x, t.y, t.z, parity);
                ev.hasTangent = true;
            }
        }

        // Bone weights, if any.
        if (aim->mNumBones > 0) {
            for (unsigned b = 0; b < aim->mNumBones; ++b) {
                const aiBone* bone = aim->mBones[b];
                if (!bone) continue;
                for (unsigned w = 0; w < bone->mNumWeights; ++w) {
                    const aiVertexWeight& vw = bone->mWeights[w];
                    if (vw.mVertexId >= sub.vertices.size()) continue;
                    EditableBoneAssignment eba;
                    eba.boneIndex = static_cast<unsigned short>(b);
                    eba.weight = vw.mWeight;
                    sub.vertices[vw.mVertexId].boneAssignments.push_back(eba);
                }
            }
        }

        // Faces — this is the whole point of this method. Without
        // aiProcess_Triangulate, aiMesh::mFaces retains the original
        // polygon structure (3 / 4 / N indices per face). Build
        // `EditableFace` directly; chunks 1+2 take care of GPU upload.
        sub.faces.reserve(aim->mNumFaces);
        bool sawNGon = false;
        for (unsigned f = 0; f < aim->mNumFaces; ++f) {
            const aiFace& face = aim->mFaces[f];
            if (face.mNumIndices < 3) continue; // points / lines — skip
            EditableFace ef;
            ef.indices.reserve(face.mNumIndices);
            bool inRange = true;
            for (unsigned k = 0; k < face.mNumIndices; ++k) {
                if (face.mIndices[k] >= aim->mNumVertices) {
                    inRange = false;
                    break;
                }
                ef.indices.push_back(face.mIndices[k]);
            }
            if (!inRange) continue;
            if (face.mNumIndices > 3) sawNGon = true;
            sub.faces.push_back(std::move(ef));
        }

        // Always populate `triangles` as the fan-triangulated mirror so
        // legacy consumers (the GPU upload path before chunk 2's defensive
        // resync, the normal-recalc path on triangle-only submeshes,
        // every existing topology op) keep working unchanged.
        triangulateFaces(sub);

        // Honour the chunk-1 invariant: leave `faces` empty when every
        // face was a triangle, so triangle-only assets don't surface as
        // n-gon submeshes downstream.
        if (!sawNGon) sub.faces.clear();

        m_subMeshes.push_back(std::move(sub));
    }

    return !m_subMeshes.empty();
}

void EditableMesh::collapseToSingleSubmeshAndWeld(float tolerance)
{
    if (m_subMeshes.size() <= 1) {
        weldByPosition(tolerance);
        return;
    }

    EditableSubMesh merged;
    merged.materialName = m_subMeshes[0].materialName;
    merged.usesSharedVertices = false;

    for (auto& sub : m_subMeshes) {
        size_t offset = merged.vertices.size();
        merged.vertices.insert(merged.vertices.end(),
            std::make_move_iterator(sub.vertices.begin()),
            std::make_move_iterator(sub.vertices.end()));
        for (auto& tri : sub.triangles) {
            EditableTriangle t;
            t.indices[0] = static_cast<unsigned int>(tri.indices[0] + offset);
            t.indices[1] = static_cast<unsigned int>(tri.indices[1] + offset);
            t.indices[2] = static_cast<unsigned int>(tri.indices[2] + offset);
            merged.triangles.push_back(t);
        }
    }

    m_subMeshes.clear();
    m_subMeshes.push_back(std::move(merged));

    weldByPosition(tolerance);
}

void EditableMesh::weldByPosition(float tolerance)
{
    for (auto& sub : m_subMeshes) {
        if (sub.vertices.empty())
            continue;

        // For each vertex, find the earliest vertex within tolerance (if any)
        // and remap to it. O(n^2) but n is small for primitive meshes.
        std::vector<size_t> remap(sub.vertices.size());
        std::vector<bool> keep(sub.vertices.size(), true);
        for (size_t i = 0; i < sub.vertices.size(); ++i)
            remap[i] = i;

        // NOTE: `tolerance` is a SQUARED distance — it's compared directly
        // against squaredDistance without another square. Default 1e-8
        // corresponds to a 1e-4 welding radius in linear units.
        for (size_t i = 0; i < sub.vertices.size(); ++i) {
            if (!keep[i]) continue;
            for (size_t j = i + 1; j < sub.vertices.size(); ++j) {
                if (!keep[j]) continue;
                float d2 = sub.vertices[i].position.squaredDistance(sub.vertices[j].position);
                if (d2 <= tolerance) {
                    remap[j] = i;
                    keep[j] = false;
                }
            }
        }

        // Build new vertex array, preserving the first-seen attributes for
        // each welded group. Compact the remap so indices are contiguous.
        std::vector<size_t> compact(sub.vertices.size(), 0);
        std::vector<EditableVertex> newVerts;
        newVerts.reserve(sub.vertices.size());
        for (size_t i = 0; i < sub.vertices.size(); ++i) {
            if (keep[i]) {
                compact[i] = newVerts.size();
                newVerts.push_back(sub.vertices[i]);
            }
        }
        // Second pass: redirect non-kept entries through their representative.
        for (size_t i = 0; i < sub.vertices.size(); ++i) {
            if (!keep[i])
                compact[i] = compact[remap[i]];
        }

        // Rewrite triangle indices
        for (auto& tri : sub.triangles) {
            for (int k = 0; k < 3; ++k) {
                size_t oldIdx = tri.indices[k];
                if (oldIdx < compact.size())
                    tri.indices[k] = static_cast<unsigned int>(compact[oldIdx]);
            }
        }

        sub.vertices = std::move(newVerts);

        // Drop degenerate triangles (any two indices equal) — welding can
        // collapse a triangle if two of its corners merge.
        sub.triangles.erase(
            std::remove_if(sub.triangles.begin(), sub.triangles.end(),
                [](const EditableTriangle& t) {
                    return t.indices[0] == t.indices[1] ||
                           t.indices[1] == t.indices[2] ||
                           t.indices[0] == t.indices[2];
                }),
            sub.triangles.end());
    }
}

bool EditableMesh::commitToEntity(Ogre::Entity* entity)
{
    if (!entity)
        return false;

    Ogre::MeshPtr meshPtr = entity->getMesh();
    if (!meshPtr)
        return false;

    Ogre::Mesh* mesh = meshPtr.get();

    if (m_subMeshes.size() != static_cast<size_t>(mesh->getNumSubMeshes()))
        return false;

    // Recalculate normals before writing back (respects current mode)
    if (m_flatNormals)
        recalculateNormalsFlat();
    else
        recalculateNormals();

    // For submeshes that use shared vertices, write the first such submesh's
    // data back to the shared buffer (all such submeshes share the same vertex data).
    bool wroteShared = false;

    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
        Ogre::SubMesh* subMesh = mesh->getSubMesh(i);
        const EditableSubMesh& editSub = m_subMeshes[i];

        if (subMesh->useSharedVertices) {
            if (!wroteShared && mesh->sharedVertexData) {
                writeVertexData(mesh->sharedVertexData, editSub.vertices);
                wroteShared = true;
            }
        } else {
            if (subMesh->vertexData) {
                writeVertexData(subMesh->vertexData, editSub.vertices);
            }
        }
    }

    // Recalculate mesh bounds
    SubMeshTransform::recalculateMeshBounds(mesh);

    // For skeletal entities, also update the SubEntity animation blend buffers
    // (Ogre renders from these, not the mesh VBO). Same pattern as SubMeshTransform.
    if (entity->hasSkeleton()) {
        for (unsigned short i = 0; i < entity->getNumSubEntities(); ++i) {
            Ogre::SubEntity* sub = entity->getSubEntity(i);
            Ogre::VertexData* animData = sub->_getSkelAnimVertexData();
            if (!animData || animData->vertexCount == 0) continue;

            const auto* posElem = animData->vertexDeclaration
                ->findElementBySemantic(Ogre::VES_POSITION);
            if (!posElem) continue;

            unsigned short source = posElem->getSource();
            auto animBuf = animData->vertexBufferBinding->getBuffer(source);
            size_t animBufSize = animBuf->getSizeInBytes();
            size_t animVertSize = animBuf->getVertexSize();

            std::vector<unsigned char> animCopy(animBufSize);
            animBuf->readData(0, animBufSize, animCopy.data());

            const auto& verts = m_subMeshes[i].vertices;
            size_t count = std::min(verts.size(), static_cast<size_t>(animData->vertexCount));
            for (size_t j = 0; j < count; ++j) {
                Ogre::Real* pReal;
                posElem->baseVertexPointerToElement(animCopy.data() + j * animVertSize, &pReal);
                pReal[0] = verts[j].position.x;
                pReal[1] = verts[j].position.y;
                pReal[2] = verts[j].position.z;
            }

            auto* dest = static_cast<unsigned char*>(
                animBuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
            memcpy(dest, animCopy.data(), animBufSize);
            animBuf->unlock();
        }
    }

    // Clear the cached source-file path: the live GPU buffers have
    // diverged from the imported asset. Subsequent enterEditMode calls
    // must use the legacy loadFromEntity path so user edits aren't
    // discarded by an n-gon re-import. (Quad migration #326, chunk 4.)
    mesh->getUserObjectBindings().eraseUserAny("qtme.source_path");
    mesh->getUserObjectBindings().eraseUserAny("qtme.source_convert_lh");

    return true;
}

void EditableMesh::buildSubMeshBuffers(Ogre::SubMesh* subMesh,
                                       const EditableSubMesh& editSubIn)
{
    if (!subMesh || editSubIn.vertices.empty()) return;

    // n-gon synchronisation: if the caller populated `faces`, that's
    // canonical and `triangles` is meant to be a fan-triangulation
    // mirror. Re-triangulate defensively here so the GPU buffer always
    // matches the live face data even if the caller forgot to call
    // `triangulateFaces()` after mutating `faces`.
    //
    // We work on a local copy when re-triangulating is needed so the
    // input EditableSubMesh stays untouched (this method takes the
    // submesh by const&). Triangle-only submeshes incur no copy.
    EditableSubMesh local;
    const EditableSubMesh* editSub = &editSubIn;
    if (!editSubIn.faces.empty()) {
        local.vertices = editSubIn.vertices; // shallow-but-fine — we don't write
        local.faces = editSubIn.faces;
        local.materialName = editSubIn.materialName;
        local.usesSharedVertices = editSubIn.usesSharedVertices;
        triangulateFaces(local);
        editSub = &local;
    }
    if (editSub->triangles.empty()) return;

    // Replace any existing vertex data with a fresh one.
    if (subMesh->vertexData) delete subMesh->vertexData;
    subMesh->useSharedVertices = false;
    subMesh->vertexData = new Ogre::VertexData();
    subMesh->vertexData->vertexCount = static_cast<uint32_t>(editSub->vertices.size());

    auto* decl = subMesh->vertexData->vertexDeclaration;
    auto* binding = subMesh->vertexData->vertexBufferBinding;

    // Build declaration: position, optional normal, uv, tangent (FLOAT4 with parity).
    size_t offset = 0;
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);

    bool hasNormals = editSub->vertices[0].hasNormal;
    if (hasNormals) {
        decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
        offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    }

    bool hasUVs = editSub->vertices[0].hasUV;
    if (hasUVs) {
        decl->addElement(0, offset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);
        offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT2);
    }

    bool hasTangents = editSub->vertices[0].hasTangent;
    if (hasTangents) {
        decl->addElement(0, offset, Ogre::VET_FLOAT4, Ogre::VES_TANGENT);
        offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT4);
    }

    // Create interleaved vertex buffer.
    size_t vertSize = decl->getVertexSize(0);
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        vertSize, editSub->vertices.size(),
        Ogre::HardwareBuffer::HBU_DYNAMIC_WRITE_ONLY, true);

    auto* dest = static_cast<float*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
    for (const auto& v : editSub->vertices) {
        *dest++ = v.position.x;
        *dest++ = v.position.y;
        *dest++ = v.position.z;
        if (hasNormals) {
            *dest++ = v.normal.x; *dest++ = v.normal.y; *dest++ = v.normal.z;
        }
        if (hasUVs) {
            *dest++ = v.uv.x; *dest++ = v.uv.y;
        }
        if (hasTangents) {
            *dest++ = v.tangent.x; *dest++ = v.tangent.y;
            *dest++ = v.tangent.z; *dest++ = v.tangent.w;
        }
    }
    vbuf->unlock();
    binding->setBinding(0, vbuf);

    // Create index buffer (16-bit if possible, else 32-bit).
    bool use32bit = editSub->vertices.size() > 65535;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        use32bit ? Ogre::HardwareIndexBuffer::IT_32BIT : Ogre::HardwareIndexBuffer::IT_16BIT,
        editSub->triangles.size() * 3,
        Ogre::HardwareBuffer::HBU_DYNAMIC_WRITE_ONLY, true);

    if (use32bit) {
        auto* idx = static_cast<uint32_t*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for (const auto& tri : editSub->triangles) {
            *idx++ = tri.indices[0];
            *idx++ = tri.indices[1];
            *idx++ = tri.indices[2];
        }
    } else {
        auto* idx = static_cast<uint16_t*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for (const auto& tri : editSub->triangles) {
            *idx++ = static_cast<uint16_t>(tri.indices[0]);
            *idx++ = static_cast<uint16_t>(tri.indices[1]);
            *idx++ = static_cast<uint16_t>(tri.indices[2]);
        }
    }
    ibuf->unlock();

    subMesh->indexData->indexBuffer = ibuf;
    subMesh->indexData->indexCount = static_cast<uint32_t>(editSub->triangles.size() * 3);
    subMesh->indexData->indexStart = 0;
}

bool EditableMesh::resizeEntityBuffers(Ogre::Entity* entity)
{
    if (!entity) return false;
    Ogre::MeshPtr meshPtr = entity->getMesh();
    if (!meshPtr) return false;
    Ogre::Mesh* mesh = meshPtr.get();

    if (m_subMeshes.size() != static_cast<size_t>(mesh->getNumSubMeshes()))
        return false;

    // Note: callers are responsible for setting normals/tangents before
    // calling this. We do NOT recalculate normals or tangents here because
    // topology ops (extrude, etc.) need to preserve original normals AND
    // tangents on unchanged vertices to avoid visible lighting shifts.

    // EditableMesh stores per-submesh vertex arrays. Migrate away from any
    // shared vertex data — each submesh gets its own fresh VertexData. This
    // avoids multiple submeshes overwriting the shared buffer.
    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
        buildSubMeshBuffers(mesh->getSubMesh(i), m_subMeshes[i]);
    }

    // Check if any submesh still uses shared vertex data. If not, we can
    // safely delete the shared vertex data.
    bool anyShared = false;
    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
        if (mesh->getSubMesh(i)->useSharedVertices) {
            anyShared = true;
            break;
        }
    }
    if (!anyShared && mesh->sharedVertexData) {
        delete mesh->sharedVertexData;
        mesh->sharedVertexData = nullptr;
    }

    // Re-register bone assignments with Ogre and compile them into blend
    // indices / weights in the vertex buffer. This is required for skeletal
    // skinning to work after a topology change.
    if (mesh->hasSkeleton()) {
        // Clear the top-level (shared) bone assignments — they referred to
        // the old shared buffer's vertex indices which are now invalid.
        mesh->clearBoneAssignments();

        for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
            Ogre::SubMesh* subMesh = mesh->getSubMesh(i);
            const EditableSubMesh& editSub = m_subMeshes[i];

            subMesh->clearBoneAssignments();

            for (size_t vi = 0; vi < editSub.vertices.size(); ++vi) {
                for (const auto& ba : editSub.vertices[vi].boneAssignments) {
                    Ogre::VertexBoneAssignment vba;
                    vba.vertexIndex = static_cast<unsigned int>(vi);
                    vba.boneIndex = ba.boneIndex;
                    vba.weight = ba.weight;
                    subMesh->addBoneAssignment(vba);
                }
            }
        }

        // Compile bone assignments into the vertex buffer.
        // Ogre will add VES_BLEND_INDICES and VES_BLEND_WEIGHTS elements
        // to the declaration and pack up to OGRE_MAX_BLEND_WEIGHTS bones
        // per vertex into a new buffer source.
        mesh->_compileBoneAssignments();
    }

    // Tangents are preserved directly through EditableVertex::tangent, so
    // there is no need to call Mesh::buildTangentVectors() here (doing so
    // would replace the source-file tangents with Ogre's computed ones,
    // causing a visible lighting shift on first edit).

    // Recalculate bounds
    SubMeshTransform::recalculateMeshBounds(mesh);

    // Topology has changed — same rationale as commitToEntity above.
    mesh->getUserObjectBindings().eraseUserAny("qtme.source_path");
    mesh->getUserObjectBindings().eraseUserAny("qtme.source_convert_lh");

    return true;
}

Ogre::MeshPtr EditableMesh::createNewMesh(const std::string& baseName)
{
    // Recalculate normals
    if (m_flatNormals)
        recalculateNormalsFlat();
    else
        recalculateNormals();

    // Generate a unique name
    static int counter = 0;
    std::string meshName = baseName + "_edited_" + std::to_string(++counter);

    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        meshName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    for (size_t s = 0; s < m_subMeshes.size(); ++s) {
        const auto& editSub = m_subMeshes[s];
        if (editSub.vertices.empty() || editSub.triangles.empty())
            continue;

        auto* subMesh = mesh->createSubMesh();
        subMesh->setMaterialName(editSub.materialName);
        buildSubMeshBuffers(subMesh, editSub);
    }

    Ogre::AxisAlignedBox bounds = calculateBounds();
    mesh->_setBounds(bounds);
    mesh->_setBoundingSphereRadius(
        (bounds.getMaximum() - bounds.getMinimum()).length() / 2.0f);
    mesh->load();

    return mesh;
}

size_t EditableMesh::totalVertexCount() const
{
    size_t total = 0;
    for (const auto& sub : m_subMeshes)
        total += sub.vertices.size();
    return total;
}

size_t EditableMesh::totalTriangleCount() const
{
    size_t total = 0;
    for (const auto& sub : m_subMeshes)
        total += sub.triangles.size();
    return total;
}

size_t totalFaceCount(const std::vector<EditableSubMesh>& subMeshes)
{
    size_t total = 0;
    for (const auto& sub : subMeshes) {
        total += sub.faces.empty() ? sub.triangles.size() : sub.faces.size();
    }
    return total;
}

void syncTriangulation(std::vector<EditableSubMesh>& subMeshes)
{
    for (auto& sub : subMeshes) {
        if (!sub.faces.empty()) triangulateFaces(sub);
    }
}

int faceIndexForTriangle(const EditableSubMesh& sub,
                         size_t localTri,
                         size_t* outFirstTri,
                         size_t* outTriCount)
{
    if (sub.faces.empty()) {
        // Legacy triangle-only submesh: every triangle IS its own face.
        if (outFirstTri) *outFirstTri = localTri;
        if (outTriCount) *outTriCount = 1;
        return -1;
    }
    // Walk the face array, accumulating each face's fan-triangulation
    // length until we contain `localTri`. The chunk-1 invariant says
    // `triangulateFaces` emits faces in order, each face producing
    // (vertexCount - 2) triangles.
    size_t running = 0;
    for (size_t k = 0; k < sub.faces.size(); ++k) {
        const auto& f = sub.faces[k];
        const size_t n = f.indices.size();
        if (n < 3) continue; // invalid face — triangulateFaces skipped it
        const size_t triCount = n - 2;
        if (localTri < running + triCount) {
            if (outFirstTri) *outFirstTri = running;
            if (outTriCount) *outTriCount = triCount;
            return static_cast<int>(k);
        }
        running += triCount;
    }
    // Out-of-range — defensive fallback to single-triangle behaviour.
    if (outFirstTri) *outFirstTri = localTri;
    if (outTriCount) *outTriCount = 1;
    return -1;
}

void EditableMesh::setVertexPosition(size_t subMeshIndex, size_t vertexIndex, const Ogre::Vector3& pos)
{
    if (subMeshIndex < m_subMeshes.size() && vertexIndex < m_subMeshes[subMeshIndex].vertices.size())
        m_subMeshes[subMeshIndex].vertices[vertexIndex].position = pos;
}

Ogre::Vector3 EditableMesh::getVertexPosition(size_t subMeshIndex, size_t vertexIndex) const
{
    if (subMeshIndex < m_subMeshes.size() && vertexIndex < m_subMeshes[subMeshIndex].vertices.size())
        return m_subMeshes[subMeshIndex].vertices[vertexIndex].position;
    return Ogre::Vector3::ZERO;
}

void EditableMesh::setVertexNormal(size_t subMeshIndex, size_t vertexIndex, const Ogre::Vector3& normal)
{
    if (subMeshIndex < m_subMeshes.size() && vertexIndex < m_subMeshes[subMeshIndex].vertices.size()) {
        m_subMeshes[subMeshIndex].vertices[vertexIndex].normal = normal;
        m_subMeshes[subMeshIndex].vertices[vertexIndex].hasNormal = true;
    }
}

Ogre::Vector3 EditableMesh::getVertexNormal(size_t subMeshIndex, size_t vertexIndex) const
{
    if (subMeshIndex < m_subMeshes.size() && vertexIndex < m_subMeshes[subMeshIndex].vertices.size())
        return m_subMeshes[subMeshIndex].vertices[vertexIndex].normal;
    return Ogre::Vector3::ZERO;
}

void EditableMesh::setVertexUV(size_t subMeshIndex, size_t vertexIndex, const Ogre::Vector2& uv)
{
    if (subMeshIndex < m_subMeshes.size() && vertexIndex < m_subMeshes[subMeshIndex].vertices.size()) {
        m_subMeshes[subMeshIndex].vertices[vertexIndex].uv = uv;
        m_subMeshes[subMeshIndex].vertices[vertexIndex].hasUV = true;
    }
}

Ogre::Vector2 EditableMesh::getVertexUV(size_t subMeshIndex, size_t vertexIndex) const
{
    if (subMeshIndex < m_subMeshes.size() && vertexIndex < m_subMeshes[subMeshIndex].vertices.size())
        return m_subMeshes[subMeshIndex].vertices[vertexIndex].uv;
    return Ogre::Vector2::ZERO;
}

void EditableMesh::recalculateNormals()
{
    for (auto& sub : m_subMeshes) {
        // n-gon sync: when faces is canonical, refresh the triangle
        // mirror so callers that consume `triangles` (GPU upload,
        // legacy ops) stay in sync with `faces`. Triangle-only
        // submeshes pay nothing here.
        if (!sub.faces.empty()) triangulateFaces(sub);

        // Zero out all normals
        for (auto& v : sub.vertices) {
            v.normal = Ogre::Vector3::ZERO;
            v.hasNormal = true;
        }

        // Always walk triangles for normal accumulation, even on
        // n-gon submeshes. The Newell-per-polygon variant produced
        // visibly correct flat shading on planar quads but broke
        // bump-map / lighting on bumped meshes (likely a sign /
        // magnitude convention mismatch with the rest of Ogre's
        // pipeline). Reverting to the always-tri loop gets parity
        // with what extrude / bevel / merge always used.
        for (const auto& tri : sub.triangles) {
            if (tri.indices[0] >= sub.vertices.size() ||
                tri.indices[1] >= sub.vertices.size() ||
                tri.indices[2] >= sub.vertices.size())
                continue;

            const Ogre::Vector3& v0 = sub.vertices[tri.indices[0]].position;
            const Ogre::Vector3& v1 = sub.vertices[tri.indices[1]].position;
            const Ogre::Vector3& v2 = sub.vertices[tri.indices[2]].position;

            Ogre::Vector3 faceNormal = (v1 - v0).crossProduct(v2 - v0);

            sub.vertices[tri.indices[0]].normal += faceNormal;
            sub.vertices[tri.indices[1]].normal += faceNormal;
            sub.vertices[tri.indices[2]].normal += faceNormal;
        }

        // Normalize
        for (auto& v : sub.vertices) {
            Ogre::Real len = v.normal.length();
            if (len > 1e-8f)
                v.normal /= len;
        }
    }
}

void EditableMesh::recalculateNormalsFlat()
{
    for (auto& sub : m_subMeshes) {
        if (!sub.faces.empty()) triangulateFaces(sub);

        // Zero out all normals
        for (auto& v : sub.vertices) {
            v.normal = Ogre::Vector3::ZERO;
            v.hasNormal = true;
        }

        // Assign face normal to each vertex of each triangle
        for (const auto& tri : sub.triangles) {
            if (tri.indices[0] >= sub.vertices.size() ||
                tri.indices[1] >= sub.vertices.size() ||
                tri.indices[2] >= sub.vertices.size())
                continue;

            const Ogre::Vector3& v0 = sub.vertices[tri.indices[0]].position;
            const Ogre::Vector3& v1 = sub.vertices[tri.indices[1]].position;
            const Ogre::Vector3& v2 = sub.vertices[tri.indices[2]].position;

            Ogre::Vector3 faceNormal = (v1 - v0).crossProduct(v2 - v0);
            Ogre::Real len = faceNormal.length();
            if (len > 1e-8f)
                faceNormal /= len;

            // Flat shading: each vertex gets the face normal directly
            sub.vertices[tri.indices[0]].normal = faceNormal;
            sub.vertices[tri.indices[1]].normal = faceNormal;
            sub.vertices[tri.indices[2]].normal = faceNormal;
        }
    }
}

int EditableMesh::countDegenerateTriangles(float epsilon) const
{
    int count = 0;
    for (const auto& sub : m_subMeshes) {
        for (const auto& tri : sub.triangles) {
            if (tri.indices[0] >= sub.vertices.size() ||
                tri.indices[1] >= sub.vertices.size() ||
                tri.indices[2] >= sub.vertices.size())
                continue;

            const Ogre::Vector3& v0 = sub.vertices[tri.indices[0]].position;
            const Ogre::Vector3& v1 = sub.vertices[tri.indices[1]].position;
            const Ogre::Vector3& v2 = sub.vertices[tri.indices[2]].position;

            float area = (v1 - v0).crossProduct(v2 - v0).length();
            if (area < epsilon)
                ++count;
        }
    }
    return count;
}

int EditableMesh::removeDegenerateTriangles(float epsilon)
{
    int totalRemoved = 0;
    for (auto& sub : m_subMeshes) {
        std::vector<EditableTriangle> kept;
        kept.reserve(sub.triangles.size());
        for (const auto& tri : sub.triangles) {
            if (tri.indices[0] >= sub.vertices.size() ||
                tri.indices[1] >= sub.vertices.size() ||
                tri.indices[2] >= sub.vertices.size()) {
                ++totalRemoved;
                continue;
            }

            const Ogre::Vector3& v0 = sub.vertices[tri.indices[0]].position;
            const Ogre::Vector3& v1 = sub.vertices[tri.indices[1]].position;
            const Ogre::Vector3& v2 = sub.vertices[tri.indices[2]].position;

            float area = (v1 - v0).crossProduct(v2 - v0).length();
            if (area < epsilon) {
                ++totalRemoved;
            } else {
                kept.push_back(tri);
            }
        }
        sub.triangles = std::move(kept);
    }
    return totalRemoved;
}

Ogre::AxisAlignedBox EditableMesh::calculateBounds() const
{
    Ogre::Vector3 minimum(std::numeric_limits<Ogre::Real>::max());
    Ogre::Vector3 maximum(std::numeric_limits<Ogre::Real>::lowest());

    bool hasVertex = false;
    for (const auto& sub : m_subMeshes) {
        for (const auto& v : sub.vertices) {
            minimum.makeFloor(v.position);
            maximum.makeCeil(v.position);
            hasVertex = true;
        }
    }

    if (!hasVertex)
        return Ogre::AxisAlignedBox();

    return Ogre::AxisAlignedBox(minimum, maximum);
}

bool EditableMesh::readVertexData(Ogre::VertexData* vertexData, std::vector<EditableVertex>& vertices)
{
    if (!vertexData || vertexData->vertexCount == 0)
        return false;

    vertices.resize(vertexData->vertexCount);

    auto* decl = vertexData->vertexDeclaration;
    auto* binding = vertexData->vertexBufferBinding;
    size_t vertCount = vertexData->vertexCount;

    // Lock an attribute's source buffer and apply `read(elem, vertexBase, j)`
    // for each vertex j. No-op if the element is missing.
    auto readAttribute = [&](const Ogre::VertexElement* elem, auto&& read) {
        if (!elem) return;
        auto vbuf = binding->getBuffer(elem->getSource());
        auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        size_t vertexSize = vbuf->getVertexSize();
        for (size_t j = 0; j < vertCount; ++j) {
            read(elem, base + j * vertexSize, j);
        }
        vbuf->unlock();
    };

    readAttribute(decl->findElementBySemantic(Ogre::VES_POSITION),
        [&vertices](const Ogre::VertexElement* e, unsigned char* base, size_t j) {
            Ogre::Real* p; e->baseVertexPointerToElement(base, &p);
            vertices[j].position = Ogre::Vector3(p[0], p[1], p[2]);
        });

    readAttribute(decl->findElementBySemantic(Ogre::VES_NORMAL),
        [&vertices](const Ogre::VertexElement* e, unsigned char* base, size_t j) {
            Ogre::Real* p; e->baseVertexPointerToElement(base, &p);
            vertices[j].normal = Ogre::Vector3(p[0], p[1], p[2]);
            vertices[j].hasNormal = true;
        });

    readAttribute(decl->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES),
        [&vertices](const Ogre::VertexElement* e, unsigned char* base, size_t j) {
            Ogre::Real* p; e->baseVertexPointerToElement(base, &p);
            vertices[j].uv = Ogre::Vector2(p[0], p[1]);
            vertices[j].hasUV = true;
        });

    readAttribute(decl->findElementBySemantic(Ogre::VES_DIFFUSE),
        [&vertices](const Ogre::VertexElement* e, unsigned char* base, size_t j) {
            Ogre::RGBA* p; e->baseVertexPointerToElement(base, &p);
            Ogre::ColourValue cv;
            cv.setAsRGBA(*p);
            vertices[j].color = cv;
            vertices[j].hasColor = true;
        });

    // Tangents may be FLOAT3 (direction only) or FLOAT4 (direction + parity w).
    if (const auto* tanElem = decl->findElementBySemantic(Ogre::VES_TANGENT)) {
        bool isFloat4 = (tanElem->getType() == Ogre::VET_FLOAT4);
        readAttribute(tanElem,
            [&vertices, isFloat4](const Ogre::VertexElement* e, unsigned char* base, size_t j) {
                Ogre::Real* p; e->baseVertexPointerToElement(base, &p);
                vertices[j].tangent.x = p[0];
                vertices[j].tangent.y = p[1];
                vertices[j].tangent.z = p[2];
                vertices[j].tangent.w = isFloat4 ? p[3] : 1.0f;
                vertices[j].hasTangent = true;
            });
    }

    return true;
}

bool EditableMesh::readIndexData(Ogre::IndexData* indexData, std::vector<EditableTriangle>& triangles)
{
    if (!indexData || indexData->indexCount == 0)
        return false;

    // Only support triangle lists (3 indices per triangle)
    if (indexData->indexCount % 3 != 0)
        return false;

    size_t triCount = indexData->indexCount / 3;
    triangles.resize(triCount);

    auto ibuf = indexData->indexBuffer;
    bool use32bit = (ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT);

    auto* data = static_cast<unsigned char*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));

    for (size_t t = 0; t < triCount; ++t) {
        if (use32bit) {
            auto* idx = reinterpret_cast<uint32_t*>(data) + t * 3;
            triangles[t].indices[0] = idx[0];
            triangles[t].indices[1] = idx[1];
            triangles[t].indices[2] = idx[2];
        } else {
            auto* idx = reinterpret_cast<uint16_t*>(data) + t * 3;
            triangles[t].indices[0] = idx[0];
            triangles[t].indices[1] = idx[1];
            triangles[t].indices[2] = idx[2];
        }
    }

    ibuf->unlock();
    return true;
}

bool EditableMesh::writeVertexData(Ogre::VertexData* vertexData, const std::vector<EditableVertex>& vertices)
{
    if (!vertexData || vertices.empty())
        return false;

    auto* decl = vertexData->vertexDeclaration;
    auto* binding = vertexData->vertexBufferBinding;
    size_t count = std::min(vertices.size(), static_cast<size_t>(vertexData->vertexCount));

    // Write a single per-vertex attribute back to its source buffer.
    // Upgrades static buffers to dynamic for immediate GPU visibility, then
    // reads the buffer, applies `mutate(buffer + j*vertexSize, j)` for each
    // vertex j, and writes the buffer back with DISCARD.
    auto writeAttribute = [&](const Ogre::VertexElement* elem,
                              auto&& mutate) {
        if (!elem) return;
        unsigned short source = elem->getSource();
        auto vbuf = binding->getBuffer(source);
        size_t bufSize = vbuf->getSizeInBytes();
        size_t vertexSize = vbuf->getVertexSize();

        // Upgrade static → dynamic for immediate GPU visibility.
        if (vbuf->getUsage() & Ogre::HardwareBuffer::HBU_STATIC) {
            std::vector<unsigned char> oldData(bufSize);
            vbuf->readData(0, bufSize, oldData.data());

            auto newBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
                vertexSize, vertexData->vertexCount,
                Ogre::HardwareBuffer::HBU_DYNAMIC_WRITE_ONLY, true);
            newBuf->writeData(0, bufSize, oldData.data(), true);
            binding->setBinding(source, newBuf);
            vbuf = newBuf;
        }

        std::vector<unsigned char> bufCopy(bufSize);
        vbuf->readData(0, bufSize, bufCopy.data());

        for (size_t j = 0; j < count; ++j) {
            mutate(elem, bufCopy.data() + j * vertexSize, j);
        }

        auto* dest = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        memcpy(dest, bufCopy.data(), bufSize);
        vbuf->unlock();
    };

    writeAttribute(decl->findElementBySemantic(Ogre::VES_POSITION),
        [&vertices](const Ogre::VertexElement* e, unsigned char* base, size_t j) {
            Ogre::Real* p; e->baseVertexPointerToElement(base, &p);
            p[0] = vertices[j].position.x;
            p[1] = vertices[j].position.y;
            p[2] = vertices[j].position.z;
        });

    writeAttribute(decl->findElementBySemantic(Ogre::VES_NORMAL),
        [&vertices](const Ogre::VertexElement* e, unsigned char* base, size_t j) {
            if (!vertices[j].hasNormal) return;
            Ogre::Real* p; e->baseVertexPointerToElement(base, &p);
            p[0] = vertices[j].normal.x;
            p[1] = vertices[j].normal.y;
            p[2] = vertices[j].normal.z;
        });

    writeAttribute(decl->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES),
        [&vertices](const Ogre::VertexElement* e, unsigned char* base, size_t j) {
            if (!vertices[j].hasUV) return;
            Ogre::Real* p; e->baseVertexPointerToElement(base, &p);
            p[0] = vertices[j].uv.x;
            p[1] = vertices[j].uv.y;
        });

    return true;
}
