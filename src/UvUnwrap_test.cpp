#include <gtest/gtest.h>
#include <QCoreApplication>

#include "UvUnwrap.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreHardwareIndexBuffer.h>
#include <OgreHardwareVertexBuffer.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>

#include <vector>
#include <array>

// Build a procedurally-tessellated N×N plane mesh: every grid cell
// becomes two triangles. Positions only — no UVs yet — which lets us
// verify the unwrap creates UV0 from scratch. The same shape is used
// for `MeshOptimizerLodTest` so we know meshoptimizer-scale inputs
// (N=8 gives 128 tris / 81 verts) round-trip through xatlas cleanly.
static Ogre::MeshPtr createPlaneNoUvs(const std::string& name, int n = 8)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    sub->useSharedVertices = true;
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);

    const int side = n + 1;
    const size_t vertCount = static_cast<size_t>(side) * side;
    std::vector<float> verts;
    verts.reserve(vertCount * 3);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            verts.push_back(static_cast<float>(x));
            verts.push_back(static_cast<float>(y));
            verts.push_back(0.0f);
        }
    }
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), vertCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    vbuf->writeData(0, verts.size() * sizeof(float), verts.data());
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = vertCount;

    std::vector<uint16_t> indices;
    indices.reserve(static_cast<size_t>(n) * n * 6);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const auto a = static_cast<uint16_t>(y * side + x);
            const auto b = static_cast<uint16_t>(a + 1);
            const auto c = static_cast<uint16_t>(a + side);
            const auto d = static_cast<uint16_t>(c + 1);
            indices.push_back(a); indices.push_back(c); indices.push_back(b);
            indices.push_back(b); indices.push_back(c); indices.push_back(d);
        }
    }
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, indices.size(),
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    ibuf->writeData(0, indices.size() * sizeof(uint16_t), indices.data());
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount  = indices.size();

    mesh->_setBounds(Ogre::AxisAlignedBox(0, 0, 0, n, n, 0));
    mesh->_setBoundingSphereRadius(static_cast<float>(n) * 1.5f);
    mesh->load();
    return mesh;
}

static Ogre::MeshPtr createPlaneWithGridUvs(const std::string& name, int n = 2)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    sub->useSharedVertices = true;
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    decl->addElement(0, 12, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES, 0);

    const int side = n + 1;
    const size_t vertCount = static_cast<size_t>(side) * side;
    const size_t stride = decl->getVertexSize(0);
    std::vector<float> verts(vertCount * (stride / sizeof(float)), 0.0f);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const size_t vi = static_cast<size_t>(y * side + x);
            float* row = verts.data() + vi * (stride / sizeof(float));
            row[0] = static_cast<float>(x);
            row[1] = static_cast<float>(y);
            row[2] = 0.0f;
            row[3] = 0.11f * static_cast<float>(vi);
            row[4] = 0.23f * static_cast<float>(vi);
        }
    }
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        stride, vertCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    vbuf->writeData(0, verts.size() * sizeof(float), verts.data());
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = vertCount;

    std::vector<uint16_t> indices;
    indices.reserve(static_cast<size_t>(n) * n * 6);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const auto a = static_cast<uint16_t>(y * side + x);
            const auto b = static_cast<uint16_t>(a + 1);
            const auto c = static_cast<uint16_t>(a + side);
            const auto d = static_cast<uint16_t>(c + 1);
            indices.push_back(a); indices.push_back(c); indices.push_back(b);
            indices.push_back(b); indices.push_back(c); indices.push_back(d);
        }
    }
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, indices.size(),
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    ibuf->writeData(0, indices.size() * sizeof(uint16_t), indices.data());
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount  = indices.size();

    mesh->_setBounds(Ogre::AxisAlignedBox(0, 0, 0, n, n, 0));
    mesh->_setBoundingSphereRadius(static_cast<float>(n) * 1.5f);
    mesh->load();
    return mesh;
}

