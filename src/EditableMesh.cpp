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

    return true;
}

void EditableMesh::buildSubMeshBuffers(Ogre::SubMesh* subMesh,
                                       const EditableSubMesh& editSub)
{
    if (!subMesh || editSub.vertices.empty() || editSub.triangles.empty())
        return;

    // Replace any existing vertex data with a fresh one.
    if (subMesh->vertexData) delete subMesh->vertexData;
    subMesh->useSharedVertices = false;
    subMesh->vertexData = new Ogre::VertexData();
    subMesh->vertexData->vertexCount = editSub.vertices.size();

    auto* decl = subMesh->vertexData->vertexDeclaration;
    auto* binding = subMesh->vertexData->vertexBufferBinding;

    // Build declaration: position, optional normal, uv, tangent (FLOAT4 with parity).
    size_t offset = 0;
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);

    bool hasNormals = editSub.vertices[0].hasNormal;
    if (hasNormals) {
        decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
        offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    }

    bool hasUVs = editSub.vertices[0].hasUV;
    if (hasUVs) {
        decl->addElement(0, offset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);
        offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT2);
    }

    bool hasTangents = editSub.vertices[0].hasTangent;
    if (hasTangents) {
        decl->addElement(0, offset, Ogre::VET_FLOAT4, Ogre::VES_TANGENT);
        offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT4);
    }

    // Create interleaved vertex buffer.
    size_t vertSize = decl->getVertexSize(0);
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        vertSize, editSub.vertices.size(),
        Ogre::HardwareBuffer::HBU_DYNAMIC_WRITE_ONLY, true);

    auto* dest = static_cast<float*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
    for (const auto& v : editSub.vertices) {
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
    bool use32bit = editSub.vertices.size() > 65535;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        use32bit ? Ogre::HardwareIndexBuffer::IT_32BIT : Ogre::HardwareIndexBuffer::IT_16BIT,
        editSub.triangles.size() * 3,
        Ogre::HardwareBuffer::HBU_DYNAMIC_WRITE_ONLY, true);

    if (use32bit) {
        auto* idx = static_cast<uint32_t*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for (const auto& tri : editSub.triangles) {
            *idx++ = tri.indices[0];
            *idx++ = tri.indices[1];
            *idx++ = tri.indices[2];
        }
    } else {
        auto* idx = static_cast<uint16_t*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for (const auto& tri : editSub.triangles) {
            *idx++ = static_cast<uint16_t>(tri.indices[0]);
            *idx++ = static_cast<uint16_t>(tri.indices[1]);
            *idx++ = static_cast<uint16_t>(tri.indices[2]);
        }
    }
    ibuf->unlock();

    subMesh->indexData->indexBuffer = ibuf;
    subMesh->indexData->indexCount = editSub.triangles.size() * 3;
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
        // Zero out all normals
        for (auto& v : sub.vertices) {
            v.normal = Ogre::Vector3::ZERO;
            v.hasNormal = true;
        }

        // Accumulate face normals (area-weighted)
        for (const auto& tri : sub.triangles) {
            if (tri.indices[0] >= sub.vertices.size() ||
                tri.indices[1] >= sub.vertices.size() ||
                tri.indices[2] >= sub.vertices.size())
                continue;

            const Ogre::Vector3& v0 = sub.vertices[tri.indices[0]].position;
            const Ogre::Vector3& v1 = sub.vertices[tri.indices[1]].position;
            const Ogre::Vector3& v2 = sub.vertices[tri.indices[2]].position;

            // Cross product gives area-weighted face normal
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
