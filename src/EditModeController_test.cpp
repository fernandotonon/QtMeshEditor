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
    }
};

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
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_modelabel_node");
    auto* entity = sceneMgr->createEntity("EditCtrl_modelabel_ent", meshPtr);
    node->attachObject(entity);

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
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_vertsel_node");
    auto* entity = sceneMgr->createEntity("EditCtrl_vertsel_ent", meshPtr);
    node->attachObject(entity);

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
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_edgesel_node");
    auto* entity = sceneMgr->createEntity("EditCtrl_edgesel_ent", meshPtr);
    node->attachObject(entity);

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
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_facesel_node");
    auto* entity = sceneMgr->createEntity("EditCtrl_facesel_ent", meshPtr);
    node->attachObject(entity);

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
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_selectall_node");
    auto* entity = sceneMgr->createEntity("EditCtrl_selectall_ent", meshPtr);
    node->attachObject(entity);

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
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_exitclears_node");
    auto* entity = sceneMgr->createEntity("EditCtrl_exitclears_ent", meshPtr);
    node->attachObject(entity);

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
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_g2l_node");
    auto* entity = sceneMgr->createEntity("EditCtrl_g2l_ent", meshPtr);
    node->attachObject(entity);

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
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_gt2l_node");
    auto* entity = sceneMgr->createEntity("EditCtrl_gt2l_ent", meshPtr);
    node->attachObject(entity);

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
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_hitvert_null_node");
    auto* entity = sceneMgr->createEntity("EditCtrl_hitvert_null_ent", meshPtr);
    node->attachObject(entity);

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
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_hitface_null_node");
    auto* entity = sceneMgr->createEntity("EditCtrl_hitface_null_ent", meshPtr);
    node->attachObject(entity);

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
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_hitedge_null_node");
    auto* entity = sceneMgr->createEntity("EditCtrl_hitedge_null_ent", meshPtr);
    node->attachObject(entity);

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
// Signal emission tests
// ===========================================================================

TEST_F(EditModeControllerSelectionTest, SignalEmission) {
    auto meshPtr = createInMemoryTriangleMesh("EditCtrl_signals");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("EditCtrl_signals_node");
    auto* entity = sceneMgr->createEntity("EditCtrl_signals_ent", meshPtr);
    node->attachObject(entity);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();

    int selModeChanges = 0;
    int editSelChanges = 0;
    auto conn1 = QObject::connect(ctrl, &EditModeController::selectionModeChanged,
                                   [&]() { ++selModeChanges; });
    auto conn2 = QObject::connect(ctrl, &EditModeController::editSelectionChanged,
                                   [&]() { ++editSelChanges; });

    ASSERT_TRUE(ctrl->enterEditMode());

    ctrl->setSelectionMode(EditModeController::EdgeMode);
    EXPECT_EQ(selModeChanges, 1);

    ctrl->selectVertex(0);
    EXPECT_EQ(editSelChanges, 2); // 1 from enterEditMode + 1 from selectVertex

    ctrl->deselectAll();
    EXPECT_EQ(editSelChanges, 3);

    QObject::disconnect(conn1);
    QObject::disconnect(conn2);

    ctrl->exitEditMode(false);
    Manager::getSingleton()->destroySceneNode("EditCtrl_signals_node");
}
