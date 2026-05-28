#include <gtest/gtest.h>
#include <QCoreApplication>

#include "ExportOptimizer.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <OgreHardwareBufferManager.h>
#include <OgreHardwareIndexBuffer.h>
#include <OgreHardwareVertexBuffer.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>

#include <random>
#include <vector>

// Build a deliberately scrambled grid mesh: an N×N quad-tessellated plane
// with its index buffer randomly shuffled per-triangle (vertex order
// within a triangle preserved so winding stays consistent). Yields a
// large ACMR (~3.0) that the on-export optimizer should drop sharply.
static Ogre::MeshPtr createScrambledGrid(const std::string& name, int n = 12)
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

    // Build triangle list as triplets; collect them so we can shuffle.
    struct Tri { uint16_t a, b, c; };
    std::vector<Tri> tris;
    tris.reserve(static_cast<size_t>(n) * n * 2);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const auto v0 = static_cast<uint16_t>(y * side + x);
            const auto v1 = static_cast<uint16_t>(v0 + 1);
            const auto v2 = static_cast<uint16_t>(v0 + side);
            const auto v3 = static_cast<uint16_t>(v2 + 1);
            tris.push_back({v0, v2, v1});
            tris.push_back({v1, v2, v3});
        }
    }
    // Deterministic shuffle so the test is reproducible.
    std::mt19937 rng(42);
    std::shuffle(tris.begin(), tris.end(), rng);

    std::vector<uint16_t> indices;
    indices.reserve(tris.size() * 3);
    for (const auto& t : tris) {
        indices.push_back(t.a);
        indices.push_back(t.b);
        indices.push_back(t.c);
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

class ExportOptimizerTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb required in CI)";
        // Fixture-level prereq: every test in this suite touches Ogre
        // hardware buffers (positions + indices) so a missing GL
        // context must fail loudly, not pass silently. Removing the
        // per-test guards is left to the individual TEST_F bodies
        // for now since some are still in flight elsewhere.
        ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";
    }
    void TearDown() override {
        Manager::kill();
    }
};

TEST_F(ExportOptimizerTest, ImprovesAcmrOnScrambledIndices) {
    auto mesh = createScrambledGrid("ExportOpt_scrambled");
    ASSERT_TRUE(mesh);

    // Attach to a scene node + entity so optimizeEntity gets a real
    // Ogre::Entity to walk.
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("ExportOpt_node");
    auto* entity = sceneMgr->createEntity("ExportOpt_entity", mesh);
    node->attachObject(entity);

    // Read the ACMR before the optimizer runs.
    auto readIndices = [&]() {
        auto* idata = mesh->getSubMesh(0)->indexData;
        std::vector<uint32_t> out(idata->indexCount);
        auto ibuf = idata->indexBuffer;
        auto* src = static_cast<unsigned char*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        const auto* p = reinterpret_cast<const uint16_t*>(src);
        for (size_t k = 0; k < out.size(); ++k) out[k] = p[k];
        ibuf->unlock();
        return out;
    };
    const double acmrBefore = ExportOptimizer::computeAcmr(
        readIndices(), mesh->sharedVertexData->vertexCount);
    EXPECT_GT(acmrBefore, 1.5)
        << "scrambled grid should start with high ACMR (~3.0)";

    const auto report = ExportOptimizer::optimizeEntity(entity, OptimizeFlags::All);
    ASSERT_FALSE(report.empty());
    EXPECT_GT(report.submeshesOptimized, 0);
    EXPECT_LT(report.weightedAcmrAfter, report.weightedAcmrBefore)
        << "optimizer should reduce ACMR";

    // Re-read the live buffer — the optimizer should have committed
    // its reorder so the next read matches the report's `after`.
    const double acmrAfter = ExportOptimizer::computeAcmr(
        readIndices(), mesh->sharedVertexData->vertexCount);
    EXPECT_NEAR(acmrAfter, report.weightedAcmrAfter, 0.05);
    EXPECT_LT(acmrAfter, acmrBefore);
}

TEST_F(ExportOptimizerTest, FlagsNoneIsNoOp) {
    auto mesh = createScrambledGrid("ExportOpt_none");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("ExportOpt_none_node");
    auto* entity = sceneMgr->createEntity("ExportOpt_none_entity", mesh);
    node->attachObject(entity);

    const auto report = ExportOptimizer::optimizeEntity(entity, OptimizeFlags::None);
    EXPECT_TRUE(report.empty())
        << "OptimizeFlags::None should short-circuit and produce no report rows";
}

TEST_F(ExportOptimizerTest, NullEntityReturnsEmptyReport) {
    const auto report = ExportOptimizer::optimizeEntity(nullptr, OptimizeFlags::All);
    EXPECT_TRUE(report.empty());
}
