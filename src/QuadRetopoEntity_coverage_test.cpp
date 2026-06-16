#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <QJsonObject>
#include <QJsonArray>

#include "QuadRetopo.h"

#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSubMesh.h>
#include <OgreMeshManager.h>
#include <OgreHardwareBufferManager.h>

#include <cstdint>
#include <string>
#include <vector>

// Coverage suite for the Ogre-backed entry point
//   QuadRetopo::retopologize(Ogre::Entity*, const QuadRetopoOptions&, Algorithm)
// (QuadRetopo.cpp lines 433-495) plus the namespace-local helper
// retopologizeSubmesh (lines 225-314). The existing QuadRetopo_test.cpp
// only exercises the pure-data retopologizeMesh overload and explicitly
// skips the entity overload in headless CI — this suite drives a real
// Ogre::Entity built from an in-memory coplanar quad-grid so triangle
// pairs actually merge into quads.
//
// Distinct suite name (QuadRetopoEntityCoverageTest) and filename to
// avoid ODR / duplicate-registration clashes with QuadRetopoTest.

namespace {

static constexpr unsigned long kSingletonSettleTimeMs = 30;

// Build an entity from a coplanar quad-grid mesh (all z = 0).
//
// `cells` x `cells` quad grid → (cells*cells*2) triangles. Every
// triangle is coplanar with every neighbour, so with a relaxed
// shape tolerance the triangle-pairing backend merges adjacent
// triangle pairs into quads.
//
// Vertex layout: row-major grid of (cells+1) x (cells+1) verts on the
// XY plane, unit spacing. Each cell emits two CCW triangles sharing
// the cell diagonal — exactly the pairable pattern.
static Ogre::Entity* createGridEntity(const std::string& meshName,
                                      const std::string& nodeName,
                                      int cells)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        meshName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();

    const int side = cells + 1;
    const int vertCount = side * side;

    mesh->sharedVertexData = new Ogre::VertexData();
    mesh->sharedVertexData->vertexCount = vertCount;
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);

    auto posBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3), vertCount,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);

    std::vector<float> positions;
    positions.reserve(vertCount * 3);
    for (int r = 0; r < side; ++r) {
        for (int c = 0; c < side; ++c) {
            positions.push_back(static_cast<float>(c)); // x
            positions.push_back(static_cast<float>(r)); // y
            positions.push_back(0.0f);                  // z (coplanar)
        }
    }
    posBuf->writeData(0, positions.size() * sizeof(float), positions.data());
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, posBuf);

    // Two CCW triangles per cell.
    std::vector<uint16_t> idx;
    idx.reserve(cells * cells * 6);
    auto vid = [side](int r, int c) -> uint16_t {
        return static_cast<uint16_t>(r * side + c);
    };
    for (int r = 0; r < cells; ++r) {
        for (int c = 0; c < cells; ++c) {
            const uint16_t v00 = vid(r,     c);
            const uint16_t v10 = vid(r,     c + 1);
            const uint16_t v01 = vid(r + 1, c);
            const uint16_t v11 = vid(r + 1, c + 1);
            // tri A: v00, v10, v11
            idx.push_back(v00); idx.push_back(v10); idx.push_back(v11);
            // tri B: v00, v11, v01  (shares diagonal v00-v11 with A)
            idx.push_back(v00); idx.push_back(v11); idx.push_back(v01);
        }
    }

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT,
        idx.size(), Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    ibuf->writeData(0, idx.size() * sizeof(uint16_t), idx.data());

    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = idx.size();
    sub->indexData->indexStart = 0;

    mesh->_setBounds(Ogre::AxisAlignedBox(
        0, 0, -0.1f,
        static_cast<float>(cells), static_cast<float>(cells), 0.1f));
    mesh->_setBoundingSphereRadius(static_cast<float>(cells) * 1.5f);
    mesh->load();

    auto* manager = Manager::getSingleton();
    auto* node = manager->addSceneNode(nodeName.c_str());
    if (!node) return nullptr;
    return manager->createEntity(node, mesh);
}

class QuadRetopoEntityCoverageTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override
    {
        SelectionSet::kill();
        Manager::kill();
        QThread::msleep(kSingletonSettleTimeMs);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
    }

    void TearDown() override
    {
        if (Manager::getSingletonPtr()) {
            SelectionSet::getSingleton()->clear();
        }
        SelectionSet::kill();
        Manager::kill();
        if (app) {
            app->processEvents();
        }
        QThread::msleep(kSingletonSettleTimeMs);
    }
};

} // namespace

