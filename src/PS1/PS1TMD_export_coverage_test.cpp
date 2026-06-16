// Coverage tests for PS1TMD::exportEntity textured/colored/aliasing branches.
//
// PS1TMD_test.cpp already covers all import primitive modes plus an UNtextured
// single-triangle export round-trip. The remaining uncovered execution in the
// writer (PS1TMD.cpp ~lines 984-1175) is:
//   * the sibling-TIM emission path (textured submesh -> convertToImage ->
//     saveOgreImageToTim16 writes "<base>.tim")
//   * the textured FT3 (UV) prim emission + its UV round-trip on reimport
//   * the per-vertex VES_DIFFUSE color emission (appendG3C) branch
//   * the multi-source vertex-buffer aliasing branches (uv/col sharing pos/nrm
//     buffer vs. living in their own separate buffer).
//
// Distinct filename + suite name (PS1TMDExportCoverageTest) from PS1TMD_test.cpp
// to avoid ODR / duplicate-registration clashes.

#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

#include <cstdint>
#include <string>

#include <OgreColourValue.h>
#include <OgreHardwareBufferManager.h>
#include <OgreImage.h>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgrePass.h>
#include <OgrePixelFormat.h>
#include <OgreSubMesh.h>
#include <OgreTechnique.h>
#include <OgreTexture.h>
#include <OgreTextureManager.h>
#include <OgreTextureUnitState.h>
#include <OgreVertexIndexData.h>

#include "PS1/PS1TMD.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

