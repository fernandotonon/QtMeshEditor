#include "PS1RipMeshBuilder.h"

#include "Manager.h"

#include <QVector>
#include <vector>

#include <OgreEntity.h>
#include <OgreResourceGroupManager.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMaterialManager.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgrePass.h>
#include <OgreSceneNode.h>
#include <OgreSubMesh.h>
#include <OgreTechnique.h>

namespace {

Ogre::MaterialPtr ensureMaterial(const QString &name)
{
    const std::string matName = name.toStdString();
    if (Ogre::MaterialManager::getSingleton().resourceExists(matName))
        return Ogre::MaterialManager::getSingleton().getByName(matName);

    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().create(
        matName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::Pass *pass = mat->getTechnique(0)->getPass(0);
    pass->setLightingEnabled(false);
    pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
    pass->setDiffuse(Ogre::ColourValue::White);
    return mat;
}

void writeSubMesh(const ReconstructedSubMesh &sub, Ogre::SubMesh *ogreSub)
{
    ogreSub->useSharedVertices = false;
    ogreSub->vertexData = new Ogre::VertexData();
    ogreSub->vertexData->vertexCount = static_cast<uint32_t>(sub.vertices.size());

    Ogre::VertexDeclaration *decl = ogreSub->vertexData->vertexDeclaration;
    Ogre::VertexBufferBinding *binding = ogreSub->vertexData->vertexBufferBinding;

    size_t offset = 0;
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT2);
    decl->addElement(1, 0, Ogre::VET_COLOUR, Ogre::VES_DIFFUSE);

    const size_t stride0 = decl->getVertexSize(0);
    Ogre::HardwareVertexBufferSharedPtr vbuf0 =
        Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            stride0, sub.vertices.size(), Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);

    std::vector<float> buf0(stride0 * sub.vertices.size() / sizeof(float), 0.0f);
    for (size_t i = 0; i < sub.vertices.size(); ++i) {
        const ReconstructedVertex &v = sub.vertices[static_cast<int>(i)];
        float *base = buf0.data() + (i * stride0) / sizeof(float);
        base[0] = v.px;
        base[1] = v.py;
        base[2] = v.pz;
        base[3] = v.nx;
        base[4] = v.ny;
        base[5] = v.nz;
        base[6] = v.u;
        base[7] = v.v;
    }
    vbuf0->writeData(0, stride0 * sub.vertices.size(), buf0.data());
    binding->setBinding(0, vbuf0);

    Ogre::HardwareVertexBufferSharedPtr vbuf1 =
        Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            Ogre::VertexElement::getTypeSize(Ogre::VET_COLOUR), sub.vertices.size(),
            Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    QVector<uint32_t> colors;
    colors.reserve(sub.vertices.size());
    for (const ReconstructedVertex &v : sub.vertices)
        colors.append(v.diffuseArgb);
    vbuf1->writeData(0, colors.size() * sizeof(uint32_t), colors.constData());
    binding->setBinding(1, vbuf1);

    ogreSub->indexData->indexCount = sub.indices.size();
    Ogre::HardwareIndexBufferSharedPtr ibuf =
        Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
            Ogre::HardwareIndexBuffer::IT_32BIT, sub.indices.size(),
            Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    ibuf->writeData(0, sub.indices.size() * sizeof(uint32_t), sub.indices.constData());
    ogreSub->indexData->indexBuffer = ibuf;
}

} // namespace

bool PS1RipMeshBuilder::attachToScene(const ReconstructedMesh &mesh, const QString &captureId,
                                        BuildResult *resultOut, QString *errorOut)
{
    if (mesh.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("Reconstructed mesh is empty");
        return false;
    }

    Manager *mgr = Manager::getSingletonPtr();
    if (!mgr) {
        if (errorOut)
            *errorOut = QStringLiteral("Editor scene is not available");
        return false;
    }

    const QString meshName = mesh.meshName + QLatin1Char('_') + captureId;
    const std::string ogreMeshName = meshName.toStdString();

    if (Ogre::MeshManager::getSingleton().resourceExists(ogreMeshName))
        Ogre::MeshManager::getSingleton().remove(ogreMeshName);

    Ogre::MeshPtr ogreMesh = Ogre::MeshManager::getSingleton().createManual(
        ogreMeshName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    Ogre::AxisAlignedBox bounds;
    bounds.setNull();

    for (const ReconstructedSubMesh &sub : mesh.subMeshes) {
        Ogre::SubMesh *ogreSub = ogreMesh->createSubMesh();
        ogreSub->setMaterialName(ensureMaterial(sub.materialName)->getName());
        writeSubMesh(sub, ogreSub);

        for (const ReconstructedVertex &v : sub.vertices)
            bounds.merge(Ogre::Vector3(v.px, v.py, v.pz));
    }

    if (bounds.isNull())
        bounds = Ogre::AxisAlignedBox(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);

    ogreMesh->_setBounds(bounds);
    ogreMesh->_setBoundingSphereRadius(bounds.getHalfSize().length());
    ogreMesh->load();

    const QString nodeName = QStringLiteral("PS1Capture_%1").arg(captureId);
    Ogre::SceneNode *node = mgr->addSceneNode(nodeName);
    Ogre::Entity *entity = mgr->createEntity(node, ogreMesh);

    if (!entity) {
        if (errorOut)
            *errorOut = QStringLiteral("Failed to create Ogre entity for reconstructed mesh");
        return false;
    }

    if (resultOut) {
        resultOut->sceneNode = node;
        resultOut->entity = entity;
        resultOut->vertexCount = mesh.vertexCount;
        resultOut->triangleCount = mesh.triangleCount;
    }
    return true;
}
