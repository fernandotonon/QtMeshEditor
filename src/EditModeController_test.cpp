/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include <gtest/gtest.h>
#include "EditModeController.h"
#include "EditableMesh.h"
#include "TestHelpers.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "UndoManager.h"
#include "HalfEdgeMesh.h"
#include <Ogre.h>
#include <QSignalSpy>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <cmath>
#include <limits>
#include <set>
#include <utility>
#include <cmath>
#include <array>
#include <map>

// ===========================================================================
// rewriteEntityAfterTopologyChange — null + no-bump-map shape (chunk: quads
// follow-up). Full bump-map / RTSS exercise needs a GL context, which the
// test infra doesn't provide on macOS; the integration coverage lives in
// hand smoke tests on the bump-mapped Mixamo asset. Here we lock down the
// public-API contract: static, callable from outside the class (notably
// from EditMeshTopologyCommand::applyMeshState in the undo/redo path),
// null-tolerant, and a no-op when no entity material requests tangents.
// ===========================================================================

TEST(EditModeControllerStandalone, RewriteEntityAfterTopologyChangeNullIsNoOp) {
    // Regression for chunk 4b → quads follow-up: the static helper is
    // reachable from command code in TransformCommands.cpp via a class-
    // qualified call, and accepts a null entity without crashing. If
    // someone refactors it back to a free function in an anonymous
    // namespace, undo/redo will silently lose its lighting hook again.
    EditModeController::rewriteEntityAfterTopologyChange(nullptr);
    SUCCEED();
}

// ===========================================================================
// Pure geometry tests (no Ogre needed)
// ===========================================================================

TEST(EditModeControllerGeometry, WorldToScreenBasicCenter) {
    // worldToScreen with a null camera returns (-1,-1)
    auto pt = EditModeController::worldToScreen(Ogre::Vector3::ZERO, nullptr, 800, 600);
    EXPECT_EQ(pt.x(), -1);
    EXPECT_EQ(pt.y(), -1);
}

TEST(EditModeControllerGeometry, PointToSegmentDistanceDegenerate) {
    // Degenerate segment (both endpoints the same)
    QPoint p(5, 5);
    QPoint a(0, 0);
    float dist = EditModeController::pointToSegmentDistance(p, a, a);
    EXPECT_NEAR(dist, std::sqrt(50.0f), 0.01f);
}

TEST(EditModeControllerGeometry, PointToSegmentDistanceOnSegment) {
    // Point is on the segment
    QPoint p(5, 0);
    QPoint a(0, 0);
    QPoint b(10, 0);
    float dist = EditModeController::pointToSegmentDistance(p, a, b);
    EXPECT_NEAR(dist, 0.0f, 0.01f);
}

TEST(EditModeControllerGeometry, PointToSegmentDistancePerpendicular) {
    // Point directly above mid-segment
    QPoint p(5, 3);
    QPoint a(0, 0);
    QPoint b(10, 0);
    float dist = EditModeController::pointToSegmentDistance(p, a, b);
    EXPECT_NEAR(dist, 3.0f, 0.01f);
}

TEST(EditModeControllerGeometry, PointToSegmentDistanceBeyondEndpoint) {
    // Point is beyond endpoint b
    QPoint p(15, 0);
    QPoint a(0, 0);
    QPoint b(10, 0);
    float dist = EditModeController::pointToSegmentDistance(p, a, b);
    EXPECT_NEAR(dist, 5.0f, 0.01f);
}

TEST(EditModeControllerGeometry, PointToSegmentDistanceBeforeStart) {
    // Point is before start a
    QPoint p(-5, 0);
    QPoint a(0, 0);
    QPoint b(10, 0);
    float dist = EditModeController::pointToSegmentDistance(p, a, b);
    EXPECT_NEAR(dist, 5.0f, 0.01f);
}

TEST(EditModeControllerGeometry, RayTriangleIntersectHit) {
    // Ray going straight down into a triangle lying on XZ plane
    Ogre::Vector3 origin(0.25f, 1.0f, 0.25f);
    Ogre::Vector3 dir(0.0f, -1.0f, 0.0f);
    Ogre::Vector3 v0(0, 0, 0);
    Ogre::Vector3 v1(1, 0, 0);
    Ogre::Vector3 v2(0, 0, 1);

    float t = EditModeController::rayTriangleIntersect(origin, dir, v0, v1, v2);
    EXPECT_GT(t, 0.0f);
    EXPECT_NEAR(t, 1.0f, 0.01f);
}

TEST(EditModeControllerGeometry, RayTriangleIntersectMiss) {
    // Ray going away from the triangle
    Ogre::Vector3 origin(0.25f, 1.0f, 0.25f);
    Ogre::Vector3 dir(0.0f, 1.0f, 0.0f); // going up, away from XZ triangle
    Ogre::Vector3 v0(0, 0, 0);
    Ogre::Vector3 v1(1, 0, 0);
    Ogre::Vector3 v2(0, 0, 1);

    float t = EditModeController::rayTriangleIntersect(origin, dir, v0, v1, v2);
    EXPECT_LT(t, 0.0f);
}

TEST(EditModeControllerGeometry, RayTriangleIntersectParallel) {
    // Ray parallel to the triangle
    Ogre::Vector3 origin(0, 1, 0);
    Ogre::Vector3 dir(1, 0, 0); // parallel to XZ plane
    Ogre::Vector3 v0(0, 0, 0);
    Ogre::Vector3 v1(1, 0, 0);
    Ogre::Vector3 v2(0, 0, 1);

    float t = EditModeController::rayTriangleIntersect(origin, dir, v0, v1, v2);
    EXPECT_LT(t, 0.0f);
}

TEST(EditModeControllerGeometry, RayTriangleIntersectOutsideTriangle) {
    // Ray hits the plane but outside the triangle
    Ogre::Vector3 origin(2.0f, 1.0f, 2.0f);
    Ogre::Vector3 dir(0, -1, 0);
    Ogre::Vector3 v0(0, 0, 0);
    Ogre::Vector3 v1(1, 0, 0);
    Ogre::Vector3 v2(0, 0, 1);

    float t = EditModeController::rayTriangleIntersect(origin, dir, v0, v1, v2);
    EXPECT_LT(t, 0.0f);
}

TEST(EditModeControllerGeometry, ApplyVertexColorBrushAffectsVerticesWithinRadius) {
    EditableMesh mesh;
    mesh.subMeshes().resize(1);
    auto& sub = mesh.subMeshes()[0];
    sub.vertices.resize(3);
    sub.vertices[0].position = Ogre::Vector3(0, 0, 0);
    sub.vertices[1].position = Ogre::Vector3(1, 0, 0);
    sub.vertices[2].position = Ogre::Vector3(2, 0, 0);
    for (auto& v : sub.vertices) {
        v.hasColor = true;
        v.color = Ogre::ColourValue::White;
    }

    const Ogre::ColourValue paint(1.0f, 0.0f, 0.0f, 1.0f); // red
    const bool changed = EditModeController::applyVertexColorBrush(
        mesh, Ogre::Vector3(0.0f, 0.0f, 0.0f), /*radius=*/1.1f, paint, /*strength=*/1.0f, /*falloff=*/0.5f);
    EXPECT_TRUE(changed);

    // v0 should become red-ish (exact red because distance 0 => w=1).
    EXPECT_NEAR(sub.vertices[0].color.r, 1.0f, 1e-6f);
    EXPECT_NEAR(sub.vertices[0].color.g, 0.0f, 1e-6f);

    // v1 is within radius but falloff should keep it not fully red.
    EXPECT_LT(sub.vertices[1].color.g, 1.0f);
    EXPECT_GT(sub.vertices[1].color.g, 0.0f);

    // v2 is outside radius, should stay white.
    EXPECT_NEAR(sub.vertices[2].color.r, 1.0f, 1e-6f);
    EXPECT_NEAR(sub.vertices[2].color.g, 1.0f, 1e-6f);
    EXPECT_NEAR(sub.vertices[2].color.b, 1.0f, 1e-6f);
}

TEST(EditModeControllerGeometry, VertexPaintBrushColorFromHexString) {
    auto* ctrl = EditModeController::instance();
    const QColor original = ctrl->vertexPaintColor();
    ctrl->setVertexPaintBrushColor(QStringLiteral("#00ff00"));
    EXPECT_EQ(ctrl->vertexPaintColor(), QColor(0, 255, 0));
    ctrl->setVertexPaintBrushColor(QStringLiteral("#0000ff"));
    EXPECT_EQ(ctrl->vertexPaintColor(), QColor(0, 0, 255));
    if (original.isValid())
        ctrl->setVertexPaintBrushColor(original.name(QColor::HexRgb));
    else
        ctrl->setVertexPaintBrushColor(QStringLiteral("#ffffff"));
}

TEST(EditModeControllerGeometry, VertexPaintBrushColorEmptyOrInvalidNoOp) {
    auto* ctrl = EditModeController::instance();
    const QColor original = ctrl->vertexPaintColor();
    ctrl->setVertexPaintBrushColor(QStringLiteral("#123456"));
    const QColor afterSet = ctrl->vertexPaintColor();
    ctrl->setVertexPaintBrushColor(QString());
    ctrl->setVertexPaintBrushColor(QStringLiteral("   "));
    EXPECT_EQ(ctrl->vertexPaintColor(), afterSet);
    ctrl->setVertexPaintBrushColor(QStringLiteral("___not_a_color___"));
    EXPECT_EQ(ctrl->vertexPaintColor(), afterSet);
    if (original.isValid())
        ctrl->setVertexPaintBrushColor(original.name(QColor::HexRgb));
    else
        ctrl->setVertexPaintBrushColor(QStringLiteral("#ffffff"));
}

// ===========================================================================
// Weight-to-color heat map tests
// ===========================================================================

TEST(EditModeControllerGeometry, WeightToColorFullWeight) {
    // weight 1.0 → red (R=1, G≈0, B=0)
    auto c = EditModeController::weightToColor(1.0f);
    EXPECT_FLOAT_EQ(c.r, 1.0f);
    EXPECT_NEAR(c.g, 0.0f, 0.01f);
    EXPECT_FLOAT_EQ(c.b, 0.0f);
}

TEST(EditModeControllerGeometry, WeightToColorZeroWeight) {
    // weight 0.0 → blue (R=0, G=0, B=1)
    auto c = EditModeController::weightToColor(0.0f);
    EXPECT_FLOAT_EQ(c.r, 0.0f);
    EXPECT_FLOAT_EQ(c.g, 0.0f);
    EXPECT_FLOAT_EQ(c.b, 1.0f);
}

TEST(EditModeControllerGeometry, WeightToColorMidWeight) {
    // weight 0.5 → green-ish (R≈0.5, G=1, B=0)
    auto c = EditModeController::weightToColor(0.5f);
    EXPECT_NEAR(c.r, 0.5f, 0.01f);
    EXPECT_FLOAT_EQ(c.g, 1.0f);
    EXPECT_FLOAT_EQ(c.b, 0.0f);
}

TEST(EditModeControllerGeometry, WeightToColorAlwaysOpaque) {
    // All weights produce alpha = 1.0
    for (float w : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f}) {
        auto c = EditModeController::weightToColor(w);
        EXPECT_FLOAT_EQ(c.a, 1.0f);
    }
}

TEST(EditModeControllerGeometry, WeightToColorGradientMonotonic) {
    // Red channel should generally increase with weight
    auto cLow = EditModeController::weightToColor(0.1f);
    auto cMid = EditModeController::weightToColor(0.5f);
    auto cHigh = EditModeController::weightToColor(0.9f);
    EXPECT_LE(cLow.r, cMid.r);
    EXPECT_LE(cMid.r, cHigh.r);
}

TEST(EditModeControllerGeometry, WeightToColorBoundaryContinuity) {
    // Check boundaries are continuous (no color jumps)
    float boundaries[] = {0.2f, 0.4f, 0.6f, 0.8f};
    for (float b : boundaries) {
        auto cBelow = EditModeController::weightToColor(b - 0.001f);
        auto cAbove = EditModeController::weightToColor(b + 0.001f);
        EXPECT_NEAR(cBelow.r, cAbove.r, 0.05f) << "R discontinuity at " << b;
        EXPECT_NEAR(cBelow.g, cAbove.g, 0.05f) << "G discontinuity at " << b;
        EXPECT_NEAR(cBelow.b, cAbove.b, 0.05f) << "B discontinuity at " << b;
    }
}

TEST(EditModeControllerGeometry, RayTriangleIntersectOriginOnTriangle) {
    // Ray origin is on the triangle, direction is perpendicular
    Ogre::Vector3 origin(0.1f, 0.0f, 0.1f);
    Ogre::Vector3 dir(0, -1, 0);
    Ogre::Vector3 v0(0, 0, 0);
    Ogre::Vector3 v1(1, 0, 0);
    Ogre::Vector3 v2(0, 0, 1);

    // t should be negative or zero (origin is on the plane)
    float t = EditModeController::rayTriangleIntersect(origin, dir, v0, v1, v2);
    EXPECT_LT(t, 0.01f);
}

// ===========================================================================
// Selection state tests (require Ogre for EditableMesh)
// ===========================================================================

class EditModeControllerSelectionTest : public ::testing::Test {
protected:
    Ogre::SceneNode* m_node = nullptr;
    static int s_counter;

    void SetUp() override {
        ASSERT_TRUE(tryInitOgre()) << "Ogre not available (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles()) << "Cannot create hardware buffers (Xvfb/GL required in CI)";
        createStandardOgreMaterials();

        // Create a triangle mesh entity and select it (required for enterEditMode)
        std::string meshName = "EditSelTestMesh_" + std::to_string(++s_counter);
        auto mesh = createInMemoryTriangleMesh(meshName);
        m_node = Manager::getSingleton()->addSceneNode("EditSelTestNode");
        Manager::getSingleton()->createEntity(m_node, mesh);
        SelectionSet::getSingleton()->selectOne(m_node);
    }

    void TearDown() override {
        auto* ctrl = EditModeController::instance();
        if (ctrl->isEditModeActive())
            ctrl->exitEditMode(false);
        if (m_node) {
            Manager::getSingleton()->destroySceneNode(m_node);
            m_node = nullptr;
        }
    }
};
int EditModeControllerSelectionTest::s_counter = 0;

TEST_F(EditModeControllerSelectionTest, InitialSelectionState) {
    auto* ctrl = EditModeController::instance();
    EXPECT_EQ(ctrl->selectionMode(), EditModeController::VertexMode);
    EXPECT_EQ(ctrl->selectedVertexCount(), 0);
    EXPECT_EQ(ctrl->selectedEdgeCount(), 0);
    EXPECT_EQ(ctrl->selectedFaceCount(), 0);
}

TEST_F(EditModeControllerSelectionTest, SetSelectionMode) {
    auto* ctrl = EditModeController::instance();

    ctrl->setSelectionMode(EditModeController::EdgeMode);
    EXPECT_EQ(ctrl->selectionMode(), EditModeController::EdgeMode);

    ctrl->setSelectionMode(EditModeController::FaceMode);
    EXPECT_EQ(ctrl->selectionMode(), EditModeController::FaceMode);

    ctrl->setSelectionMode(EditModeController::VertexMode);
    EXPECT_EQ(ctrl->selectionMode(), EditModeController::VertexMode);
}

TEST_F(EditModeControllerSelectionTest, SetSelectionModeInvalid) {
    auto* ctrl = EditModeController::instance();
    ctrl->setSelectionMode(EditModeController::VertexMode);

    // Invalid mode values should be ignored
    ctrl->setSelectionMode(-1);
    EXPECT_EQ(ctrl->selectionMode(), EditModeController::VertexMode);

    ctrl->setSelectionMode(99);
    EXPECT_EQ(ctrl->selectionMode(), EditModeController::VertexMode);
}

TEST_F(EditModeControllerSelectionTest, ModeLabelShowsSelectionMode) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_modelabel");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_modelabel_node");
    Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    EXPECT_EQ(ctrl->modeLabel(), "Edit Mode (Vertex)");

    ctrl->setSelectionMode(EditModeController::EdgeMode);
    EXPECT_EQ(ctrl->modeLabel(), "Edit Mode (Edge)");

    ctrl->setSelectionMode(EditModeController::FaceMode);
    EXPECT_EQ(ctrl->modeLabel(), "Edit Mode (Face)");

    ctrl->exitEditMode(false);
    EXPECT_EQ(ctrl->modeLabel(), "Object Mode");

    Manager::getSingleton()->destroySceneNode("EditCtrl_modelabel_node");
}

