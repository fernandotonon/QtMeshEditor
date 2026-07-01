#include "MeshGenBuilder.h"

#include "Manager.h"

#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubMesh.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

// Accumulate face normals into per-vertex normals and normalize. Marching-cubes
// output is welded but carries no normals, so we derive them here (Slice C's
// requirement). Degenerate faces contribute nothing.
std::vector<float> computeNormals(const std::vector<float>& pos,
                                  const std::vector<uint32_t>& idx)
{
    std::vector<float> nrm(pos.size(), 0.0f);
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const uint32_t a = idx[t + 0], b = idx[t + 1], c = idx[t + 2];
        const float* pa = &pos[static_cast<size_t>(a) * 3];
        const float* pb = &pos[static_cast<size_t>(b) * 3];
        const float* pc = &pos[static_cast<size_t>(c) * 3];
        const float e1[3] = {pb[0]-pa[0], pb[1]-pa[1], pb[2]-pa[2]};
        const float e2[3] = {pc[0]-pa[0], pc[1]-pa[1], pc[2]-pa[2]};
        const float fn[3] = {
            e1[1]*e2[2] - e1[2]*e2[1],
            e1[2]*e2[0] - e1[0]*e2[2],
            e1[0]*e2[1] - e1[1]*e2[0],
        };
        for (uint32_t v : {a, b, c}) {
            nrm[static_cast<size_t>(v)*3 + 0] += fn[0];
            nrm[static_cast<size_t>(v)*3 + 1] += fn[1];
            nrm[static_cast<size_t>(v)*3 + 2] += fn[2];
        }
    }
    for (size_t i = 0; i < nrm.size(); i += 3) {
        float len = std::sqrt(nrm[i]*nrm[i] + nrm[i+1]*nrm[i+1] + nrm[i+2]*nrm[i+2]);
        if (len < 1e-12f) { nrm[i+1] = 1.0f; len = 1.0f; }   // fallback +Y
        nrm[i]   /= len; nrm[i+1] /= len; nrm[i+2] /= len;
    }
    return nrm;
}

} // namespace

namespace MeshGenBuilder {

Ogre::Mesh* buildMesh(const MeshGenPredictor::Result& result, const QString& meshName)
{
    if (result.vertexCount <= 0 || result.triangleCount <= 0
        || result.positions.size() != static_cast<size_t>(result.vertexCount) * 3)
        return nullptr;

    const bool hasColor =
        result.colors.size() == static_cast<size_t>(result.vertexCount) * 3;
    const std::vector<float> normals = computeNormals(result.positions, result.indices);

    auto& mm = Ogre::MeshManager::getSingleton();
    const std::string name = meshName.toStdString();
    if (mm.resourceExists(name))
        mm.remove(name);
    Ogre::MeshPtr mesh = mm.createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    Ogre::SubMesh* sub = mesh->createSubMesh();
    sub->useSharedVertices = true;

    auto* vd = new Ogre::VertexData();
    mesh->sharedVertexData = vd;
    vd->vertexCount = static_cast<size_t>(result.vertexCount);
    auto* decl = vd->vertexDeclaration;

    size_t offset = 0;
    offset += decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION).getSize();
    offset += decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL).getSize();
    if (hasColor)
        offset += decl->addElement(0, offset, Ogre::VET_COLOUR, Ogre::VES_DIFFUSE).getSize();

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), vd->vertexCount,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);

    Ogre::Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
    {
        auto* p = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        auto* root = Ogre::Root::getSingletonPtr();
        for (int i = 0; i < result.vertexCount; ++i) {
            float* f = reinterpret_cast<float*>(p);
            const float x = result.positions[3*i+0];
            const float y = result.positions[3*i+1];
            const float z = result.positions[3*i+2];
            f[0] = x; f[1] = y; f[2] = z;
            f[3] = normals[3*i+0]; f[4] = normals[3*i+1]; f[5] = normals[3*i+2];
            p += 6 * sizeof(float);
            if (hasColor) {
                Ogre::ColourValue cv(result.colors[3*i+0], result.colors[3*i+1],
                                     result.colors[3*i+2], 1.0f);
                Ogre::RGBA packed;
                if (root) root->convertColourValue(cv, &packed);
                else      packed = cv.getAsARGB();
                *reinterpret_cast<Ogre::RGBA*>(p) = packed;
                p += sizeof(Ogre::RGBA);
            }
            mn.makeFloor(Ogre::Vector3(x, y, z));
            mx.makeCeil(Ogre::Vector3(x, y, z));
        }
        vbuf->unlock();
    }
    vd->vertexBufferBinding->setBinding(0, vbuf);

    // Index buffer: 16-bit when it fits, else 32-bit.
    const size_t idxCount = result.indices.size();
    const bool use32 = result.vertexCount > 65535;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        use32 ? Ogre::HardwareIndexBuffer::IT_32BIT : Ogre::HardwareIndexBuffer::IT_16BIT,
        idxCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    if (use32) {
        ibuf->writeData(0, idxCount * sizeof(uint32_t), result.indices.data(), true);
    } else {
        std::vector<uint16_t> idx16(idxCount);
        for (size_t i = 0; i < idxCount; ++i)
            idx16[i] = static_cast<uint16_t>(result.indices[i]);
        ibuf->writeData(0, idxCount * sizeof(uint16_t), idx16.data(), true);
    }
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount  = idxCount;
    sub->indexData->indexStart  = 0;

    Ogre::AxisAlignedBox aabb(mn, mx);
    mesh->_setBounds(aabb);
    mesh->_setBoundingSphereRadius(0.5f * (mx - mn).length());
    mesh->load();
    return mesh.get();
}

Ogre::SceneNode* buildSceneNode(const MeshGenPredictor::Result& result,
                                const QString& baseName)
{
    Ogre::Mesh* mesh = buildMesh(result, baseName + QStringLiteral("_mesh"));
    if (!mesh) return nullptr;

    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    Ogre::SceneNode* node = mgr->addSceneNode(baseName);
    if (!node) return nullptr;
    Ogre::MeshPtr ptr = Ogre::MeshManager::getSingleton().getByName(mesh->getName());
    mgr->createEntity(node, ptr);
    return node;
}

} // namespace MeshGenBuilder
