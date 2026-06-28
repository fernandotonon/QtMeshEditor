#include <gtest/gtest.h>
#include <QCoreApplication>

#include "UvUnwrap.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <OgreHardwareBufferManager.h>
#include <OgreHardwareIndexBuffer.h>
#include <OgreHardwareVertexBuffer.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>

#include <vector>

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
    auto mesh = createPlaneNoUvs("UvUnwrapTest_partial", 2);
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("UvUnwrapTest_partial_node");
    auto* entity = sceneMgr->createEntity("UvUnwrapTest_partial_entity", mesh);
    node->attachObject(entity);

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
}