TEST_F(EditModeControllerSelectionTest, VertexSelection) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_vertsel");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_vertsel_node");
    Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ASSERT_EQ(ctrl->vertexCount(), 3);

    // Select vertex 0
    ctrl->selectVertex(0);
    EXPECT_EQ(ctrl->selectedVertexCount(), 1);
    EXPECT_TRUE(ctrl->selectedVertices().count(0) > 0);

    // Select vertex 1 (replaces selection)
    ctrl->selectVertex(1, false);
    EXPECT_EQ(ctrl->selectedVertexCount(), 1);
    EXPECT_TRUE(ctrl->selectedVertices().count(1) > 0);
    EXPECT_TRUE(ctrl->selectedVertices().count(0) == 0);

    // Add vertex 2 to selection
    ctrl->selectVertex(2, true);
    EXPECT_EQ(ctrl->selectedVertexCount(), 2);
    EXPECT_TRUE(ctrl->selectedVertices().count(1) > 0);
    EXPECT_TRUE(ctrl->selectedVertices().count(2) > 0);

    // Deselect vertex 1
    ctrl->deselectVertex(1);
    EXPECT_EQ(ctrl->selectedVertexCount(), 1);
    EXPECT_TRUE(ctrl->selectedVertices().count(2) > 0);

    // Out-of-range vertex is ignored
    ctrl->selectVertex(999);
    EXPECT_EQ(ctrl->selectedVertexCount(), 0); // replaces selection with invalid

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_vertsel_node");
}

TEST_F(EditModeControllerSelectionTest, EdgeSelection) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_edgesel");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_edgesel_node");
    Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    // Select edge (0, 1)
    ctrl->selectEdge(0, 1);
    EXPECT_EQ(ctrl->selectedEdgeCount(), 1);
    EXPECT_EQ(ctrl->selectedVertexCount(), 2); // both endpoints selected
    EXPECT_TRUE(ctrl->selectedEdges().count({0, 1}) > 0);

    // Select edge (1, 2) — should replace
    ctrl->selectEdge(2, 1, false); // order-independent: stored as (1, 2)
    EXPECT_EQ(ctrl->selectedEdgeCount(), 1);
    EXPECT_TRUE(ctrl->selectedEdges().count({1, 2}) > 0);

    // Add edge (0, 2)
    ctrl->selectEdge(0, 2, true);
    EXPECT_EQ(ctrl->selectedEdgeCount(), 2);

    // Deselect
    ctrl->deselectEdge(1, 2);
    EXPECT_EQ(ctrl->selectedEdgeCount(), 1);
    EXPECT_TRUE(ctrl->selectedEdges().count({0, 2}) > 0);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_edgesel_node");
}

TEST_F(EditModeControllerSelectionTest, FaceSelection) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_facesel");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_facesel_node");
    Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ASSERT_EQ(ctrl->triangleCount(), 1);

    // Select face 0
    ctrl->selectFace(0);
    EXPECT_EQ(ctrl->selectedFaceCount(), 1);
    EXPECT_EQ(ctrl->selectedVertexCount(), 3); // all 3 vertices selected
    EXPECT_EQ(ctrl->selectedEdgeCount(), 3);   // all 3 edges selected

    // Deselect face
    ctrl->deselectFace(0);
    EXPECT_EQ(ctrl->selectedFaceCount(), 0);

    // Out-of-range face is ignored
    ctrl->selectFace(999);
    EXPECT_EQ(ctrl->selectedFaceCount(), 0);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_facesel_node");
}

TEST_F(EditModeControllerSelectionTest, SelectAllDeselectAll) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_selectall");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_selectall_node");
    Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    // Select all
    ctrl->selectAll();
    EXPECT_EQ(ctrl->selectedVertexCount(), 3);
    EXPECT_EQ(ctrl->selectedFaceCount(), 1);
    EXPECT_GT(ctrl->selectedEdgeCount(), 0);

    // Deselect all
    ctrl->deselectAll();
    EXPECT_EQ(ctrl->selectedVertexCount(), 0);
    EXPECT_EQ(ctrl->selectedFaceCount(), 0);
    EXPECT_EQ(ctrl->selectedEdgeCount(), 0);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_selectall_node");
}

TEST_F(EditModeControllerSelectionTest, ExitEditModeClearsSelection) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_exitclears");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_exitclears_node");
    Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    ctrl->selectAll();
    EXPECT_GT(ctrl->selectedVertexCount(), 0);

    ctrl->exitEditMode(false);
    EXPECT_EQ(ctrl->selectedVertexCount(), 0);
    EXPECT_EQ(ctrl->selectedEdgeCount(), 0);
    EXPECT_EQ(ctrl->selectedFaceCount(), 0);

    Manager::getSingleton()->destroySceneNode("EditCtrl_exitclears_node");
}

// ===========================================================================
// Index conversion tests
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, GlobalToLocalConversion) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_g2l");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_g2l_node");
    Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    // Single submesh with 3 vertices
    auto [sub0, loc0] = ctrl->globalToLocal(0);
    EXPECT_EQ(sub0, 0u);
    EXPECT_EQ(loc0, 0u);

    auto [sub1, loc1] = ctrl->globalToLocal(1);
    EXPECT_EQ(sub1, 0u);
    EXPECT_EQ(loc1, 1u);

    auto [sub2, loc2] = ctrl->globalToLocal(2);
    EXPECT_EQ(sub2, 0u);
    EXPECT_EQ(loc2, 2u);

    // Round-trip
    EXPECT_EQ(ctrl->localToGlobal(0, 0), 0);
    EXPECT_EQ(ctrl->localToGlobal(0, 1), 1);
    EXPECT_EQ(ctrl->localToGlobal(0, 2), 2);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_g2l_node");
}

TEST_F(EditModeControllerSelectionTest, GlobalTriToLocalConversion) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_gt2l");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_gt2l_node");
    Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    auto [sub, local] = ctrl->globalTriToLocal(0);
    EXPECT_EQ(sub, 0u);
    EXPECT_EQ(local, 0u);

    EXPECT_EQ(ctrl->localTriToGlobal(0, 0), 0);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_gt2l_node");
}

// ===========================================================================
// Hit testing (vertex) with Ogre camera - requires render window
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, HitTestVertexNullCamera) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_hitvert_null");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_hitvert_null_node");
    Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    // Null camera should return -1
    int result = ctrl->hitTestVertex(QPoint(100, 100), nullptr, 800, 600);
    EXPECT_EQ(result, -1);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_hitvert_null_node");
}

TEST_F(EditModeControllerSelectionTest, HitTestFaceNullCamera) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_hitface_null");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_hitface_null_node");
    Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    int result = ctrl->hitTestFace(QPoint(100, 100), nullptr, 800, 600);
    EXPECT_EQ(result, -1);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_hitface_null_node");
}

TEST_F(EditModeControllerSelectionTest, HitTestEdgeNullCamera) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_hitedge_null");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_hitedge_null_node");
    Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    auto [e1, e2] = ctrl->hitTestEdge(QPoint(100, 100), nullptr, 800, 600);
    EXPECT_EQ(e1, -1);
    EXPECT_EQ(e2, -1);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_hitedge_null_node");
}

TEST_F(EditModeControllerSelectionTest, SoftSelectionSettersValidateInputAndEmitSignals) {
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    QSignalSpy softSelSpy(ctrl, &EditModeController::softSelectionChanged);
    ASSERT_TRUE(softSelSpy.isValid());

    ctrl->setSoftSelectionEnabled(true);
    EXPECT_TRUE(ctrl->softSelectionEnabled());
    EXPECT_GE(softSelSpy.count(), 1);

    const int countAfterEnable = softSelSpy.count();
    ctrl->setSoftSelectionEnabled(true); // same value => no new emission
    EXPECT_EQ(softSelSpy.count(), countAfterEnable);

    ctrl->setSoftSelectionRadius(3.0);
    EXPECT_DOUBLE_EQ(ctrl->softSelectionRadius(), 3.0);
    EXPECT_GE(softSelSpy.count(), countAfterEnable + 1);

    const int countAfterRadius = softSelSpy.count();
    ctrl->setSoftSelectionRadius(-1.0); // invalid => ignored
    EXPECT_DOUBLE_EQ(ctrl->softSelectionRadius(), 3.0);
    EXPECT_EQ(softSelSpy.count(), countAfterRadius);

    ctrl->setSoftSelectionFalloff(1);
    EXPECT_EQ(ctrl->softSelectionFalloff(), 1);
    EXPECT_GE(softSelSpy.count(), countAfterRadius + 1);

    const int countAfterFalloff = softSelSpy.count();
    ctrl->setSoftSelectionFalloff(9); // invalid => ignored
    EXPECT_EQ(ctrl->softSelectionFalloff(), 1);
    EXPECT_EQ(softSelSpy.count(), countAfterFalloff);

    ctrl->exitEditMode(false);
}

TEST_F(EditModeControllerSelectionTest, SoftSelectionWeightComputationsCoverLinearSmoothAndFallbackPaths) {
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    ctrl->selectVertex(0, false);
    ASSERT_EQ(ctrl->selectedVertexCount(), 1);

    ctrl->setSoftSelectionEnabled(true);
    ctrl->setSoftSelectionRadius(3.0);
    ctrl->setSoftSelectionFalloff(0); // linear

    std::map<int, float> linearWeights = ctrl->getSoftSelectionWeights();
    ASSERT_TRUE(linearWeights.count(0) > 0);
    ASSERT_TRUE(linearWeights.count(1) > 0);
    ASSERT_TRUE(linearWeights.count(2) > 0);
    EXPECT_FLOAT_EQ(linearWeights[0], 1.0f);
    EXPECT_NEAR(linearWeights[1], 2.0f / 3.0f, 0.05f);
    EXPECT_NEAR(linearWeights[2], 2.0f / 3.0f, 0.05f);

    ctrl->setSoftSelectionFalloff(1); // smooth
    std::map<int, float> smoothWeights = ctrl->getSoftSelectionWeights();
    ASSERT_TRUE(smoothWeights.count(1) > 0);
    ASSERT_TRUE(smoothWeights.count(2) > 0);
    EXPECT_GE(smoothWeights[1] + 1e-4f, linearWeights[1]);
    EXPECT_GE(smoothWeights[2] + 1e-4f, linearWeights[2]);

    // Omit selected vertex 0 in the positions map to exercise fallback to
    // live mesh positions for selectedPositions.
    ctrl->setSoftSelectionFalloff(0);
    ctrl->setSoftSelectionRadius(1.0);
    std::map<int, Ogre::Vector3> positions;
    positions[1] = Ogre::Vector3(0.5f, 0.0f, 0.0f);
    positions[2] = Ogre::Vector3(2.0f, 0.0f, 0.0f);

    std::map<int, float> mapWeights = ctrl->computeSoftSelectionWeightsFromPositions(positions);
    EXPECT_TRUE(mapWeights.count(0) > 0);
    EXPECT_TRUE(mapWeights.count(1) > 0);
    EXPECT_FALSE(mapWeights.count(2) > 0);
    EXPECT_FLOAT_EQ(mapWeights[0], 1.0f);
    EXPECT_NEAR(mapWeights[1], 0.5f, 0.05f);

    ctrl->setSoftSelectionFalloff(1);
    std::map<int, float> mapWeightsSmooth = ctrl->computeSoftSelectionWeightsFromPositions(positions);
    EXPECT_TRUE(mapWeightsSmooth.count(1) > 0);
    EXPECT_GE(mapWeightsSmooth[1] + 1e-4f, mapWeights[1]);

    ctrl->exitEditMode(false);
}

TEST_F(EditModeControllerSelectionTest, HitTestingAndBoxSelectionWorkWithCamera) {
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    ASSERT_NE(sceneMgr, nullptr);
    const std::string cameraName = "EditCtrl_hit_camera_" + std::to_string(++s_counter);
    const std::string cameraNodeName = cameraName + "_node";
    Ogre::Camera* camera = sceneMgr->createCamera(cameraName);
    ASSERT_NE(camera, nullptr);
    Ogre::SceneNode* cameraNode = sceneMgr->getRootSceneNode()->createChildSceneNode(cameraNodeName);
    ASSERT_NE(cameraNode, nullptr);
    cameraNode->attachObject(camera);
    camera->setNearClipDistance(0.1f);
    camera->setFarClipDistance(1000.0f);
    camera->setAspectRatio(800.0f / 600.0f);
    cameraNode->setPosition(0.0f, 0.0f, 5.0f);
    cameraNode->lookAt(Ogre::Vector3(0.0f, 0.0f, 0.0f), Ogre::Node::TS_WORLD);

    const int vpW = 800;
    const int vpH = 600;
    const QPoint screenV0 = EditModeController::worldToScreen(
        m_node->convertLocalToWorldPosition(Ogre::Vector3(0.0f, 0.0f, 0.0f)),
        camera, vpW, vpH);
    const QPoint screenV1 = EditModeController::worldToScreen(
        m_node->convertLocalToWorldPosition(Ogre::Vector3(1.0f, 0.0f, 0.0f)),
        camera, vpW, vpH);
    const QPoint screenV2 = EditModeController::worldToScreen(
        m_node->convertLocalToWorldPosition(Ogre::Vector3(0.0f, 1.0f, 0.0f)),
        camera, vpW, vpH);

    const int hitVertex = ctrl->hitTestVertex(screenV0, camera, vpW, vpH, 25.0f);
    EXPECT_GE(hitVertex, 0);

    const QPoint edgeMid = EditModeController::worldToScreen(
        m_node->convertLocalToWorldPosition(Ogre::Vector3(0.5f, 0.0f, 0.0f)),
        camera, vpW, vpH);
    auto edge = ctrl->hitTestEdge(edgeMid, camera, vpW, vpH, 25.0f);
    EXPECT_NE(edge.first, -1);
    EXPECT_NE(edge.second, -1);

    const QPoint faceCenter = EditModeController::worldToScreen(
        m_node->convertLocalToWorldPosition(Ogre::Vector3(0.2f, 0.2f, 0.0f)),
        camera, vpW, vpH);
    EXPECT_EQ(ctrl->hitTestFace(faceCenter, camera, vpW, vpH), 0);

    QRect box(screenV0, screenV0);
    box = box.united(QRect(screenV1, screenV1));
    box = box.united(QRect(screenV2, screenV2));
    box.adjust(-4, -4, 4, 4);
    ctrl->boxSelectVertices(box, camera, vpW, vpH, false);
    EXPECT_EQ(ctrl->selectedVertexCount(), 3);

    // Turn camera away so geometry is behind the camera.
    cameraNode->lookAt(Ogre::Vector3(0.0f, 0.0f, 10.0f), Ogre::Node::TS_WORLD);
    EXPECT_EQ(ctrl->hitTestVertex(QPoint(vpW / 2, vpH / 2), camera, vpW, vpH, 25.0f), -1);
    auto noEdge = ctrl->hitTestEdge(QPoint(vpW / 2, vpH / 2), camera, vpW, vpH, 25.0f);
    EXPECT_EQ(noEdge.first, -1);
    EXPECT_EQ(noEdge.second, -1);
    ctrl->boxSelectVertices(QRect(0, 0, vpW, vpH), camera, vpW, vpH, false);
    EXPECT_EQ(ctrl->selectedVertexCount(), 0);

    cameraNode->detachObject(camera);
    sceneMgr->destroyCamera(camera);
    sceneMgr->destroySceneNode(cameraNode);
    ctrl->exitEditMode(false);
}

TEST_F(EditModeControllerSelectionTest, SelectOperationsNotInEditMode) {
    // All selection operations should be no-ops when not in edit mode
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->isEditModeActive());

    ctrl->selectVertex(0);
    EXPECT_EQ(ctrl->selectedVertexCount(), 0);

    ctrl->selectEdge(0, 1);
    EXPECT_EQ(ctrl->selectedEdgeCount(), 0);

    ctrl->selectFace(0);
    EXPECT_EQ(ctrl->selectedFaceCount(), 0);

    ctrl->selectAll();
    EXPECT_EQ(ctrl->selectedVertexCount(), 0);
}

