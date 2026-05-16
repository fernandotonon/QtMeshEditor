#include <gtest/gtest.h>
#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <cstring>
#include <vector>

#include <OgreHardwareBufferManager.h>
#include <OgreImage.h>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgreSubMesh.h>
#include <OgreTextureManager.h>
#include <OgreTextureUnitState.h>
#include <OgreVertexIndexData.h>

#include "PS1/PS1TIM.h"
#include "PS1/PS1TMD.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

namespace {

constexpr unsigned long kSingletonSettleMs = 30;

/** PS1TMD::buildMeshFromSoup clones BaseMaterial; tests only create BaseWhite*. */
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

static void writeU32le(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v & 0xFF);
    p[1] = uint8_t((v >> 8) & 0xFF);
    p[2] = uint8_t((v >> 16) & 0xFF);
    p[3] = uint8_t((v >> 24) & 0xFF);
}

static void writeU16le(uint8_t* p, uint16_t v)
{
    p[0] = uint8_t(v & 0xFF);
    p[1] = uint8_t((v >> 8) & 0xFF);
}

static void writeVertex8(int16_t x, int16_t y, int16_t z, uint8_t* out8)
{
    writeU16le(out8 + 0, static_cast<uint16_t>(x));
    writeU16le(out8 + 2, static_cast<uint16_t>(y));
    writeU16le(out8 + 4, static_cast<uint16_t>(z));
    writeU16le(out8 + 6, 0);
}

/** Minimal TMD: one object, three vertices, three normals, one G3 triangle (matches PS1TMD::appendG3). */
static QByteArray makeMinimalG3Tmd()
{
    constexpr uint32_t kTmdId = 0x41u;
    constexpr size_t kHead = 12u;
    constexpr size_t kObjH = 28u;
    const size_t vAbs = kHead + kObjH;
    const size_t nAbs = vAbs + 3u * 8u;
    const size_t pAbs = nAbs + 3u * 8u;
    const uint32_t vOff = static_cast<uint32_t>(vAbs - 12u);
    const uint32_t nOff = static_cast<uint32_t>(nAbs - 12u);
    const uint32_t pOff = static_cast<uint32_t>(pAbs - 12u);

    QByteArray buf(static_cast<int>(pAbs + 20u), '\0');
    uint8_t* d = reinterpret_cast<uint8_t*>(buf.data());

    writeU32le(d, kTmdId);
    writeU32le(d + 4, 0);
    writeU32le(d + 8, 1);

    uint8_t* oh = d + kHead;
    writeU32le(oh, vOff);
    writeU32le(oh + 4, 3);
    writeU32le(oh + 8, nOff);
    writeU32le(oh + 12, 3);
    writeU32le(oh + 16, pOff);
    writeU32le(oh + 20, 1);
    writeU32le(oh + 24, 0);

    writeVertex8(0, 0, 0, d + vAbs);
    writeVertex8(4096, 0, 0, d + vAbs + 8);
    writeVertex8(0, 4096, 0, d + vAbs + 16);

    writeVertex8(0, 0, 4096, d + nAbs);
    writeVertex8(0, 0, 4096, d + nAbs + 8);
    writeVertex8(0, 0, 4096, d + nAbs + 16);

    uint8_t* pkt = d + pAbs;
    pkt[0] = 6;
    pkt[1] = 4;
    pkt[2] = 0;
    pkt[3] = 0x30;
    pkt[4] = 200;
    pkt[5] = 200;
    pkt[6] = 200;
    pkt[7] = 0x30;
    writeU16le(pkt + 8, 0);
    writeU16le(pkt + 10, 0);
    writeU16le(pkt + 12, 1);
    writeU16le(pkt + 14, 1);
    writeU16le(pkt + 16, 2);
    writeU16le(pkt + 18, 2);

    return buf;
}

/** Minimal TMD: one object, three vertices, three normals, one G3 triangle with per-vertex colors (mode 0x30, ilen 6). */
static QByteArray makeMinimalG3ColorTmd()
{
    constexpr uint32_t kTmdId = 0x41u;
    constexpr size_t kHead = 12u;
    constexpr size_t kObjH = 28u;
    const size_t vAbs = kHead + kObjH;
    const size_t nAbs = vAbs + 3u * 8u;
    const size_t pAbs = nAbs + 3u * 8u;
    const uint32_t vOff = static_cast<uint32_t>(vAbs - 12u);
    const uint32_t nOff = static_cast<uint32_t>(nAbs - 12u);
    const uint32_t pOff = static_cast<uint32_t>(pAbs - 12u);

    QByteArray buf(static_cast<int>(pAbs + 4u + 24u), '\0');
    uint8_t* d = reinterpret_cast<uint8_t*>(buf.data());

    writeU32le(d, kTmdId);
    writeU32le(d + 4, 0);
    writeU32le(d + 8, 1);

    uint8_t* oh = d + kHead;
    writeU32le(oh, vOff);
    writeU32le(oh + 4, 3);
    writeU32le(oh + 8, nOff);
    writeU32le(oh + 12, 3);
    writeU32le(oh + 16, pOff);
    writeU32le(oh + 20, 1);
    writeU32le(oh + 24, 0);

    writeVertex8(0, 0, 0, d + vAbs);
    writeVertex8(4096, 0, 0, d + vAbs + 8);
    writeVertex8(0, 4096, 0, d + vAbs + 16);

    writeVertex8(0, 0, 4096, d + nAbs);
    writeVertex8(0, 0, 4096, d + nAbs + 8);
    writeVertex8(0, 0, 4096, d + nAbs + 16);

    uint8_t* pkt = d + pAbs;
    pkt[0] = 8;
    pkt[1] = 6;
    pkt[2] = 0;
    pkt[3] = 0x30;
    uint8_t* pay = pkt + 4;
    // RGB0, RGB1, RGB2 (with pad bytes).
    pay[0] = 255; pay[1] = 0; pay[2] = 0; pay[3] = 0;
    pay[4] = 0; pay[5] = 255; pay[6] = 0; pay[7] = 0;
    pay[8] = 0; pay[9] = 0; pay[10] = 255; pay[11] = 0;
    writeU16le(pay + 12, 0); writeU16le(pay + 14, 0);
    writeU16le(pay + 16, 1); writeU16le(pay + 18, 1);
    writeU16le(pay + 20, 2); writeU16le(pay + 22, 2);

    return buf;
}

