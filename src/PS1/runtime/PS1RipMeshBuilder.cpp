#include "PS1RipMeshBuilder.h"

#include "Manager.h"
#include "MeshReconstructor.h"
#include "SentryReporter.h"
#include "TextureDecoder.h"
#include "VramSnapshot.h"

#include <QHash>
#include <QRegularExpression>
#include <QVector>
#include <algorithm>
#include <atomic>
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
constexpr int kPageTexels = 256;

std::atomic<int> g_nextSessionId{0};

int allocateSessionId()
{
    return ++g_nextSessionId;
}

QString sessionResourceGroup(int sessionId)
{
    return QStringLiteral("PS1Rip_Session%1").arg(sessionId);
}

void ensureResourceGroup(const QString &groupName)
{
    Ogre::ResourceGroupManager &rgm = Ogre::ResourceGroupManager::getSingleton();
    const std::string group = groupName.toStdString();
    if (!rgm.resourceGroupExists(group))
        rgm.createResourceGroup(group);
}

bool parseTpageClutMaterial(const QString &name, uint16_t &tpageOut, uint16_t &clutOut,
                            uint8_t &semiTransOut, uint8_t &drawModeBitOut)
{
    static const QRegularExpression re(
        QStringLiteral("^PS1Rip_tpage_([0-9a-fA-F]+)_clut_([0-9a-fA-F]+)(?:_st([0-3]))?(?:_dm([01]))?$"));
    const QRegularExpressionMatch match = re.match(name);
    if (!match.hasMatch())
        return false;
    tpageOut = static_cast<uint16_t>(match.captured(1).toUInt(nullptr, 16));
    clutOut = static_cast<uint16_t>(match.captured(2).toUInt(nullptr, 16));
    semiTransOut = match.captured(3).isEmpty() ? 0
                                               : static_cast<uint8_t>(match.captured(3).toUInt());
    drawModeBitOut = match.captured(4).isEmpty()
                         ? 0
                         : static_cast<uint8_t>(match.captured(4).toUInt());
    return true;
}

TextureDecoder::MaterialKey materialKeyFromParsedName(uint16_t tpage, uint16_t clutWord,
                                                      uint8_t semiTrans, uint8_t drawModeBit)
{
    TextureDecoder::MaterialKey key{};
    key.tpage = tpage;
    TextureDecoder::clutCoordsFromClutWord(clutWord, key.clutX, key.clutY);
    key.bitDepth = TextureDecoder::bitDepthFromTpage(tpage);
    key.semiTrans = semiTrans;
    key.drawModeBits = drawModeBit ? (1u << 11) : 0u;
    return key;
}

void applySemiTransBlend(Ogre::Pass *pass, uint8_t semiTrans)
{
    if (!pass)
        return;

    switch (semiTrans & 3) {
    case 0:
        pass->setSceneBlending(Ogre::SBF_DEST_COLOUR, Ogre::SBF_SOURCE_COLOUR);
        pass->setSceneBlendingOperation(Ogre::SBO_ADD);
        break;
    case 1:
        pass->setSceneBlending(Ogre::SBF_ONE, Ogre::SBF_ONE);
        pass->setSceneBlendingOperation(Ogre::SBO_ADD);
        break;
    case 2:
        pass->setSceneBlending(Ogre::SBF_ONE, Ogre::SBF_ONE);
        pass->setSceneBlendingOperation(Ogre::SBO_SUBTRACT);
        break;
    case 3:
        // PS1: B + F/4 — Ogre has no constant 0.25 factor; scale pass alpha as approximation.
        pass->setSceneBlending(Ogre::SBF_ONE, Ogre::SBF_SOURCE_ALPHA);
        pass->setDiffuse(Ogre::ColourValue(1.0f, 1.0f, 1.0f, 0.25f));
        break;
    default:
        pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        break;
    }
}

QString textureResourceName(const QString &captureId, const QString &materialName)
{
    QString safe = materialName;
    safe.replace(QLatin1Char('/'), QLatin1Char('_'));
    return QStringLiteral("ps1rip_%1_%2").arg(captureId, safe);
}

QString scopedMaterialResourceName(const QString &captureId, const QString &logicalName)
{
    return QStringLiteral("PS1Rip_%1_%2").arg(captureId, logicalName);
}

