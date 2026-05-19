#include "PS1RipMeshBuilder.h"

#include "Manager.h"
#include "TextureDecoder.h"
#include "VramSnapshot.h"

#include <QRegularExpression>
#include <QVector>
#include <algorithm>
#include <vector>

#include <OgreAxisAlignedBox.h>
#include <OgreEntity.h>
#include <OgreVector3.h>
#include <OgreImage.h>
#include <OgreResourceGroupManager.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMaterialManager.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgrePass.h>
#include <OgreSceneNode.h>
#include <OgreSubMesh.h>
#include <OgreTechnique.h>
#include <OgreTexture.h>
#include <OgreTextureManager.h>
#include <OgreTextureUnitState.h>
#include <OgreException.h>

namespace {

constexpr float kTargetMaxExtent = 2.0f;

bool parseTpageClutMaterial(const QString &name, uint16_t &tpageOut, uint16_t &clutOut)
{
    static const QRegularExpression re(
        QStringLiteral("^PS1Rip/tpage_([0-9a-fA-F]+)_clut_([0-9a-fA-F]+)$"));
    const QRegularExpressionMatch match = re.match(name);
    if (!match.hasMatch())
        return false;
    tpageOut = static_cast<uint16_t>(match.captured(1).toUInt(nullptr, 16));
    clutOut = static_cast<uint16_t>(match.captured(2).toUInt(nullptr, 16));
    return true;
}

TextureDecoder::BitDepth bitDepthFromTpage(uint16_t tpage)
{
    switch ((tpage >> 7) & 3) {
    case 0:
        return TextureDecoder::BitDepth::Bpp4;
    case 1:
        return TextureDecoder::BitDepth::Bpp8;
    default:
        return TextureDecoder::BitDepth::Bpp15;
    }
}

void clutCoordsFromClutWord(uint16_t clut, uint16_t &clutXOut, uint16_t &clutYOut)
{
    clutXOut = static_cast<uint16_t>((clut & 0x3F) << 4);
    clutYOut = static_cast<uint16_t>((clut >> 6) & 0x3FF);
}

QString textureResourceName(const QString &captureId, const QString &materialName)
{
    QString safe = materialName;
    safe.replace(QLatin1Char('/'), QLatin1Char('_'));
    return QStringLiteral("ps1rip_%1_%2").arg(captureId, safe);
}

bool uploadTextureToOgre(const QString &resourceName, const QImage &image, QString *errorOut)
{
    if (image.isNull() || image.width() < 1 || image.height() < 1) {
        if (errorOut)
            *errorOut = QStringLiteral("Texture image is empty");
        return false;
    }

    QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull() || rgba.width() < 1 || rgba.height() < 1) {
        if (errorOut)
            *errorOut = QStringLiteral("Texture image conversion failed");
        return false;
    }

    // Ogre 14 loadDynamicImage(..., depth, format, autoDelete, numFaces, numMipMaps) — there is
    // no row-pitch parameter. A QImage row stride (bytesPerLine) must not be passed as numFaces.
    const int tightStride = rgba.width() * 4;
    if (rgba.bytesPerLine() != tightStride)
        rgba = rgba.copy();

    Ogre::Image ogreImage;
    try {
        ogreImage.loadDynamicImage(rgba.bits(), static_cast<Ogre::uint32>(rgba.width()),
                                   static_cast<Ogre::uint32>(rgba.height()), 1, Ogre::PF_BYTE_RGBA,
                                   false);
    } catch (const Ogre::Exception &e) {
        if (errorOut)
            *errorOut = QString::fromStdString(e.getFullDescription());
        return false;
    }

    const std::string texName = resourceName.toStdString();
    if (Ogre::TextureManager::getSingleton().resourceExists(texName))
        Ogre::TextureManager::getSingleton().remove(texName);

    Ogre::TexturePtr texture = Ogre::TextureManager::getSingleton().createManual(
        texName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, Ogre::TEX_TYPE_2D,
        ogreImage.getWidth(), ogreImage.getHeight(), 0, ogreImage.getFormat(),
        Ogre::TU_STATIC_WRITE_ONLY);
    if (!texture)
        return false;

    Ogre::HardwarePixelBufferSharedPtr pixelBuffer = texture->getBuffer(0, 0);
    if (!pixelBuffer) {
        if (errorOut)
            *errorOut = QStringLiteral("Failed to allocate texture buffer");
        return false;
    }