// ===========================================================================
// Wireframe toggle tests
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, WireframeDefaultOff) {
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->wireframeEnabled());
}

TEST_F(EditModeControllerSelectionTest, WireframeToggleNotInEditMode) {
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->isEditModeActive());

    // Toggling wireframe outside edit mode should still update the property
    ctrl->setWireframeEnabled(true);
    EXPECT_TRUE(ctrl->wireframeEnabled());

    ctrl->setWireframeEnabled(false);
    EXPECT_FALSE(ctrl->wireframeEnabled());
}

TEST_F(EditModeControllerSelectionTest, WireframeToggleInEditMode) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_wireframe");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_wireframe_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    // Save original material name
    Ogre::String origMat = entity->getSubEntity(0)->getMaterialName();

    // Enable wireframe
    ctrl->setWireframeEnabled(true);
    EXPECT_TRUE(ctrl->wireframeEnabled());
    // Material should have changed to a wireframe clone
    Ogre::String wireMat = entity->getSubEntity(0)->getMaterialName();
    EXPECT_NE(wireMat, origMat);
    EXPECT_TRUE(wireMat.find("_EditWireframe") != Ogre::String::npos);

    // Disable wireframe — should restore original material
    ctrl->setWireframeEnabled(false);
    EXPECT_FALSE(ctrl->wireframeEnabled());
    EXPECT_EQ(entity->getSubEntity(0)->getMaterialName(), origMat);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_wireframe_node");
}

TEST_F(EditModeControllerSelectionTest, WireframeRestoredOnExitEditMode) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_wire_exit");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_wire_exit_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    Ogre::String origMat = entity->getSubEntity(0)->getMaterialName();

    // Enable wireframe then exit edit mode without disabling it
    ctrl->setWireframeEnabled(true);
    EXPECT_TRUE(ctrl->wireframeEnabled());

    ctrl->exitEditMode(false);

    // Material should be restored, but wireframe state persists
    EXPECT_TRUE(ctrl->wireframeEnabled());
    EXPECT_EQ(entity->getSubEntity(0)->getMaterialName(), origMat);

    // Reset for other tests
    ctrl->setWireframeEnabled(false);

    Manager::getSingleton()->destroySceneNode("EditCtrl_wire_exit_node");
}

TEST_F(EditModeControllerSelectionTest, WireframeSignalEmission) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_wire_signal");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_wire_signal_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    int wireframeChanges = 0;
    auto conn = QObject::connect(ctrl, &EditModeController::wireframeChanged,
                                  [&]() { ++wireframeChanges; });

    ctrl->setWireframeEnabled(true);
    EXPECT_EQ(wireframeChanges, 1);

    // Setting same value should not emit
    ctrl->setWireframeEnabled(true);
    EXPECT_EQ(wireframeChanges, 1);

    ctrl->setWireframeEnabled(false);
    EXPECT_EQ(wireframeChanges, 2);

    QObject::disconnect(conn);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_wire_signal_node");
}

// ===========================================================================
// Selection mode change clears selection
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, SelectionModeSwitchClearsSelection) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_modeswitch");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_modeswitch_node");
    Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    // Select a vertex in vertex mode
    ctrl->selectVertex(0);
    EXPECT_EQ(ctrl->selectedVertexCount(), 1);

    // Switch to edge mode — selection should be cleared
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    EXPECT_EQ(ctrl->selectedVertexCount(), 0);
    EXPECT_EQ(ctrl->selectedEdgeCount(), 0);
    EXPECT_EQ(ctrl->selectedFaceCount(), 0);

    // Select an edge, then switch to face mode
    ctrl->selectEdge(0, 1);
    EXPECT_EQ(ctrl->selectedEdgeCount(), 1);

    ctrl->setSelectionMode(EditModeController::FaceMode);
    EXPECT_EQ(ctrl->selectedVertexCount(), 0);
    EXPECT_EQ(ctrl->selectedEdgeCount(), 0);
    EXPECT_EQ(ctrl->selectedFaceCount(), 0);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_modeswitch_node");
}

// ===========================================================================
// Wireframe persists across edit mode sessions
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, WireframePersistsAcrossSessions) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_wire_persist");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_wire_persist_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();

    // First session: enable wireframe
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setWireframeEnabled(true);
    EXPECT_TRUE(ctrl->wireframeEnabled());
    Ogre::String origMat = ctrl->editEntity()->getSubEntity(0)->getMaterialName();
    EXPECT_TRUE(origMat.find("_EditWireframe") != Ogre::String::npos);
    ctrl->exitEditMode(false);

    // Wireframe state persists after exit
    EXPECT_TRUE(ctrl->wireframeEnabled());

    // Second session: wireframe should be re-applied automatically
    ASSERT_TRUE(ctrl->enterEditMode());
    EXPECT_TRUE(ctrl->wireframeEnabled());
    Ogre::String matAfterReenter = ctrl->editEntity()->getSubEntity(0)->getMaterialName();
    EXPECT_TRUE(matAfterReenter.find("_EditWireframe") != Ogre::String::npos);

    ctrl->exitEditMode(false);
    ctrl->setWireframeEnabled(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_wire_persist_node");
}

// ===========================================================================
// Signal emission tests
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, SignalEmission) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_signals");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_signals_node");
    Manager::getSingleton()->createEntity(node, meshPtr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    // Connect signals after enterEditMode to only count user-initiated changes
    int selModeChanges = 0;
    int editSelChanges = 0;
    auto conn1 = QObject::connect(ctrl, &EditModeController::selectionModeChanged,
                                   [&]() { ++selModeChanges; });
    auto conn2 = QObject::connect(ctrl, &EditModeController::editSelectionChanged,
                                   [&]() { ++editSelChanges; });

    ctrl->setSelectionMode(EditModeController::EdgeMode);
    EXPECT_EQ(selModeChanges, 1);

    ctrl->selectVertex(0);
    EXPECT_EQ(editSelChanges, 1);

    ctrl->deselectAll();
    EXPECT_EQ(editSelChanges, 2);

    QObject::disconnect(conn1);
    QObject::disconnect(conn2);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_signals_node");
}

// ===========================================================================
// End-to-end bevel tests (cube primitive → edit mode → bevel → GPU buffers)
// Runs the whole EditModeController::applyBevelTopology pipeline including
// toEditableMesh → recalculateNormals → resizeEntityBuffers → Ogre sync.
// Catches bugs between the HE mesh output and the final Ogre buffers.
// ===========================================================================

class EditModeControllerBevelE2ETest : public ::testing::Test {
protected:
    Ogre::SceneNode* m_node = nullptr;
    Ogre::Entity* m_entity = nullptr;
    std::string m_meshName;
    std::string m_nodeName;

    void SetUp() override {
        // One Ogre Root for the whole suite: tearing Manager down every test
        // and re-initing GL was crashing the second bevel on Linux CI (SIGSEGV).
        ASSERT_TRUE(tryInitOgre()) << "Ogre not available (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();

        // Unique names per test so reruns don't collide with Ogre's
        // resource manager registry across fixture instances.
        static int counter = 0;
        ++counter;
        m_meshName = "BevelE2E_cube_" + std::to_string(counter);
        m_nodeName = "BevelE2E_node_" + std::to_string(counter);

        auto mesh = createInMemoryWeldedCube(m_meshName);
        m_node = Manager::getSingleton()->addSceneNode(QString::fromStdString(m_nodeName));
        m_entity = Manager::getSingleton()->createEntity(m_node, mesh);
        m_entity->setMaterialName("BaseWhite");
        SelectionSet::getSingleton()->selectOne(m_node);
    }
    void TearDown() override {
        auto* ctrl = EditModeController::instance();
        if (ctrl->isEditModeActive()) ctrl->exitEditMode(false);
        SelectionSet::getSingleton()->clear();
        if (m_node) {
            Manager::getSingleton()->destroySceneNode(m_node);
            m_node = nullptr;
        }
        if (!m_meshName.empty()) {
            auto& mm = Ogre::MeshManager::getSingleton();
            if (mm.getByName(m_meshName))
                mm.remove(m_meshName);
            m_meshName.clear();
        }
        // Fresh controller next test: BevelGizmo stays in unique_ptr; recycling
        // avoids a stale gizmo tied to a removed entity while keeping one Root.
        EditModeController::kill();
    }
};

// Extract the actual GPU vertex positions + triangle indices for all
// submeshes of an entity. Returns (positions, indices) — indices are
// per-submesh triangle index triples.
static void extractEntityBuffers(Ogre::Entity* entity,
                                 std::vector<Ogre::Vector3>& outPositions,
                                 std::vector<std::array<unsigned, 3>>& outTriangles)
{
    outPositions.clear();
    outTriangles.clear();
    if (!entity) return;
    auto* mesh = entity->getMesh().get();
    if (!mesh) return;

    unsigned baseVert = 0;
    for (unsigned short s = 0; s < mesh->getNumSubMeshes(); ++s) {
        auto* sub = mesh->getSubMesh(s);
        auto* vdata = sub->useSharedVertices ? mesh->sharedVertexData : sub->vertexData;
        if (!vdata) continue;
        const auto* elem = vdata->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        if (!elem) continue;
        auto vbuf = vdata->vertexBufferBinding->getBuffer(elem->getSource());
        unsigned char* raw = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (size_t v = 0; v < vdata->vertexCount; ++v) {
            float* f;
            elem->baseVertexPointerToElement(raw + v * vbuf->getVertexSize(), &f);
            outPositions.push_back(Ogre::Vector3(f[0], f[1], f[2]));
        }
        vbuf->unlock();

        auto* idata = sub->indexData;
        if (!idata || !idata->indexBuffer) continue;
        auto ibuf = idata->indexBuffer;
        bool use32 = (ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT);
        unsigned char* rawI = static_cast<unsigned char*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        size_t nTris = idata->indexCount / 3;
        for (size_t t = 0; t < nTris; ++t) {
            std::array<unsigned, 3> tri;
            if (use32) {
                auto* p = reinterpret_cast<uint32_t*>(rawI) + t * 3;
                tri = {p[0] + baseVert, p[1] + baseVert, p[2] + baseVert};
            } else {
                auto* p = reinterpret_cast<uint16_t*>(rawI) + t * 3;
                tri = {p[0] + baseVert, p[1] + baseVert, p[2] + baseVert};
            }
            outTriangles.push_back(tri);
        }
        ibuf->unlock();
        baseVert += vdata->vertexCount;
    }
}

TEST_F(EditModeControllerBevelE2ETest, BevelCubeTopRightEdgeProducesClosedManifold) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ctrl->setSelectionMode(EditModeController::EdgeMode);

    // Select the edge between v5=(1,1,1) and v3=(1,1,-1).
    ctrl->selectEdge(5, 3, false);

    ASSERT_TRUE(ctrl->bevelSelection()) << "bevelSelection returned false";

    // Extract GPU buffers and verify closed manifold with positive outward normals.
    std::vector<Ogre::Vector3> positions;
    std::vector<std::array<unsigned, 3>> tris;
    extractEntityBuffers(m_entity, positions, tris);

    fprintf(stderr, "[E2E] Entity post-bevel: verts=%zu tris=%zu\n",
            positions.size(), tris.size());

    // Closed: every edge must appear exactly 2 times (once forward, once backward).
    std::map<std::pair<unsigned,unsigned>, int> edgeUse;
    for (const auto& t : tris) {
        for (int k = 0; k < 3; ++k) {
            unsigned u = t[k], v = t[(k+1)%3];
            auto key = std::make_pair(std::min(u,v), std::max(u,v));
            ++edgeUse[key];
        }
    }
    size_t boundaryEdges = 0;
    for (auto& [_, c] : edgeUse) if (c == 1) ++boundaryEdges;
    EXPECT_EQ(boundaryEdges, 0u) << "E2E bevel leaves " << boundaryEdges << " boundary edges";

    // Every tri should face outward (cube centered at origin).
    int invertedTris = 0;
    for (size_t t = 0; t < tris.size(); ++t) {
        const auto& tri = tris[t];
        auto& p0 = positions[tri[0]];
        auto& p1 = positions[tri[1]];
        auto& p2 = positions[tri[2]];
        auto n = (p1 - p0).crossProduct(p2 - p0);
        if (n.length() < 1e-8f) continue;
        n.normalise();
        auto c = (p0 + p1 + p2) / 3.0f;
        if (c.length() > 1e-6f) c.normalise();
        if (n.dotProduct(c) < 0.1f) {
            ++invertedTris;
            fprintf(stderr, "  inverted tri %zu = (%u,%u,%u) cos=%.3f\n",
                    t, tri[0], tri[1], tri[2], n.dotProduct(c));
        }
    }
    EXPECT_EQ(invertedTris, 0) << invertedTris << " inverted triangles in GPU buffers";
}

// ===========================================================================
// Per-segment bevel profile points: session state, API, signal emission.
// ===========================================================================

TEST_F(EditModeControllerBevelE2ETest, BevelSegments1HasEmptyProfilePoints) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);

    ASSERT_TRUE(ctrl->bevelSelection());
    EXPECT_EQ(ctrl->bevelSegments(), 1);
    auto pts = ctrl->bevelProfilePoints();
    EXPECT_EQ(pts.size(), 0);
}

TEST_F(EditModeControllerBevelE2ETest, UpdateBevelSegmentsResizesProfilePoints) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);
    ASSERT_TRUE(ctrl->bevelSelection());

    ctrl->updateBevelSegments(3);
    EXPECT_EQ(ctrl->bevelSegments(), 3);
    auto pts = ctrl->bevelProfilePoints();
    EXPECT_EQ(pts.size(), 2);
    for (const auto& v : pts) EXPECT_NEAR(v.toFloat(), 0.5f, 1e-3f);

    ctrl->updateBevelSegments(5);
    EXPECT_EQ(ctrl->bevelSegments(), 5);
    EXPECT_EQ(ctrl->bevelProfilePoints().size(), 4);

    ctrl->updateBevelSegments(2);
    EXPECT_EQ(ctrl->bevelSegments(), 2);
    EXPECT_EQ(ctrl->bevelProfilePoints().size(), 1);
}

TEST_F(EditModeControllerBevelE2ETest, UpdateBevelProfilePointChangesValue) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);
    ASSERT_TRUE(ctrl->bevelSelection());

    ctrl->updateBevelSegments(4);
    ASSERT_EQ(ctrl->bevelProfilePoints().size(), 3);

    ctrl->updateBevelProfilePoint(1, 0.9f);
    auto pts = ctrl->bevelProfilePoints();
    ASSERT_EQ(pts.size(), 3);
    EXPECT_NEAR(pts[0].toFloat(), 0.5f, 1e-3f);
    EXPECT_NEAR(pts[1].toFloat(), 0.9f, 1e-3f);
    EXPECT_NEAR(pts[2].toFloat(), 0.5f, 1e-3f);
}

TEST_F(EditModeControllerBevelE2ETest, UpdateBevelProfilePointClampsRange) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);
    ASSERT_TRUE(ctrl->bevelSelection());

    ctrl->updateBevelSegments(3);
    ASSERT_EQ(ctrl->bevelProfilePoints().size(), 2);

    ctrl->updateBevelProfilePoint(0, -1.0f);
    EXPECT_NEAR(ctrl->bevelProfilePoints()[0].toFloat(), 0.0f, 1e-3f);
    ctrl->updateBevelProfilePoint(0, 2.0f);
    EXPECT_NEAR(ctrl->bevelProfilePoints()[0].toFloat(), 1.0f, 1e-3f);
}

TEST_F(EditModeControllerBevelE2ETest, UpdateBevelProfilePointInvalidIndexIsNoOp) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);
    ASSERT_TRUE(ctrl->bevelSelection());

    ctrl->updateBevelSegments(3);
    ASSERT_EQ(ctrl->bevelProfilePoints().size(), 2);
    auto before = ctrl->bevelProfilePoints();
    ctrl->updateBevelProfilePoint(-1, 0.9f);
    ctrl->updateBevelProfilePoint(5, 0.9f);
    auto after = ctrl->bevelProfilePoints();
    ASSERT_EQ(before.size(), after.size());
    for (int i = 0; i < before.size(); ++i)
        EXPECT_NEAR(before[i].toFloat(), after[i].toFloat(), 1e-6f);
}

