#include "MeshWeldOps.h"
#include "TestHelpers.h"
#include "Manager.h"

#include <gtest/gtest.h>

#include <Ogre.h>

#include <string>

// Tests for the co-located-vertex weld (validation flow). GL-gated —
// building the fixture mesh needs hardware buffers, so the suite skips
// when the environment cannot create a render system (macOS local runs;
// Linux CI under Xvfb runs it).

namespace {

std::string uniqueWeldName(const char* prefix)
{
    static int counter = 0;
    return std::string(prefix) + std::to_string(++counter);
}

// Two triangles that SHOULD share an edge but were exported disconnected
// (the generated-mesh case):
//   v0 (0,0,0) uv(0,0)              tri A = 0,1,2
//   v1 (1,0,0) uv(1,0)
//   v2 (0,1,0) uv(0,1)
//   v3 (1,0,0) uv(1,0)   byte-identical twin of v1  → weldable
//   v4 (0,1,0) uv(.9,.9) co-located with v2, different UV → seam twin
//   v5 (1,1,0) uv(1,1)              tri B = 3,4,5
// Skinned: everything on bone 1 except v4, which gets bone 0 — the
// weight-mismatch cluster that tears during animation.
Ogre::Entity* createWeldFixtureEntity(const std::string& name, bool skinned,
                                      bool unweightedV2 = false)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name + "_mesh", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    decl->addElement(0, sizeof(float) * 3, Ogre::VET_FLOAT2,
                     Ogre::VES_TEXTURE_COORDINATES, 0);

    const float verts[6 * 5] = {
        0, 0, 0,  0.0f, 0.0f,
        1, 0, 0,  1.0f, 0.0f,
        0, 1, 0,  0.0f, 1.0f,
        1, 0, 0,  1.0f, 0.0f,   // v3 = byte-identical twin of v1
        0, 1, 0,  0.9f, 0.9f,   // v4 = co-located with v2, different UV
        1, 1, 0,  1.0f, 1.0f,
    };
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 6, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    vbuf->writeData(0, sizeof(verts), verts);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = 6;

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 6,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    const Ogre::uint16 idx[6] = { 0, 1, 2, 3, 4, 5 };
    ibuf->writeData(0, sizeof(idx), idx);
    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 6;

    if (skinned) {
        auto skel = Ogre::SkeletonManager::getSingleton().create(
            name + "_skel",
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        auto* root = skel->createBone("Root", 0);
        root->setPosition(Ogre::Vector3(0, 0, 0));
        auto* child = skel->createBone("Child", 1);
        child->setPosition(Ogre::Vector3(0, 1, 0));
        root->addChild(child);
        skel->setBindingPose();

        Ogre::VertexBoneAssignment vba;
        vba.weight = 1.0f;
        for (unsigned v = 0; v < 6; ++v) {
            if (unweightedV2 && v == 2) continue;   // v2 has NO assignments
            vba.vertexIndex = v;
            vba.boneIndex = (v == 4) ? 0 : 1;   // v4 mismatches its twin v2
            mesh->addBoneAssignment(vba);
        }
        mesh->_notifySkeleton(skel);
    }

    mesh->_setBounds(Ogre::AxisAlignedBox(-1, -1, -1, 2, 2, 2));
    mesh->_setBoundingSphereRadius(3.0f);
    mesh->load();

    auto* sm = Manager::getSingleton()->getSceneMgr();
    return sm->createEntity(name + "_ent", mesh);
}

std::vector<Ogre::uint16> readIndices(Ogre::Entity* entity)
{
    Ogre::SubMesh* sub = entity->getMesh()->getSubMesh(0);
    const auto& buf = sub->indexData->indexBuffer;
    std::vector<Ogre::uint16> out(sub->indexData->indexCount);
    buf->readData(0, out.size() * sizeof(Ogre::uint16), out.data());
    return out;
}

} // namespace

class MeshWeldOpsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed — invalid test environment";
        if (!canLoadMeshFiles()) GTEST_SKIP() << "no GL context (macOS local run)";
    }
};

TEST_F(MeshWeldOpsTest, AnalyzeReportsClustersWeldablesAndWeightMismatch)
{
    Ogre::Entity* ent =
        createWeldFixtureEntity(uniqueWeldName("weldAnalyze"), /*skinned=*/true);
    ASSERT_NE(ent, nullptr);

    const MeshWeldOps::Report rep = MeshWeldOps::analyze(ent);
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_TRUE(rep.skinned);
    EXPECT_EQ(rep.duplicateClusters, 2);        // {v1,v3} and {v2,v4}
    EXPECT_EQ(rep.duplicateVertices, 4);
    EXPECT_EQ(rep.weldableVertices, 1);         // only v3 is byte-identical
    EXPECT_EQ(rep.weightMismatchClusters, 1);   // {v2,v4} weights differ
    // analyze() must not mutate.
    EXPECT_EQ(rep.weldedVertices, 0);
    EXPECT_EQ(rep.weightsUnified, 0);
    const auto idx = readIndices(ent);
    EXPECT_EQ(idx[3], 3) << "analyze must leave the index buffer untouched";
}