/** One textured tri, no-light (mode 0x25, flag 1, ilen 6) — Net Yaroze layout. */
static QByteArray makeMinimal25NoLightTmd()
{
    constexpr uint32_t kTmdId = 0x41u;
    constexpr size_t kHead = 12u;
    constexpr size_t kObjH = 28u;
    const size_t vAbs = kHead + kObjH;
    const size_t nAbs = vAbs + 3u * 8u;
    const size_t pAbs = nAbs + 1u * 8u;
    const uint32_t vOff = static_cast<uint32_t>(vAbs - 12u);
    const uint32_t nOff = static_cast<uint32_t>(nAbs - 12u);
    const uint32_t pOff = static_cast<uint32_t>(pAbs - 12u);

    QByteArray buf(static_cast<int>(pAbs + 4u + 24u), '\0');
    uint8_t* d = reinterpret_cast<uint8_t*>(buf.data());

    writeU32le(d, kTmdId);
    writeU32le(d + 4, 0);
    writeU32le(d + 8, 1);

    uint8_t* oh = d + kHead;
    writeU32le(oh, vOff);
    writeU32le(oh + 4, 3);
    writeU32le(oh + 8, nOff);
    writeU32le(oh + 12, 1);
    writeU32le(oh + 16, pOff);
    writeU32le(oh + 20, 1);
    writeU32le(oh + 24, 0);

    writeVertex8(0, 0, 0, d + vAbs);
    writeVertex8(4096, 0, 0, d + vAbs + 8);
    writeVertex8(0, 4096, 0, d + vAbs + 16);

    writeVertex8(0, 0, 4096, d + nAbs);

    uint8_t* pkt = d + pAbs;
    pkt[0] = 7;
    pkt[1] = 6;
    pkt[2] = 1;
    pkt[3] = 0x25;
    uint8_t* pay = pkt + 4;
    pay[0] = 10;
    pay[1] = 20;
    writeU16le(pay + 2, 0);
    pay[4] = 30;
    pay[5] = 40;
    writeU16le(pay + 6, 0);
    pay[8] = 50;
    pay[9] = 60;
    pay[10] = 0;
    pay[11] = 0;
    pay[12] = 40;
    pay[13] = 40;
    pay[14] = 40;
    pay[15] = 0;
    writeU16le(pay + 16, 0);
    writeU16le(pay + 18, 1);
    writeU16le(pay + 20, 2);

    return buf;
}

/** One Gouraud-textured tri, no-light (mode 0x35, flag 1, ilen 8). */
static QByteArray makeMinimal35NoLightTmd()
{
    constexpr uint32_t kTmdId = 0x41u;
    constexpr size_t kHead = 12u;
    constexpr size_t kObjH = 28u;
    const size_t vAbs = kHead + kObjH;
    const size_t nAbs = vAbs + 3u * 8u;
    const size_t pAbs = nAbs + 1u * 8u;
    const uint32_t vOff = static_cast<uint32_t>(vAbs - 12u);
    const uint32_t nOff = static_cast<uint32_t>(nAbs - 12u);
    const uint32_t pOff = static_cast<uint32_t>(pAbs - 12u);

    QByteArray buf(static_cast<int>(pAbs + 4u + 32u), '\0');
    uint8_t* d = reinterpret_cast<uint8_t*>(buf.data());

    writeU32le(d, kTmdId);
    writeU32le(d + 4, 0);
    writeU32le(d + 8, 1);

    uint8_t* oh = d + kHead;
    writeU32le(oh, vOff);
    writeU32le(oh + 4, 3);
    writeU32le(oh + 8, nOff);
    writeU32le(oh + 12, 1);
    writeU32le(oh + 16, pOff);
    writeU32le(oh + 20, 1);
    writeU32le(oh + 24, 0);

    writeVertex8(0, 0, 0, d + vAbs);
    writeVertex8(4096, 0, 0, d + vAbs + 8);
    writeVertex8(0, 4096, 0, d + vAbs + 16);

    writeVertex8(0, 0, 4096, d + nAbs);

    uint8_t* pkt = d + pAbs;
    pkt[0] = 9;
    pkt[1] = 8;
    pkt[2] = 1;
    pkt[3] = 0x35;
    uint8_t* pay = pkt + 4;
    pay[0] = 10;
    pay[1] = 20;
    writeU16le(pay + 2, 0);
    pay[4] = 30;
    pay[5] = 40;
    writeU16le(pay + 6, 0);
    pay[8] = 50;
    pay[9] = 60;
    pay[10] = 0;
    pay[11] = 0;
    // RGB triplets + pads (12 bytes)
    for (int i = 0; i < 12; ++i)
        pay[12 + i] = static_cast<uint8_t>(i + 1);
    writeU16le(pay + 24, 0);
    writeU16le(pay + 26, 1);
    writeU16le(pay + 28, 2);

    return buf;
}

/** One flat-colored quad (mode 0x28, flag 0, ilen 4). */
static QByteArray makeMinimal28FlatQuadTmd()
{
    constexpr uint32_t kTmdId = 0x41u;
    constexpr size_t kHead = 12u;
    constexpr size_t kObjH = 28u;
    const size_t vAbs = kHead + kObjH;
    const size_t nAbs = vAbs + 4u * 8u;
    const size_t pAbs = nAbs + 1u * 8u;
    const uint32_t vOff = static_cast<uint32_t>(vAbs - 12u);
    const uint32_t nOff = static_cast<uint32_t>(nAbs - 12u);
    const uint32_t pOff = static_cast<uint32_t>(pAbs - 12u);

    QByteArray buf(static_cast<int>(pAbs + 4u + 16u), '\0');
    uint8_t* d = reinterpret_cast<uint8_t*>(buf.data());

    writeU32le(d, kTmdId);
    writeU32le(d + 4, 0);
    writeU32le(d + 8, 1);

    uint8_t* oh = d + kHead;
    writeU32le(oh, vOff);
    writeU32le(oh + 4, 4);
    writeU32le(oh + 8, nOff);
    writeU32le(oh + 12, 1);
    writeU32le(oh + 16, pOff);
    writeU32le(oh + 20, 1);
    writeU32le(oh + 24, 0);

    // Unit square in XY plane.
    writeVertex8(0, 0, 0, d + vAbs);
    writeVertex8(4096, 0, 0, d + vAbs + 8);
    writeVertex8(4096, 4096, 0, d + vAbs + 16);
    writeVertex8(0, 4096, 0, d + vAbs + 24);

    // +Z normal.
    writeVertex8(0, 0, 4096, d + nAbs);

    uint8_t* pkt = d + pAbs;
    pkt[0] = 6;
    pkt[1] = 4;
    pkt[2] = 0;
    pkt[3] = 0x28;
    uint8_t* pay = pkt + 4;
    pay[0] = 128;
    pay[1] = 128;
    pay[2] = 128;
    pay[3] = 0;
    writeU16le(pay + 4, 0); // normal index
    writeU16le(pay + 6, 0);
    writeU16le(pay + 8, 1);
    writeU16le(pay + 10, 2);
    writeU16le(pay + 12, 3);
    writeU16le(pay + 14, 0);

    return buf;
}