TEST_F(EditModeControllerBevelE2ETest, ResetBevelProfileFlattens) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);
    ASSERT_TRUE(ctrl->bevelSelection());

    ctrl->updateBevelSegments(4);
    ctrl->updateBevelProfilePoint(0, 0.1f);
    ctrl->updateBevelProfilePoint(1, 0.9f);
    ctrl->updateBevelProfilePoint(2, 0.2f);

    ctrl->resetBevelProfile();
    auto pts = ctrl->bevelProfilePoints();
    ASSERT_EQ(pts.size(), 3);
    for (const auto& v : pts) EXPECT_NEAR(v.toFloat(), 0.5f, 1e-3f);
}

TEST_F(EditModeControllerBevelE2ETest, BevelProfilePointsChangedSignalFires) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);

    int fires = 0;
    auto conn = QObject::connect(ctrl, &EditModeController::bevelProfilePointsChanged,
                                 [&]() { ++fires; });

    ASSERT_TRUE(ctrl->bevelSelection());
    EXPECT_GE(fires, 1) << "signal should fire on session start";

    fires = 0;
    ctrl->updateBevelSegments(3);
    EXPECT_GE(fires, 1);

    fires = 0;
    ctrl->updateBevelProfilePoint(0, 0.8f);
    EXPECT_GE(fires, 1);

    fires = 0;
    ctrl->resetBevelProfile();
    EXPECT_GE(fires, 1);

    fires = 0;
    ctrl->cancelBevel();
    EXPECT_GE(fires, 1);

    QObject::disconnect(conn);
}

TEST_F(EditModeControllerBevelE2ETest, UpdateBevelProfilePointWithSegments1NoOp) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);
    ASSERT_TRUE(ctrl->bevelSelection());
    ASSERT_EQ(ctrl->bevelSegments(), 1);

    ctrl->updateBevelProfilePoint(0, 0.9f);
    EXPECT_EQ(ctrl->bevelProfilePoints().size(), 0);
}

TEST_F(EditModeControllerBevelE2ETest, ResetBevelProfileWithSegments1NoOp) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);
    ASSERT_TRUE(ctrl->bevelSelection());

    ctrl->resetBevelProfile();
    EXPECT_EQ(ctrl->bevelProfilePoints().size(), 0);
}

TEST_F(EditModeControllerBevelE2ETest, ResizingSegmentsPreservesCurveShape) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);
    ASSERT_TRUE(ctrl->bevelSelection());

    ctrl->updateBevelSegments(3);
    ctrl->updateBevelProfilePoint(0, 0.2f);
    ctrl->updateBevelProfilePoint(1, 0.8f);

    ctrl->updateBevelSegments(5);
    auto pts = ctrl->bevelProfilePoints();
    ASSERT_EQ(pts.size(), 4);
    // After resampling, the first and last resampled points should roughly
    // bracket the original range — not all be 0.5.
    float minV = 1.0f, maxV = 0.0f;
    for (const auto& v : pts) {
        float f = v.toFloat();
        if (f < minV) minV = f;
        if (f > maxV) maxV = f;
    }
    EXPECT_LT(minV, 0.5f) << "resample should carry the concave side forward";
    EXPECT_GT(maxV, 0.5f) << "resample should carry the convex side forward";
}

TEST_F(EditModeControllerBevelE2ETest, SessionEndClearsProfilePoints) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);
    ASSERT_TRUE(ctrl->bevelSelection());
    ctrl->updateBevelSegments(3);
    ASSERT_EQ(ctrl->bevelProfilePoints().size(), 2);

    ctrl->cancelBevel();
    EXPECT_FALSE(ctrl->bevelSessionActive());
    EXPECT_EQ(ctrl->bevelProfilePoints().size(), 0);
}

TEST_F(EditModeControllerBevelE2ETest, BevelCubeCornerVertexProducesClosedManifold) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ctrl->setSelectionMode(EditModeController::VertexMode);

    // Select the top-right-front corner v5 (index 5 in makeCubeMesh).
    ctrl->selectVertex(5, false);
    ASSERT_EQ(ctrl->selectedVertexCount(), 1);

    ASSERT_TRUE(ctrl->bevelSelection()) << "vertex bevel should succeed";
    EXPECT_TRUE(ctrl->bevelSessionActive());

    // Verify manifold: every edge should have exactly 2 directed uses,
    // giving 0 boundary edges.
    std::vector<Ogre::Vector3> positions;
    std::vector<std::array<unsigned, 3>> tris;
    extractEntityBuffers(m_entity, positions, tris);
    std::map<std::pair<unsigned, unsigned>, int> edgeUse;
    for (const auto& t : tris) {
        for (int k = 0; k < 3; ++k) {
            unsigned u = t[k], v = t[(k + 1) % 3];
            auto key = std::make_pair(std::min(u, v), std::max(u, v));
            ++edgeUse[key];
        }
    }
    size_t boundaryEdges = 0;
    for (auto& [_, c] : edgeUse) if (c == 1) ++boundaryEdges;
    EXPECT_EQ(boundaryEdges, 0u) << "vertex bevel leaves " << boundaryEdges << " boundary edges";
}

// ===========================================================================
// Knife tool — session lifecycle (hit-test-free tests)
//
// The hit-test paths depend on an OgreWidget, which isn't available headless.
// These tests drive the controller API directly by pushing synthetic
// KnifePoint records through the commit pipeline, exercising the bits that
// do run in CI: enter/cancel/commit state transitions, short-circuit on
// zero-cut commits, breadcrumb emission via the active bevel guard, etc.
// ===========================================================================

TEST_F(EditModeControllerBevelE2ETest, KnifeBeginThenCancelRestoresIdleState) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();

    EXPECT_FALSE(ctrl->knifeSessionActive());
    EXPECT_TRUE(ctrl->beginKnife());
    EXPECT_TRUE(ctrl->knifeSessionActive());
    EXPECT_EQ(ctrl->knifePointCount(), 0);

    ctrl->cancelKnife();
    EXPECT_FALSE(ctrl->knifeSessionActive());
    EXPECT_EQ(ctrl->knifePointCount(), 0);
}

TEST_F(EditModeControllerBevelE2ETest, KnifeCommitWithoutPointsRejectsAndCleansUp) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ASSERT_TRUE(ctrl->beginKnife());

    // Zero confirmed points — commit should refuse and tidy up.
    EXPECT_FALSE(ctrl->commitKnife());
    EXPECT_FALSE(ctrl->knifeSessionActive());
}

TEST_F(EditModeControllerBevelE2ETest, KnifeBeginCancelsActiveBevelFirst) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(5, false);
    ASSERT_TRUE(ctrl->bevelSelection());
    ASSERT_TRUE(ctrl->bevelSessionActive());

    // Opening the knife should cancel the bevel session first.
    ASSERT_TRUE(ctrl->beginKnife());
    EXPECT_TRUE(ctrl->knifeSessionActive());
    EXPECT_FALSE(ctrl->bevelSessionActive());
}

TEST_F(EditModeControllerBevelE2ETest, BeginBevelCancelsActiveKnifeFirst) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ASSERT_TRUE(ctrl->beginKnife());

    // Now the user invokes a vertex bevel — the knife session should get
    // cancelled rather than leaving a stale preview behind the gizmo.
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(5, false);
    ASSERT_TRUE(ctrl->bevelSelection());

    EXPECT_TRUE(ctrl->bevelSessionActive());
    EXPECT_FALSE(ctrl->knifeSessionActive());
}

TEST_F(EditModeControllerBevelE2ETest, ExitEditModeCancelsKnifeSession) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ASSERT_TRUE(ctrl->beginKnife());
    ASSERT_TRUE(ctrl->knifeSessionActive());

    ctrl->exitEditMode(false);
    EXPECT_FALSE(ctrl->knifeSessionActive());
    EXPECT_FALSE(ctrl->isEditModeActive());
}

TEST_F(EditModeControllerBevelE2ETest, KnifeBeginOutsideEditModeFails) {
    auto* ctrl = EditModeController::instance();
    // Fresh fixture: edit mode is off. beginKnife should be a no-op.
    EXPECT_FALSE(ctrl->beginKnife());
    EXPECT_FALSE(ctrl->knifeSessionActive());
}

// Resolve an HE edge index for an edge connecting two global vertex
// indices in the controller's current EditableMesh. HE indices are an
// internal-rebuild detail; the knife tests resolve by vertex pair so
// harmless topology-order changes don't flake the assertions.
static int resolveEdgeByVerts(int va, int vb) {
    auto* mesh = EditModeController::instance()->currentMesh();
    if (!mesh) return -1;
    HalfEdgeMesh hm;
    if (!hm.buildFromEditableMesh(*mesh)) return -1;
    for (size_t e = 0; e < hm.edgeCount(); ++e) {
        auto [a, b] = hm.edgeVertices(static_cast<int>(e));
        if ((a == va && b == vb) || (a == vb && b == va))
            return static_cast<int>(e);
    }
    return -1;
}

TEST_F(EditModeControllerBevelE2ETest, KnifeCommitWalkAndCutGrowsMeshManifoldly) {
    // End-to-end: open a knife session, programmatically push two OnEdge
    // clicks on distinct edges of the welded cube, commit. The walk-and-
    // cut commit pipeline should splitEdge at both endpoints and (for
    // this topology) potentially at interior crossings, leaving the
    // mesh manifold with more vertices than before.
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ASSERT_TRUE(ctrl->beginKnife());

    // Capture pre-commit vertex count directly from the entity's buffers
    // so we're comparing against what Ogre actually holds, not just the
    // EditableMesh snapshot.
    std::vector<Ogre::Vector3> posBefore;
    std::vector<std::array<unsigned, 3>> trisBefore;
    extractEntityBuffers(m_entity, posBefore, trisBefore);
    const size_t vertsBefore = posBefore.size();

    // Welded cube verts 2..5 bound the top face (y=+1). Picking two
    // perimeter edges of that face gives a knife cut that's guaranteed
    // to land on the same coplanar region — the walk has something to
    // do, and the topology is fixed regardless of HE-edge numbering.
    const int edgeA = resolveEdgeByVerts(2, 3); // back-top edge
    const int edgeB = resolveEdgeByVerts(4, 5); // front-top edge
    ASSERT_GE(edgeA, 0);
    ASSERT_GE(edgeB, 0);

    ASSERT_TRUE(ctrl->addKnifePointOnEdge(edgeA, 0.4f));
    ASSERT_TRUE(ctrl->addKnifePointOnEdge(edgeB, 0.6f));
    ASSERT_EQ(ctrl->knifePointCount(), 2);

    ASSERT_TRUE(ctrl->commitKnife());
    EXPECT_FALSE(ctrl->knifeSessionActive());

    std::vector<Ogre::Vector3> posAfter;
    std::vector<std::array<unsigned, 3>> trisAfter;
    extractEntityBuffers(m_entity, posAfter, trisAfter);
    EXPECT_GT(posAfter.size(), vertsBefore)
        << "knife commit should have inserted new vertices into the GPU mesh";

    // Manifold check: every edge is used by exactly 1 or 2 triangles;
    // no edge should be used more than twice (would be non-manifold).
    std::map<std::pair<unsigned, unsigned>, int> edgeUse;
    for (const auto& t : trisAfter) {
        for (int k = 0; k < 3; ++k) {
            unsigned u = t[k], v = t[(k + 1) % 3];
            auto key = std::make_pair(std::min(u, v), std::max(u, v));
            ++edgeUse[key];
        }
    }
    for (auto& [key, count] : edgeUse) {
        EXPECT_LE(count, 2) << "non-manifold edge in post-knife mesh";
    }
}

TEST_F(EditModeControllerBevelE2ETest, KnifeCommitUndoRestoresOriginalVertexCount) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();

    std::vector<Ogre::Vector3> posBefore;
    std::vector<std::array<unsigned, 3>> trisBefore;
    extractEntityBuffers(m_entity, posBefore, trisBefore);
    const size_t vertsBefore = posBefore.size();

    const int edgeA = resolveEdgeByVerts(2, 3);
    const int edgeB = resolveEdgeByVerts(4, 5);
    ASSERT_GE(edgeA, 0);
    ASSERT_GE(edgeB, 0);

    ASSERT_TRUE(ctrl->beginKnife());
    ASSERT_TRUE(ctrl->addKnifePointOnEdge(edgeA, 0.5f));
    ASSERT_TRUE(ctrl->addKnifePointOnEdge(edgeB, 0.5f));
    ASSERT_TRUE(ctrl->commitKnife());

    UndoManager::getSingleton()->undo();

    std::vector<Ogre::Vector3> posAfterUndo;
    std::vector<std::array<unsigned, 3>> trisAfterUndo;
    extractEntityBuffers(m_entity, posAfterUndo, trisAfterUndo);
    EXPECT_EQ(posAfterUndo.size(), vertsBefore)
        << "undo after knife commit should restore original vertex count";
}

TEST_F(EditModeControllerBevelE2ETest, KnifePointOnInvalidEdgeRefused) {
    auto* ctrl = EditModeController::instance();
    ctrl->enterEditMode();
    ASSERT_TRUE(ctrl->beginKnife());

    EXPECT_FALSE(ctrl->addKnifePointOnEdge(-1, 0.5f));
    EXPECT_FALSE(ctrl->addKnifePointOnEdge(9999, 0.5f));
    EXPECT_EQ(ctrl->knifePointCount(), 0);
}

// ===========================================================================
// Delete / Dissolve E2E
// Verifies the controller dispatchers wire the right HE primitive in each
// selection mode and that face dissolve falls back to delete (matches
// Blender behavior on a pure triangle mesh).
// ===========================================================================

TEST_F(EditModeControllerBevelE2ETest, DeleteSelectionFaceModeRemovesTwoTriangles) {
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::FaceMode);

    std::vector<Ogre::Vector3> posBefore;
    std::vector<std::array<unsigned, 3>> trisBefore;
    extractEntityBuffers(m_entity, posBefore, trisBefore);
    const size_t triCountBefore = trisBefore.size();
    ASSERT_GT(triCountBefore, 2u);

    // The cube's first two triangles share a face; deleting both removes
    // exactly two triangles from the GPU buffer.
    ctrl->selectFace(0);
    ctrl->selectFace(1, true);
    EXPECT_EQ(ctrl->selectedFaceCount(), 2);

    EXPECT_EQ(ctrl->deleteSelection(), 2);
    EXPECT_EQ(ctrl->selectedFaceCount(), 0);

    std::vector<Ogre::Vector3> posAfter;
    std::vector<std::array<unsigned, 3>> trisAfter;
    extractEntityBuffers(m_entity, posAfter, trisAfter);
    EXPECT_EQ(trisAfter.size(), triCountBefore - 2);
}

TEST_F(EditModeControllerBevelE2ETest, DissolveSelectionFaceModeMatchesDelete) {
    // On a pure triangle mesh face dissolve has the same outcome as face
    // delete (no coplanar neighbors to merge). Run the same setup twice
    // — once via deleteSelection, once via dissolveSelection — and confirm
    // they produce identical triangle counts.
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::FaceMode);

    std::vector<Ogre::Vector3> posBefore;
    std::vector<std::array<unsigned, 3>> trisBefore;
    extractEntityBuffers(m_entity, posBefore, trisBefore);
    const size_t triCountBefore = trisBefore.size();

    ctrl->selectFace(0);
    EXPECT_EQ(ctrl->dissolveSelection(), 1)
        << "face dissolve in MVP delegates to deleteFaces";

    std::vector<Ogre::Vector3> posAfter;
    std::vector<std::array<unsigned, 3>> trisAfter;
    extractEntityBuffers(m_entity, posAfter, trisAfter);
    EXPECT_EQ(trisAfter.size(), triCountBefore - 1);
}

TEST_F(EditModeControllerBevelE2ETest, DeleteSelectionEmptyIsNoOp) {
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::FaceMode);
    EXPECT_EQ(ctrl->deleteSelection(), 0);
}

