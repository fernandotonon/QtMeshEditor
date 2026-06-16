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

// Coverage suite for MeshOptimizerLod focused on the gaps the original
// MeshOptimizerLod_test.cpp leaves open:
//   * the explicit non-default `errorBudget` argument of generateLods
//   * the per-submesh `LodLevel::actualReductions` output (never asserted)
//   * the no-UV-channel path (meshopt_simplify, NOT simplifyWithAttributes)
//   * destroyLevel on a partially-consumed level (some indices nulled out,
//     as MeshDecimator does when it moves an IndexData into mLodFaceList)
//
// Distinct suite name (MeshOptimizerLodCoverageTest) + distinct file to
// avoid ODR / duplicate-registration clashes with the existing suite.

namespace {

// Subdivided N×N plane WITH UV0 (position + TEXCOORD_0). 2*N*N tris,
// (N+1)² verts. Used for the UV-aware (simplifyWithAttributes) branch.
Ogre::MeshPtr createPlaneWithUvs(const std::string& name, int n = 8)
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
            verts.push_back(u);
            verts.push_back(v);
            verts.push_back(0.0f);
            verts.push_back(u);
            verts.push_back(v);
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

    mesh->_setBounds(Ogre::AxisAlignedBox(0, 0, 0, 1, 1, 0));
    mesh->_setBoundingSphereRadius(1.5f);
    mesh->load();
    return mesh;
}

// Position-ONLY plane (no TEXCOORD_0). Copied from UvUnwrap_test.cpp so
// extractUV0() returns empty and generateLods falls through to the
// non-attribute meshopt_simplify branch.
Ogre::MeshPtr createPlaneNoUvs(const std::string& name, int n = 8)
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

} // namespace

class MeshOptimizerLodCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb required in CI)";
        createStandardOgreMaterials();
    }
    void TearDown() override {
        Manager::kill();
    }
};

// --- (a) explicit non-default errorBudget + actualReductions ---------------

TEST_F(MeshOptimizerLodCoverageTest, ExplicitErrorBudgetPopulatesActualReductions) {
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";

    auto mesh = createPlaneWithUvs("MeshOptLodCov_budget");
    ASSERT_TRUE(mesh);

    const size_t baseIdx = mesh->getSubMesh(0)->indexData->indexCount;
    ASSERT_GT(baseIdx, 90u);

    // Large error budget (0.1 = 10% of bounding diagonal) lets the
    // simplifier collapse more aggressively than the default 0.01.
    std::vector<float> reductions = {0.5f};
    auto levels = MeshOptimizerLod::generateLods(mesh.get(), reductions, 0.1f);
    ASSERT_EQ(levels.size(), 1u);

    // actualReductions must have one entry per submesh, aligned with indices.
    ASSERT_EQ(levels[0].actualReductions.size(), levels[0].indices.size());
    ASSERT_EQ(levels[0].actualReductions.size(), 1u);
    ASSERT_NE(levels[0].indices[0], nullptr);

    const float actual = levels[0].actualReductions[0];
    // Reported reduction must be a sane ratio in (0, 1].
    EXPECT_GT(actual, 0.0f) << "a large budget should achieve some reduction";
    EXPECT_LE(actual, 1.0f);

    // actualReductions should track the achieved index-count change.
    const size_t lodIdx = levels[0].indices[0]->indexCount;
    const float observed = 1.0f - static_cast<float>(lodIdx) / static_cast<float>(baseIdx);
    EXPECT_NEAR(actual, observed, 1e-4f)
        << "actualReductions[0] must match the real index-count drop";
    EXPECT_LT(lodIdx, baseIdx) << "large budget at 50% target should drop tris";

    MeshOptimizerLod::destroyLevel(levels[0]);
}

TEST_F(MeshOptimizerLodCoverageTest, LargerBudgetReducesAtLeastAsMuch) {
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";

    auto meshSmall = createPlaneWithUvs("MeshOptLodCov_small_budget");
    auto meshLarge = createPlaneWithUvs("MeshOptLodCov_large_budget");

    auto small = MeshOptimizerLod::generateLods(meshSmall.get(), {0.75f}, 0.001f);
    auto large = MeshOptimizerLod::generateLods(meshLarge.get(), {0.75f}, 0.5f);
    ASSERT_EQ(small.size(), 1u);
    ASSERT_EQ(large.size(), 1u);
    ASSERT_NE(small[0].indices[0], nullptr);
    ASSERT_NE(large[0].indices[0], nullptr);

    // A bigger error budget removes vertex-movement constraints, so the
    // achieved reduction should be >= the tightly-budgeted one.
    EXPECT_GE(large[0].actualReductions[0], small[0].actualReductions[0] - 1e-4f);
    EXPECT_LE(large[0].indices[0]->indexCount, small[0].indices[0]->indexCount);

    MeshOptimizerLod::destroyLevel(small[0]);
    MeshOptimizerLod::destroyLevel(large[0]);
}

// --- (b) no-UV-channel fallback (meshopt_simplify, not WithAttributes) -----