/** One gradated quad (mode 0x28, flag 4, ilen 7). */
static QByteArray makeMinimal28GradQuadTmd()
{
    constexpr uint32_t kTmdId = 0x41u;
    constexpr size_t kHead = 12u;
    constexpr size_t kObjH = 28u;
    const size_t vAbs = kHead + kObjH;
    const size_t nAbs = vAbs + 4u * 8u;
    const size_t pAbs = nAbs + 1u * 8u;
    const uint32_t vOff = static_cast<uint32_t>(vAbs - 12u);
    const uint32_t nOff = static_cast<uint32_t>(nAbs - 12u);
    const uint32_t pOff = static_cast<uint32_t>(pAbs - 12u);

    QByteArray buf(static_cast<int>(pAbs + 4u + 28u), '\0');
    uint8_t* d = reinterpret_cast<uint8_t*>(buf.data());

    writeU32le(d, kTmdId);
    writeU32le(d + 4, 0);
    writeU32le(d + 8, 1);

    uint8_t* oh = d + kHead;
    writeU32le(oh, vOff);
    writeU32le(oh + 4, 4);
    writeU32le(oh + 8, nOff);
    writeU32le(oh + 12, 1);
    writeU32le(oh + 16, pOff);
    writeU32le(oh + 20, 1);
    writeU32le(oh + 24, 0);

    // Unit square in XY plane.
    writeVertex8(0, 0, 0, d + vAbs);
    writeVertex8(4096, 0, 0, d + vAbs + 8);
    writeVertex8(4096, 4096, 0, d + vAbs + 16);
    writeVertex8(0, 4096, 0, d + vAbs + 24);

    // +Z normal.
    writeVertex8(0, 0, 4096, d + nAbs);

    uint8_t* pkt = d + pAbs;
    pkt[0] = 8;
    pkt[1] = 7;
    pkt[2] = 4;
    pkt[3] = 0x28;
    uint8_t* pay = pkt + 4;
    // Colors (RGB + pad) * 4
    pay[0] = 255; pay[1] = 0;   pay[2] = 0;   pay[3] = 0;
    pay[4] = 0;   pay[5] = 255; pay[6] = 0;   pay[7] = 0;
    pay[8] = 0;   pay[9] = 0;   pay[10] = 255; pay[11] = 0;
    pay[12] = 255; pay[13] = 255; pay[14] = 0; pay[15] = 0;
    writeU16le(pay + 16, 0); // normal index
    writeU16le(pay + 18, 0);
    writeU16le(pay + 20, 1);
    writeU16le(pay + 22, 2);
    writeU16le(pay + 24, 3);
    writeU16le(pay + 26, 0);

    return buf;
}