namespace {

constexpr unsigned long kSettleMs = 30;

/** PS1TMD::buildMeshFromSoup (import path) clones "BaseMaterial". */
static void ensureBaseMaterialForTmdImport()
{
    if (Ogre::MaterialManager::getSingleton().getByName(
            "BaseMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)) {
        return;
    }
    Ogre::MaterialPtr m = Ogre::MaterialManager::getSingleton().create(
        "BaseMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    m->getTechnique(0)->getPass(0)->setDiffuse(1.0f, 1.0f, 1.0f, 1.0f);
    m->getTechnique(0)->getPass(0)->setAmbient(1.0f, 1.0f, 1.0f);
}

/** Create (or recreate) a 2x2 RGBA manual texture so a material can bind a real diffuse map. */
static Ogre::TexturePtr ensureDiffuseTexture(const std::string& texName)
{
    if (auto old = Ogre::TextureManager::getSingleton().getByName(texName))
        Ogre::TextureManager::getSingleton().remove(old->getHandle());

    static uint8_t pixels[2 * 2 * 4] = {
        255, 0,   0,   255,  0,   255, 0,   255,
        0,   0,   255, 255,  255, 255, 0,   255,
    };
    Ogre::Image img;
    img.loadDynamicImage(pixels, 2, 2, 1, Ogre::PF_BYTE_RGBA, false);
    Ogre::TexturePtr tex = Ogre::TextureManager::getSingleton().createManual(
        texName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
        Ogre::TEX_TYPE_2D, 2, 2, 0, Ogre::PF_BYTE_RGBA);
    tex->loadImage(img);
    return tex;
}

/** Create (or recreate) a material whose pass0 has a single named diffuse TUS. */
static Ogre::MaterialPtr ensureDiffuseMaterial(const std::string& matName, const std::string& texName)
{
    if (auto old = Ogre::MaterialManager::getSingleton().getByName(matName))
        Ogre::MaterialManager::getSingleton().remove(old);
    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().create(
        matName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::Pass* p = mat->getTechnique(0)->getPass(0);
    p->setLightingEnabled(false);
    Ogre::TextureUnitState* tus = p->createTextureUnitState(texName);
    tus->setName("diffuse_map");
    mat->load();
    return mat;
}

static void removeMeshIfExists(const std::string& name)
{
    if (auto old = Ogre::MeshManager::getSingleton().getByName(name))
        Ogre::MeshManager::getSingleton().remove(old);
}

/**
 * Build a single-triangle mesh.
 *
 *  separateUv  : if true, UV lives in its own vertex buffer (source 1), forcing the
 *                "lock a separate uv buffer" branch. If false, UV is interleaved into
 *                source 0 alongside POSITION/NORMAL, exercising the uvSrc==posSrc alias.
 *  withColor   : if true, add a VES_DIFFUSE color element. When separateColor is true the
 *                color lives in its own source (separate lock); otherwise it is interleaved.
 *  withUv      : add VES_TEXTURE_COORDINATES at all.
 */
static Ogre::MeshPtr buildTriMesh(const std::string& name,
                                  bool withUv, bool separateUv,
                                  bool withColor, bool separateColor,
                                  const std::string& materialName)
{
    removeMeshIfExists(name);

    Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::SubMesh* sm = mesh->createSubMesh();
    sm->setMaterialName(materialName);
    sm->useSharedVertices = false;

    Ogre::VertexData* vd = new Ogre::VertexData();
    sm->vertexData = vd;
    vd->vertexCount = 3;
    Ogre::VertexDeclaration* decl = vd->vertexDeclaration;
    Ogre::VertexBufferBinding* bind = vd->vertexBufferBinding;

    // Source 0 always holds POSITION + NORMAL, and optionally UV / COLOR when interleaved.
    size_t off0 = 0;
    decl->addElement(0, off0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    off0 += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, off0, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    off0 += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);

    const bool uvInSrc0 = withUv && !separateUv;
    size_t uvOff0 = 0;
    if (uvInSrc0) {
        uvOff0 = off0;
        decl->addElement(0, off0, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);
        off0 += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT2);
    }
    const bool colInSrc0 = withColor && !separateColor;
    size_t colOff0 = 0;
    if (colInSrc0) {
        colOff0 = off0;
        decl->addElement(0, off0, Ogre::VET_COLOUR, Ogre::VES_DIFFUSE);
        off0 += Ogre::VertexElement::getTypeSize(Ogre::VET_COLOUR);
    }

    const size_t v0size = decl->getVertexSize(0);
    auto vbuf0 = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        v0size, 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);

    const float tri[3][6] = {
        {0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        {1.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        {0.f, 1.f, 0.f, 0.f, 0.f, 1.f},
    };
    const float uvs[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    // Distinct per-vertex colors so the round-trip / color emission has signal.
    const Ogre::ColourValue cols[3] = {
        Ogre::ColourValue(1.0f, 0.0f, 0.0f, 1.0f),
        Ogre::ColourValue(0.0f, 1.0f, 0.0f, 1.0f),
        Ogre::ColourValue(0.0f, 0.0f, 1.0f, 1.0f),
    };

    {
        uint8_t* dst = static_cast<uint8_t*>(vbuf0->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for (int i = 0; i < 3; ++i) {
            uint8_t* row = dst + size_t(i) * v0size;
            float* p = nullptr;
            decl->findElementBySemantic(Ogre::VES_POSITION)->baseVertexPointerToElement(row, &p);
            p[0] = tri[i][0]; p[1] = tri[i][1]; p[2] = tri[i][2];
            decl->findElementBySemantic(Ogre::VES_NORMAL)->baseVertexPointerToElement(row, &p);
            p[0] = tri[i][3]; p[1] = tri[i][4]; p[2] = tri[i][5];
            if (uvInSrc0) {
                float* t = nullptr;
                decl->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES)->baseVertexPointerToElement(row, &t);
                t[0] = uvs[i][0]; t[1] = uvs[i][1];
            }
            if (colInSrc0) {
                Ogre::RGBA* c = nullptr;
                decl->findElementBySemantic(Ogre::VES_DIFFUSE)->baseVertexPointerToElement(row, &c);
                *c = cols[i].getAsARGB();
            }
        }
        vbuf0->unlock();
    }
    bind->setBinding(0, vbuf0);

    unsigned short nextSource = 1;

    if (withUv && separateUv) {
        const unsigned short src = nextSource++;
        decl->addElement(src, 0, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);
        const size_t sz = decl->getVertexSize(src);
        auto buf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            sz, 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        uint8_t* dst = static_cast<uint8_t*>(buf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for (int i = 0; i < 3; ++i) {
            float* t = reinterpret_cast<float*>(dst + size_t(i) * sz);
            t[0] = uvs[i][0]; t[1] = uvs[i][1];
        }
        buf->unlock();
        bind->setBinding(src, buf);
    }

    if (withColor && separateColor) {
        const unsigned short src = nextSource++;
        decl->addElement(src, 0, Ogre::VET_COLOUR, Ogre::VES_DIFFUSE);
        const size_t sz = decl->getVertexSize(src);
        auto buf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            sz, 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        uint8_t* dst = static_cast<uint8_t*>(buf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for (int i = 0; i < 3; ++i) {
            Ogre::RGBA* c = reinterpret_cast<Ogre::RGBA*>(dst + size_t(i) * sz);
            *c = cols[i].getAsARGB();
        }
        buf->unlock();
        bind->setBinding(src, buf);
    }

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = {0, 1, 2};
    ibuf->writeData(0, sizeof(idx), idx);
    sm->indexData->indexBuffer = ibuf;
    sm->indexData->indexCount = 3;
    sm->indexData->indexStart = 0;

    mesh->_setBounds(Ogre::AxisAlignedBox(0, 0, 0, 1, 1, 0));
    mesh->_setBoundingSphereRadius(2.0f);
    mesh->load();
    return mesh;
}

} // namespace

class PS1TMDExportCoverageTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    int meshSeq = 0;

    void SetUp() override
    {
        SelectionSet::kill();
        Manager::kill();
        QThread::msleep(kSettleMs);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed";
        ASSERT_TRUE(canLoadMeshFiles()) << "GL/hardware buffers required (Xvfb in CI)";
        createStandardOgreMaterials();
        ensureBaseMaterialForTmdImport();
    }

    void TearDown() override
    {
        if (Manager::getSingletonPtr())
            SelectionSet::getSingleton()->clear();
        SelectionSet::kill();
        Manager::kill();
        if (app)
            app->processEvents();
        QThread::msleep(kSettleMs);
    }

    std::string uniqueName(const char* base)
    {
        return std::string(base) + std::to_string(meshSeq++);
    }

    // Attach a mesh into the scene and return the entity (Manager owns it).
    Ogre::Entity* attach(const Ogre::MeshPtr& mesh, const std::string& nodeName)
    {
        auto* mgr = Manager::getSingleton();
        Ogre::SceneNode* node = mgr->addSceneNode(QString::fromStdString(nodeName));
        EXPECT_NE(node, nullptr);
        if (!node)
            return nullptr;
        return mgr->createEntity(node, mesh);
    }
};

// --- Textured (separate UV buffer) export: sibling .tim is written + UV survives reimport. ---
TEST_F(PS1TMDExportCoverageTest, TexturedExportWritesSiblingTimAndUvRoundTrips)
{
    const std::string texName = uniqueName("PS1TmdCovTex");
    const std::string matName = uniqueName("PS1TmdCovMat");
    ensureDiffuseTexture(texName);
    ensureDiffuseMaterial(matName, texName);

    const std::string meshName = uniqueName("PS1TmdCovTexturedMesh");
    Ogre::MeshPtr mesh = buildTriMesh(meshName, /*withUv*/ true, /*separateUv*/ true,
                                      /*withColor*/ false, /*separateColor*/ false, matName);
    ASSERT_TRUE(mesh);

    Ogre::Entity* ent = attach(mesh, uniqueName("PS1TmdCovNode"));
    ASSERT_NE(ent, nullptr);
    // Confirm the subentity material actually carries the textured diffuse TUS.
    ent->getSubEntity(0)->setMaterialName(matName);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString tmdPath = QDir(dir.path()).filePath(QStringLiteral("textured_model.tmd"));
    const QString timPath = QDir(dir.path()).filePath(QStringLiteral("textured_model.tim"));

    ASSERT_TRUE(PS1TMD::exportEntity(ent, tmdPath));
    EXPECT_TRUE(QFileInfo(tmdPath).exists());
    // Sibling TIM emitted by the textured branch (convertToImage -> saveOgreImageToTim16).
    EXPECT_TRUE(QFileInfo(timPath).exists());
    EXPECT_GT(QFileInfo(timPath).size(), 0);

    // Re-import the produced TMD: a textured FT3 triangle decodes to a submesh with UVs,
    // validating that the exporter wrote real UV bytes.
    const std::string reName = uniqueName("PS1TmdCovTexturedReimport");
    removeMeshIfExists(reName);
    Ogre::MeshPtr re = PS1TMD::importTmd(tmdPath, reName);
    ASSERT_TRUE(re);
    ASSERT_GE(re->getNumSubMeshes(), 1u);
    const Ogre::SubMesh* rsm = re->getSubMesh(0);
    ASSERT_NE(rsm->vertexData, nullptr);
    EXPECT_GE(rsm->vertexData->vertexCount, 3u);
    const auto* uvEl =
        rsm->vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES);
    EXPECT_NE(uvEl, nullptr) << "textured export should round-trip a UV channel";

    removeMeshIfExists(reName);
}

// --- Textured with UV interleaved into the position buffer: exercises uvSrc==posSrc alias. ---
TEST_F(PS1TMDExportCoverageTest, TexturedExportInterleavedUvAliasesPositionBuffer)
{
    const std::string texName = uniqueName("PS1TmdCovTexI");
    const std::string matName = uniqueName("PS1TmdCovMatI");
    ensureDiffuseTexture(texName);
    ensureDiffuseMaterial(matName, texName);

    const std::string meshName = uniqueName("PS1TmdCovInterleavedMesh");
    Ogre::MeshPtr mesh = buildTriMesh(meshName, /*withUv*/ true, /*separateUv*/ false,
                                      /*withColor*/ false, /*separateColor*/ false, matName);
    ASSERT_TRUE(mesh);
    Ogre::Entity* ent = attach(mesh, uniqueName("PS1TmdCovNodeI"));
    ASSERT_NE(ent, nullptr);
    ent->getSubEntity(0)->setMaterialName(matName);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString tmdPath = QDir(dir.path()).filePath(QStringLiteral("interleaved.tmd"));
    const QString timPath = QDir(dir.path()).filePath(QStringLiteral("interleaved.tim"));

    ASSERT_TRUE(PS1TMD::exportEntity(ent, tmdPath));
    EXPECT_TRUE(QFileInfo(tmdPath).exists());
    EXPECT_TRUE(QFileInfo(timPath).exists());

    const std::string reName = uniqueName("PS1TmdCovInterleavedReimport");
    removeMeshIfExists(reName);
    Ogre::MeshPtr re = PS1TMD::importTmd(tmdPath, reName);
    ASSERT_TRUE(re);
    ASSERT_GE(re->getNumSubMeshes(), 1u);
    const auto* uvEl =
        re->getSubMesh(0)->vertexData->vertexDeclaration->findElementBySemantic(
            Ogre::VES_TEXTURE_COORDINATES);
    EXPECT_NE(uvEl, nullptr);
    removeMeshIfExists(reName);
}

// --- Per-vertex VES_DIFFUSE color export, color in a separate buffer (untextured material). ---
TEST_F(PS1TMDExportCoverageTest, UntexturedPerVertexColorExportsG3CSeparateBuffer)
{
    const std::string meshName = uniqueName("PS1TmdCovColorSepMesh");
    // Untextured material so the writer takes the else-branch -> colEl -> appendG3C.
    Ogre::MeshPtr mesh = buildTriMesh(meshName, /*withUv*/ false, /*separateUv*/ false,
                                      /*withColor*/ true, /*separateColor*/ true, "BaseWhite");
    ASSERT_TRUE(mesh);
    Ogre::Entity* ent = attach(mesh, uniqueName("PS1TmdCovColorSepNode"));
    ASSERT_NE(ent, nullptr);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString tmdPath = QDir(dir.path()).filePath(QStringLiteral("colored_sep.tmd"));
    const QString timPath = QDir(dir.path()).filePath(QStringLiteral("colored_sep.tim"));

    ASSERT_TRUE(PS1TMD::exportEntity(ent, tmdPath));
    EXPECT_TRUE(QFileInfo(tmdPath).exists());
    // No diffuse texture -> no sibling TIM should be written.
    EXPECT_FALSE(QFileInfo(timPath).exists());

    // Re-import: the colored G3 triangle (mode 0x30, ilen 6) yields a VES_DIFFUSE channel.
    const std::string reName = uniqueName("PS1TmdCovColorSepReimport");
    removeMeshIfExists(reName);
    Ogre::MeshPtr re = PS1TMD::importTmd(tmdPath, reName);
    ASSERT_TRUE(re);
    ASSERT_GE(re->getNumSubMeshes(), 1u);
    const Ogre::SubMesh* rsm = re->getSubMesh(0);
    ASSERT_NE(rsm->vertexData, nullptr);
    EXPECT_GE(rsm->vertexData->vertexCount, 3u);
    const auto* colEl =
        rsm->vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_DIFFUSE);
    EXPECT_NE(colEl, nullptr) << "per-vertex color export should round-trip a diffuse channel";
    removeMeshIfExists(reName);
}

// --- Per-vertex VES_DIFFUSE color export, color interleaved in the position buffer. ---
TEST_F(PS1TMDExportCoverageTest, UntexturedPerVertexColorExportsG3CInterleaved)
{
    const std::string meshName = uniqueName("PS1TmdCovColorIntMesh");
    Ogre::MeshPtr mesh = buildTriMesh(meshName, /*withUv*/ false, /*separateUv*/ false,
                                      /*withColor*/ true, /*separateColor*/ false, "BaseWhite");
    ASSERT_TRUE(mesh);
    Ogre::Entity* ent = attach(mesh, uniqueName("PS1TmdCovColorIntNode"));
    ASSERT_NE(ent, nullptr);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString tmdPath = QDir(dir.path()).filePath(QStringLiteral("colored_int.tmd"));

    ASSERT_TRUE(PS1TMD::exportEntity(ent, tmdPath));
    EXPECT_TRUE(QFileInfo(tmdPath).exists());

    const std::string reName = uniqueName("PS1TmdCovColorIntReimport");
    removeMeshIfExists(reName);
    Ogre::MeshPtr re = PS1TMD::importTmd(tmdPath, reName);
    ASSERT_TRUE(re);
    ASSERT_GE(re->getNumSubMeshes(), 1u);
    const auto* colEl =
        re->getSubMesh(0)->vertexData->vertexDeclaration->findElementBySemantic(
            Ogre::VES_DIFFUSE);
    EXPECT_NE(colEl, nullptr);
    removeMeshIfExists(reName);
}

// --- Textured + per-vertex color in separate buffers: exercises the full multi-source
//     lock/alias path (pos in src0, uv in src1, col in src2 all distinct). ---
TEST_F(PS1TMDExportCoverageTest, TexturedPlusSeparateColorMultiSourceExport)
{
    const std::string texName = uniqueName("PS1TmdCovTexM");
    const std::string matName = uniqueName("PS1TmdCovMatM");
    ensureDiffuseTexture(texName);
    ensureDiffuseMaterial(matName, texName);

    const std::string meshName = uniqueName("PS1TmdCovMultiMesh");
    Ogre::MeshPtr mesh = buildTriMesh(meshName, /*withUv*/ true, /*separateUv*/ true,
                                      /*withColor*/ true, /*separateColor*/ true, matName);
    ASSERT_TRUE(mesh);
    Ogre::Entity* ent = attach(mesh, uniqueName("PS1TmdCovMultiNode"));
    ASSERT_NE(ent, nullptr);
    ent->getSubEntity(0)->setMaterialName(matName);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString tmdPath = QDir(dir.path()).filePath(QStringLiteral("multi.tmd"));
    const QString timPath = QDir(dir.path()).filePath(QStringLiteral("multi.tim"));

    ASSERT_TRUE(PS1TMD::exportEntity(ent, tmdPath));
    EXPECT_TRUE(QFileInfo(tmdPath).exists());
    // Textured -> FT3 path is taken (UV wins over color), and sibling TIM is written.
    EXPECT_TRUE(QFileInfo(timPath).exists());

    const std::string reName = uniqueName("PS1TmdCovMultiReimport");
    removeMeshIfExists(reName);
    Ogre::MeshPtr re = PS1TMD::importTmd(tmdPath, reName);
    ASSERT_TRUE(re);
    ASSERT_GE(re->getNumSubMeshes(), 1u);
    const auto* uvEl =
        re->getSubMesh(0)->vertexData->vertexDeclaration->findElementBySemantic(
            Ogre::VES_TEXTURE_COORDINATES);
    EXPECT_NE(uvEl, nullptr) << "textured-with-color export still emits UV (FT3) prims";
    removeMeshIfExists(reName);
}

// --- Textured material is referenced but its bound texture name does not resolve in
//     TextureManager: writer must still succeed (TMD written) and skip the sibling TIM. ---
TEST_F(PS1TMDExportCoverageTest, TexturedMaterialMissingTextureStillExportsTmdNoTim)
{
    const std::string matName = uniqueName("PS1TmdCovMatMissing");
    // Material with a named diffuse TUS pointing at a texture that was never created.
    if (auto old = Ogre::MaterialManager::getSingleton().getByName(matName))
        Ogre::MaterialManager::getSingleton().remove(old);
    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().create(
        matName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::Pass* p = mat->getTechnique(0)->getPass(0);
    p->setLightingEnabled(false);
    Ogre::TextureUnitState* tus = p->createTextureUnitState("PS1TmdCovDoesNotExist.png");
    tus->setName("diffuse_map");

    const std::string meshName = uniqueName("PS1TmdCovMissingMesh");
    Ogre::MeshPtr mesh = buildTriMesh(meshName, /*withUv*/ true, /*separateUv*/ true,
                                      /*withColor*/ false, /*separateColor*/ false, matName);
    ASSERT_TRUE(mesh);
    Ogre::Entity* ent = attach(mesh, uniqueName("PS1TmdCovMissingNode"));
    ASSERT_NE(ent, nullptr);
    ent->getSubEntity(0)->setMaterialName(matName);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString tmdPath = QDir(dir.path()).filePath(QStringLiteral("missingtex.tmd"));
    const QString timPath = QDir(dir.path()).filePath(QStringLiteral("missingtex.tim"));

    // submeshHasDiffuseTexture() is true (named, non-empty), but getByName() returns null,
    // so the sibling-TIM block is skipped while geometry export still proceeds.
    EXPECT_TRUE(PS1TMD::exportEntity(ent, tmdPath));
    EXPECT_TRUE(QFileInfo(tmdPath).exists());
    EXPECT_FALSE(QFileInfo(timPath).exists());
}