bool uploadTextureToOgre(const QString &resourceGroup, const QString &resourceName,
                         const QImage &image, QString *errorOut)
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
    const std::string group = resourceGroup.toStdString();
    if (Ogre::TextureManager::getSingleton().resourceExists(texName))
        Ogre::TextureManager::getSingleton().remove(texName);

    Ogre::TexturePtr texture = Ogre::TextureManager::getSingleton().createManual(
        texName, group, Ogre::TEX_TYPE_2D, ogreImage.getWidth(), ogreImage.getHeight(), 0,
        ogreImage.getFormat(), Ogre::TU_STATIC_WRITE_ONLY);
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

QImage pageImageFromCachedTile(const QImage &tile, const QRect &boundsOnPage)
{
    QImage page(kPageTexels, kPageTexels, QImage::Format_ARGB32);
    page.fill(Qt::transparent);
    if (tile.isNull() || !boundsOnPage.isValid())
        return page;

    const QRect pageRect(0, 0, kPageTexels, kPageTexels);
    const QRect dstRect = boundsOnPage.intersected(pageRect);
    if (dstRect.isEmpty())
        return page;

    const int srcX0 = dstRect.x() - boundsOnPage.x();
    const int srcY0 = dstRect.y() - boundsOnPage.y();
    for (int y = 0; y < dstRect.height(); ++y) {
        const QRgb *src = reinterpret_cast<const QRgb *>(tile.constScanLine(srcY0 + y));
        QRgb *dst = reinterpret_cast<QRgb *>(page.scanLine(dstRect.y() + y));
        for (int x = 0; x < dstRect.width(); ++x)
            dst[dstRect.x() + x] = src[srcX0 + x];
    }
    return page;
}

struct TextureBuildContext {
    int sessionId = 0;
    QString resourceGroup;
    QString captureId;
    TextureDecoder decoder;
    VramSnapshot vram;
    QHash<QString, TextureDecoder::MaterialKey> materialKeys;
    QHash<QString, Ogre::MaterialPtr> materials;
};

void predecodeCaptureTextures(const CaptureSnapshot *textureSource, TextureBuildContext *ctx)
{
    if (!textureSource || !textureSource->hasVram() || !ctx)
        return;

    ctx->vram.mutablePixels() = textureSource->vramCells;

    QHash<TextureDecoder::MaterialKey, QRect> boundsByKey;
    for (const PrimRecord &prim : textureSource->prims) {
        if (!TextureDecoder::isTexturedPrim(prim))
            continue;

        const TextureDecoder::MaterialKey key = TextureDecoder::materialKeyFromPrim(prim);
        TextureDecoder::accumulateUvBounds(prim, boundsByKey[key]);

        const QString matName = MeshReconstructor::textureMaterialName(
            prim.tpage, prim.clut, prim.semiTrans, prim.drawModeBits);
        ctx->materialKeys.insert(matName, key);
    }

    for (auto it = boundsByKey.constBegin(); it != boundsByKey.constEnd(); ++it)
        ctx->decoder.decodeMaterial(ctx->vram, it.key(), it.value());

    const TextureDecoder::DecodeStats stats = ctx->decoder.stats();
    SentryReporter::addBreadcrumb(
        QStringLiteral("ps1.rip.texture.decoded"),
        QStringLiteral("materials=%1 rgba_bytes=%2 warnings=%3")
            .arg(stats.decodedMaterials)
            .arg(stats.rgbaBytes)
            .arg(ctx->decoder.warnings().size()));
}