TEST_F(MeshOptimizerLodCoverageTest, NoUvChannelUsesSimplifyFallback) {
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";

    auto mesh = createPlaneNoUvs("MeshOptLodCov_nouv");
    ASSERT_TRUE(mesh);

    const size_t baseIdx = mesh->getSubMesh(0)->indexData->indexCount;
    ASSERT_GT(baseIdx, 90u);

    // No TEXCOORD_0 -> extractUV0 empty -> non-attribute meshopt_simplify path.
    auto levels = MeshOptimizerLod::generateLods(mesh.get(), {0.5f});
    ASSERT_EQ(levels.size(), 1u);
    ASSERT_EQ(levels[0].indices.size(), 1u);
    ASSERT_NE(levels[0].indices[0], nullptr);
    ASSERT_EQ(levels[0].actualReductions.size(), 1u);

    const size_t lodIdx = levels[0].indices[0]->indexCount;
    EXPECT_LT(lodIdx, baseIdx) << "position-only plane should still decimate";
    EXPECT_EQ(lodIdx % 3, 0u) << "result must be whole triangles";

    const float actual = levels[0].actualReductions[0];
    EXPECT_GT(actual, 0.0f);
    EXPECT_LE(actual, 1.0f);
    const float observed = 1.0f - static_cast<float>(lodIdx) / static_cast<float>(baseIdx);
    EXPECT_NEAR(actual, observed, 1e-4f);

    MeshOptimizerLod::destroyLevel(levels[0]);
}

TEST_F(MeshOptimizerLodCoverageTest, NoUvChannelWithExplicitBudget) {
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";

    auto mesh = createPlaneNoUvs("MeshOptLodCov_nouv_budget");

    // Exercise the fallback path AND the explicit-budget argument together.
    auto levels = MeshOptimizerLod::generateLods(mesh.get(), {0.6f}, 0.2f);
    ASSERT_EQ(levels.size(), 1u);
    ASSERT_NE(levels[0].indices[0], nullptr);
    EXPECT_GT(levels[0].actualReductions[0], 0.0f);
    EXPECT_LE(levels[0].actualReductions[0], 1.0f);

    MeshOptimizerLod::destroyLevel(levels[0]);
}

// --- (c) destroyLevel on a partially-consumed level ------------------------

TEST_F(MeshOptimizerLodCoverageTest, DestroyLevelHandlesNulledIndexEntry) {
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";

    auto mesh = createPlaneWithUvs("MeshOptLodCov_partial");
    auto levels = MeshOptimizerLod::generateLods(mesh.get(), {0.5f});
    ASSERT_EQ(levels.size(), 1u);
    ASSERT_EQ(levels[0].indices.size(), 1u);
    ASSERT_NE(levels[0].indices[0], nullptr);

    // Simulate MeshDecimator moving the IndexData into mLodFaceList: it
    // takes ownership and nulls the entry. destroyLevel must skip nulls
    // (the `if (idx)` guard) and not double-free / crash.
    Ogre::IndexData* moved = levels[0].indices[0];
    levels[0].indices[0] = nullptr;

    // destroyLevel iterates a vector with a nullptr entry — must be safe.
    EXPECT_NO_THROW(MeshOptimizerLod::destroyLevel(levels[0]));
    EXPECT_TRUE(levels[0].indices.empty());
    EXPECT_TRUE(levels[0].actualReductions.empty());

    // We own `moved` now — clean it up the same way destroyLevel would.
    ASSERT_NE(moved, nullptr);
    moved->indexBuffer.reset();
    OGRE_DELETE moved;
}

TEST_F(MeshOptimizerLodCoverageTest, DestroyLevelOnAllNullEntries) {
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";

    // A level whose every index slot was consumed/nulled (multi-submesh
    // decimate that committed all levels). destroyLevel must no-op safely.
    MeshOptimizerLod::LodLevel level;
    level.indices = {nullptr, nullptr, nullptr};
    level.actualReductions = {0.0f, 0.0f, 0.0f};

    EXPECT_NO_THROW(MeshOptimizerLod::destroyLevel(level));
    EXPECT_TRUE(level.indices.empty());
    EXPECT_TRUE(level.actualReductions.empty());
}

TEST_F(MeshOptimizerLodCoverageTest, DestroyLevelOnEmptyLevelIsNoOp) {
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";

    MeshOptimizerLod::LodLevel level; // default-constructed, empty vectors
    EXPECT_NO_THROW(MeshOptimizerLod::destroyLevel(level));
    EXPECT_TRUE(level.indices.empty());
    EXPECT_TRUE(level.actualReductions.empty());
}

// --- non-positive reduction branch (continue path) -------------------------

TEST_F(MeshOptimizerLodCoverageTest, NonPositiveReductionIsSkipped) {
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";

    auto mesh = createPlaneWithUvs("MeshOptLodCov_skip");
    // 0.0 and a negative are rejected (continue); only the 0.5 survives.
    auto levels = MeshOptimizerLod::generateLods(mesh.get(), {0.0f, -0.3f, 0.5f}, 0.05f);
    ASSERT_EQ(levels.size(), 1u) << "only the positive reduction yields a level";
    ASSERT_NE(levels[0].indices[0], nullptr);
    EXPECT_GE(levels[0].actualReductions[0], 0.0f);

    MeshOptimizerLod::destroyLevel(levels[0]);
}

// --- reduction >= 1.0 clamp branch -----------------------------------------

TEST_F(MeshOptimizerLodCoverageTest, ReductionAtOrAboveOneIsClamped) {
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";

    auto mesh = createPlaneWithUvs("MeshOptLodCov_clamp");
    const size_t baseIdx = mesh->getSubMesh(0)->indexData->indexCount;

    // reduction == 1.0 is clamped to 0.99 (collapse to ~1 triangle) rather
    // than dropped. Pair with an explicit budget to exercise that arg too.
    auto levels = MeshOptimizerLod::generateLods(mesh.get(), {1.0f}, 0.3f);
    ASSERT_EQ(levels.size(), 1u);
    ASSERT_NE(levels[0].indices[0], nullptr);

    const size_t lodIdx = levels[0].indices[0]->indexCount;
    EXPECT_LT(lodIdx, baseIdx) << "clamped 1.0 still produces a heavily reduced LOD";
    EXPECT_EQ(lodIdx % 3, 0u);
    EXPECT_GT(levels[0].actualReductions[0], 0.0f);

    MeshOptimizerLod::destroyLevel(levels[0]);
}