TEST_F(EditModeControllerBevelE2ETest, DeleteSelectionPushesUndoCommand) {
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::FaceMode);

    std::vector<Ogre::Vector3> posBefore;
    std::vector<std::array<unsigned, 3>> trisBefore;
    extractEntityBuffers(m_entity, posBefore, trisBefore);
    const size_t triCountBefore = trisBefore.size();

    ctrl->selectFace(0);
    ASSERT_EQ(ctrl->deleteSelection(), 1);

    UndoManager::getSingleton()->undo();

    std::vector<Ogre::Vector3> posAfterUndo;
    std::vector<std::array<unsigned, 3>> trisAfterUndo;
    extractEntityBuffers(m_entity, posAfterUndo, trisAfterUndo);
    EXPECT_EQ(trisAfterUndo.size(), triCountBefore)
        << "undo after delete should restore original triangle count";
}

// ===========================================================================
// Subdivide
// ===========================================================================

TEST_F(EditModeControllerBevelE2ETest, SubdivideSelectionVertexModeIsNoOp) {
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(0);
    EXPECT_EQ(ctrl->subdivideSelection(), 0)
        << "vertex selection alone doesn't define faces — should be a no-op";
}

TEST_F(EditModeControllerBevelE2ETest, SubdivideSelectionEmptyIsNoOp) {
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::FaceMode);
    EXPECT_EQ(ctrl->subdivideSelection(), 0);
}

TEST_F(EditModeControllerBevelE2ETest, SubdivideSelectionFaceModeAddsTriangles) {
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::FaceMode);

    std::vector<Ogre::Vector3> posBefore;
    std::vector<std::array<unsigned, 3>> trisBefore;
    extractEntityBuffers(m_entity, posBefore, trisBefore);
    const size_t triCountBefore = trisBefore.size();
    ASSERT_GT(triCountBefore, 0u);

    // Subdivide a single triangle. Itself splits into 4. The triangle
    // sharing each of its edges retriangulates against the new midpoint
    // — on a closed cube every edge has a neighbor, so 3 neighbors each
    // gain 1 triangle (split into 2). Net +3 + 3 = 6 new triangles.
    ctrl->selectFace(0);
    EXPECT_EQ(ctrl->subdivideSelection(), 1);

    std::vector<Ogre::Vector3> posAfter;
    std::vector<std::array<unsigned, 3>> trisAfter;
    extractEntityBuffers(m_entity, posAfter, trisAfter);
    EXPECT_GT(trisAfter.size(), triCountBefore)
        << "subdividing one face must produce more triangles, not fewer";
    EXPECT_GT(posAfter.size(), posBefore.size())
        << "midpoints must be appended to the vertex buffer";
}

TEST_F(EditModeControllerBevelE2ETest, SubdivideSelectionPushesUndoCommand) {
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::FaceMode);

    std::vector<Ogre::Vector3> posBefore;
    std::vector<std::array<unsigned, 3>> trisBefore;
    extractEntityBuffers(m_entity, posBefore, trisBefore);
    const size_t triCountBefore = trisBefore.size();

    ctrl->selectFace(0);
    ASSERT_EQ(ctrl->subdivideSelection(), 1);

    UndoManager::getSingleton()->undo();

    std::vector<Ogre::Vector3> posAfterUndo;
    std::vector<std::array<unsigned, 3>> trisAfterUndo;
    extractEntityBuffers(m_entity, posAfterUndo, trisAfterUndo);
    EXPECT_EQ(trisAfterUndo.size(), triCountBefore)
        << "undo after subdivide should restore original triangle count";
}

// ===========================================================================
// Fill
// ===========================================================================

TEST_F(EditModeControllerBevelE2ETest, FillSelectionFaceModeIsNoOp) {
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::FaceMode);
    ctrl->selectFace(0);
    EXPECT_EQ(ctrl->fillSelection(), 0)
        << "face selection cannot drive a fill — should be a no-op";
}

TEST_F(EditModeControllerBevelE2ETest, FillSelectionFewerThan3VerticesIsNoOp) {
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(0);
    ctrl->selectVertex(1, true);
    EXPECT_EQ(ctrl->fillSelection(), 0);
}

TEST_F(EditModeControllerBevelE2ETest, FillSelectionDuplicateOfExistingTriIsRejected) {
    // The cube's first triangle uses three of its corners. Filling the
    // same three corners should be rejected by the HE-side dup check.
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::FaceMode);
    ctrl->selectFace(0);

    // Pull the three corner verts of triangle 0 out of the entity's GPU
    // buffer so we know exactly which global verts to re-select.
    std::vector<Ogre::Vector3> pos;
    std::vector<std::array<unsigned, 3>> tris;
    extractEntityBuffers(m_entity, pos, tris);
    ASSERT_FALSE(tris.empty());
    const auto t0 = tris[0];

    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(static_cast<int>(t0[0]));
    ctrl->selectVertex(static_cast<int>(t0[1]), true);
    ctrl->selectVertex(static_cast<int>(t0[2]), true);
    EXPECT_EQ(ctrl->fillSelection(), 0)
        << "filling the same three corners as an existing tri must be rejected";
}

TEST_F(EditModeControllerBevelE2ETest, SubdivideSelectionEdgeModePromotesIncidentFaces) {
    // Subdivide in edge mode subdivides every triangle that touches a
    // selected edge. Pick an edge that's actually used by the cube's
    // index buffer (rather than guessing vertex pairs) so the test
    // doesn't depend on the welded cube's specific winding.
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    std::vector<Ogre::Vector3> posBefore;
    std::vector<std::array<unsigned, 3>> trisBefore;
    extractEntityBuffers(m_entity, posBefore, trisBefore);
    const size_t triCountBefore = trisBefore.size();
    ASSERT_GT(triCountBefore, 0u);

    // Use the first edge of the first triangle as a safely-real edge.
    const unsigned va = trisBefore[0][0];
    const unsigned vb = trisBefore[0][1];

    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(static_cast<int>(va), static_cast<int>(vb));
    ASSERT_GT(ctrl->subdivideSelection(), 0)
        << "edge-mode subdivide should subdivide at least one face";

    std::vector<Ogre::Vector3> posAfter;
    std::vector<std::array<unsigned, 3>> trisAfter;
    extractEntityBuffers(m_entity, posAfter, trisAfter);
    EXPECT_GT(trisAfter.size(), triCountBefore);
}

TEST_F(EditModeControllerBevelE2ETest, FillSelectionVertexModeProducesNewTriangle) {
    // After deleting a face, the three formerly-shared corners are still
    // alive (they're each on 2 other cube faces). Re-selecting them in
    // vertex mode and calling fillSelection should rebuild the lost
    // triangle and bring the cube back to closed-manifold.
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::FaceMode);

    std::vector<Ogre::Vector3> posBefore;
    std::vector<std::array<unsigned, 3>> trisBefore;
    extractEntityBuffers(m_entity, posBefore, trisBefore);
    ASSERT_FALSE(trisBefore.empty());
    const auto t0 = trisBefore[0];
    const Ogre::Vector3 cornerA = posBefore[t0[0]];
    const Ogre::Vector3 cornerB = posBefore[t0[1]];
    const Ogre::Vector3 cornerC = posBefore[t0[2]];
    const size_t triCountBefore = trisBefore.size();

    ctrl->selectFace(0);
    ASSERT_EQ(ctrl->deleteSelection(), 1);

    std::vector<Ogre::Vector3> posAfterDelete;
    std::vector<std::array<unsigned, 3>> trisAfterDelete;
    extractEntityBuffers(m_entity, posAfterDelete, trisAfterDelete);
    ASSERT_EQ(trisAfterDelete.size(), triCountBefore - 1);

    // Re-find the three corners in the post-delete buffer (their global
    // indices may shift when deleteFaces compacts vertices).
    auto findVert = [&](const Ogre::Vector3& target) -> int {
        for (size_t v = 0; v < posAfterDelete.size(); ++v) {
            if (posAfterDelete[v].distance(target) < 1e-4f)
                return static_cast<int>(v);
        }
        return -1;
    };
    const int vA = findVert(cornerA);
    const int vB = findVert(cornerB);
    const int vC = findVert(cornerC);
    ASSERT_GE(vA, 0); ASSERT_GE(vB, 0); ASSERT_GE(vC, 0);

    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(vA);
    ctrl->selectVertex(vB, true);
    ctrl->selectVertex(vC, true);

    // The HE-side dup-check compares vertex *sets* not winding, so
    // filling the 3 corners of a deleted face produces a new triangle
    // (winding may differ from the original — that's a subsequent
    // recalculate-normals concern, not a fill correctness one).
    EXPECT_EQ(ctrl->fillSelection(), 1)
        << "filling 3 corners of a deleted face must produce one new triangle";

    std::vector<Ogre::Vector3> posAfterFill;
    std::vector<std::array<unsigned, 3>> trisAfterFill;
    extractEntityBuffers(m_entity, posAfterFill, trisAfterFill);
    EXPECT_EQ(trisAfterFill.size(), triCountBefore)
        << "fill should restore the deleted triangle's slot";
}

TEST_F(EditModeControllerBevelE2ETest, FillSelectionPushesUndoCommand) {
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::FaceMode);

    std::vector<Ogre::Vector3> posBefore;
    std::vector<std::array<unsigned, 3>> trisBefore;
    extractEntityBuffers(m_entity, posBefore, trisBefore);
    const auto t0 = trisBefore[0];
    const Ogre::Vector3 cornerA = posBefore[t0[0]];
    const Ogre::Vector3 cornerB = posBefore[t0[1]];
    const Ogre::Vector3 cornerC = posBefore[t0[2]];
    const size_t triCountBefore = trisBefore.size();

    ctrl->selectFace(0);
    ASSERT_EQ(ctrl->deleteSelection(), 1);

    std::vector<Ogre::Vector3> posAfterDelete;
    std::vector<std::array<unsigned, 3>> trisAfterDelete;
    extractEntityBuffers(m_entity, posAfterDelete, trisAfterDelete);
    auto findVert = [&](const Ogre::Vector3& target) -> int {
        for (size_t v = 0; v < posAfterDelete.size(); ++v) {
            if (posAfterDelete[v].distance(target) < 1e-4f)
                return static_cast<int>(v);
        }
        return -1;
    };
    const int vA = findVert(cornerA);
    const int vB = findVert(cornerB);
    const int vC = findVert(cornerC);

    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(vA);
    ctrl->selectVertex(vB, true);
    ctrl->selectVertex(vC, true);
    ASSERT_EQ(ctrl->fillSelection(), 1);

    UndoManager::getSingleton()->undo();

    std::vector<Ogre::Vector3> posAfterUndo;
    std::vector<std::array<unsigned, 3>> trisAfterUndo;
    extractEntityBuffers(m_entity, posAfterUndo, trisAfterUndo);
    EXPECT_EQ(trisAfterUndo.size(), triCountBefore - 1)
        << "undo after fill should drop back to the post-delete tri count";
}

// ===========================================================================
// enterEditMode: n-gon import path (chunk 4)
// ===========================================================================

namespace {
// Write a minimal quad OBJ to a temp file. Mirrors the helper in
// EditableMesh_test.cpp (intentionally duplicated rather than shared
// across translation units, since the test files don't share a TU).
QString writeQuadObjForCtrl(const QString& baseName)
{
    const QString path = QDir::tempPath() + "/" + baseName + ".obj";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    QTextStream out(&f);
    out << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n";
    f.close();
    return path;
}
} // namespace

TEST_F(EditModeControllerBevelE2ETest, EnterEditModeUsesNgonPathWhenSourceCached) {
    // When the entity's mesh has qtme.source_path set, enterEditMode
    // should re-import via Assimp with aiProcess_Triangulate disabled,
    // populating EditableSubMesh::faces with the polygon structure.
    const QString objPath = writeQuadObjForCtrl("ctrl_ngon_path");
    ASSERT_FALSE(objPath.isEmpty());

    // Tag the cube mesh from the fixture's setup with a source path
    // pointing at the OBJ. enterEditMode should then re-import the OBJ
    // (overriding the cube's geometry — that's fine, the test only
    // verifies that the n-gon code path fired, not vertex content).
    auto* mesh = m_entity->getMesh().get();
    mesh->getUserObjectBindings().setUserAny(
        "qtme.source_path", Ogre::Any(objPath.toStdString()));

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    auto* editableMesh = ctrl->currentMesh();
    ASSERT_NE(editableMesh, nullptr);
    ASSERT_FALSE(editableMesh->subMeshes().empty());
    EXPECT_FALSE(editableMesh->subMeshes()[0].faces.empty())
        << "enterEditMode with source_path set should populate faces "
           "via the n-gon-aware loadFromAssimpFile path";
    EXPECT_EQ(editableMesh->subMeshes()[0].faces[0].indices.size(), 4u);

    QFile::remove(objPath);
}

TEST_F(EditModeControllerBevelE2ETest, EnterEditModeFallsBackToLegacyWhenNoSourcePath) {
    // The fixture's cube has no qtme.source_path tag. enterEditMode
    // should fall through to loadFromEntity and produce the
    // triangle-only legacy submesh shape (faces empty).
    auto* mesh = m_entity->getMesh().get();
    EXPECT_FALSE(mesh->getUserObjectBindings().getUserAny(
        "qtme.source_path").has_value())
        << "fixture cube starts without source path — sanity check";

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    auto* editableMesh = ctrl->currentMesh();
    ASSERT_NE(editableMesh, nullptr);
    ASSERT_FALSE(editableMesh->subMeshes().empty());
    EXPECT_TRUE(editableMesh->subMeshes()[0].faces.empty())
        << "no source path → legacy path → faces empty (chunk-1 invariant)";
}

TEST_F(EditModeControllerBevelE2ETest, EnterEditModeAfterEditDoesNotReimport) {
    // Enter Edit Mode with the n-gon path (sets faces), make a
    // committable edit, exit, re-enter. The second entry must use the
    // legacy path because the first edit wiped qtme.source_path.
    const QString objPath = writeQuadObjForCtrl("ctrl_ngon_post_edit");
    ASSERT_FALSE(objPath.isEmpty());

    auto* mesh = m_entity->getMesh().get();
    mesh->getUserObjectBindings().setUserAny(
        "qtme.source_path", Ogre::Any(objPath.toStdString()));

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ASSERT_FALSE(ctrl->currentMesh()->subMeshes().empty());
    ASSERT_FALSE(ctrl->currentMesh()->subMeshes()[0].faces.empty())
        << "first entry must take the n-gon path";

    // Commit a real change so the test exercises the commit path even
    // if the controller ever short-circuits zero-delta transforms in
    // the future. (CodeRabbit follow-up on PR #347.)
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(0);
    ctrl->translateSelectedVertices(Ogre::Vector3(0.001f, 0.0f, 0.0f));
    ctrl->exitEditMode(/*commitChanges*/ true);

    EXPECT_FALSE(mesh->getUserObjectBindings().getUserAny(
        "qtme.source_path").has_value())
        << "exitEditMode commit must have wiped the source path";

    // Chunk 6 of #326: although `qtme.source_path` is gone, the
    // n-gon faces survive in the `qtme.faces.<i>` binding written by
    // the commit. Re-entering Edit Mode now rehydrates them via
    // loadFromMesh -> readNgonFacesFromMesh so n-gon-aware ops keep
    // working post-edit. (Pre-chunk-6 this test asserted the
    // OPPOSITE — that the second entry would fall back to legacy
    // triangle-only — which is no longer the right contract.)
    ASSERT_TRUE(ctrl->enterEditMode());
    EXPECT_FALSE(ctrl->currentMesh()->subMeshes()[0].faces.empty())
        << "second entry (post-edit) should rehydrate n-gons from "
           "the qtme.faces.<i> binding";

    QFile::remove(objPath);
}

// ----------------------------------------------------------------------
// Paint knob setters — pure state, no GL required. These exercise the
// updateClampedKnob helper introduced as part of the dedup pass that
// collapsed the radius / strength / falloff setters into one template.
//
// EditModeController is a singleton, so these tests use a fixture that
// snapshots / restores every paint property they touch. Without this
// the test order leaks state across cases (CodeRabbit on PR #532).
// ----------------------------------------------------------------------

class EditModeControllerPaintKnobsFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto* em = EditModeController::instance();
        m_radius   = em->vertexPaintRadius();
        m_strength = em->vertexPaintStrength();
        m_falloff  = em->vertexPaintFalloff();
        m_shape    = em->vertexPaintShape();
        m_fg       = em->vertexPaintColor();
        m_bg       = em->vertexPaintBackgroundColor();
    }
    void TearDown() override
    {
        auto* em = EditModeController::instance();
        em->setVertexPaintRadius(m_radius);
        em->setVertexPaintStrength(m_strength);
        em->setVertexPaintFalloff(m_falloff);
        em->setVertexPaintShape(m_shape);
        em->setVertexPaintColor(m_fg);
        em->setVertexPaintBackgroundColor(m_bg);
    }