TEST_F(MeshWeldOpsTest, ApplyWeldsIdenticalTwinAndUnifiesSeamWeights)
{
    Ogre::Entity* ent =
        createWeldFixtureEntity(uniqueWeldName("weldApply"), /*skinned=*/true);
    ASSERT_NE(ent, nullptr);
    Ogre::MeshPtr mesh = ent->getMesh();

    const MeshWeldOps::Report rep = MeshWeldOps::apply(ent);
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_EQ(rep.weldedVertices, 1);   // one index reference (v3 in tri B)
    EXPECT_EQ(rep.weightsUnified, 1);   // v4 rewritten to v2's weights

    // Index buffer: tri B's first corner now references v1.
    const auto idx = readIndices(ent);
    EXPECT_EQ(idx[3], 1);
    // Seam twin v4 keeps its own index (different UV — never welded).
    EXPECT_EQ(idx[4], 4);

    // v4's weights now match v2's (bone 1).
    bool foundV4 = false;
    for (const auto& [vi, vba] : mesh->getBoneAssignments()) {
        if (vba.vertexIndex == 4) {
            foundV4 = true;
            EXPECT_EQ(vba.boneIndex, 1);
            EXPECT_NEAR(vba.weight, 1.0f, 1e-5f);
        }
    }
    EXPECT_TRUE(foundV4);

    // Re-analyzing the fixed mesh reports nothing left to fix.
    const MeshWeldOps::Report again = MeshWeldOps::analyze(ent);
    ASSERT_TRUE(again.ok);
    EXPECT_EQ(again.weightMismatchClusters, 0);
}

TEST_F(MeshWeldOpsTest, UnskinnedMeshWeldsWithoutWeightWork)
{
    Ogre::Entity* ent = createWeldFixtureEntity(uniqueWeldName("weldStatic"),
                                                /*skinned=*/false);
    ASSERT_NE(ent, nullptr);

    const MeshWeldOps::Report rep = MeshWeldOps::apply(ent);
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_FALSE(rep.skinned);
    EXPECT_EQ(rep.weldedVertices, 1);
    EXPECT_EQ(rep.weightsUnified, 0);
    EXPECT_EQ(rep.weightMismatchClusters, 0);
}

TEST_F(MeshWeldOpsTest, CleanMeshReportsNothing)
{
    Ogre::MeshPtr mesh =
        createInMemoryTriangleMesh(uniqueWeldName("weldClean"));
    ASSERT_TRUE(mesh);
    auto* sm = Manager::getSingleton()->getSceneMgr();
    Ogre::Entity* ent =
        sm->createEntity(uniqueWeldName("weldCleanEnt"), mesh);

    const MeshWeldOps::Report rep = MeshWeldOps::analyze(ent);
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_EQ(rep.duplicateClusters, 0);
    EXPECT_EQ(rep.weldableVertices, 0);
    EXPECT_EQ(rep.weightMismatchClusters, 0);
}

TEST_F(MeshWeldOpsTest, NullEntityFailsGracefully)
{
    const MeshWeldOps::Report rep = MeshWeldOps::analyze(nullptr);
    EXPECT_FALSE(rep.ok);
    EXPECT_FALSE(rep.error.isEmpty());
}

TEST_F(MeshWeldOpsTest, UnweightedClusterMemberNeverBecomesTheRepresentative)
{
    // v2 carries NO assignments while its co-located twin v4 is weighted to
    // bone 0. The representative must be the WEIGHTED member — copying the
    // empty map would delete valid skinning data instead of repairing it.
    Ogre::Entity* ent = createWeldFixtureEntity(
        uniqueWeldName("weldEmptyRep"), /*skinned=*/true, /*unweightedV2=*/true);
    ASSERT_NE(ent, nullptr);

    const MeshWeldOps::Report rep = MeshWeldOps::apply(ent);
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_GE(rep.weightsUnified, 1);

    // v2 gained v4's weights (bone 0); v4 kept them.
    float w2 = -1.0f, w4 = -1.0f;
    for (const auto& [vi, vba] : ent->getMesh()->getBoneAssignments()) {
        if (vba.vertexIndex == 2 && vba.boneIndex == 0) w2 = vba.weight;
        if (vba.vertexIndex == 4 && vba.boneIndex == 0) w4 = vba.weight;
    }
    EXPECT_NEAR(w2, 1.0f, 1e-5f) << "the unweighted twin must ADOPT weights";
    EXPECT_NEAR(w4, 1.0f, 1e-5f) << "the weighted twin must keep its weights";
}
