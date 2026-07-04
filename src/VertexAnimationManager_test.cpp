#include <gtest/gtest.h>

#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "VertexAnimationManager.h"

#include <OgreAnimation.h>
#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgrePose.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubMesh.h>

#include <cmath>
#include <vector>

namespace {

// A minimal static mesh (one triangle, 3 verts) the vertex-anim clip is built
// against — same shape MeshGenBuilder/MeshProcessor produce (non-shared
// submesh, POSITION+NORMAL). Vertex-anim poses target submesh handle 1.
Ogre::MeshPtr createStaticMesh(const std::string& name, int nverts = 3)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* sub = mesh->createSubMesh();
    sub->useSharedVertices = false;
    sub->vertexData = new Ogre::VertexData();
    auto* decl = sub->vertexData->vertexDeclaration;
    size_t off = 0;
    decl->addElement(0, off, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    off += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, off, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), nverts, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    std::vector<float> verts(static_cast<size_t>(nverts) * 6, 0.0f);
    for (int v = 0; v < nverts; ++v) {
        verts[v * 6 + 0] = static_cast<float>(v);  // x = index — distinct bind
        verts[v * 6 + 5] = 1.0f;                    // normal +Z
    }
    vbuf->writeData(0, verts.size() * sizeof(float), verts.data());
    sub->vertexData->vertexBufferBinding->setBinding(0, vbuf);
    sub->vertexData->vertexCount = static_cast<size_t>(nverts);

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = {0, 1, 2};
    ibuf->writeData(0, sizeof(idx), idx);
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 3;

    mesh->_setBounds(Ogre::AxisAlignedBox(-1, -1, -1, 4, 4, 4));
    mesh->_setBoundingSphereRadius(4.0f);
    mesh->load();
    return mesh;
}

// A synthetic vertex-animation buffer: `frameCount` frames of `nverts` vertices
// wobbling in Y over time — the stand-in for a decoded Alembic cache. Bind
// positions match createStaticMesh (x=index), so frame deltas are pure Y.
VertexAnimationManager::FrameSet makeWobble(int nverts, int frameCount, int fps = 30)
{
    VertexAnimationManager::FrameSet fs;
    fs.vertexCount = nverts;
    fs.fps = fps;
    for (int f = 0; f < frameCount; ++f) {
        VertexAnimationManager::FrameData fd;
        fd.time = static_cast<float>(f) / static_cast<float>(fps);
        fd.positions.resize(static_cast<size_t>(nverts) * 3);
        for (int v = 0; v < nverts; ++v) {
            fd.positions[v * 3 + 0] = static_cast<float>(v);
            fd.positions[v * 3 + 1] =
                0.5f * std::sin(static_cast<float>(f) * 0.5f + static_cast<float>(v));
            fd.positions[v * 3 + 2] = 0.0f;
        }
        fs.frames.push_back(std::move(fd));
    }
    return fs;
}

}  // namespace

// =============================================================================
// Pure-data (no Ogre) — heuristic + FrameSet validity
// =============================================================================

TEST(VertexAnimationManagerStandalone, InstanceIsSingleton) {
    EXPECT_EQ(VertexAnimationManager::instance(), VertexAnimationManager::instance());
}

TEST(VertexAnimationManagerStandalone, HeuristicSplitsAtThreshold) {
    using S = VertexAnimationManager::Storage;
    EXPECT_EQ(VertexAnimationManager::sampleHeuristic(2),  S::Poses);
    EXPECT_EQ(VertexAnimationManager::sampleHeuristic(31), S::Poses);
    EXPECT_EQ(VertexAnimationManager::sampleHeuristic(32), S::Stream);
    EXPECT_EQ(VertexAnimationManager::sampleHeuristic(1000), S::Stream);
}

TEST(VertexAnimationManagerStandalone, FrameSetOkRequiresTwoFrames) {
    EXPECT_FALSE(VertexAnimationManager::FrameSet{}.ok());
    auto one = makeWobble(3, 1);
    EXPECT_FALSE(one.ok());
    auto two = makeWobble(3, 2);
    EXPECT_TRUE(two.ok());
}