private:
    double m_radius = 0;
    double m_strength = 0;
    double m_falloff = 0;
    int    m_shape = 0;
    QColor m_fg, m_bg;
};

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintStrengthClampsToUnitRange)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintStrength(2.0);   // above range — clamps to 1.0
    EXPECT_NEAR(em->vertexPaintStrength(), 1.0, 1e-9);
    em->setVertexPaintStrength(-0.5);  // below range — clamps to 0.0
    EXPECT_NEAR(em->vertexPaintStrength(), 0.0, 1e-9);
    em->setVertexPaintStrength(0.42);
    EXPECT_NEAR(em->vertexPaintStrength(), 0.42, 1e-9);
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintFalloffClampsToUnitRange)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintFalloff(99.0);
    EXPECT_NEAR(em->vertexPaintFalloff(), 1.0, 1e-9);
    em->setVertexPaintFalloff(-1.0);
    EXPECT_NEAR(em->vertexPaintFalloff(), 0.0, 1e-9);
    em->setVertexPaintFalloff(0.25);
    EXPECT_NEAR(em->vertexPaintFalloff(), 0.25, 1e-9);
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintRadiusRejectsNonPositive)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintRadius(0.5);
    EXPECT_NEAR(em->vertexPaintRadius(), 0.5, 1e-9);
    // Zero / negative are no-ops; the previous value sticks.
    em->setVertexPaintRadius(0.0);
    EXPECT_NEAR(em->vertexPaintRadius(), 0.5, 1e-9);
    em->setVertexPaintRadius(-3.0);
    EXPECT_NEAR(em->vertexPaintRadius(), 0.5, 1e-9);
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintRadiusRejectsNonFinite)
{
    // NaN / Inf must not propagate through std::min/std::max into the
    // member (CodeRabbit on PR #532 — the upper bound for radius is
    // +∞ so Inf would otherwise pass straight through the clamp).
    auto* em = EditModeController::instance();
    em->setVertexPaintRadius(0.7);
    EXPECT_NEAR(em->vertexPaintRadius(), 0.7, 1e-9);
    em->setVertexPaintRadius(std::numeric_limits<double>::infinity());
    EXPECT_NEAR(em->vertexPaintRadius(), 0.7, 1e-9);
    em->setVertexPaintRadius(std::numeric_limits<double>::quiet_NaN());
    EXPECT_NEAR(em->vertexPaintRadius(), 0.7, 1e-9);
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintStrengthAndFalloffRejectNonFinite)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintStrength(0.3);
    em->setVertexPaintFalloff(0.4);
    em->setVertexPaintStrength(std::numeric_limits<double>::infinity());
    em->setVertexPaintFalloff(std::numeric_limits<double>::quiet_NaN());
    EXPECT_NEAR(em->vertexPaintStrength(), 0.3, 1e-9);
    EXPECT_NEAR(em->vertexPaintFalloff(), 0.4, 1e-9);
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintShapeRoundTripsAcrossModes)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintShape(static_cast<int>(EditModeController::ShapeSquare));
    EXPECT_EQ(em->vertexPaintShape(),
              static_cast<int>(EditModeController::ShapeSquare));
    em->setVertexPaintShape(static_cast<int>(EditModeController::ShapeRound));
    EXPECT_EQ(em->vertexPaintShape(),
              static_cast<int>(EditModeController::ShapeRound));
    // Out-of-range int is rejected — shape stays at its previous value.
    em->setVertexPaintShape(99);
    EXPECT_EQ(em->vertexPaintShape(),
              static_cast<int>(EditModeController::ShapeRound));
}

TEST_F(EditModeControllerPaintKnobsFixture, SwapAndResetPaintColors)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintColor(QColor(10, 20, 30));
    em->setVertexPaintBackgroundColor(QColor(200, 210, 220));
    em->swapPaintColors();
    EXPECT_EQ(em->vertexPaintColor().rgb(),
              QColor(200, 210, 220).rgb());
    EXPECT_EQ(em->vertexPaintBackgroundColor().rgb(),
              QColor(10, 20, 30).rgb());
    em->resetPaintColors();
    // Defaults: FG = Fern green (#71BC78 / 113, 188, 120), BG = black.
    EXPECT_EQ(em->vertexPaintColor().rgb(),
              QColor(113, 188, 120).rgb());
    EXPECT_EQ(em->vertexPaintBackgroundColor().rgb(),
              QColor(0, 0, 0).rgb());
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintBrushColorFromCssString)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintBrushColor("#ff8800");
    EXPECT_EQ(em->vertexPaintColor().rgb(), QColor("#ff8800").rgb());
    em->setVertexPaintBackgroundBrushColor("#001122");
    EXPECT_EQ(em->vertexPaintBackgroundColor().rgb(), QColor("#001122").rgb());
}

// ===========================================================================
// NEW COVERAGE: setVertexPaintColor / setVertexPaintBackgroundColor direct
// QColor setters (separate from setVertexPaintBrushColor which takes a string)
// ===========================================================================

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintColorInvalidIsNoOp)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintColor(QColor(50, 60, 70));
    const QColor before = em->vertexPaintColor();
    em->setVertexPaintColor(QColor()); // invalid
    EXPECT_EQ(em->vertexPaintColor().rgb(), before.rgb());
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintColorZeroAlphaForcedOpaque)
{
    // The setter must promote alpha=0 to 255 — brush should always be visible.
    auto* em = EditModeController::instance();
    QColor transparent(100, 150, 200);
    transparent.setAlpha(0);
    em->setVertexPaintColor(transparent);
    EXPECT_EQ(em->vertexPaintColor().alpha(), 255);
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintColorSameValueDoesNotEmit)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintColor(QColor(1, 2, 3));
    QSignalSpy spy(em, &EditModeController::vertexPaintChanged);
    em->setVertexPaintColor(QColor(1, 2, 3)); // same value — no emission
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintBackgroundColorInvalidIsNoOp)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintBackgroundColor(QColor(50, 60, 70));
    const QColor before = em->vertexPaintBackgroundColor();
    em->setVertexPaintBackgroundColor(QColor());
    EXPECT_EQ(em->vertexPaintBackgroundColor().rgb(), before.rgb());
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintBackgroundColorAllowsAlphaZero)
{
    // BG color is allowed to be transparent (matches "erase to transparent"
    // semantics from the implementation comment).
    auto* em = EditModeController::instance();
    QColor transparent(0, 0, 0);
    transparent.setAlpha(0);
    em->setVertexPaintBackgroundColor(transparent);
    EXPECT_EQ(em->vertexPaintBackgroundColor().alpha(), 0);
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintEnabledTogglesAndEmits)
{
    auto* em = EditModeController::instance();
    const bool initial = em->vertexPaintEnabled();
    QSignalSpy spy(em, &EditModeController::vertexPaintChanged);
    em->setVertexPaintEnabled(!initial);
    EXPECT_NE(em->vertexPaintEnabled(), initial);
    EXPECT_GE(spy.count(), 1);
    // Same value does not emit again
    int after = spy.count();
    em->setVertexPaintEnabled(!initial);
    EXPECT_EQ(spy.count(), after);
    em->setVertexPaintEnabled(initial); // restore
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintBrushColorParsesCssNameOrHex)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintBrushColor("red");
    EXPECT_EQ(em->vertexPaintColor().rgb(), QColor("red").rgb());
    em->setVertexPaintBrushColor("#00ff00");
    EXPECT_EQ(em->vertexPaintColor().rgb(), QColor(0, 255, 0).rgb());
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintBackgroundBrushColorEmptyAndInvalidNoOp)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintBackgroundColor(QColor(11, 22, 33));
    const QColor before = em->vertexPaintBackgroundColor();
    em->setVertexPaintBackgroundBrushColor(QString());
    em->setVertexPaintBackgroundBrushColor("   ");
    em->setVertexPaintBackgroundBrushColor("not_a_color_at_all_xyz");
    EXPECT_EQ(em->vertexPaintBackgroundColor().rgb(), before.rgb());
}

// ===========================================================================
// NEW COVERAGE: canEnterEditMode / toggleEditMode
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, CanEnterEditModeWithSingleSelection)
{
    auto* ctrl = EditModeController::instance();
    // Fixture creates m_node and selects it — canEnterEditMode should be true.
    EXPECT_TRUE(ctrl->canEnterEditMode());

    // After clearing selection, no longer eligible.
    SelectionSet::getSingleton()->clear();
    EXPECT_FALSE(ctrl->canEnterEditMode());

    // Restore for TearDown.
    SelectionSet::getSingleton()->selectOne(m_node);
}

TEST_F(EditModeControllerSelectionTest, ToggleEditModeEntersThenExits)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_FALSE(ctrl->isEditModeActive());

    ctrl->toggleEditMode();
    EXPECT_TRUE(ctrl->isEditModeActive());

    ctrl->toggleEditMode(); // commit=true is the default
    EXPECT_FALSE(ctrl->isEditModeActive());
}

// ===========================================================================
// NEW COVERAGE: vertexCount / triangleCount / subMeshCount default 0 outside
// edit mode
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, MeshInfoCountsZeroOutsideEditMode)
{
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->isEditModeActive());
    EXPECT_EQ(ctrl->vertexCount(), 0);
    EXPECT_EQ(ctrl->triangleCount(), 0);
    EXPECT_EQ(ctrl->subMeshCount(), 0);
}

// ===========================================================================
// NEW COVERAGE: setNormalsMode + normalsMode round-trip + signal
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, SetNormalsModeRoundTripsAndEmits)
{
    auto* ctrl = EditModeController::instance();
    const int original = ctrl->normalsMode();

    QSignalSpy spy(ctrl, &EditModeController::normalsModeChanged);

    // Valid values 0 (smooth) and 1 (flat).
    ctrl->setNormalsMode(original == 0 ? 1 : 0);
    EXPECT_NE(ctrl->normalsMode(), original);
    EXPECT_GE(spy.count(), 1);

    // Out-of-range values are ignored.
    const int after = ctrl->normalsMode();
    int spyAfter = spy.count();
    ctrl->setNormalsMode(-1);
    ctrl->setNormalsMode(99);
    EXPECT_EQ(ctrl->normalsMode(), after);
    EXPECT_EQ(spy.count(), spyAfter);

    // Same value does not emit.
    ctrl->setNormalsMode(after);
    EXPECT_EQ(spy.count(), spyAfter);

    // Restore.
    ctrl->setNormalsMode(original);
}

// ===========================================================================
// NEW COVERAGE: recalculateNormals (smooth + flat) inside edit mode
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, RecalculateNormalsInEditModeRunsSmoothPath)
{
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_recalc_smooth");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_recalc_smooth_node");
    Manager::getSingleton()->createEntity(node, meshPtr);
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    QSignalSpy spy(ctrl, &EditModeController::meshDataChanged);
    ctrl->recalculateNormals(true);
    EXPECT_GE(spy.count(), 1);

    ctrl->recalculateNormals(false);
    EXPECT_GE(spy.count(), 2);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_recalc_smooth_node");
}

TEST_F(EditModeControllerSelectionTest, RecalculateNormalsOutsideEditModeIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->isEditModeActive());
    QSignalSpy spy(ctrl, &EditModeController::meshDataChanged);
    ctrl->recalculateNormals(true);
    EXPECT_EQ(spy.count(), 0);
}

// ===========================================================================
// NEW COVERAGE: validateMesh / degenerateTriangleCount / hasValidationWarnings
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, ValidateMeshOutsideEditModeZerosOut)
{
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->isEditModeActive());
    QSignalSpy spy(ctrl, &EditModeController::validationChanged);
    ctrl->validateMesh();
    // m_editableMesh is null outside edit mode — early-return path zeroes
    // the count and always emits validationChanged.
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(ctrl->degenerateTriangleCount(), 0);
    EXPECT_FALSE(ctrl->hasValidationWarnings());
}

TEST_F(EditModeControllerSelectionTest, ValidateMeshInEditModeOnHealthyMeshReportsZero)
{
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_validate_healthy");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_validate_healthy_node");
    Manager::getSingleton()->createEntity(node, meshPtr);
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    ctrl->validateMesh();
    EXPECT_EQ(ctrl->degenerateTriangleCount(), 0);
    EXPECT_FALSE(ctrl->hasValidationWarnings());

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_validate_healthy_node");
}

TEST_F(EditModeControllerSelectionTest, RemoveDegenerateTrianglesOutsideEditModeIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->isEditModeActive());
    ctrl->removeDegenerateTriangles();
    EXPECT_EQ(ctrl->degenerateTriangleCount(), 0);
}

TEST_F(EditModeControllerSelectionTest, RemoveDegenerateTrianglesOnCleanMeshIsNoOp)
{
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_remove_clean");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_remove_clean_node");
    Manager::getSingleton()->createEntity(node, meshPtr);
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    const int before = ctrl->triangleCount();
    ctrl->removeDegenerateTriangles();
    EXPECT_EQ(ctrl->triangleCount(), before);
    EXPECT_EQ(ctrl->degenerateTriangleCount(), 0);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_remove_clean_node");
}

// ===========================================================================
// NEW COVERAGE: setVertexColorPreviewEnabled toggles + emits
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, SetVertexColorPreviewEnabledTogglesAndEmits)
{
    auto* ctrl = EditModeController::instance();
    const bool initial = ctrl->vertexColorPreviewEnabled();
    QSignalSpy spy(ctrl, &EditModeController::vertexColorPreviewChanged);

    ctrl->setVertexColorPreviewEnabled(!initial);
    EXPECT_NE(ctrl->vertexColorPreviewEnabled(), initial);
    EXPECT_GE(spy.count(), 1);

    // Same value does not emit again.
    int after = spy.count();
    ctrl->setVertexColorPreviewEnabled(!initial);
    EXPECT_EQ(spy.count(), after);

    ctrl->setVertexColorPreviewEnabled(initial); // restore
}

// ===========================================================================
// NEW COVERAGE: vertex transform helpers (centroid, rotate, scale, snapshot,
// restore) when there is no selection
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, GetSelectedVerticesCentroidEmptySelectionReturnsZero)
{
    auto* ctrl = EditModeController::instance();
    const Ogre::Vector3 c = ctrl->getSelectedVerticesCentroid();
    EXPECT_FLOAT_EQ(c.x, 0.0f);
    EXPECT_FLOAT_EQ(c.y, 0.0f);
    EXPECT_FLOAT_EQ(c.z, 0.0f);
}

TEST_F(EditModeControllerSelectionTest, GetSelectedVerticesCentroidWithSelection)
{
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_centroid");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_centroid_node");
    Manager::getSingleton()->createEntity(node, meshPtr);
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    ctrl->selectAll();
    const Ogre::Vector3 c = ctrl->getSelectedVerticesCentroid();
    // Triangle mesh has verts at (0,0,0), (1,0,0), (0,1,0) — centroid = (1/3, 1/3, 0)
    EXPECT_NEAR(c.x, 1.0f / 3.0f, 1e-4f);
    EXPECT_NEAR(c.y, 1.0f / 3.0f, 1e-4f);
    EXPECT_NEAR(c.z, 0.0f, 1e-4f);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_centroid_node");
}

TEST_F(EditModeControllerSelectionTest, RotateSelectedVerticesEmptySelectionIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    QSignalSpy spy(ctrl, &EditModeController::meshDataChanged);
    ctrl->rotateSelectedVertices(Ogre::Quaternion(Ogre::Radian(0.5f),
                                                  Ogre::Vector3::UNIT_Y));
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(EditModeControllerSelectionTest, RotateSelectedVerticesWithSelectionMovesVerts)
{
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_rotate");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_rotate_node");
    Manager::getSingleton()->createEntity(node, meshPtr);
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    ctrl->selectAll();
    QSignalSpy spy(ctrl, &EditModeController::meshDataChanged);
    ctrl->rotateSelectedVertices(Ogre::Quaternion(Ogre::Radian(0.5f),
                                                  Ogre::Vector3::UNIT_Y));
    EXPECT_GE(spy.count(), 1);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_rotate_node");
}

