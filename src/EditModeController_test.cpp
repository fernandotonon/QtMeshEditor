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
#include <Ogre.h>
#include <set>
#include <utility>
#include <cmath>

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
        if (!tryInitOgre()) {
            GTEST_SKIP() << "Ogre not available";
            return;
        }
        if (!canLoadMeshFiles()) {
            GTEST_SKIP() << "Cannot create hardware buffers";
            return;
        }
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
