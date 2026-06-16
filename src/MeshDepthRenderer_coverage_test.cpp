// Coverage tests for MeshDepthRenderer (issue #403).
//
// The class is always compiled into the test binary (the production
// call sites are ENABLE_STABLE_DIFFUSION-guarded, but the class itself
// links unconditionally — see commit "compile MeshDepthRenderer into
// test binary"). These tests exercise renderDepthMap()'s error branches
// plus the full RTT happy path, and shutdown()'s idempotency.
//
// Distinct filename + suite names from any future MeshDepthRenderer_test.cpp
// to avoid ODR / duplicate-registration clashes.

#include <gtest/gtest.h>

#include <QImage>
#include <QString>

#include "MeshDepthRenderer.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

// ---------------------------------------------------------------------------
// Pure (no-Ogre-required) branches: null entity + shutdown() idempotency.
// These run regardless of whether the render system is available.
// ---------------------------------------------------------------------------

TEST(MeshDepthRendererCoverageTest, NullEntityReturnsNullImageWithError)
{
    QString err = QStringLiteral("untouched");
    QImage img = MeshDepthRenderer::renderDepthMap(nullptr, 256, &err);
    EXPECT_TRUE(img.isNull());
    EXPECT_EQ(err, QStringLiteral("null entity"));
}

TEST(MeshDepthRendererCoverageTest, NullEntityWithNullErrorOutDoesNotCrash)
{
    // errorOut == nullptr must be tolerated on the null-entity path.
    QImage img = MeshDepthRenderer::renderDepthMap(nullptr, 256, nullptr);
    EXPECT_TRUE(img.isNull());
}

TEST(MeshDepthRendererCoverageTest, ShutdownIsIdempotentWhenNothingAllocated)
{
    // Safe to call when nothing has been allocated, and safe to call twice.
    MeshDepthRenderer::shutdown();
    MeshDepthRenderer::shutdown();
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Ogre-backed fixture: full render-target path, auto-frame, readback, and
// shutdown after allocation.
// ---------------------------------------------------------------------------

class MeshDepthRendererOgreTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();

        mesh_ = createInMemoryTriangleMesh(uniqueName("MDRcovMesh"));
        ASSERT_TRUE(static_cast<bool>(mesh_));

        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        ASSERT_NE(sceneMgr, nullptr);
        node_ = Manager::getSingleton()->addSceneNode(
            QString::fromStdString(uniqueName("MDRcovNode")));
        ASSERT_NE(node_, nullptr);
        entity_ = sceneMgr->createEntity(uniqueName("MDRcovEnt"), mesh_);
        ASSERT_NE(entity_, nullptr);
        node_->attachObject(entity_);
    }

    void TearDown() override
    {
        // Release any RTT / camera / nodes the renderer cached.
        MeshDepthRenderer::shutdown();
    }

    static std::string uniqueName(const char* base)
    {
        static int counter = 0;
        return std::string(base) + std::to_string(++counter);
    }

    Ogre::MeshPtr   mesh_;
    Ogre::SceneNode* node_ = nullptr;
    Ogre::Entity*    entity_ = nullptr;
};

TEST_F(MeshDepthRendererOgreTest, HappyPathRendersGrayscaleImageOfRequestedSize)
{
    QString err = QStringLiteral("untouched");
    QImage img = MeshDepthRenderer::renderDepthMap(entity_, 64, &err);

    EXPECT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 64);
    EXPECT_EQ(img.height(), 64);
    // Output is collapsed to grayscale then re-expanded to RGB888.
    EXPECT_EQ(img.format(), QImage::Format_RGB888);
    // On success errorOut is left untouched.
    EXPECT_EQ(err, QStringLiteral("untouched"));
}

TEST_F(MeshDepthRendererOgreTest, HappyPathToleratesNullErrorOut)
{
    QImage img = MeshDepthRenderer::renderDepthMap(entity_, 64, nullptr);
    EXPECT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 64);
}

TEST_F(MeshDepthRendererOgreTest, SizeIsClampedToMinimumOf64)
{
    // size is clamped to [64, 2048]; a tiny request still yields 64x64.
    QImage img = MeshDepthRenderer::renderDepthMap(entity_, 1, nullptr);
    EXPECT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 64);
    EXPECT_EQ(img.height(), 64);
}