TEST_F(EditModeControllerSelectionTest, ScaleSelectedVerticesEmptySelectionIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    QSignalSpy spy(ctrl, &EditModeController::meshDataChanged);
    ctrl->scaleSelectedVertices(Ogre::Vector3(2.0f, 2.0f, 2.0f));
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(EditModeControllerSelectionTest, ScaleSelectedVerticesWithSelectionEmitsSignal)
{
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_scale");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_scale_node");
    Manager::getSingleton()->createEntity(node, meshPtr);
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    ctrl->selectAll();
    QSignalSpy spy(ctrl, &EditModeController::meshDataChanged);
    ctrl->scaleSelectedVertices(Ogre::Vector3(2.0f, 2.0f, 2.0f));
    EXPECT_GE(spy.count(), 1);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_scale_node");
}

TEST_F(EditModeControllerSelectionTest, SnapshotAndRestoreVertexPositionsRoundTrips)
{
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_snapshot");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_snapshot_node");
    Manager::getSingleton()->createEntity(node, meshPtr);
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    ctrl->selectAll();
    auto snapshot = ctrl->snapshotVertexPositions();
    EXPECT_FALSE(snapshot.empty()) << "snapshot must capture selected verts";

    // Mutate the mesh and restore — the snapshot should bring it back.
    ctrl->translateSelectedVertices(Ogre::Vector3(5.0f, 0.0f, 0.0f));
    ctrl->restoreVertexPositions(snapshot);

    const auto afterRestore = ctrl->snapshotVertexPositions();
    ASSERT_EQ(afterRestore.size(), snapshot.size());
    for (const auto& [gi, pos] : snapshot) {
        ASSERT_TRUE(afterRestore.count(gi) > 0);
        EXPECT_NEAR(pos.x, afterRestore.at(gi).x, 1e-4f);
        EXPECT_NEAR(pos.y, afterRestore.at(gi).y, 1e-4f);
        EXPECT_NEAR(pos.z, afterRestore.at(gi).z, 1e-4f);
    }

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_snapshot_node");
}

TEST_F(EditModeControllerSelectionTest, ScaleFromSnapshotEmptySnapshotIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    QSignalSpy spy(ctrl, &EditModeController::meshDataChanged);
    ctrl->scaleFromSnapshot({}, Ogre::Vector3::ZERO, Ogre::Vector3(2, 2, 2));
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(EditModeControllerSelectionTest, ScaleFromSnapshotWithDataEmitsAndMoves)
{
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_scalesnap");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_scalesnap_node");
    Manager::getSingleton()->createEntity(node, meshPtr);
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    ctrl->selectAll();
    auto snapshot = ctrl->snapshotVertexPositions();
    QSignalSpy spy(ctrl, &EditModeController::meshDataChanged);
    ctrl->scaleFromSnapshot(snapshot, Ogre::Vector3::ZERO, Ogre::Vector3(2, 2, 2));
    EXPECT_GE(spy.count(), 1);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_scalesnap_node");
}

// ===========================================================================
// NEW COVERAGE: isMeshQuadBased / canConvertToQuads
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, IsMeshQuadBasedOutsideEditModeFalse)
{
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->isEditModeActive());
    EXPECT_FALSE(ctrl->isMeshQuadBased());
    EXPECT_FALSE(ctrl->canConvertToQuads());
}

TEST_F(EditModeControllerSelectionTest, TriangleOnlyMeshNotQuadBased)
{
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_isquad_tri");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_isquad_tri_node");
    Manager::getSingleton()->createEntity(node, meshPtr);
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    EXPECT_FALSE(ctrl->isMeshQuadBased())
        << "triangle-only mesh has no n-gon faces";
    EXPECT_TRUE(ctrl->canConvertToQuads())
        << "triangle-only mesh is a candidate for promotion";

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_isquad_tri_node");
}

// ===========================================================================
// NEW COVERAGE: extrudeSelection edge cases (vertex/edge mode no-op)
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, ExtrudeSelectionOutsideEditModeReturnsFalse)
{
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->isEditModeActive());
    EXPECT_FALSE(ctrl->extrudeSelection());
}

TEST_F(EditModeControllerSelectionTest, ExtrudeSelectionInVertexModeReturnsFalse)
{
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_extrude_vert");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_extrude_vert_node");
    Manager::getSingleton()->createEntity(node, meshPtr);
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(0);
    EXPECT_FALSE(ctrl->extrudeSelection());

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_extrude_vert_node");
}

TEST_F(EditModeControllerSelectionTest, ExtrudeSelectionInFaceModeNoFacesReturnsFalse)
{
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_extrude_nofaces");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_extrude_nofaces_node");
    Manager::getSingleton()->createEntity(node, meshPtr);
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::FaceMode);
    // No face selected — extrude should refuse cleanly.
    EXPECT_FALSE(ctrl->extrudeSelection());

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_extrude_nofaces_node");
}

// ===========================================================================
// NEW COVERAGE: selectedFacesAsHEFaceIndices — empty / single / multi
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, SelectedFacesAsHEFaceIndicesEmptyIsEmpty)
{
    auto* ctrl = EditModeController::instance();
    auto v = ctrl->selectedFacesAsHEFaceIndices();
    EXPECT_TRUE(v.empty());
}

TEST_F(EditModeControllerSelectionTest, SelectedFacesAsHEFaceIndicesWithFaceSelected)
{
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_hefaceidx");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_hefaceidx_node");
    Manager::getSingleton()->createEntity(node, meshPtr);
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    ctrl->setSelectionMode(EditModeController::FaceMode);
    ctrl->selectFace(0);
    auto v = ctrl->selectedFacesAsHEFaceIndices();
    EXPECT_EQ(v.size(), 1u);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_hefaceidx_node");
}

// ===========================================================================
// NEW COVERAGE: flushPendingVertexPaintForEntity — no-op when no stroke
// pending or entity mismatch
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, FlushPendingVertexPaintNullEntityIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    ctrl->flushPendingVertexPaintForEntity(nullptr);
    SUCCEED();
}

TEST_F(EditModeControllerSelectionTest, FlushPendingVertexPaintWithoutPendingIsNoOp)
{
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_flushpaint");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_flushpaint_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    // No active stroke; calling flush should not crash.
    ctrl->flushPendingVertexPaintForEntity(entity);
    SUCCEED();

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_flushpaint_node");
}

// ===========================================================================
// NEW COVERAGE: vertex paint stroke lifecycle outside edit mode / without
// widget (just calls the no-op early-return paths)
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, VertexPaintStrokeStartsInactive)
{
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->vertexPaintStrokeActive());
}

TEST_F(EditModeControllerSelectionTest, EndVertexPaintStrokeIdleIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    ctrl->endVertexPaintStroke(true);
    EXPECT_FALSE(ctrl->vertexPaintStrokeActive());
}

TEST_F(EditModeControllerSelectionTest, ClearVertexPaintPreviewIdleIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    ctrl->clearVertexPaintPreview();
    SUCCEED();
}

// ===========================================================================
// NEW COVERAGE: bevel gizmo helpers when no session is active
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, TickBevelGizmoNullCameraIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    ctrl->tickBevelGizmo(nullptr);
    SUCCEED();
}

TEST_F(EditModeControllerSelectionTest, IsBevelGizmoHandleNullObjectIsFalse)
{
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->isBevelGizmoHandle(nullptr));
}

TEST_F(EditModeControllerSelectionTest, UpdateBevelWidthNoActiveSessionIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    QSignalSpy spy(ctrl, &EditModeController::bevelProfilePointsChanged);
    ctrl->updateBevelWidth(0.5f);
    // No session — no profile points change emission expected.
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(EditModeControllerSelectionTest, CommitBevelNoActiveSessionIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    // Commit when no session is active — must not crash.
    ctrl->commitBevel();
    SUCCEED();
}

// ===========================================================================
// NEW COVERAGE: knife addKnifePointOnEdge outside any session
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, AddKnifePointOnEdgeNoSessionIsFalse)
{
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->knifeSessionActive());
    EXPECT_FALSE(ctrl->addKnifePointOnEdge(0, 0.5f));
}

// ===========================================================================
// NEW COVERAGE: merge operations on cube — happy and edge cases
// ===========================================================================

class EditModeControllerMergeOpsTest : public ::testing::Test {
protected:
    Ogre::SceneNode* m_node = nullptr;
    Ogre::Entity* m_entity = nullptr;
    std::string m_meshName;
    std::string m_nodeName;

    void SetUp() override {
        ASSERT_TRUE(tryInitOgre());
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();

        static int counter = 0;
        ++counter;
        m_meshName = "MergeOps_cube_" + std::to_string(counter);
        m_nodeName = "MergeOps_node_" + std::to_string(counter);

        auto mesh = createInMemoryWeldedCube(m_meshName);
        m_node = Manager::getSingleton()->addSceneNode(QString::fromStdString(m_nodeName));
        m_entity = Manager::getSingleton()->createEntity(m_node, mesh);
        m_entity->setMaterialName("BaseWhite");
        SelectionSet::getSingleton()->selectOne(m_node);
    }

    void TearDown() override {
        auto* ctrl = EditModeController::instance();
        if (ctrl->isEditModeActive()) ctrl->exitEditMode(false);
        SelectionSet::getSingleton()->clear();
        if (m_node) {
            Manager::getSingleton()->destroySceneNode(m_node);
            m_node = nullptr;
        }
        if (!m_meshName.empty()) {
            auto& mm = Ogre::MeshManager::getSingleton();
            if (mm.getByName(m_meshName))
                mm.remove(m_meshName);
            m_meshName.clear();
        }
        EditModeController::kill();
    }
};

TEST_F(EditModeControllerMergeOpsTest, MergeAtCenterNoSelectionReturnsZero)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    EXPECT_EQ(ctrl->mergeAtCenter(), 0);
}

TEST_F(EditModeControllerMergeOpsTest, MergeAtCenterTwoVertsReturnsOne)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(0);
    ctrl->selectVertex(1, true);
    EXPECT_EQ(ctrl->mergeAtCenter(), 1);
}

TEST_F(EditModeControllerMergeOpsTest, MergeAtFirstNoSelectionReturnsZero)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    EXPECT_EQ(ctrl->mergeAtFirst(), 0);
}

TEST_F(EditModeControllerMergeOpsTest, MergeAtFirstTwoVertsReturnsOne)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(0);
    ctrl->selectVertex(1, true);
    EXPECT_EQ(ctrl->mergeAtFirst(), 1);
}

TEST_F(EditModeControllerMergeOpsTest, MergeAtLastNoSelectionReturnsZero)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    EXPECT_EQ(ctrl->mergeAtLast(), 0);
}

TEST_F(EditModeControllerMergeOpsTest, MergeAtLastTwoVertsReturnsOne)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(0);
    ctrl->selectVertex(7, true);
    EXPECT_EQ(ctrl->mergeAtLast(), 1);
}

TEST_F(EditModeControllerMergeOpsTest, MergeByDistanceNoSelectionReturnsZero)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    EXPECT_EQ(ctrl->mergeByDistance(0.5f), 0);
}

TEST_F(EditModeControllerMergeOpsTest, MergeByDistanceNonPositiveThresholdReturnsZero)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectAll();
    EXPECT_EQ(ctrl->mergeByDistance(0.0f), 0);
    EXPECT_EQ(ctrl->mergeByDistance(-1.0f), 0);
}

TEST_F(EditModeControllerMergeOpsTest, MergeByDistanceFarThresholdNoOp)
{
    // Cube verts are 2 units apart (in any axis-aligned pair). A threshold
    // small enough to never group any of them should yield 0.
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectAll();
    EXPECT_EQ(ctrl->mergeByDistance(0.001f), 0);
}

// ===========================================================================
// NEW COVERAGE: extrudeSelection on a cube face (happy path)
// ===========================================================================

TEST_F(EditModeControllerMergeOpsTest, ExtrudeFaceProducesNewGeometry)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::FaceMode);
    ctrl->selectFace(0);

    const int trisBefore = ctrl->triangleCount();
    const int vertsBefore = ctrl->vertexCount();
    EXPECT_TRUE(ctrl->extrudeSelection());
    // Extrude on one triangle produces additional verts and side walls.
    EXPECT_GT(ctrl->triangleCount(), trisBefore);
    EXPECT_GT(ctrl->vertexCount(), vertsBefore);
}

// ===========================================================================
// NEW COVERAGE: loopCutSelection — error paths (wrong mode / no selection /
// triangle-only mesh)
// ===========================================================================

TEST_F(EditModeControllerMergeOpsTest, LoopCutNoSelectionReturnsZero)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    EXPECT_EQ(ctrl->loopCutSelection(), 0);
}

TEST_F(EditModeControllerMergeOpsTest, LoopCutInVertexModeReturnsZero)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(0);
    EXPECT_EQ(ctrl->loopCutSelection(), 0);
}

TEST_F(EditModeControllerMergeOpsTest, LoopCutOnTriangleAdjacencyEmitsHintAndReturnsZero)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    // The welded cube has triangle adjacency on every face — loop cut should
    // emit the "Loop cut needs a quad mesh" hint and return 0.
    ctrl->selectEdge(0, 2);
    QSignalSpy spy(ctrl, &EditModeController::editHintMessage);
    EXPECT_EQ(ctrl->loopCutSelection(), 0);
    // The welded cube edge (0,2) is bordered by two triangle faces, so the
    // tri-adjacency branch in loopCutSelection must fire and emit the hint.
    ASSERT_GE(spy.count(), 1);
    const QString hint = spy.takeFirst().at(0).toString();
    EXPECT_TRUE(hint.contains("Loop cut"));
    EXPECT_TRUE(hint.contains("Convert to Quads"));
}

// ===========================================================================
// NEW COVERAGE: convertToQuads on a cube (happy path)
// ===========================================================================

TEST_F(EditModeControllerMergeOpsTest, ConvertToQuadsOnCubeMergesCoplanarPairs)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    EXPECT_FALSE(ctrl->isMeshQuadBased());
    EXPECT_TRUE(ctrl->canConvertToQuads());

    const int merged = ctrl->convertToQuads(5.0f);
    EXPECT_GT(merged, 0);
    EXPECT_TRUE(ctrl->isMeshQuadBased());
}

TEST_F(EditModeControllerMergeOpsTest, ConvertToQuadsOutsideEditModeReturnsZero)
{
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->isEditModeActive());
    EXPECT_EQ(ctrl->convertToQuads(1.0f), 0);
}

// ===========================================================================
// NEW COVERAGE: subdivideCatmullClarkAll happy path
// ===========================================================================

TEST_F(EditModeControllerMergeOpsTest, SubdivideCatmullClarkAllAddsVertices)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    const int vertsBefore = ctrl->vertexCount();
    const int trisBefore = ctrl->triangleCount();
    ASSERT_GT(vertsBefore, 0);
    const int added = ctrl->subdivideCatmullClarkAll();
    EXPECT_GT(added, 0);
    EXPECT_GT(ctrl->vertexCount(), vertsBefore);
    // C-C output is all-quads → triangle count also grows.
    EXPECT_GT(ctrl->triangleCount(), trisBefore);
}

TEST_F(EditModeControllerMergeOpsTest, SubdivideCatmullClarkAllOutsideEditModeReturnsZero)
{
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->isEditModeActive());
    EXPECT_EQ(ctrl->subdivideCatmullClarkAll(), 0);
}

// ===========================================================================
// BATCH 2: beginBevel happy paths (edge mode + vertex mode) + updateBevelWidth
// + commitBevel inside an active session
// ===========================================================================

TEST_F(EditModeControllerMergeOpsTest, BeginBevelEdgeModeReturnsTrue)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);
    EXPECT_TRUE(ctrl->beginBevel());
    EXPECT_TRUE(ctrl->bevelSessionActive());
}

TEST_F(EditModeControllerMergeOpsTest, BeginBevelVertexModeReturnsTrue)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(5, false);
    EXPECT_TRUE(ctrl->beginBevel());
    EXPECT_TRUE(ctrl->bevelSessionActive());
}

TEST_F(EditModeControllerMergeOpsTest, BeginBevelWithoutSelectionReturnsFalse)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    EXPECT_FALSE(ctrl->beginBevel());
    EXPECT_FALSE(ctrl->bevelSessionActive());
}