// ─── Error path: null entity (lines 442-445) ────────────────────────────────
TEST_F(QuadRetopoEntityCoverageTest, NullEntityReturnsErrorReport)
{
    QuadRetopoOptions opts;
    auto report = QuadRetopo::retopologize(
        static_cast<Ogre::Entity*>(nullptr), opts,
        QuadRetopo::Algorithm::TrianglePair);

    EXPECT_FALSE(report.applied);
    EXPECT_FALSE(report.error.isEmpty());
    EXPECT_TRUE(report.error.contains("null entity"));
    EXPECT_TRUE(report.submeshes.isEmpty());
}

// ─── Error path: unsupported algorithm (lines 438-441) ───────────────────────
// The enum currently has a single value (TrianglePair). Cast an
// out-of-range integer to drive the `algo != Algorithm::TrianglePair`
// early-return branch without depending on a second enumerator existing.
TEST_F(QuadRetopoEntityCoverageTest, UnsupportedAlgorithmReturnsErrorReport)
{
    auto* entity = createGridEntity(
        "QuadRetopoCovUnsupAlgoMesh", "QuadRetopoCovUnsupAlgoNode", 2);
    ASSERT_NE(entity, nullptr);

    QuadRetopoOptions opts;
    const auto bogusAlgo = static_cast<QuadRetopo::Algorithm>(99);
    auto report = QuadRetopo::retopologize(entity, opts, bogusAlgo);

    EXPECT_FALSE(report.applied);
    EXPECT_FALSE(report.error.isEmpty());
    EXPECT_TRUE(report.error.contains("TrianglePair"));
}

// ─── Happy path: coplanar grid pairs into quads (lines 447-494) ──────────────
TEST_F(QuadRetopoEntityCoverageTest, CoplanarGridProducesQuads)
{
    // 3x3 cells → 18 triangles, all coplanar (z=0).
    const int cells = 3;
    const int expectedTris = cells * cells * 2; // 18
    auto* entity = createGridEntity(
        "QuadRetopoCovGridMesh", "QuadRetopoCovGridNode", cells);
    ASSERT_NE(entity, nullptr);

    QuadRetopoOptions opts;
    opts.maxAngleDeg = 90.0;        // coplanar -> trivially passes
    opts.shapeToleranceDeg = 90.0;  // accept any quad shape
    opts.maxAspectRatio = 10.0;     // permissive aspect

    auto report = QuadRetopo::retopologize(
        entity, opts, QuadRetopo::Algorithm::TrianglePair);

    EXPECT_TRUE(report.applied);
    EXPECT_TRUE(report.error.isEmpty());
    EXPECT_EQ(report.totalTrianglesBefore, expectedTris);
    EXPECT_GT(report.totalQuadsAfter, 0);
    // Each quad replaces two triangles, so faces < triangles-before.
    EXPECT_LT(report.totalFacesAfter, expectedTris);
    // facesAfter == quadsAfter + trianglesAfterRetopo.
    EXPECT_EQ(report.totalFacesAfter,
              report.totalQuadsAfter + report.totalTrianglesAfterRetopo);
    // Mesh name flows through from the live mesh.
    EXPECT_FALSE(report.meshName.isEmpty());

    // Submesh list populated with one entry for the single submesh.
    ASSERT_EQ(report.submeshes.size(), 1);
    const auto& s = report.submeshes.first();
    EXPECT_EQ(s.submeshIndex, 0);
    EXPECT_EQ(s.trianglesBefore, expectedTris);
    EXPECT_EQ(s.facesAfter, s.quadsAfter + s.trianglesAfter);
    EXPECT_GT(s.quadsAfter, 0);

    // Quad dominance > 0 since at least one pair merged.
    EXPECT_GT(report.quadDominance(), 0.0);

    // JSON projection of the entity-backed report works end-to-end.
    auto json = QuadRetopo::reportToJson(report);
    EXPECT_TRUE(json["applied"].toBool());
    EXPECT_EQ(json["totalTrianglesBefore"].toInt(), expectedTris);
    EXPECT_EQ(json["totalQuadsAfter"].toInt(), report.totalQuadsAfter);
    EXPECT_EQ(json["submeshes"].toArray().size(), 1);
}