TEST_F(MeshDepthRendererOgreTest, RenderTargetIsReusedAcrossSameSizeCalls)
{
    // First call allocates the RTT at 64; a second 64 call must reuse it
    // (ensureRenderTarget early-returns) and still produce a valid image.
    QImage a = MeshDepthRenderer::renderDepthMap(entity_, 64, nullptr);
    QImage b = MeshDepthRenderer::renderDepthMap(entity_, 64, nullptr);
    EXPECT_FALSE(a.isNull());
    EXPECT_FALSE(b.isNull());
    EXPECT_EQ(a.size(), b.size());
}

TEST_F(MeshDepthRendererOgreTest, ChangingSizeReallocatesRenderTarget)
{
    QImage small = MeshDepthRenderer::renderDepthMap(entity_, 64, nullptr);
    QImage large = MeshDepthRenderer::renderDepthMap(entity_, 128, nullptr);
    EXPECT_EQ(small.width(), 64);
    EXPECT_EQ(large.width(), 128);
}

TEST_F(MeshDepthRendererOgreTest, OriginalMaterialRestoredAfterRender)
{
    // The renderer swaps every sub-entity to the depth material then must
    // restore the original via the RAII Restorer.
    ASSERT_GT(entity_->getNumSubEntities(), 0u);
    const Ogre::String before = entity_->getSubEntity(0)->getMaterialName();

    QImage img = MeshDepthRenderer::renderDepthMap(entity_, 64, nullptr);
    EXPECT_FALSE(img.isNull());

    const Ogre::String after = entity_->getSubEntity(0)->getMaterialName();
    EXPECT_EQ(before, after);
    EXPECT_NE(after, std::string("QtMesh/DepthControlNet"));
}

TEST_F(MeshDepthRendererOgreTest, SceneFogRestoredAfterRender)
{
    auto* sm = Manager::getSingleton()->getSceneMgr();
    const Ogre::FogMode beforeMode = sm->getFogMode();

    QImage img = MeshDepthRenderer::renderDepthMap(entity_, 64, nullptr);
    EXPECT_FALSE(img.isNull());

    // Fog is enabled during capture and restored afterwards.
    EXPECT_EQ(sm->getFogMode(), beforeMode);
}

TEST_F(MeshDepthRendererOgreTest, ZeroSizeBoundingBoxReturnsError)
{
    // Build a degenerate mesh whose bounds are a single point: half-size
    // length is 0, so the radius guard fires.
    auto degenerate = Ogre::MeshManager::getSingleton().createManual(
        uniqueName("MDRcovDegenerate"),
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* sub = degenerate->createSubMesh();
    degenerate->sharedVertexData = new Ogre::VertexData();
    auto* decl = degenerate->sharedVertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    float verts[] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    vbuf->writeData(0, sizeof(verts), verts);
    degenerate->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    degenerate->sharedVertexData->vertexCount = 3;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = {0, 1, 2};
    ibuf->writeData(0, sizeof(idx), idx);
    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 3;
    // Collapse the bounding box to a single point so getHalfSize() is zero.
    degenerate->_setBounds(Ogre::AxisAlignedBox(0, 0, 0, 0, 0, 0), false);
    degenerate->_setBoundingSphereRadius(0.0f);
    degenerate->load();

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode(
        QString::fromStdString(uniqueName("MDRcovDegNode")));
    auto* ent = sceneMgr->createEntity(uniqueName("MDRcovDegEnt"), degenerate);
    node->attachObject(ent);

    QString err = QStringLiteral("untouched");
    QImage img = MeshDepthRenderer::renderDepthMap(ent, 64, &err);
    EXPECT_TRUE(img.isNull());
    EXPECT_EQ(err, QStringLiteral("entity has zero-size bounding box"));
}

TEST_F(MeshDepthRendererOgreTest, ShutdownAfterAllocationIsIdempotent)
{
    // Allocate the RTT / camera by rendering once...
    QImage img = MeshDepthRenderer::renderDepthMap(entity_, 64, nullptr);
    EXPECT_FALSE(img.isNull());

    // ...then shut down twice. The second call must be a safe no-op.
    MeshDepthRenderer::shutdown();
    MeshDepthRenderer::shutdown();

    // And rendering still works after a shutdown (re-allocates).
    QImage again = MeshDepthRenderer::renderDepthMap(entity_, 64, nullptr);
    EXPECT_FALSE(again.isNull());
    EXPECT_EQ(again.width(), 64);
}
