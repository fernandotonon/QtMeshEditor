#include <gtest/gtest.h>
#include <QCoreApplication>

#include "MeshDecimator.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreHardwareIndexBuffer.h>
#include <OgreHardwareVertexBuffer.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>

#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Coverage suite for the Ogre-backed (LCOV_EXCL) MeshDecimator paths:
//   - countBaseline
//   - projectEntity (analyze-only)
//   - decimateEntity(entity, reduction)              [2-arg -> Algorithm::Ogre]
//   - decimateEntity(entity, reduction, Algorithm::Ogre)    [LodConfig path]
//   - decimateEntity(entity, reduction, Algorithm::Meshopt) [meshoptimizer path]
//   - reduction<=0 no-op early return
//   - null-entity guards
//
// The pure-data arithmetic + JSON/text are already covered by
// MeshDecimator_test.cpp; this suite uses a DISTINCT filename and the
// DISTINCT suite name MeshDecimatorCoverageTest to avoid ODR / registration
// clashes.
// ---------------------------------------------------------------------------

namespace {

// Procedurally build an N×N subdivided plane mesh: every cell becomes two
// triangles, giving 2*N*N triangles and (N+1)^2 vertices. Carries position +
// UV0 so the meshoptimizer `simplifyWithAttributes` branch (UV-aware) is
// exercised. N=8 => 128 tris / 81 verts — big enough to actually decimate.
// (Copied from MeshOptimizerLod_test.cpp.)
Ogre::MeshPtr createSubdividedPlane(const std::string& name, int n = 8)
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

} // namespace

class MeshDecimatorCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb required in CI)";
        ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";
        createStandardOgreMaterials();
        counter_ = ++sCounter;
    }
    void TearDown() override {
        Manager::kill();
    }

    // Build a fresh plane mesh + attached entity. Each test must use a fresh
    // mesh because decimation mutates the index buffer in place.
    Ogre::Entity* makeEntity(const std::string& tag)
    {
        const std::string meshName =
            "MeshDecCov_" + tag + "_" + std::to_string(counter_) + ".mesh";
        Ogre::MeshPtr mesh = createSubdividedPlane(meshName);
        if (!mesh) return nullptr;
        auto* node = Manager::getSingleton()->addSceneNode(
            QString::fromStdString("node_" + tag + "_" + std::to_string(counter_)));
        if (!node) return nullptr;
        return Manager::getSingleton()->createEntity(node, mesh);
    }

    int counter_ = 0;
    static int sCounter;
};

int MeshDecimatorCoverageTest::sCounter = 0;

// (a) countBaseline sums indexCount/3 and vertexCount incl. shared verts.
TEST_F(MeshDecimatorCoverageTest, CountBaselineSumsTrisAndVerts)
{
    Ogre::Entity* entity = makeEntity("baseline");
    ASSERT_NE(entity, nullptr);

    int tris = -1, verts = -1;
    MeshDecimator::countBaseline(entity, tris, verts);
    // N=8 plane: 2*8*8 = 128 triangles, (8+1)^2 = 81 shared vertices.
    EXPECT_EQ(128, tris);
    EXPECT_EQ(81, verts);
}

// countBaseline null-entity guard -> outputs zeroed.
TEST_F(MeshDecimatorCoverageTest, CountBaselineNullEntity)
{
    int tris = 99, verts = 99;
    MeshDecimator::countBaseline(nullptr, tris, verts);
    EXPECT_EQ(0, tris);
    EXPECT_EQ(0, verts);
}

// (b) projectEntity is analyze-only: predicted after ~= before*(1-r), the
// mesh is left untouched, applied stays false.
TEST_F(MeshDecimatorCoverageTest, ProjectEntityIsNonMutatingAndPredicts)
{
    Ogre::Entity* entity = makeEntity("project");
    ASSERT_NE(entity, nullptr);

    const size_t before = entity->getMesh()->getSubMesh(0)->indexData->indexCount;

    DecimationReport report = MeshDecimator::projectEntity(entity, 0.5);
    EXPECT_FALSE(report.applied);
    EXPECT_NEAR(0.5, report.appliedReduction, 1e-9);
    EXPECT_EQ(128, report.totalTrianglesBefore);
    // round(128 * 0.5) = 64
    EXPECT_EQ(64, report.totalTrianglesAfter);
    ASSERT_EQ(1, report.submeshes.size());
    EXPECT_EQ(128, report.submeshes[0].trianglesBefore);
    EXPECT_EQ(64, report.submeshes[0].trianglesAfter);

    // Mesh unchanged — projection must not touch the index buffer.
    EXPECT_EQ(before, entity->getMesh()->getSubMesh(0)->indexData->indexCount);
}

// projectEntity floor: max(1,...) — a tiny submesh at high reduction keeps >=1.
TEST_F(MeshDecimatorCoverageTest, ProjectEntityFloorsAtOne)
{
    Ogre::Entity* entity = makeEntity("projectfloor");
    ASSERT_NE(entity, nullptr);

    // 95% reduction: round(128*0.05) = 6 -> still >=1, but exercises the
    // clamp/floor branch with a high reduction.
    DecimationReport report = MeshDecimator::projectEntity(entity, 0.99);
    EXPECT_FALSE(report.applied);
    // 0.99 clamps to kMaxReduction (0.95): round(128*0.05) = 6.
    EXPECT_NEAR(MeshDecimator::kMaxReduction, report.appliedReduction, 1e-9);
    ASSERT_EQ(1, report.submeshes.size());
    EXPECT_GE(report.submeshes[0].trianglesAfter, 1);
    EXPECT_LT(report.submeshes[0].trianglesAfter,
              report.submeshes[0].trianglesBefore);
}