Ogre::MaterialPtr ensureMaterial(const QString &logicalName, TextureBuildContext *ctx)
{
    if (!ctx)
        return {};

    if (ctx->materials.contains(logicalName))
        return ctx->materials.value(logicalName);

    const QString ogreName = scopedMaterialResourceName(ctx->captureId, logicalName);
    const std::string matName = ogreName.toStdString();
    Ogre::MaterialManager &matMgr = Ogre::MaterialManager::getSingleton();

    Ogre::MaterialPtr mat;
    if (matMgr.resourceExists(matName))
        mat = matMgr.getByName(matName);
    else
        mat = matMgr.create(matName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    Ogre::Technique *technique = mat->getTechnique(0);
    while (technique->getNumPasses() > 1)
        technique->removePass(1);
    Ogre::Pass *pass = technique->getPass(0);
    pass->removeAllTextureUnitStates();
    pass->setLightingEnabled(false);
    pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
    pass->setDiffuse(Ogre::ColourValue::White);
    pass->setSceneBlending(Ogre::SBT_REPLACE);

    uint16_t tpage = 0;
    uint16_t clut = 0;
    uint8_t semiTrans = 0;
    uint8_t drawModeBit = 0;
    if (parseTpageClutMaterial(logicalName, tpage, clut, semiTrans, drawModeBit)) {
        applySemiTransBlend(pass, semiTrans);

        TextureDecoder::MaterialKey key = ctx->materialKeys.value(logicalName);
        if (!ctx->materialKeys.contains(logicalName))
            key = materialKeyFromParsedName(tpage, clut, semiTrans, drawModeBit);
        else if (((key.drawModeBits >> 11) & 1u) != drawModeBit)
            key.drawModeBits = drawModeBit ? (1u << 11) : 0u;

        const QImage tile = ctx->decoder.cachedMaterial(key);
        const QRect bounds = ctx->decoder.cachedBoundsOnPage(key);
        if (!tile.isNull()) {
            const QImage page = pageImageFromCachedTile(tile, bounds);
            const QString texResource = textureResourceName(ctx->captureId, logicalName);
            QString uploadErr;
            if (uploadTextureToOgre(ctx->resourceGroup, texResource, page, &uploadErr)) {
                pass->setVertexColourTracking(Ogre::TVC_AMBIENT | Ogre::TVC_DIFFUSE);
                Ogre::TextureUnitState *tus = pass->createTextureUnitState(texResource.toStdString());
                tus->setTextureFiltering(Ogre::TFO_NONE);
                tus->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);
            }
        }
    }

    ctx->materials.insert(logicalName, mat);
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

void purgeSessionResourceGroups()
{
    Ogre::ResourceGroupManager &rgm = Ogre::ResourceGroupManager::getSingleton();
    const int lastSessionId = g_nextSessionId.load();
    for (int id = 1; id <= lastSessionId; ++id) {
        const std::string group = sessionResourceGroup(id).toStdString();
        if (!rgm.resourceGroupExists(group))
            continue;
        try {
            rgm.destroyResourceGroup(group);
        } catch (...) {
        }
    }
}

void purgePriorCaptureGpuResources()
{
    Ogre::MeshManager &meshMgr = Ogre::MeshManager::getSingleton();
    std::vector<std::string> meshesToRemove;
    Ogre::ResourceManager::ResourceMapIterator meshIt = meshMgr.getResourceIterator();
    while (meshIt.hasMoreElements()) {
        const std::string name = meshIt.peekNextValue()->getName();
        if (name.rfind("ps1_capture_", 0) == 0 || name.rfind("ps1_part_", 0) == 0
            || name.rfind("ps1_unique_", 0) == 0)
            meshesToRemove.push_back(name);
        meshIt.moveNext();
    }
    for (const std::string &name : meshesToRemove) {
        try {
            meshMgr.remove(name);
        } catch (...) {
        }
    }

    Ogre::MaterialManager &matMgr = Ogre::MaterialManager::getSingleton();
    std::vector<std::string> materialsToRemove;
    Ogre::ResourceManager::ResourceMapIterator matIt = matMgr.getResourceIterator();
    while (matIt.hasMoreElements()) {
        const std::string name = matIt.peekNextValue()->getName();
        if (name.rfind("PS1Rip_", 0) == 0 || name.rfind("PS1Rip/", 0) == 0)
            materialsToRemove.push_back(name);
        matIt.moveNext();
    }
    for (const std::string &name : materialsToRemove) {
        try {
            matMgr.remove(name);
        } catch (...) {
        }
    }

    Ogre::TextureManager &texMgr = Ogre::TextureManager::getSingleton();
    std::vector<std::string> texturesToRemove;
    Ogre::ResourceManager::ResourceMapIterator texIt = texMgr.getResourceIterator();
    while (texIt.hasMoreElements()) {
        const std::string name = texIt.peekNextValue()->getName();
        if (name.rfind("ps1rip_", 0) == 0)
            texturesToRemove.push_back(name);
        texIt.moveNext();
    }
    for (const std::string &name : texturesToRemove) {
        try {
            texMgr.remove(name);
        } catch (...) {
        }
    }

    purgeSessionResourceGroups();
}

void rollbackFailedAttach(Manager *mgr, const QStringList &nodeNames, const QString &reason)
{
    if (mgr) {
        for (const QString &name : nodeNames)
            mgr->destroySceneNode(name);
    }
    purgePriorCaptureGpuResources();
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.attach"), reason, QStringLiteral("error"));
}

void beginCaptureAttach(Manager *mgr, const QString &captureId, const CaptureSnapshot *textureSource,
                        TextureBuildContext *texCtx)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.attach"), QStringLiteral("start"));
    removePriorCaptureNodes(mgr);
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.attach"), QStringLiteral("purge"));
    purgePriorCaptureGpuResources();

    texCtx->sessionId = allocateSessionId();
    texCtx->resourceGroup = sessionResourceGroup(texCtx->sessionId);
    texCtx->captureId = captureId;
    ensureResourceGroup(texCtx->resourceGroup);
    predecodeCaptureTextures(textureSource, texCtx);
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

bool buildMeshResources(const ReconstructedMesh &mesh, const QString &captureId,
                        const CaptureSnapshot *textureSource, TextureBuildContext *ctx,
                        Ogre::MeshPtr &ogreMeshOut, Ogre::AxisAlignedBox &localBoundsOut)
{
    const QString meshName = mesh.meshName + QLatin1Char('_') + captureId;
    const std::string ogreMeshName = meshName.toStdString();

    if (Ogre::MeshManager::getSingleton().resourceExists(ogreMeshName))
        Ogre::MeshManager::getSingleton().remove(ogreMeshName);

    ogreMeshOut = Ogre::MeshManager::getSingleton().createManual(
        ogreMeshName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    localBoundsOut = meshBounds(mesh);
    Ogre::AxisAlignedBox bounds = localBoundsOut;
    if (bounds.isNull())
        bounds = Ogre::AxisAlignedBox(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);

    for (const ReconstructedSubMesh &sub : mesh.subMeshes) {
        Ogre::SubMesh *ogreSub = ogreMeshOut->createSubMesh();
        ogreSub->setMaterialName(ensureMaterial(sub.materialName, ctx)->getName());
        writeSubMesh(sub, ogreSub);
    }

    ogreMeshOut->_setBounds(bounds);
    ogreMeshOut->_setBoundingSphereRadius(bounds.getHalfSize().length());
    ogreMeshOut->load();
    return true;
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

    TextureBuildContext texCtx;
    beginCaptureAttach(mgr, captureId, textureSource, &texCtx);

    Ogre::MeshPtr ogreMesh;
    Ogre::AxisAlignedBox localBounds;
    buildMeshResources(mesh, captureId, textureSource, &texCtx, ogreMesh, localBounds);

    const QString nodeName = QStringLiteral("PS1Capture_%1").arg(captureId);
    Ogre::SceneNode *node = mgr->addSceneNode(nodeName);
    applyEditorPlacement(node, localBounds);
    Ogre::Entity *entity = mgr->createEntity(node, ogreMesh);

    if (!entity) {
        rollbackFailedAttach(mgr, {nodeName},
                             QStringLiteral("createEntity failed capture=%1").arg(captureId));
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
    SentryReporter::addBreadcrumb(
        QStringLiteral("ps1.rip.attach"),
        QStringLiteral("success capture=%1 verts=%2 tris=%3")
            .arg(captureId)
            .arg(mesh.vertexCount)
            .arg(mesh.triangleCount));
    return true;
}

bool PS1RipMeshBuilder::attachCaptureSetToScene(const ReconstructedCaptureSet &captureSet,
                                                const QString &captureId,
                                                const CaptureSnapshot *textureSource,
                                                BuildResult *resultOut, QString *errorOut)
{
    if (captureSet.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("Reconstructed capture set is empty");
        return false;
    }

    Manager *mgr = Manager::getSingletonPtr();
    if (!mgr) {
        if (errorOut)
            *errorOut = QStringLiteral("Editor scene is not available");
        return false;
    }

    QVector<Ogre::AxisAlignedBox> localBoundsByMesh(captureSet.uniqueMeshes.size());
    for (int i = 0; i < captureSet.uniqueMeshes.size(); ++i)
        localBoundsByMesh[i] = meshBounds(captureSet.uniqueMeshes[i]);

    Ogre::AxisAlignedBox combinedBounds;
    combinedBounds.setNull();
    for (const ReconstructedInstance &inst : captureSet.instances) {
        if (inst.uniqueMeshIndex < 0 || inst.uniqueMeshIndex >= localBoundsByMesh.size())
            continue;
        const Ogre::AxisAlignedBox &local = localBoundsByMesh[inst.uniqueMeshIndex];
        if (local.isNull())
            continue;
        const Ogre::Vector3 t(inst.px, inst.py, inst.pz);
        combinedBounds.merge(local.getMinimum() + t);
        combinedBounds.merge(local.getMaximum() + t);
    }
    if (combinedBounds.isNull()) {
        for (const Ogre::AxisAlignedBox &local : localBoundsByMesh) {
            if (!local.isNull())
                combinedBounds.merge(local);
        }
    }
    if (combinedBounds.isNull())
        combinedBounds = Ogre::AxisAlignedBox(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);

    const Ogre::Vector3 size = combinedBounds.getSize();
    const float maxExtent = std::max(size.x, std::max(size.y, size.z));
    float placementScale = 1.0f;
    if (maxExtent > 1e-6f) {
        placementScale = kTargetMaxExtent / maxExtent;
        if (maxExtent < 0.01f)
            placementScale = 3.0f / maxExtent;
    }

    TextureBuildContext texCtx;
    beginCaptureAttach(mgr, captureId, textureSource, &texCtx);

    int totalVerts = 0;
    int totalTris = 0;
    Ogre::SceneNode *firstNode = nullptr;
    Ogre::Entity *firstEntity = nullptr;

    QStringList createdNodeNames;
    int instanceOrdinal = 0;
    for (int meshIndex = 0; meshIndex < captureSet.uniqueMeshes.size(); ++meshIndex) {
        const ReconstructedMesh &mesh = captureSet.uniqueMeshes[meshIndex];
        Ogre::MeshPtr ogreMesh;
        Ogre::AxisAlignedBox localBounds;
        buildMeshResources(mesh, captureId, textureSource, &texCtx, ogreMesh, localBounds);

        totalVerts += mesh.vertexCount;
        totalTris += mesh.triangleCount;

        for (const ReconstructedInstance &inst : captureSet.instances) {
            if (inst.uniqueMeshIndex != meshIndex)
                continue;

            const QString nodeName =
                QStringLiteral("PS1Capture_%1_inst%2").arg(captureId).arg(instanceOrdinal++);
            Ogre::SceneNode *node = mgr->addSceneNode(nodeName);
            createdNodeNames.append(nodeName);
            node->setPosition(inst.px * placementScale, inst.py * placementScale,
                              inst.pz * placementScale);
            node->setScale(placementScale, placementScale, placementScale);

            Ogre::Entity *entity = mgr->createEntity(node, ogreMesh);
            if (!entity) {
                rollbackFailedAttach(
                    mgr, createdNodeNames,
                    QStringLiteral("createEntity failed capture=%1 instance=%2")
                        .arg(captureId)
                        .arg(instanceOrdinal - 1));
                if (errorOut)
                    *errorOut = QStringLiteral("Failed to create Ogre entity for instance");
                return false;
            }
            if (!firstNode) {
                firstNode = node;
                firstEntity = entity;
            }
        }
    }

    if (resultOut) {
        resultOut->sceneNode = firstNode;
        resultOut->entity = firstEntity;
        resultOut->vertexCount = totalVerts;
        resultOut->triangleCount = totalTris;
    }
    SentryReporter::addBreadcrumb(
        QStringLiteral("ps1.rip.attach"),
        QStringLiteral("success capture=%1 verts=%2 tris=%3 instances=%4")
            .arg(captureId)
            .arg(totalVerts)
            .arg(totalTris)
            .arg(captureSet.instances.size()));
    return true;
}