// ─── targetFaces global-budget conversion (lines 464-469, 268-292) ───────────
// A positive total targetFaces is converted into a global reduction
// budget; retopologizeSubmesh consumes from it. With targetFaces equal
// to the triangle count, no reduction budget remains, so no quads form.
TEST_F(QuadRetopoEntityCoverageTest, TargetFacesEqualToTrianglesProducesNoQuads)
{
    const int cells = 2;
    const int expectedTris = cells * cells * 2; // 8
    auto* entity = createGridEntity(
        "QuadRetopoCovTargetNoneMesh", "QuadRetopoCovTargetNoneNode", cells);
    ASSERT_NE(entity, nullptr);

    QuadRetopoOptions opts;
    opts.maxAngleDeg = 90.0;
    opts.shapeToleranceDeg = 90.0;
    opts.maxAspectRatio = 10.0;
    opts.targetFaces = expectedTris; // zero reduction budget

    auto report = QuadRetopo::retopologize(
        entity, opts, QuadRetopo::Algorithm::TrianglePair);

    EXPECT_TRUE(report.applied);
    EXPECT_EQ(report.totalTrianglesBefore, expectedTris);
    EXPECT_EQ(report.totalQuadsAfter, 0);
    EXPECT_EQ(report.totalFacesAfter, expectedTris);
    EXPECT_EQ(report.totalTrianglesAfterRetopo, expectedTris);
}

// ─── targetFaces partial budget exercises the decrement path (288-292) ───────
// A targetFaces below the triangle count leaves a positive reduction
// budget; retopologizeSubmesh computes the per-submesh floor/desired
// face math (lines 268-277) and decrements the consumed budget.
TEST_F(QuadRetopoEntityCoverageTest, PartialTargetFacesReducesTowardBudget)
{
    const int cells = 3;
    const int expectedTris = cells * cells * 2; // 18
    auto* entity = createGridEntity(
        "QuadRetopoCovTargetPartialMesh", "QuadRetopoCovTargetPartialNode", cells);
    ASSERT_NE(entity, nullptr);

    QuadRetopoOptions opts;
    opts.maxAngleDeg = 90.0;
    opts.shapeToleranceDeg = 90.0;
    opts.maxAspectRatio = 10.0;
    opts.targetFaces = expectedTris - 4; // allow up to 4 pair ops

    auto report = QuadRetopo::retopologize(
        entity, opts, QuadRetopo::Algorithm::TrianglePair);

    EXPECT_TRUE(report.applied);
    EXPECT_EQ(report.totalTrianglesBefore, expectedTris);
    // Some merging happened, but it was bounded by the budget.
    EXPECT_GT(report.totalQuadsAfter, 0);
    EXPECT_LE(report.totalFacesAfter, expectedTris);
    // Never reduces below the theoretical floor (every tri paired).
    EXPECT_GE(report.totalFacesAfter, (expectedTris + 1) / 2);
    // Budget caps total reduction at 4 pair ops.
    EXPECT_GE(report.totalFacesAfter, opts.targetFaces);
}

// ─── Single coplanar quad: one pair, deterministic outcome ───────────────────
TEST_F(QuadRetopoEntityCoverageTest, SingleCellMergesToExactlyOneQuad)
{
    auto* entity = createGridEntity(
        "QuadRetopoCovSingleMesh", "QuadRetopoCovSingleNode", 1); // 2 tris
    ASSERT_NE(entity, nullptr);

    QuadRetopoOptions opts;
    opts.maxAngleDeg = 90.0;
    opts.shapeToleranceDeg = 90.0;
    opts.maxAspectRatio = 10.0;

    auto report = QuadRetopo::retopologize(
        entity, opts, QuadRetopo::Algorithm::TrianglePair);

    EXPECT_TRUE(report.applied);
    EXPECT_EQ(report.totalTrianglesBefore, 2);
    EXPECT_EQ(report.totalQuadsAfter, 1);
    EXPECT_EQ(report.totalTrianglesAfterRetopo, 0);
    EXPECT_EQ(report.totalFacesAfter, 1);
    EXPECT_NEAR(report.quadDominance(), 1.0, 1e-6);
}

// ─── Default options on the entity path (no relaxed gates) ───────────────────
// Drives the entity path with default QuadRetopoOptions{}; the grid is
// perfectly coplanar so the default 25° angle gate passes and default
// shape/aspect gates accept the square cells.
TEST_F(QuadRetopoEntityCoverageTest, DefaultOptionsStillMergeCoplanarSquares)
{
    auto* entity = createGridEntity(
        "QuadRetopoCovDefaultMesh", "QuadRetopoCovDefaultNode", 2); // 8 tris
    ASSERT_NE(entity, nullptr);

    QuadRetopoOptions opts; // all defaults
    auto report = QuadRetopo::retopologize(entity, opts);

    EXPECT_TRUE(report.applied);
    EXPECT_EQ(report.totalTrianglesBefore, 8);
    EXPECT_GT(report.totalQuadsAfter, 0);

    // Text projection of an entity-backed report works.
    QString text = QuadRetopo::reportToText(report);
    EXPECT_TRUE(text.contains("Quad Retopology"));
    EXPECT_TRUE(text.contains("Triangles in:"));
}