static std::array<Ogre::Vector2, 3> readTriangleUvs(Ogre::Entity* entity, unsigned triIndex)
{
    std::array<Ogre::Vector2, 3> out{};
    if (!entity || !entity->getMesh())
        return out;
    Ogre::Mesh* mesh = entity->getMesh().get();
    Ogre::SubMesh* sub = mesh->getSubMesh(0);
    Ogre::VertexData* vd = sub->useSharedVertices ? mesh->sharedVertexData : sub->vertexData;
    if (!vd || !sub->indexData || !sub->indexData->indexBuffer)
        return out;
    const auto* uvElem = vd->vertexDeclaration->findElementBySemantic(
        Ogre::VES_TEXTURE_COORDINATES, 0);
    if (!uvElem)
        return out;

    auto ibuf = sub->indexData->indexBuffer;
    const size_t base = static_cast<size_t>(triIndex) * 3 + sub->indexData->indexStart;
    auto vbuf = vd->vertexBufferBinding->getBuffer(uvElem->getSource());
    auto* ibase = static_cast<unsigned char*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    ibase += base * ibuf->getIndexSize();
    auto* vbase = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));

    for (int k = 0; k < 3; ++k) {
        uint32_t vi = 0;
        if (ibuf->getType() == Ogre::HardwareIndexBuffer::IT_16BIT)
            vi = reinterpret_cast<uint16_t*>(ibase)[k];
        else
            vi = reinterpret_cast<uint32_t*>(ibase)[k];
        float* uv = nullptr;
        uvElem->baseVertexPointerToElement(vbase + vi * vbuf->getVertexSize(), &uv);
        out[static_cast<size_t>(k)] = Ogre::Vector2(uv[0], uv[1]);
    }

    ibuf->unlock();
    vbuf->unlock();
    return out;
}

class UvUnwrapTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb required in CI)";
        ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";
    }
    void TearDown() override { Manager::kill(); }
};

TEST_F(UvUnwrapTest, UnwrapPopulatesAtlas) {
    auto mesh = createPlaneNoUvs("UvUnwrapTest_plane");
    ASSERT_TRUE(mesh);

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UvUnwrapTest_node");
    auto* entity = sceneMgr->createEntity("UvUnwrapTest_entity", mesh);
    node->attachObject(entity);

    UvUnwrapOptions opts;
    opts.resolution = 512;
    opts.padding    = 2;
    opts.channel    = 0;

    const auto report = UvUnwrap::unwrapEntity(entity, opts);
    ASSERT_TRUE(report.applied) << report.error.toStdString();

    EXPECT_GT(report.chartCount, 0);
    EXPECT_GT(report.atlasWidth, 0);
    EXPECT_GT(report.atlasHeight, 0);
    // 128 source triangles, every vertex split at chart boundaries
    // — output verts must be at least as many as input.
    EXPECT_GE(report.verticesAfter, report.verticesBefore);
    EXPECT_EQ(report.trianglesProcessed, 128);
}

TEST_F(UvUnwrapTest, NullEntityReturnsErrorReport) {
    const auto report = UvUnwrap::unwrapEntity(nullptr);
    EXPECT_FALSE(report.applied);
    EXPECT_FALSE(report.error.isEmpty());
}

TEST_F(UvUnwrapTest, InfoReportsUvChannels) {
    auto mesh = createPlaneNoUvs("UvUnwrapTest_info");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UvUnwrapTest_info_node");
    auto* entity = sceneMgr->createEntity("UvUnwrapTest_info_entity", mesh);
    node->attachObject(entity);

    const auto info = UvUnwrap::infoForEntity(entity);
    ASSERT_EQ(info.size(), 1);
    EXPECT_EQ(info[0].triangleCount, 128);
    EXPECT_FALSE(info[0].hasUv0);  // plane was built without UVs
}

TEST_F(UvUnwrapTest, PartialFaceMaskUnwrapsSelectedTrisOnly) {
    auto mesh = createPlaneWithGridUvs("UvUnwrapTest_partial", 2);
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UvUnwrapTest_partial_node");
    auto* entity = sceneMgr->createEntity("UvUnwrapTest_partial_entity", mesh);
    node->attachObject(entity);

    const auto beforeIgnored = readTriangleUvs(entity, 2);

    UvUnwrapOptions opts;
    opts.resolution = 256;
    opts.padding = 2;
    UvUnwrapOptions::FaceMask mask;
    mask.subMeshIndex = 0;
    mask.includeTriangle.assign(8, false);
    mask.includeTriangle[0] = true;
    mask.includeTriangle[1] = true;
    opts.faceMasks.push_back(std::move(mask));

    const auto report = UvUnwrap::unwrapEntity(entity, opts);
    ASSERT_TRUE(report.applied) << report.error.toStdString();
    EXPECT_GT(report.chartCount, 0);

    const auto afterIgnored = readTriangleUvs(entity, 2);
    for (int k = 0; k < 3; ++k) {
        EXPECT_NEAR(afterIgnored[static_cast<size_t>(k)].x, beforeIgnored[static_cast<size_t>(k)].x, 1e-5f);
        EXPECT_NEAR(afterIgnored[static_cast<size_t>(k)].y, beforeIgnored[static_cast<size_t>(k)].y, 1e-5f);
    }
}