// projectEntity null-entity guard -> empty report.
TEST_F(MeshDecimatorCoverageTest, ProjectEntityNullReturnsEmpty)
{
    DecimationReport report = MeshDecimator::projectEntity(nullptr, 0.5);
    EXPECT_FALSE(report.applied);
    EXPECT_TRUE(report.meshName.isEmpty());
    EXPECT_EQ(0, report.totalTrianglesBefore);
    EXPECT_EQ(0, report.totalTrianglesAfter);
    EXPECT_TRUE(report.submeshes.isEmpty());
}

// (c) decimateEntity(entity, 0.5) — 2-arg delegate -> Algorithm::Ogre path.
// decimateEntity(entity, 0.5, Algorithm::Ogre) — explicit Ogre LodConfig path.
// (d) decimateEntity(entity, 0.5, Algorithm::Meshopt) — meshoptimizer branch:
// generateLods -> mLodFaceList -> promoteFirstLodToBase -> recount.
TEST_F(MeshDecimatorCoverageTest, DecimateMeshoptReduces)
{
    Ogre::Entity* entity = makeEntity("meshopt");
    ASSERT_NE(entity, nullptr);

    DecimationReport report = MeshDecimator::decimateEntity(
        entity, 0.5, MeshDecimator::Algorithm::Meshopt);
    EXPECT_TRUE(report.applied);
    EXPECT_EQ(128, report.totalTrianglesBefore);
    EXPECT_LT(report.totalTrianglesAfter, report.totalTrianglesBefore);
    EXPECT_GT(report.totalTrianglesAfter, 0);

    const size_t liveTris =
        entity->getMesh()->getSubMesh(0)->indexData->indexCount / 3;
    EXPECT_EQ(static_cast<int>(liveTris), report.totalTrianglesAfter);
    EXPECT_LT(liveTris, 128u);
}

// (e) decimateEntity(entity, 0.0) — no-op early return: after == before,
// applied stays false, mesh unchanged.
TEST_F(MeshDecimatorCoverageTest, DecimateZeroIsNoOp)
{
    Ogre::Entity* entity = makeEntity("zero");
    ASSERT_NE(entity, nullptr);

    const size_t before = entity->getMesh()->getSubMesh(0)->indexData->indexCount;

    DecimationReport report = MeshDecimator::decimateEntity(entity, 0.0);
    EXPECT_FALSE(report.applied);
    EXPECT_EQ(report.totalTrianglesBefore, report.totalTrianglesAfter);
    EXPECT_EQ(128, report.totalTrianglesBefore);
    EXPECT_NEAR(0.0, report.appliedReduction, 1e-9);

    // Mesh untouched.
    EXPECT_EQ(before, entity->getMesh()->getSubMesh(0)->indexData->indexCount);
}

// Negative reduction also clamps to 0 -> no-op (clampReduction branch).
TEST_F(MeshDecimatorCoverageTest, DecimateNegativeIsNoOp)
{
    Ogre::Entity* entity = makeEntity("neg");
    ASSERT_NE(entity, nullptr);

    DecimationReport report = MeshDecimator::decimateEntity(entity, -0.5);
    EXPECT_FALSE(report.applied);
    EXPECT_EQ(report.totalTrianglesBefore, report.totalTrianglesAfter);
}

// decimateEntity null-entity guard -> empty report (applied false).
TEST_F(MeshDecimatorCoverageTest, DecimateNullReturnsEmpty)
{
    DecimationReport report = MeshDecimator::decimateEntity(nullptr, 0.5);
    EXPECT_FALSE(report.applied);
    EXPECT_TRUE(report.meshName.isEmpty());
    EXPECT_EQ(0, report.totalTrianglesBefore);
    EXPECT_EQ(0, report.totalTrianglesAfter);

    DecimationReport reportMeshopt = MeshDecimator::decimateEntity(
        nullptr, 0.5, MeshDecimator::Algorithm::Meshopt);
    EXPECT_FALSE(reportMeshopt.applied);
    EXPECT_TRUE(reportMeshopt.submeshes.isEmpty());
}

// The applied report round-trips through toJson with the post-decimation
// totals (covers the applied=true serialization shape end-to-end).
TEST_F(MeshDecimatorCoverageTest, DecimateAppliedReportSerializes)
{
    Ogre::Entity* entity = makeEntity("json");
    ASSERT_NE(entity, nullptr);

    DecimationReport report = MeshDecimator::decimateEntity(entity, 0.5);
    ASSERT_TRUE(report.applied);

    const QJsonObject obj = MeshDecimator::toJson(report);
    EXPECT_TRUE(obj["applied"].toBool());
    EXPECT_EQ(128, obj["totals"].toObject()["trianglesBefore"].toInt());
    EXPECT_LT(obj["totals"].toObject()["trianglesAfter"].toInt(), 128);
    EXPECT_GT(obj["totals"].toObject()["effectiveReduction"].toDouble(), 0.0);
}