static Ogre::MeshPtr createSingleTriMesh(const std::string& name)
{
    if (auto old = Ogre::MeshManager::getSingleton().getByName(name))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::SubMesh* sm = mesh->createSubMesh();
    sm->setMaterialName("BaseOutlined");
    sm->useSharedVertices = false;

    Ogre::VertexData* vd = new Ogre::VertexData();
    sm->vertexData = vd;
    vd->vertexCount = 3;
    Ogre::VertexDeclaration* decl = vd->vertexDeclaration;
    Ogre::VertexBufferBinding* bind = vd->vertexBufferBinding;
    size_t off = 0;
    decl->addElement(0, off, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    off += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, off, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    off += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    const size_t vsize = decl->getVertexSize(0);
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        vsize, 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint8_t* dst = static_cast<uint8_t*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
    const float tri[][6] = {
        {0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        {1.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        {0.f, 1.f, 0.f, 0.f, 0.f, 1.f},
    };
    for (int i = 0; i < 3; ++i) {
        uint8_t* row = dst + i * vsize;
        float* p = nullptr;
        decl->findElementBySemantic(Ogre::VES_POSITION)->baseVertexPointerToElement(row, &p);
        p[0] = tri[i][0];
        p[1] = tri[i][1];
        p[2] = tri[i][2];
        decl->findElementBySemantic(Ogre::VES_NORMAL)->baseVertexPointerToElement(row, &p);
        p[0] = tri[i][3];
        p[1] = tri[i][4];
        p[2] = tri[i][5];
    }
    vbuf->unlock();
    bind->setBinding(0, vbuf);

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

class PS1TMDTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override
    {
        SelectionSet::kill();
        Manager::kill();
        QThread::msleep(kSingletonSettleMs);

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
        QThread::msleep(kSingletonSettleMs);
    }
};

TEST_F(PS1TMDTest, ImportMode25NoLightTexturedTriangle)
{
    QTemporaryFile tmp(QDir::tempPath() + "/qtmesh_ps1tmd_25_XXXXXX.tmd");
    tmp.setAutoRemove(true);
    ASSERT_TRUE(tmp.open());
    const QByteArray blob = makeMinimal25NoLightTmd();
    ASSERT_EQ(tmp.write(blob), blob.size());
    tmp.flush();

    const std::string meshName = "PS1Tmd25Mesh";
    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr mesh = PS1TMD::importTmd(tmp.fileName(), meshName);
    ASSERT_TRUE(mesh);
    ASSERT_EQ(mesh->getNumSubMeshes(), 1u);
    Ogre::SubMesh* sm = mesh->getSubMesh(0);
    EXPECT_EQ(sm->vertexData->vertexCount, 3u);

    Ogre::VertexData* vd = sm->vertexData;
    const auto* uvEl = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES);
    ASSERT_NE(uvEl, nullptr);
    auto uvBuf = vd->vertexBufferBinding->getBuffer(uvEl->getSource());
    const uint8_t* ubase = static_cast<const uint8_t*>(uvBuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    const size_t stride = uvBuf->getVertexSize();
    float* pf = nullptr;
    uvEl->baseVertexPointerToElement(const_cast<uint8_t*>(ubase), &pf);
    // pay[0]=10, pay[1]=20 → (10.5/256, 20.5/256) after import (first corner, winding permuted to slot 0)
    EXPECT_NEAR(pf[0], 10.5f / 256.0f, 1e-5f);
    EXPECT_NEAR(pf[1], 20.5f / 256.0f, 1e-5f);
    uvBuf->unlock();
}

/** Lit textured quad (mode 0x2c, flag 0, ilen 7) — Sony tmd.h TMD_F_4T. */
static QByteArray makeMinimal2cTexturedQuadTmd()
{
    constexpr uint32_t kTmdId = 0x41u;
    constexpr size_t kHead = 12u;
    constexpr size_t kObjH = 28u;
    const size_t vAbs = kHead + kObjH;
    const size_t nAbs = vAbs + 4u * 8u;
    const size_t pAbs = nAbs + 1u * 8u;
    const uint32_t vOff = static_cast<uint32_t>(vAbs - 12u);
    const uint32_t nOff = static_cast<uint32_t>(nAbs - 12u);
    const uint32_t pOff = static_cast<uint32_t>(pAbs - 12u);

    QByteArray buf(static_cast<int>(pAbs + 4u + 28u), '\0');
    uint8_t* d = reinterpret_cast<uint8_t*>(buf.data());

    writeU32le(d, kTmdId);
    writeU32le(d + 4, 0);
    writeU32le(d + 8, 1);

    uint8_t* oh = d + kHead;
    writeU32le(oh, vOff);
    writeU32le(oh + 4, 4);
    writeU32le(oh + 8, nOff);
    writeU32le(oh + 12, 1);
    writeU32le(oh + 16, pOff);
    writeU32le(oh + 20, 1);
    writeU32le(oh + 24, 0);

    writeVertex8(0, 0, 0, d + vAbs);
    writeVertex8(4096, 0, 0, d + vAbs + 8);
    writeVertex8(4096, 4096, 0, d + vAbs + 16);
    writeVertex8(0, 4096, 0, d + vAbs + 24);

    writeVertex8(0, 0, 4096, d + nAbs);

    uint8_t* pkt = d + pAbs;
    pkt[0] = 9;
    pkt[1] = 7;
    pkt[2] = 0;
    pkt[3] = 0x2c;
    uint8_t* pay = pkt + 4;
    pay[0] = 0;
    pay[1] = 0;
    writeU16le(pay + 2, 0);
    pay[4] = 255;
    pay[5] = 0;
    writeU16le(pay + 6, 0);
    pay[8] = 255;
    pay[9] = 255;
    writeU16le(pay + 10, 0);
    pay[12] = 0;
    pay[13] = 255;
    writeU16le(pay + 14, 0);
    writeU16le(pay + 16, 0);
    writeU16le(pay + 18, 0);
    writeU16le(pay + 20, 1);
    writeU16le(pay + 22, 2);
    writeU16le(pay + 24, 3);
    writeU16le(pay + 26, 0);

    return buf;
}

TEST_F(PS1TMDTest, ImportMode2cLitTexturedQuad)
{
    QTemporaryFile tmp(QDir::tempPath() + "/qtmesh_ps1tmd_2c_XXXXXX.tmd");
    tmp.setAutoRemove(true);
    ASSERT_TRUE(tmp.open());
    const QByteArray blob = makeMinimal2cTexturedQuadTmd();
    ASSERT_EQ(tmp.write(blob), blob.size());
    tmp.flush();

    const std::string meshName = "PS1Tmd2cMesh";
    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr mesh = PS1TMD::importTmd(tmp.fileName(), meshName);
    ASSERT_TRUE(mesh);
    ASSERT_EQ(mesh->getNumSubMeshes(), 1u);
    EXPECT_EQ(mesh->getSubMesh(0)->vertexData->vertexCount, 6u);
}

static bool posNear(const Ogre::Vector3& a, const Ogre::Vector3& b, float eps = 1e-3f)
{
    return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps && std::abs(a.z - b.z) < eps;
}

static bool triContainsAll(const Ogre::Vector3& p0, const Ogre::Vector3& p1, const Ogre::Vector3& p2,
                           const Ogre::Vector3& a, const Ogre::Vector3& b, const Ogre::Vector3& c)
{
    const Ogre::Vector3 tri[3] = {p0, p1, p2};
    const Ogre::Vector3 exp[3] = {a, b, c};
    for (const Ogre::Vector3& e : exp) {
        bool found = false;
        for (const Ogre::Vector3& t : tri) {
            if (posNear(t, e)) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

TEST_F(PS1TMDTest, ImportMode28QuadsUseRequestedTriangulation)
{
    auto run = [&](const QByteArray& blob, const std::string& meshName) {
        QTemporaryFile tmp(QDir::tempPath() + "/qtmesh_ps1tmd_28_XXXXXX.tmd");
        tmp.setAutoRemove(true);
        ASSERT_TRUE(tmp.open());
        ASSERT_EQ(tmp.write(blob), blob.size());
        tmp.flush();

        if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
            Ogre::MeshManager::getSingleton().remove(old);

        Ogre::MeshPtr mesh = PS1TMD::importTmd(tmp.fileName(), meshName);
        ASSERT_TRUE(mesh);
        ASSERT_EQ(mesh->getNumSubMeshes(), 1u);
        Ogre::SubMesh* sm = mesh->getSubMesh(0);
        ASSERT_TRUE(sm->vertexData);
        ASSERT_EQ(sm->vertexData->vertexCount, 6u);

        // Expected positions after import transform: editor scale then 180° about Z.
        const Ogre::Vector3 v0(0.f, 0.f, 0.f);
        const Ogre::Vector3 v1(-10.f, 0.f, 0.f);
        const Ogre::Vector3 v2(-10.f, -10.f, 0.f);
        const Ogre::Vector3 v3(0.f, -10.f, 0.f);

        Ogre::VertexData* vd = sm->vertexData;
        const auto* posEl = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        ASSERT_NE(posEl, nullptr);
        auto posBuf = vd->vertexBufferBinding->getBuffer(posEl->getSource());
        const uint8_t* vbase = static_cast<const uint8_t*>(posBuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        const size_t stride = posBuf->getVertexSize();
        float* pf = nullptr;
        Ogre::Vector3 p[6];
        for (int i = 0; i < 6; ++i) {
            posEl->baseVertexPointerToElement(const_cast<uint8_t*>(vbase + size_t(i) * stride), &pf);
            p[i] = Ogre::Vector3(pf[0], pf[1], pf[2]);
        }
        posBuf->unlock();

        // Triangulation should be (v0,v1,v2) + (v1,v2,v3) (order may flip per-tri).
        EXPECT_TRUE(triContainsAll(p[0], p[1], p[2], v0, v1, v2));
        EXPECT_TRUE(triContainsAll(p[3], p[4], p[5], v1, v2, v3));
    };

    run(makeMinimal28FlatQuadTmd(), "PS1Tmd28FlatQuad");
    run(makeMinimal28GradQuadTmd(), "PS1Tmd28GradQuad");
}

TEST_F(PS1TMDTest, ImportMode35NoLightGouraudTexturedTriangle)
{
    QTemporaryFile tmp(QDir::tempPath() + "/qtmesh_ps1tmd_35_XXXXXX.tmd");
    tmp.setAutoRemove(true);
    ASSERT_TRUE(tmp.open());
    const QByteArray blob = makeMinimal35NoLightTmd();
    ASSERT_EQ(tmp.write(blob), blob.size());
    tmp.flush();

    const std::string meshName = "PS1Tmd35Mesh";
    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr mesh = PS1TMD::importTmd(tmp.fileName(), meshName);
    ASSERT_TRUE(mesh);
    ASSERT_EQ(mesh->getNumSubMeshes(), 1u);
    EXPECT_EQ(mesh->getSubMesh(0)->vertexData->vertexCount, 3u);
}

TEST_F(PS1TMDTest, ImportMinimalG3Triangle)
{
    QTemporaryFile tmp(QDir::tempPath() + "/qtmesh_ps1tmd_XXXXXX.tmd");
    tmp.setAutoRemove(true);
    ASSERT_TRUE(tmp.open());
    const QByteArray blob = makeMinimalG3Tmd();
    ASSERT_EQ(tmp.write(blob), blob.size());
    tmp.flush();

    const std::string meshName = "PS1TmdImportTestMesh";
    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr mesh = PS1TMD::importTmd(tmp.fileName(), meshName);
    ASSERT_TRUE(mesh);
    ASSERT_EQ(mesh->getNumSubMeshes(), 1u);
    Ogre::SubMesh* sm = mesh->getSubMesh(0);
    ASSERT_TRUE(sm->vertexData);
    EXPECT_EQ(sm->vertexData->vertexCount, 3u);
    EXPECT_EQ(sm->indexData->indexCount, 3u);

    // After transform: v0=(0,0,0), v1=(-10,0,0), v2=(0,-10,0). Importer emits v0,v2,v1 for winding.
    Ogre::VertexData* vd = sm->vertexData;
    const auto* posEl = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    ASSERT_NE(posEl, nullptr);
    auto posBuf = vd->vertexBufferBinding->getBuffer(posEl->getSource());
    const uint8_t* vbase = static_cast<const uint8_t*>(posBuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    const size_t stride = posBuf->getVertexSize();
    float* pf = nullptr;
    posEl->baseVertexPointerToElement(const_cast<uint8_t*>(vbase), &pf);
    EXPECT_NEAR(pf[0], 0.f, 1e-4f);
    EXPECT_NEAR(pf[1], 0.f, 1e-4f);
    EXPECT_NEAR(pf[2], 0.f, 1e-4f);
    posEl->baseVertexPointerToElement(const_cast<uint8_t*>(vbase + stride), &pf);
    EXPECT_NEAR(pf[0], 0.f, 1e-3f);
    EXPECT_NEAR(pf[1], -10.f, 1e-3f);
    EXPECT_NEAR(pf[2], 0.f, 1e-4f);
    posEl->baseVertexPointerToElement(const_cast<uint8_t*>(vbase + 2 * stride), &pf);
    EXPECT_NEAR(pf[0], -10.f, 1e-3f);
    EXPECT_NEAR(pf[1], 0.f, 1e-4f);
    EXPECT_NEAR(pf[2], 0.f, 1e-4f);
    posBuf->unlock();
}

TEST_F(PS1TMDTest, ImportMode30GouraudTriangleWithVertexColors)
{
    QTemporaryFile tmp(QDir::tempPath() + "/qtmesh_ps1tmd_g3c_XXXXXX.tmd");
    tmp.setAutoRemove(true);
    ASSERT_TRUE(tmp.open());
    const QByteArray blob = makeMinimalG3ColorTmd();
    ASSERT_EQ(tmp.write(blob), blob.size());
    tmp.flush();

    const std::string meshName = "PS1TmdG3ColorMesh";
    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr mesh = PS1TMD::importTmd(tmp.fileName(), meshName);
    ASSERT_TRUE(mesh);
    Ogre::SubMesh* sm = mesh->getSubMesh(0);
    ASSERT_TRUE(sm && sm->vertexData);
    const auto* colEl = sm->vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_DIFFUSE);
    ASSERT_NE(colEl, nullptr);
}

TEST_F(PS1TMDTest, ExportImportRoundTripSingleTriangle)
{
    ASSERT_TRUE(canLoadMeshFiles());

    const std::string meshName = "PS1TmdRtMesh";
    Ogre::MeshPtr mesh = createSingleTriMesh(meshName);
    ASSERT_TRUE(mesh);

    auto* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("PS1TmdRtNode");
    ASSERT_NE(node, nullptr);
    Ogre::Entity* ent = mgr->createEntity(node, mesh);
    ASSERT_NE(ent, nullptr);

    QTemporaryFile outTmd(QDir::tempPath() + "/qtmesh_ps1tmd_rt_XXXXXX.tmd");
    outTmd.setAutoRemove(true);
    ASSERT_TRUE(outTmd.open());
    outTmd.close();
    const QString path = outTmd.fileName();

    ASSERT_TRUE(PS1TMD::exportEntity(ent, path));

    mgr->destroySceneNode(QStringLiteral("PS1TmdRtNode"));
    Ogre::MeshManager::getSingleton().remove(meshName);

    const std::string reName = "PS1TmdRtMeshReimport";
    if (auto old = Ogre::MeshManager::getSingleton().getByName(reName))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr re = PS1TMD::importTmd(path, reName);
    ASSERT_TRUE(re);
    ASSERT_EQ(re->getNumSubMeshes(), 1u);
    EXPECT_EQ(re->getSubMesh(0)->vertexData->vertexCount, 3u);
}

TEST_F(PS1TMDTest, ImportFails_WhenFileMissing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("missing_subdir/model.tmd"));
    EXPECT_FALSE(PS1TMD::importTmd(path, "PS1TmdMissingMesh"));
}

TEST_F(PS1TMDTest, ImportFails_WhenBadFileMagic)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("badmagic.tmd"));
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    QByteArray buf(256, '\0');
    reinterpret_cast<uint8_t*>(buf.data())[0] = 0xff;
    f.write(buf);
    f.close();

    EXPECT_FALSE(PS1TMD::importTmd(path, "PS1TmdBadMagicMesh"));
}

TEST_F(PS1TMDTest, ImportUvMeshAppliesSiblingTimTexture)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString tmdPath =
        QDir(dir.path()).filePath(QStringLiteral("Ps1SiblingUvModel.tmd"));
    const QString timPath =
        QDir(dir.path()).filePath(QStringLiteral("Ps1SiblingUvModel.tim"));
    QFile wf(tmdPath);
    ASSERT_TRUE(wf.open(QIODevice::WriteOnly));
    wf.write(makeMinimal25NoLightTmd());
    wf.close();

    std::vector<uint8_t> rgba = {200, 10, 200, 255};
    Ogre::Image img;
    img.loadDynamicImage(rgba.data(), 1, 1, 1, Ogre::PF_BYTE_RGBA, false);
    QString err;
    ASSERT_TRUE(PS1TIM::saveOgreImageToTim16(img, timPath, &err)) << err.toStdString();

    const std::string meshName = "PS1TmdUvSiblingMesh";
    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr mesh = PS1TMD::importTmd(tmdPath, meshName);
    ASSERT_TRUE(mesh);

    const std::string matName = std::string("TMD/") + meshName;
    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().getByName(matName);
    ASSERT_TRUE(mat);
    mat->load();
    ASSERT_GE(mat->getTechnique(0)->getPass(0)->getNumTextureUnitStates(), 1u);

    EXPECT_EQ(mat->getTechnique(0)->getPass(0)->getTextureUnitState(0)->getTextureName(),
              std::string("Ps1SiblingUvModel.tim"));

    Ogre::MeshManager::getSingleton().remove(meshName);

    const std::string timRes = QFileInfo(timPath).completeBaseName().toStdString() + ".tim";
    const Ogre::ResourcePtr rp = Ogre::TextureManager::getSingleton().getByName(timRes);
    if (rp)
        Ogre::TextureManager::getSingleton().remove(rp->getHandle());
    try {
        Ogre::MaterialManager::getSingleton().remove(matName);
    } catch (...) {
    }
}

/** Flat lit triangle (mode 0x20, flag 0, ilen 3) — single shared normal. */
static QByteArray makeMinimal20FlatLitTriTmd()
{
    constexpr uint32_t kTmdId = 0x41u;
    constexpr size_t kHead = 12u;
    constexpr size_t kObjH = 28u;
    const size_t vAbs = kHead + kObjH;
    const size_t nAbs = vAbs + 3u * 8u;
    const size_t pAbs = nAbs + 1u * 8u;
    const uint32_t vOff = static_cast<uint32_t>(vAbs - 12u);
    const uint32_t nOff = static_cast<uint32_t>(nAbs - 12u);
    const uint32_t pOff = static_cast<uint32_t>(pAbs - 12u);

    QByteArray buf(static_cast<int>(pAbs + 4u + 12u), '\0');
    uint8_t* d = reinterpret_cast<uint8_t*>(buf.data());

    writeU32le(d, kTmdId);
    writeU32le(d + 4, 0);
    writeU32le(d + 8, 1);

    uint8_t* oh = d + kHead;
    writeU32le(oh, vOff);
    writeU32le(oh + 4, 3);
    writeU32le(oh + 8, nOff);
    writeU32le(oh + 12, 1);
    writeU32le(oh + 16, pOff);
    writeU32le(oh + 20, 1);
    writeU32le(oh + 24, 0);

    writeVertex8(0, 0, 0, d + vAbs);
    writeVertex8(4096, 0, 0, d + vAbs + 8);
    writeVertex8(0, 4096, 0, d + vAbs + 16);
    writeVertex8(0, 0, 4096, d + nAbs);

    uint8_t* pkt = d + pAbs;
    pkt[0] = 5;
    pkt[1] = 3;
    pkt[2] = 0;
    pkt[3] = 0x20;
    uint8_t* pay = pkt + 4;
    pay[0] = 180;
    pay[1] = 90;
    pay[2] = 45;
    pay[3] = 0;
    writeU16le(pay + 4, 0);
    writeU16le(pay + 6, 0);
    writeU16le(pay + 8, 1);
    writeU16le(pay + 10, 2);

    return buf;
}

/** Lit textured triangle (mode 0x24, flag 0, ilen 5). */
static QByteArray makeMinimal24LitTexturedTriTmd()
{
    constexpr uint32_t kTmdId = 0x41u;
    constexpr size_t kHead = 12u;
    constexpr size_t kObjH = 28u;
    const size_t vAbs = kHead + kObjH;
    const size_t nAbs = vAbs + 3u * 8u;
    const size_t pAbs = nAbs + 1u * 8u;
    const uint32_t vOff = static_cast<uint32_t>(vAbs - 12u);
    const uint32_t nOff = static_cast<uint32_t>(nAbs - 12u);
    const uint32_t pOff = static_cast<uint32_t>(pAbs - 12u);

    QByteArray buf(static_cast<int>(pAbs + 4u + 20u), '\0');
    uint8_t* d = reinterpret_cast<uint8_t*>(buf.data());

    writeU32le(d, kTmdId);
    writeU32le(d + 4, 0);
    writeU32le(d + 8, 1);

    uint8_t* oh = d + kHead;
    writeU32le(oh, vOff);
    writeU32le(oh + 4, 3);
    writeU32le(oh + 8, nOff);
    writeU32le(oh + 12, 1);
    writeU32le(oh + 16, pOff);
    writeU32le(oh + 20, 1);
    writeU32le(oh + 24, 0);

    writeVertex8(0, 0, 0, d + vAbs);
    writeVertex8(4096, 0, 0, d + vAbs + 8);
    writeVertex8(0, 4096, 0, d + vAbs + 16);
    writeVertex8(0, 0, 4096, d + nAbs);

    uint8_t* pkt = d + pAbs;
    pkt[0] = 7;
    pkt[1] = 5;
    pkt[2] = 0;
    pkt[3] = 0x24;
    uint8_t* pay = pkt + 4;
    pay[0] = 32;
    pay[1] = 64;
    pay[2] = 0;
    pay[3] = 0;
    pay[4] = 200;
    pay[5] = 16;
    pay[6] = 0;
    pay[7] = 0;
    pay[8] = 16;
    pay[9] = 200;
    writeU16le(pay + 12, 0);
    writeU16le(pay + 14, 0);
    writeU16le(pay + 16, 1);
    writeU16le(pay + 18, 2);

    return buf;
}

/** Gouraud textured triangle (mode 0x34, flag 0, ilen 6). */
static QByteArray makeMinimal34GouraudTexturedTriTmd()
{
    constexpr uint32_t kTmdId = 0x41u;
    constexpr size_t kHead = 12u;
    constexpr size_t kObjH = 28u;
    const size_t vAbs = kHead + kObjH;
    const size_t nAbs = vAbs + 3u * 8u;
    const size_t pAbs = nAbs + 3u * 8u;
    const uint32_t vOff = static_cast<uint32_t>(vAbs - 12u);
    const uint32_t nOff = static_cast<uint32_t>(nAbs - 12u);
    const uint32_t pOff = static_cast<uint32_t>(pAbs - 12u);

    QByteArray buf(static_cast<int>(pAbs + 4u + 24u), '\0');
    uint8_t* d = reinterpret_cast<uint8_t*>(buf.data());

    writeU32le(d, kTmdId);
    writeU32le(d + 4, 0);
    writeU32le(d + 8, 1);

    uint8_t* oh = d + kHead;
    writeU32le(oh, vOff);
    writeU32le(oh + 4, 3);
    writeU32le(oh + 8, nOff);
    writeU32le(oh + 12, 3);
    writeU32le(oh + 16, pOff);
    writeU32le(oh + 20, 1);
    writeU32le(oh + 24, 0);

    writeVertex8(0, 0, 0, d + vAbs);
    writeVertex8(4096, 0, 0, d + vAbs + 8);
    writeVertex8(0, 4096, 0, d + vAbs + 16);
    writeVertex8(0, 0, 4096, d + nAbs);
    writeVertex8(0, 0, 4096, d + nAbs + 8);
    writeVertex8(0, 0, 4096, d + nAbs + 16);

    uint8_t* pkt = d + pAbs;
    pkt[0] = 8;
    pkt[1] = 6;
    pkt[2] = 0;
    pkt[3] = 0x34;
    uint8_t* pay = pkt + 4;
    pay[0] = 10;
    pay[1] = 20;
    pay[4] = 30;
    pay[5] = 40;
    pay[8] = 50;
    pay[9] = 60;
    writeU16le(pay + 12, 0);
    writeU16le(pay + 14, 0);
    writeU16le(pay + 16, 1);
    writeU16le(pay + 18, 1);
    writeU16le(pay + 20, 2);
    writeU16le(pay + 22, 2);

    return buf;
}

/** Textured quad, no light (mode 0x2d, flag 1, ilen 7). */
static QByteArray makeMinimal2dNoLightTexturedQuadTmd()
{
    QByteArray buf = makeMinimal2cTexturedQuadTmd();
    uint8_t* pkt = reinterpret_cast<uint8_t*>(buf.data()) + 12u + 28u + 4u * 8u + 1u * 8u;
    pkt[2] = 1;
    pkt[3] = 0x2d;
    uint8_t* pay = pkt + 4;
    pay[16] = 90;
    pay[17] = 120;
    pay[18] = 30;
    writeU16le(pay + 20, 0);
    writeU16le(pay + 22, 1);
    writeU16le(pay + 24, 2);
    writeU16le(pay + 26, 3);
    return buf;
}

/** Gouraud textured quad (mode 0x3c, flag 0, ilen 8). */
static QByteArray makeMinimal3cGouraudTexturedQuadTmd()
{
    constexpr uint32_t kTmdId = 0x41u;
    constexpr size_t kHead = 12u;
    constexpr size_t kObjH = 28u;
    const size_t vAbs = kHead + kObjH;
    const size_t nAbs = vAbs + 4u * 8u;
    const size_t pAbs = nAbs + 4u * 8u;
    const uint32_t vOff = static_cast<uint32_t>(vAbs - 12u);
    const uint32_t nOff = static_cast<uint32_t>(nAbs - 12u);
    const uint32_t pOff = static_cast<uint32_t>(pAbs - 12u);

    QByteArray buf(static_cast<int>(pAbs + 4u + 32u), '\0');
    uint8_t* d = reinterpret_cast<uint8_t*>(buf.data());

    writeU32le(d, kTmdId);
    writeU32le(d + 4, 0);
    writeU32le(d + 8, 1);

    uint8_t* oh = d + kHead;
    writeU32le(oh, vOff);
    writeU32le(oh + 4, 4);
    writeU32le(oh + 8, nOff);
    writeU32le(oh + 12, 4);
    writeU32le(oh + 16, pOff);
    writeU32le(oh + 20, 1);
    writeU32le(oh + 24, 0);

    writeVertex8(0, 0, 0, d + vAbs);
    writeVertex8(4096, 0, 0, d + vAbs + 8);
    writeVertex8(4096, 4096, 0, d + vAbs + 16);
    writeVertex8(0, 4096, 0, d + vAbs + 24);
    for (int i = 0; i < 4; ++i)
        writeVertex8(0, 0, 4096, d + nAbs + static_cast<size_t>(i) * 8u);

    uint8_t* pkt = d + pAbs;
    pkt[0] = 10;
    pkt[1] = 8;
    pkt[2] = 0;
    pkt[3] = 0x3c;
    uint8_t* pay = pkt + 4;
    pay[0] = 0;
    pay[1] = 0;
    pay[4] = 128;
    pay[5] = 0;
    pay[8] = 128;
    pay[9] = 128;
    pay[12] = 0;
    pay[13] = 128;
    writeU16le(pay + 16, 0);
    writeU16le(pay + 18, 0);
    writeU16le(pay + 20, 1);
    writeU16le(pay + 22, 1);
    writeU16le(pay + 24, 2);
    writeU16le(pay + 26, 2);
    writeU16le(pay + 28, 3);
    writeU16le(pay + 30, 3);

    return buf;
}

/** Gouraud textured quad, no light (mode 0x3d, flag 1, ilen 10). */
static QByteArray makeMinimal3dGouraudTexturedQuadNoLightTmd()
{
    constexpr uint32_t kTmdId = 0x41u;
    constexpr size_t kHead = 12u;
    constexpr size_t kObjH = 28u;
    const size_t vAbs = kHead + kObjH;
    const size_t nAbs = vAbs + 4u * 8u;
    const size_t pAbs = nAbs + 1u * 8u;
    const uint32_t vOff = static_cast<uint32_t>(vAbs - 12u);
    const uint32_t nOff = static_cast<uint32_t>(nAbs - 12u);
    const uint32_t pOff = static_cast<uint32_t>(pAbs - 12u);

    QByteArray buf(static_cast<int>(pAbs + 4u + 40u), '\0');
    uint8_t* d = reinterpret_cast<uint8_t*>(buf.data());

    writeU32le(d, kTmdId);
    writeU32le(d + 4, 0);
    writeU32le(d + 8, 1);

    uint8_t* oh = d + kHead;
    writeU32le(oh, vOff);
    writeU32le(oh + 4, 4);
    writeU32le(oh + 8, nOff);
    writeU32le(oh + 12, 1);
    writeU32le(oh + 16, pOff);
    writeU32le(oh + 20, 1);
    writeU32le(oh + 24, 0);

    writeVertex8(0, 0, 0, d + vAbs);
    writeVertex8(4096, 0, 0, d + vAbs + 8);
    writeVertex8(4096, 4096, 0, d + vAbs + 16);
    writeVertex8(0, 4096, 0, d + vAbs + 24);
    writeVertex8(0, 0, 4096, d + nAbs);

    uint8_t* pkt = d + pAbs;
    pkt[0] = 12;
    pkt[1] = 10;
    pkt[2] = 1;
    pkt[3] = 0x3d;
    uint8_t* pay = pkt + 4;
    pay[0] = 64;
    pay[1] = 32;
    pay[4] = 192;
    pay[5] = 32;
    pay[8] = 192;
    pay[9] = 192;
    pay[12] = 64;
    pay[13] = 192;
    for (int i = 0; i < 16; ++i)
        pay[16 + i] = static_cast<uint8_t>(i + 1);
    writeU16le(pay + 32, 0);
    writeU16le(pay + 34, 1);
    writeU16le(pay + 36, 2);
    writeU16le(pay + 38, 3);

    return buf;
}

static void importTmdBlobAndExpectTris(const QByteArray& blob, const std::string& meshName,
                                       unsigned minVerts)
{
    QTemporaryFile tmp(QDir::tempPath() + "/qtmesh_ps1tmd_cov_XXXXXX.tmd");
    tmp.setAutoRemove(true);
    ASSERT_TRUE(tmp.open());
    ASSERT_EQ(tmp.write(blob), blob.size());
    tmp.flush();

    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr mesh = PS1TMD::importTmd(tmp.fileName(), meshName);
    ASSERT_TRUE(mesh);
    ASSERT_GE(mesh->getNumSubMeshes(), 1u);
    const Ogre::SubMesh* sm = mesh->getSubMesh(0);
    ASSERT_NE(sm->vertexData, nullptr);
    EXPECT_GE(sm->vertexData->vertexCount, minVerts);
    Ogre::MeshManager::getSingleton().remove(meshName);
}

TEST_F(PS1TMDTest, ImportMode20FlatLitTriangle)
{
    importTmdBlobAndExpectTris(makeMinimal20FlatLitTriTmd(), "PS1Tmd20Mesh", 3u);
}

TEST_F(PS1TMDTest, ImportMode24LitTexturedTriangle_HasUvs)
{
    const std::string meshName = "PS1Tmd24Mesh";
    QTemporaryFile tmp(QDir::tempPath() + "/qtmesh_ps1tmd_24_XXXXXX.tmd");
    tmp.setAutoRemove(true);
    ASSERT_TRUE(tmp.open());
    const QByteArray blob = makeMinimal24LitTexturedTriTmd();
    ASSERT_EQ(tmp.write(blob), blob.size());
    tmp.flush();

    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr mesh = PS1TMD::importTmd(tmp.fileName(), meshName);
    ASSERT_TRUE(mesh);
    ASSERT_GE(mesh->getNumSubMeshes(), 1u);
    const Ogre::SubMesh* sm = mesh->getSubMesh(0);
    ASSERT_NE(sm, nullptr);
    const Ogre::VertexData* vd = sm->vertexData;
    ASSERT_NE(vd, nullptr);
    const auto* uvEl = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES);
    ASSERT_NE(uvEl, nullptr);
    Ogre::MeshManager::getSingleton().remove(meshName);
}

TEST_F(PS1TMDTest, ImportMode34GouraudTexturedTriangle)
{
    importTmdBlobAndExpectTris(makeMinimal34GouraudTexturedTriTmd(), "PS1Tmd34Mesh", 3u);
}

TEST_F(PS1TMDTest, ImportMode2dNoLightTexturedQuad)
{
    importTmdBlobAndExpectTris(makeMinimal2dNoLightTexturedQuadTmd(), "PS1Tmd2dMesh", 4u);
}

TEST_F(PS1TMDTest, ImportMode3cGouraudTexturedQuad)
{
    importTmdBlobAndExpectTris(makeMinimal3cGouraudTexturedQuadTmd(), "PS1Tmd3cMesh", 4u);
}

TEST_F(PS1TMDTest, ImportMode3dGouraudTexturedQuadNoLight)
{
    importTmdBlobAndExpectTris(makeMinimal3dGouraudTexturedQuadNoLightTmd(), "PS1Tmd3dMesh", 4u);
}

TEST_F(PS1TMDTest, ExportEntity_NullEntityReturnsFalse)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("null.tmd"));
    EXPECT_FALSE(PS1TMD::exportEntity(nullptr, path));
}

TEST_F(PS1TMDTest, ExportEntity_MeshWithNoSubmeshesReturnsFalse)
{
    ASSERT_TRUE(canLoadMeshFiles());
    const std::string meshName = "PS1TmdEmptySubmeshMesh";
    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual(
        meshName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    mesh->load();

    auto* mgr = Manager::getSingleton();
    auto* sn = mgr->addSceneNode(QStringLiteral("PS1TmdEmptySubNode"));
    auto* ent = mgr->createEntity(sn, mesh);
    ASSERT_NE(ent, nullptr);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("empty.tmd"));
    EXPECT_FALSE(PS1TMD::exportEntity(ent, path));

    mgr->destroySceneNode(sn);
    Ogre::MeshManager::getSingleton().remove(meshName);
}

TEST_F(PS1TMDTest, ImportFails_WhenPrimitiveStreamTruncated)
{
    QByteArray blob = makeMinimalG3Tmd();
    blob.chop(8);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("trunc.tmd"));
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write(blob), blob.size());
    f.close();

    EXPECT_FALSE(PS1TMD::importTmd(path, "PS1TmdTruncMesh"));
}