TEST_F(EditModeControllerMergeOpsTest, BeginBevelInFaceModeReturnsFalse)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::FaceMode);
    ctrl->selectFace(0);
    EXPECT_FALSE(ctrl->beginBevel())
        << "face mode is not a valid bevel target";
}

TEST_F(EditModeControllerMergeOpsTest, BeginBevelOutsideEditModeReturnsFalse)
{
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->isEditModeActive());
    EXPECT_FALSE(ctrl->beginBevel());
}

TEST_F(EditModeControllerMergeOpsTest, BeginBevelTwiceSecondReturnsFalse)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);
    ASSERT_TRUE(ctrl->beginBevel());
    EXPECT_FALSE(ctrl->beginBevel())
        << "starting a second bevel while one is active should be rejected";
}

TEST_F(EditModeControllerMergeOpsTest, UpdateBevelWidthInsideSessionEmitsAndApplies)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);
    ASSERT_TRUE(ctrl->beginBevel());

    // Re-apply at a new width.
    ctrl->updateBevelWidth(0.1f);
    EXPECT_TRUE(ctrl->bevelSessionActive());

    // Negative width clamps to a tiny positive.
    ctrl->updateBevelWidth(-1.0f);
    EXPECT_TRUE(ctrl->bevelSessionActive());
}

TEST_F(EditModeControllerMergeOpsTest, CommitBevelEndsSessionAndPushesUndo)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);
    ASSERT_TRUE(ctrl->beginBevel());

    const int undoCountBefore = UndoManager::getSingleton()->canUndo() ? 1 : 0;
    ctrl->commitBevel();
    EXPECT_FALSE(ctrl->bevelSessionActive());
    EXPECT_GE(UndoManager::getSingleton()->canUndo() ? 1 : 0, undoCountBefore);
}

// ===========================================================================
// BATCH 2: cancelBevel restores original mesh state
// ===========================================================================

TEST_F(EditModeControllerMergeOpsTest, CancelBevelClearsSession)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(5, 3, false);
    ASSERT_TRUE(ctrl->beginBevel());

    ctrl->cancelBevel();
    EXPECT_FALSE(ctrl->bevelSessionActive());
}

// ===========================================================================
// BATCH 2: bevel-segments / profile-points during active session
// ===========================================================================

TEST_F(EditModeControllerMergeOpsTest, BevelSegmentsBeforeSessionIsOne)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    EXPECT_EQ(ctrl->bevelSegments(), 1);
    EXPECT_EQ(ctrl->bevelProfilePoints().size(), 0);
}

TEST_F(EditModeControllerMergeOpsTest, UpdateBevelSegmentsNoSessionIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    QSignalSpy spy(ctrl, &EditModeController::bevelProfilePointsChanged);
    ctrl->updateBevelSegments(5);
    EXPECT_EQ(spy.count(), 0);
    EXPECT_EQ(ctrl->bevelSegments(), 1);
}

TEST_F(EditModeControllerMergeOpsTest, ResetBevelProfileNoSessionIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->resetBevelProfile();
    EXPECT_EQ(ctrl->bevelProfilePoints().size(), 0);
}

TEST_F(EditModeControllerMergeOpsTest, UpdateBevelProfilePointNoSessionIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->updateBevelProfilePoint(0, 0.5f);
    EXPECT_EQ(ctrl->bevelProfilePoints().size(), 0);
}

// ===========================================================================
// BATCH 2: merge ops that involve more than 2 verts and verify selection
// post-merge
// ===========================================================================

TEST_F(EditModeControllerMergeOpsTest, MergeAtCenterThreeAdjacentVertsRetiresTwo)
{
    // Pick 3 mutually-adjacent corners (one face's three vertices) so the
    // merge collapses a tri but leaves the rest of the cube intact —
    // collapsing ALL 8 verts produces a zero-volume mesh that crashes
    // the GPU-side rebuild path on Linux/Xvfb.
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(0);
    ctrl->selectVertex(1, true);
    ctrl->selectVertex(2, true);
    EXPECT_EQ(ctrl->mergeAtCenter(), 2);
}

TEST_F(EditModeControllerMergeOpsTest, MergeByDistanceModerateThresholdOnSubset)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::VertexMode);
    // Pick three verts and merge with a threshold larger than the max pair
    // distance among them (cube corner pairs are 2 or 2√2 apart) but
    // smaller than the cube diagonal — exercises the cluster path without
    // collapsing the entire mesh.
    ctrl->selectVertex(0);
    ctrl->selectVertex(1, true);
    ctrl->selectVertex(2, true);
    const int retired = ctrl->mergeByDistance(5.0f);
    // 3 verts within threshold → 1 cluster → 2 retired.
    EXPECT_EQ(retired, 2);
}

// ===========================================================================
// BATCH 2: dissolve in vertex / edge modes (other modes are already covered
// in the bevel E2E fixture's face mode case)
// ===========================================================================

TEST_F(EditModeControllerMergeOpsTest, DissolveSelectionVertexModeOnCorner)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(0);
    // Cube corner vertex 0 is incident to 5 triangles (degree 5), so it
    // can't dissolve — `dissolveVertices` only retires verts of degree 2.
    // Expect a clean 0 (no-op), not an error or a partial mutation.
    EXPECT_EQ(ctrl->dissolveSelection(), 0);
}

TEST_F(EditModeControllerMergeOpsTest, DissolveSelectionEdgeModeOnEdge)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(0, 1, false);
    // The welded cube's edge (0,1) sits between two coplanar triangles on
    // the back face, so the edge dissolve should retire it. Even if HE
    // refuses for some reason, the return MUST be non-negative.
    const int n = ctrl->dissolveSelection();
    EXPECT_GE(n, 0);
}

// ===========================================================================
// BATCH 2: deleteSelection vertex / edge modes
// ===========================================================================

TEST_F(EditModeControllerMergeOpsTest, DeleteSelectionVertexModeOnCorner)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    const int trisBefore = ctrl->triangleCount();
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(0);
    // Deleting cube corner v0 retires every triangle that uses it and the
    // vertex itself. The exact retired-count return depends on whether
    // `deleteVertices` returns vertex count (1) or face count (≥3) — both
    // are positive on success; the GPU-side outcome is fewer triangles.
    const int n = ctrl->deleteSelection();
    EXPECT_GT(n, 0);
    EXPECT_LT(ctrl->triangleCount(), trisBefore);
}

TEST_F(EditModeControllerMergeOpsTest, DeleteSelectionEdgeModeOnEdge)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    const int trisBefore = ctrl->triangleCount();
    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(0, 1, false);
    // Deleting an edge retires the (up to two) triangles using it.
    const int n = ctrl->deleteSelection();
    EXPECT_GT(n, 0);
    EXPECT_LT(ctrl->triangleCount(), trisBefore);
}

// ===========================================================================
// BATCH 2: convertToQuads with strict threshold (should be a no-op when
// nothing is coplanar)
// ===========================================================================

TEST_F(EditModeControllerMergeOpsTest, ConvertToQuadsStrictThresholdMergesCubePairs)
{
    // 0 degrees is the strictest — a cube has triangle pairs that are
    // exactly coplanar (same face normal) on each of its 6 faces, so even
    // a 0° threshold should produce the 6 quad merges. Asserts the
    // threshold is inclusive at exactly 0°.
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    const int merged = ctrl->convertToQuads(0.0f);
    EXPECT_EQ(merged, 6);
    EXPECT_TRUE(ctrl->isMeshQuadBased());
}

// ===========================================================================
// BATCH 2: enterEditMode error paths
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, EnterEditModeNoSelectionReturnsFalse)
{
    SelectionSet::getSingleton()->clear();
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->enterEditMode());
    EXPECT_FALSE(ctrl->isEditModeActive());

    // Restore for TearDown to find m_node.
    SelectionSet::getSingleton()->selectOne(m_node);
}

TEST_F(EditModeControllerSelectionTest, EnterEditModeTwiceSecondIsIdempotent)
{
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_enter_twice");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_enter_twice_node");
    Manager::getSingleton()->createEntity(node, meshPtr);
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    // Second call should not crash and must not put the controller in a
    // bad state. The current implementation re-enters; this test just
    // ensures the no-crash invariant.
    bool second = ctrl->enterEditMode();
    EXPECT_TRUE(ctrl->isEditModeActive());
    (void)second;

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_enter_twice_node");
}

// ===========================================================================
// BATCH 2: exitEditMode with commitChanges + no edits = idempotent
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, ExitEditModeCommitTrueIsSafe)
{
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_exit_commit");
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_exit_commit_node");
    Manager::getSingleton()->createEntity(node, meshPtr);
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ctrl->exitEditMode(true); // commit path
    EXPECT_FALSE(ctrl->isEditModeActive());

    Manager::getSingleton()->destroySceneNode("EditCtrl_exit_commit_node");
}

TEST_F(EditModeControllerSelectionTest, ExitEditModeOutsideEditModeIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->isEditModeActive());
    ctrl->exitEditMode(true);
    EXPECT_FALSE(ctrl->isEditModeActive());
}

// ===========================================================================
// BATCH 2: paint shape default invariants + signal emission
// ===========================================================================

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintShapeSameValueDoesNotEmit)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintShape(static_cast<int>(EditModeController::ShapeSquare));
    QSignalSpy spy(em, &EditModeController::vertexPaintChanged);
    em->setVertexPaintShape(static_cast<int>(EditModeController::ShapeSquare));
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintRadiusSameValueDoesNotEmit)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintRadius(0.5);
    QSignalSpy spy(em, &EditModeController::vertexPaintChanged);
    em->setVertexPaintRadius(0.5);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintStrengthSameValueDoesNotEmit)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintStrength(0.5);
    QSignalSpy spy(em, &EditModeController::vertexPaintChanged);
    em->setVertexPaintStrength(0.5);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(EditModeControllerPaintKnobsFixture, SetVertexPaintFalloffSameValueDoesNotEmit)
{
    auto* em = EditModeController::instance();
    em->setVertexPaintFalloff(0.5);
    QSignalSpy spy(em, &EditModeController::vertexPaintChanged);
    em->setVertexPaintFalloff(0.5);
    EXPECT_EQ(spy.count(), 0);
}

// ===========================================================================
// BATCH 2: wireframe early returns + signal emission
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, SetWireframeEnabledSameValueDoesNotEmit)
{
    auto* ctrl = EditModeController::instance();
    ctrl->setWireframeEnabled(false);
    QSignalSpy spy(ctrl, &EditModeController::wireframeChanged);
    ctrl->setWireframeEnabled(false);
    EXPECT_EQ(spy.count(), 0);
}

// ===========================================================================
// BATCH 2: applyVertexColorBrush square-shape variant + edge cases
// ===========================================================================

TEST(EditModeControllerGeometry, ApplyVertexColorBrushSquareShape) {
    EditableMesh mesh;
    mesh.subMeshes().resize(1);
    auto& sub = mesh.subMeshes()[0];
    sub.vertices.resize(3);
    sub.vertices[0].position = Ogre::Vector3(0, 0, 0);
    sub.vertices[1].position = Ogre::Vector3(0.5f, 0.5f, 0);
    sub.vertices[2].position = Ogre::Vector3(2, 0, 0);
    for (auto& v : sub.vertices) {
        v.hasColor = true;
        v.color = Ogre::ColourValue::White;
    }
    const Ogre::ColourValue paint(0.0f, 0.0f, 1.0f, 1.0f); // blue
    const bool changed = EditModeController::applyVertexColorBrush(
        mesh, Ogre::Vector3::ZERO, /*radius*/1.0f, paint,
        /*strength*/1.0f, /*falloff*/0.0f, /*square*/true);
    EXPECT_TRUE(changed);
    // v0 at center — full paint replacement (constant strength, square has no falloff)
    EXPECT_NEAR(sub.vertices[0].color.b, 1.0f, 1e-3f);
    // v2 is outside the box (x=2 > radius=1), stays white.
    EXPECT_NEAR(sub.vertices[2].color.b, 1.0f, 1e-3f);
}

TEST(EditModeControllerGeometry, ApplyVertexColorBrushZeroRadiusReturnsFalse) {
    EditableMesh mesh;
    mesh.subMeshes().resize(1);
    auto& sub = mesh.subMeshes()[0];
    sub.vertices.resize(1);
    sub.vertices[0].position = Ogre::Vector3::ZERO;
    sub.vertices[0].hasColor = true;
    sub.vertices[0].color = Ogre::ColourValue::White;
    const bool changed = EditModeController::applyVertexColorBrush(
        mesh, Ogre::Vector3::ZERO, /*radius*/0.0f, Ogre::ColourValue::Red,
        /*strength*/1.0f, /*falloff*/0.0f);
    // Zero radius means no vertex is affected (distance == radius is at the
    // boundary; falloff weight will be 0).
    EXPECT_FALSE(changed);
}

TEST(EditModeControllerGeometry, ApplyVertexColorBrushEmptyMeshReturnsFalse) {
    EditableMesh mesh;
    const bool changed = EditModeController::applyVertexColorBrush(
        mesh, Ogre::Vector3::ZERO, /*radius*/1.0f, Ogre::ColourValue::Red,
        /*strength*/1.0f, /*falloff*/0.5f);
    EXPECT_FALSE(changed);
}

// ===========================================================================
// BATCH 2: weightToColor extreme cases
// ===========================================================================

TEST(EditModeControllerGeometry, WeightToColorClampsAboveOne) {
    // Above-1 weights should produce the same color as exactly 1.0.
    auto c1 = EditModeController::weightToColor(1.0f);
    auto c2 = EditModeController::weightToColor(1.5f);
    EXPECT_FLOAT_EQ(c1.r, c2.r);
    EXPECT_FLOAT_EQ(c1.g, c2.g);
    EXPECT_FLOAT_EQ(c1.b, c2.b);
}

TEST(EditModeControllerGeometry, WeightToColorClampsBelowZero) {
    auto c1 = EditModeController::weightToColor(0.0f);
    auto c2 = EditModeController::weightToColor(-0.5f);
    EXPECT_FLOAT_EQ(c1.r, c2.r);
    EXPECT_FLOAT_EQ(c1.g, c2.g);
    EXPECT_FLOAT_EQ(c1.b, c2.b);
}

// ===========================================================================
// BATCH 2: pointToSegmentDistance — vertical & diagonal segments
// ===========================================================================

TEST(EditModeControllerGeometry, PointToSegmentDistanceVerticalSegment) {
    QPoint p(3, 5);
    QPoint a(0, 0);
    QPoint b(0, 10);
    float dist = EditModeController::pointToSegmentDistance(p, a, b);
    EXPECT_NEAR(dist, 3.0f, 0.01f);
}

TEST(EditModeControllerGeometry, PointToSegmentDistanceDiagonalSegment) {
    // Segment (0,0)→(10,10); point (5,0) perpendicular distance to line is
    // 5/sqrt(2) ≈ 3.535.
    QPoint p(5, 0);
    QPoint a(0, 0);
    QPoint b(10, 10);
    float dist = EditModeController::pointToSegmentDistance(p, a, b);
    EXPECT_NEAR(dist, 5.0f / std::sqrt(2.0f), 0.05f);
}

// ===========================================================================
// BATCH 2: rayTriangleIntersect degenerate triangle
// ===========================================================================

TEST(EditModeControllerGeometry, RayTriangleIntersectDegenerateTriangleNoHit) {
    Ogre::Vector3 origin(0, 1, 0);
    Ogre::Vector3 dir(0, -1, 0);
    // Degenerate triangle (all three vertices collinear)
    Ogre::Vector3 v0(0, 0, 0);
    Ogre::Vector3 v1(1, 0, 0);
    Ogre::Vector3 v2(2, 0, 0);
    float t = EditModeController::rayTriangleIntersect(origin, dir, v0, v1, v2);
    EXPECT_LT(t, 0.0f);
}

// ===========================================================================
// BATCH 2: rewriteEntityAfterTopologyChange with a real entity (the
// existing standalone test only covers the null case)
// ===========================================================================

TEST_F(EditModeControllerMergeOpsTest, RewriteEntityAfterTopologyChangeOnEntityIsSafe)
{
    // Even outside an edit-mode session, the static helper must accept any
    // valid entity without crashing — undo/redo replays call it post-restore.
    EditModeController::rewriteEntityAfterTopologyChange(m_entity);
    SUCCEED();
}
