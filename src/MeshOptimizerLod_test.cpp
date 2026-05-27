#include <gtest/gtest.h>
#include <QCoreApplication>

#include "MeshOptimizerLod.h"
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

// Procedurally build an N×N subdivided plane mesh: every cell becomes
// two triangles, giving 2*N*N triangles and (N+1)² vertices. Carries
// position + UV0 (the UV is just the normalised grid coordinate) so
// `MeshOptimizerLod::generateLods` exercises the UV-aware
// `simplifyWithAttributes` branch.
//
// Wanted: enough triangles that meshoptimizer can actually decimate
// without bottoming out (a single triangle is uncollapsible). N=8
// gives 128 tris / 81 verts — small enough to test fast, big enough
// to reduce.
static Ogre::MeshPtr createSubdividedPlane(const std::string& name, int n = 8)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    sub->useSharedVertices = true;
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;

    size_t offset = 0;
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

    const int side = n + 1;
    const size_t vertCount = static_cast<size_t>(side) * side;
    std::vector<float> verts;
    verts.reserve(vertCount * 5);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(n);
            const float v = static_cast<float>(y) / static_cast<float>(n);
            verts.push_back(u);     // pos.x
            verts.push_back(v);     // pos.y
            verts.push_back(0.0f);  // pos.z
            verts.push_back(u);     // uv.s
            verts.push_back(v);     // uv.t
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
            const uint16_t a = static_cast<uint16_t>(y * side + x);
            const uint16_t b = static_cast<uint16_t>(a + 1);
            const uint16_t c = static_cast<uint16_t>(a + side);
            const uint16_t d = static_cast<uint16_t>(c + 1);
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

    mesh->_setBounds(Ogre::AxisAlignedBox(0, 0, 0, 1, 1, 0));
    mesh->_setBoundingSphereRadius(1.5f);
    mesh->load();
    return mesh;
}

class MeshOptimizerLodTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb required in CI)";
    }
    void TearDown() override {
        Manager::kill();
    }
};

TEST_F(MeshOptimizerLodTest, ReducesTriangleCount) {
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";

    auto mesh = createSubdividedPlane("MeshOptLodTest_plane");
    ASSERT_TRUE(mesh);

    const size_t baseTris = mesh->getSubMesh(0)->indexData->indexCount / 3;
    ASSERT_GT(baseTris, 30u) << "test mesh must be large enough to decimate";

    std::vector<float> reductions = {0.5f};
    auto levels = MeshOptimizerLod::generateLods(mesh.get(), reductions);
    ASSERT_EQ(levels.size(), 1u);
    ASSERT_EQ(levels[0].indices.size(), 1u);
    ASSERT_NE(levels[0].indices[0], nullptr);

    const size_t lodTris = levels[0].indices[0]->indexCount / 3;
    EXPECT_LT(lodTris, baseTris)
        << "meshopt should reduce a regular subdivided plane at 50% target";

    MeshOptimizerLod::destroyLevel(levels[0]);
}

TEST_F(MeshOptimizerLodTest, EmptyReductionsReturnsEmpty) {
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";

    auto mesh = createSubdividedPlane("MeshOptLodTest_plane_empty");
    auto levels = MeshOptimizerLod::generateLods(mesh.get(), {});
    EXPECT_TRUE(levels.empty());
}

TEST_F(MeshOptimizerLodTest, MultipleLevelsAreMonotonicallyReduced) {
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";

    auto mesh = createSubdividedPlane("MeshOptLodTest_plane_multi");
    auto levels = MeshOptimizerLod::generateLods(mesh.get(), {0.25f, 0.5f, 0.75f});
    ASSERT_EQ(levels.size(), 3u);

    size_t prev = mesh->getSubMesh(0)->indexData->indexCount;
    for (auto& level : levels) {
        ASSERT_FALSE(level.indices.empty());
        ASSERT_NE(level.indices[0], nullptr);
        const size_t cur = level.indices[0]->indexCount;
        EXPECT_LE(cur, prev) << "higher target ratio should not produce more tris";
        prev = cur;
    }
    for (auto& level : levels) MeshOptimizerLod::destroyLevel(level);
}

TEST_F(MeshOptimizerLodTest, NullMeshReturnsEmpty) {
    auto levels = MeshOptimizerLod::generateLods(nullptr, {0.5f});
    EXPECT_TRUE(levels.empty());
}