// =============================================================================
// Ogre-backed — clip construction + enumeration (headless-safe)
// =============================================================================

class VertexAnimationManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    }
    void TearDown() override {
        auto& mm = Ogre::MeshManager::getSingleton();
        for (const char* n : {"vam_build", "vam_enum", "vam_mismatch"})
            if (mm.resourceExists(n)) mm.remove(n);
    }
};

TEST_F(VertexAnimationManagerTest, BuildClipFromFramesCreatesPosesAndTrack) {
    auto mesh = createStaticMesh("vam_build", 3);
    auto fs = makeWobble(3, 8);

    ASSERT_TRUE(VertexAnimationManager::buildClipFromFrames(mesh.get(), "wobble", fs));

    // One Animation named "wobble" with a vertex track and one pose per frame.
    ASSERT_TRUE(mesh->hasAnimation("wobble"));
    Ogre::Animation* a = mesh->getAnimation("wobble");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(mesh->getPoseCount(), 8u);           // one pose per frame
    Ogre::VertexAnimationTrack* track = a->getVertexTrack(1);  // submesh handle 1
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->getAnimationType(), Ogre::VAT_POSE);
    EXPECT_EQ(track->getNumKeyFrames(), 8u);       // one keyframe per frame
    // Clip length spans the frame times (8 frames @30fps → 7/30 s).
    EXPECT_NEAR(a->getLength(), 7.0f / 30.0f, 1e-4f);
}

// Rebuilding the same clip must not leak the previous run's per-frame poses
// ("<clip>/frameN"). Ogre poses are mesh-level; a leak would accumulate and
// shift pose indices (regression from the B3 review).
TEST_F(VertexAnimationManagerTest, RebuildDoesNotLeakPoses) {
    auto mesh = createStaticMesh("vam_rebuild", 3);

    ASSERT_TRUE(VertexAnimationManager::buildClipFromFrames(
        mesh.get(), "wobble", makeWobble(3, 8)));
    EXPECT_EQ(mesh->getPoseCount(), 8u);

    // Rebuild with a DIFFERENT frame count — pose count must reflect only the
    // new build, not 8 + 5.
    ASSERT_TRUE(VertexAnimationManager::buildClipFromFrames(
        mesh.get(), "wobble", makeWobble(3, 5)));
    EXPECT_EQ(mesh->getPoseCount(), 5u);
    ASSERT_TRUE(mesh->hasAnimation("wobble"));
    EXPECT_EQ(mesh->getAnimation("wobble")->getVertexTrack(1)->getNumKeyFrames(), 5u);

    // A second, differently-named clip must coexist (only same-named frames
    // are dropped).
    ASSERT_TRUE(VertexAnimationManager::buildClipFromFrames(
        mesh.get(), "other", makeWobble(3, 4)));
    EXPECT_EQ(mesh->getPoseCount(), 9u);   // 5 (wobble) + 4 (other)
}

TEST_F(VertexAnimationManagerTest, BuildClipRejectsVertexCountMismatch) {
    auto mesh = createStaticMesh("vam_mismatch", 3);
    auto fs = makeWobble(5, 4);  // 5 verts vs the mesh's 3
    EXPECT_FALSE(VertexAnimationManager::buildClipFromFrames(mesh.get(), "bad", fs));
    EXPECT_FALSE(mesh->hasAnimation("bad"));
}

TEST_F(VertexAnimationManagerTest, EnumeratesVertexClipsOnEntity) {
    auto mesh = createStaticMesh("vam_enum", 3);
    ASSERT_TRUE(VertexAnimationManager::buildClipFromFrames(
        mesh.get(), "sim", makeWobble(3, 4)));

    Ogre::SceneManager* sm = Manager::getSingleton()->getSceneMgr();
    Ogre::Entity* ent = sm->createEntity("vam_enum_ent", "vam_enum");

    auto* vam = VertexAnimationManager::instance();
    EXPECT_TRUE(vam->hasVertexAnimation(ent));
    const QStringList clips = vam->vertexClipsFor(ent);
    ASSERT_EQ(clips.size(), 1);
    EXPECT_EQ(clips.first(), QStringLiteral("sim"));

    sm->destroyEntity(ent);
}