    pixelBuffer->blitFromMemory(ogreImage.getPixelBox());
    texture->load();
    return true;
}

Ogre::MaterialPtr ensureMaterial(const QString &name, const CaptureSnapshot *textureSource,
                                   const QString &captureId, TextureDecoder *decoder)
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

    if (!textureSource || !textureSource->hasVram() || !decoder)
        return mat;

    uint16_t tpage = 0;
    uint16_t clut = 0;
    if (!parseTpageClutMaterial(name, tpage, clut))
        return mat;

    VramSnapshot vram;
    vram.mutablePixels() = textureSource->vramCells;

    TextureDecoder::TileKey key{};
    key.tpage = tpage;
    clutCoordsFromClutWord(clut, key.clutX, key.clutY);
    key.bitDepth = bitDepthFromTpage(tpage);
    const QRect page = VramSnapshot::tpageRect(tpage);

    QString decodeErr;
    const QImage tile = decoder->decodeTile(vram, key, page, &decodeErr);
    if (tile.isNull())
        return mat;

    const QString texResource = textureResourceName(captureId, name);
    QString uploadErr;
    if (!uploadTextureToOgre(texResource, tile, &uploadErr))
        return mat;

    pass->setVertexColourTracking(Ogre::TVC_AMBIENT | Ogre::TVC_DIFFUSE);
    Ogre::TextureUnitState *tus = pass->createTextureUnitState(texResource.toStdString());
    tus->setTextureFiltering(Ogre::TFO_NONE);
    tus->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);
    return mat;
}

void removePriorCaptureNodes(Manager *mgr)
{
    QStringList toRemove;
    for (Ogre::SceneNode *node : mgr->getSceneNodes()) {
        const QString name = QString::fromStdString(node->getName());
        if (name.startsWith(QStringLiteral("PS1Capture_")))
            toRemove.append(name);
    }
    for (const QString &name : toRemove)
        mgr->destroySceneNode(name);
}

Ogre::AxisAlignedBox meshBounds(const ReconstructedMesh &mesh)
{
    Ogre::AxisAlignedBox bounds;
    bounds.setNull();
    for (const ReconstructedSubMesh &sub : mesh.subMeshes) {
        for (const ReconstructedVertex &v : sub.vertices)
            bounds.merge(Ogre::Vector3(v.px, v.py, v.pz));
    }
    return bounds;
}

void applyEditorPlacement(Ogre::SceneNode *node, const Ogre::AxisAlignedBox &bounds)
{
    if (!node || bounds.isNull())
        return;

    const Ogre::Vector3 center = bounds.getCenter();
    const Ogre::Vector3 size = bounds.getSize();
    const float maxExtent = std::max(size.x, std::max(size.y, size.z));
    float scale = 1.0f;
    if (maxExtent > 1e-6f) {
        scale = kTargetMaxExtent / maxExtent;
        if (maxExtent < 0.01f)
            scale = 3.0f / maxExtent;
    }

    node->setPosition(-center * scale);
    node->setScale(scale, scale, scale);
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
                                        const CaptureSnapshot *textureSource, BuildResult *resultOut,
                                        QString *errorOut)
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

    const Ogre::AxisAlignedBox localBounds = meshBounds(mesh);
    Ogre::AxisAlignedBox bounds = localBounds;
    if (bounds.isNull())
        bounds = Ogre::AxisAlignedBox(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);

    TextureDecoder textureDecoder;

    for (const ReconstructedSubMesh &sub : mesh.subMeshes) {
        Ogre::SubMesh *ogreSub = ogreMesh->createSubMesh();
        ogreSub->setMaterialName(
            ensureMaterial(sub.materialName, textureSource, captureId, &textureDecoder)->getName());
        writeSubMesh(sub, ogreSub);
    }

    ogreMesh->_setBounds(bounds);
    ogreMesh->_setBoundingSphereRadius(bounds.getHalfSize().length());
    ogreMesh->load();

    removePriorCaptureNodes(mgr);

    const QString nodeName = QStringLiteral("PS1Capture_%1").arg(captureId);
    Ogre::SceneNode *node = mgr->addSceneNode(nodeName);
    applyEditorPlacement(node, localBounds);
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
