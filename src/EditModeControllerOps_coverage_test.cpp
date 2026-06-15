/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

// ===========================================================================
// EditModeControllerOpsCoverage — coverage for the n-gon / quad-aware
// execution paths of EditModeController that the generic-topology tests in
// EditModeController_test.cpp do not reach.
//
// Survey-targeted branches:
//   * subdivideSelection() face-mode-on-quad dedup (selectedFacesAsHEFaceIndices
//     maps two fan-triangulated children of one quad to ONE HE face) and
//     edge-mode dilation across the SHARED interior edge of two quads.
//   * convertToQuads() "already quad-based, nothing to convert" → returns 0
//     branch, plus the outside-edit-mode early return.
//   * isMeshQuadBased() / canConvertToQuads() in-edit-mode TRUE branches on a
//     genuinely quad-based mesh (only the outside-edit-mode FALSE branch is
//     covered elsewhere).
//   * loopCutSelection() controller-level success on a real quad mesh that
//     pushes an undo command and grows the mesh.
//   * subdivideCatmullClarkAll() vertex/face growth + undo-command push
//     assertion (the existing test only asserts growth, not undo).
//
// These exercise the real HalfEdgeMesh forwarding + EditMeshTopologyCommand
// undo push + rewriteEntityAfterTopologyChange paths that the unit-level HE
// tests cannot reach.
//
// Distinct suite names (EditModeControllerOpsCoverage*) avoid any ODR /
// duplicate-registration clash with EditModeController_test.cpp.
// ===========================================================================

#include <gtest/gtest.h>
#include "EditModeController.h"
#include "EditableMesh.h"
#include "TestHelpers.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "UndoManager.h"
#include <Ogre.h>
#include <QSignalSpy>
#include <string>

// ---------------------------------------------------------------------------
// Fixture: a welded cube attached + selected + entered into edit mode. The
// per-test SetUp builds a unique mesh/node so reruns don't collide in Ogre's
// resource registry. EditModeController::kill() in TearDown gives every test a
// fresh controller (matching the EditModeControllerMergeOpsTest pattern in the
// sibling file).
// ---------------------------------------------------------------------------
class EditModeControllerOpsCoverage : public ::testing::Test {
protected:
    Ogre::SceneNode* m_node = nullptr;
    Ogre::Entity* m_entity = nullptr;
    std::string m_meshName;
    std::string m_nodeName;

    void SetUp() override {
        ASSERT_TRUE(tryInitOgre()) << "Ogre not available (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles()) << "Cannot create hardware buffers (Xvfb/GL required)";
        createStandardOgreMaterials();

        static int counter = 0;
        ++counter;
        m_meshName = "EMOpsCov_cube_" + std::to_string(counter);
        m_nodeName = "EMOpsCov_node_" + std::to_string(counter);

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
        UndoManager::getSingleton()->clear();
        EditModeController::kill();
    }

    // Convert the live triangle cube into a quad-based mesh in edit mode.
    // Returns the merged-pair count (must be > 0 for the cube layout).
    int makeQuadBased(EditModeController* ctrl) {
        const int merged = ctrl->convertToQuads(5.0f);
        EXPECT_GT(merged, 0) << "cube → quads must merge at least one pair";
        return merged;
    }
};

// ===========================================================================
// isMeshQuadBased() / canConvertToQuads() — in-edit-mode TRUE branches.
//
// The sibling file covers (a) outside-edit-mode FALSE and (b) triangle-only
// in-edit-mode (isMeshQuadBased == false, canConvertToQuads == true). Here we
// drive the genuinely-quad-based branches: after convertToQuads, every face
// has an n-gon `.faces` entry, so isMeshQuadBased flips true and
// canConvertToQuads flips false ("nothing left to promote").
// ===========================================================================

TEST_F(EditModeControllerOpsCoverage, QuadBasedMeshReportsQuadBasedTrueInEditMode)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    // Pre-conversion: triangle-only.
    EXPECT_FALSE(ctrl->isMeshQuadBased());
    EXPECT_TRUE(ctrl->canConvertToQuads());

    makeQuadBased(ctrl);

    // Post-conversion: fully n-gon.
    EXPECT_TRUE(ctrl->isMeshQuadBased())
        << "every face now has an n-gon binding";
    EXPECT_FALSE(ctrl->canConvertToQuads())
        << "a fully-quad mesh has nothing left to promote";
}

// ===========================================================================
// convertToQuads() — "already quad-based, nothing to convert" returns zero.
//
// First convertToQuads succeeds; a second call on the now-quad mesh must
// short-circuit (no triangle pairs left) and return 0 without pushing undo.
// ===========================================================================

TEST_F(EditModeControllerOpsCoverage, ConvertToQuadsSecondPassReturnsZero)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    const int first = makeQuadBased(ctrl);
    EXPECT_GT(first, 0);
    ASSERT_TRUE(ctrl->isMeshQuadBased());

    // Undo stack depth grows by exactly one for the first (real) conversion.
    auto* undo = UndoManager::getSingleton();
    const int depthAfterFirst = undo->stack()->index();

    // Second pass: mesh is already quad based → nothing to convert → 0,
    // and no new undo command is pushed.
    const int second = ctrl->convertToQuads(5.0f);
    EXPECT_EQ(second, 0) << "already-quad mesh: nothing to convert";
    EXPECT_TRUE(ctrl->isMeshQuadBased());
    EXPECT_EQ(undo->stack()->index(), depthAfterFirst)
        << "no-op convertToQuads must not push an undo command";
}

TEST_F(EditModeControllerOpsCoverage, ConvertToQuadsOutsideEditModeReturnsZero)
{
    auto* ctrl = EditModeController::instance();
    EXPECT_FALSE(ctrl->isEditModeActive());
    // Outside edit mode there is no editable mesh → early return 0.
    EXPECT_EQ(ctrl->convertToQuads(5.0f), 0);
    EXPECT_EQ(ctrl->convertToQuads(0.0f), 0);
}

// ===========================================================================
// convertToQuads() pushes exactly one undo command on success.
// ===========================================================================

TEST_F(EditModeControllerOpsCoverage, ConvertToQuadsPushesUndoCommand)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    auto* undo = UndoManager::getSingleton();
    const int before = undo->stack()->index();

    const int merged = ctrl->convertToQuads(5.0f);
    EXPECT_GT(merged, 0);
    EXPECT_TRUE(undo->canUndo());
    EXPECT_EQ(undo->stack()->index(), before + 1)
        << "a successful convertToQuads pushes exactly one command";
}

// ===========================================================================
// subdivideSelection() FACE mode on a QUAD — the n-gon dedup branch.
//
// After convertToQuads, the back face (cube tris 0,2,1 / 1,2,3) is a single
// HE quad fan-triangulated into 2 child triangles. Selecting EITHER child
// triangle (or both) must map to ONE HE face via selectedFacesAsHEFaceIndices
// and subdivide that single quad — not double-count it. We assert the dedup
// directly (1 unique HE face for the two children) and that subdivide grows
// the mesh + pushes undo.
// ===========================================================================

TEST_F(EditModeControllerOpsCoverage, SubdivideFaceOnQuadDedupsTrianglesToOneHEFace)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    makeQuadBased(ctrl);

    ctrl->setSelectionMode(EditModeController::FaceMode);

    // Select both fan-triangulated children of the first quad. The cube's
    // back face is the first two triangles (0 and 1).
    ctrl->selectFace(0, false);
    ctrl->selectFace(1, true);
    EXPECT_EQ(ctrl->selectedFaceCount(), 2)
        << "two child triangles selected";

    // The dedup contract: both children map to a single HE face.
    auto heFaces = ctrl->selectedFacesAsHEFaceIndices();
    EXPECT_EQ(heFaces.size(), 1u)
        << "two children of one quad must dedup to one HE face index";

    const int vertsBefore = ctrl->vertexCount();
    const int trisBefore = ctrl->triangleCount();

    auto* undo = UndoManager::getSingleton();
    const int undoBefore = undo->stack()->index();

    const int changed = ctrl->subdivideSelection();
    EXPECT_GT(changed, 0) << "subdividing the quad must change topology";
    EXPECT_GT(ctrl->vertexCount(), vertsBefore);
    EXPECT_GT(ctrl->triangleCount(), trisBefore);
    EXPECT_EQ(undo->stack()->index(), undoBefore + 1)
        << "subdivide pushes exactly one undo command";
}

TEST_F(EditModeControllerOpsCoverage, SubdivideFaceSingleChildTriangleStillSubdividesWholeQuad)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    makeQuadBased(ctrl);

    ctrl->setSelectionMode(EditModeController::FaceMode);
    // Select just ONE child triangle of the quad — it still resolves to the
    // single owning HE face (dedup branch with a single entry).
    ctrl->selectFace(0, false);
    EXPECT_EQ(ctrl->selectedFaceCount(), 1);

    auto heFaces = ctrl->selectedFacesAsHEFaceIndices();
    EXPECT_EQ(heFaces.size(), 1u);

    const int vertsBefore = ctrl->vertexCount();
    const int changed = ctrl->subdivideSelection();
    EXPECT_GT(changed, 0);
    EXPECT_GT(ctrl->vertexCount(), vertsBefore);
}

// ===========================================================================
// subdivideSelection() EDGE mode dilation across the SHARED interior edge of
// two quads.
//
// After convertToQuads, edge (1,2) is the interior diagonal that the two back-
// face tris merged across — but more importantly any boundary edge shared
// between two quad faces drives the "incident faces" dilation. We select an
// edge of the cube and assert the edge-mode subdivide grows the mesh + pushes
// a single undo command (Blender convention: subdividing an edge subdivides
// every face incident to it).
// ===========================================================================

TEST_F(EditModeControllerOpsCoverage, SubdivideEdgeModeDilatesAcrossIncidentQuads)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    makeQuadBased(ctrl);

    ctrl->setSelectionMode(EditModeController::EdgeMode);
    // Edge (0,1) on the cube borders two faces (back + bottom). In the quad
    // mesh it is a shared edge between two quad faces, exercising the multi-
    // incident-face dilation branch.
    ctrl->selectEdge(0, 1, false);
    EXPECT_EQ(ctrl->selectedEdgeCount(), 1);

    const int vertsBefore = ctrl->vertexCount();
    const int trisBefore = ctrl->triangleCount();

    auto* undo = UndoManager::getSingleton();
    const int undoBefore = undo->stack()->index();

    const int changed = ctrl->subdivideSelection();
    EXPECT_GT(changed, 0)
        << "edge-mode subdivide must dilate to incident faces and change topology";
    EXPECT_GT(ctrl->vertexCount(), vertsBefore);
    EXPECT_GT(ctrl->triangleCount(), trisBefore);
    EXPECT_EQ(undo->stack()->index(), undoBefore + 1)
        << "subdivide pushes exactly one undo command";
}

TEST_F(EditModeControllerOpsCoverage, SubdivideVertexModeIsNoOp)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    makeQuadBased(ctrl);

    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(0);
    // Vertex selection alone doesn't define faces to split.
    EXPECT_EQ(ctrl->subdivideSelection(), 0);
}

// ===========================================================================
// loopCutSelection() — controller-level SUCCESS on a real quad mesh.
//
// The sibling file only covers the rejections (wrong mode, empty selection,
// triangle-adjacency hint). Here, after convertToQuads the cube faces are
// quads, so a loop cut starting from a quad boundary edge can walk the quad
// ring and insert new vertices, push one undo command, and grow the mesh.
// ===========================================================================

TEST_F(EditModeControllerOpsCoverage, LoopCutOnQuadMeshGrowsMeshAndPushesUndo)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    makeQuadBased(ctrl);
    ASSERT_TRUE(ctrl->isMeshQuadBased());

    ctrl->setSelectionMode(EditModeController::EdgeMode);

    auto* undo = UndoManager::getSingleton();

    // Try each cube boundary edge as a loop-cut seed until one walks a quad
    // ring successfully. The cube's 8 corner verts give a small candidate set;
    // a closed cube has loops around it so at least one seed must succeed.
    static const std::pair<int,int> candidates[] = {
        {0,1},{1,3},{2,3},{0,2},{4,5},{5,7},{6,7},{4,6},
        {0,6},{1,7},{2,4},{3,5},
    };

    int inserted = 0;
    int undoBefore = 0;
    bool succeeded = false;
    for (const auto& e : candidates) {
        ctrl->deselectAll();
        ctrl->selectEdge(e.first, e.second, false);
        if (ctrl->selectedEdgeCount() == 0)
            continue;
        const int vertsBefore = ctrl->vertexCount();
        undoBefore = undo->stack()->index();
        inserted = ctrl->loopCutSelection();
        if (inserted > 0) {
            EXPECT_GT(ctrl->vertexCount(), vertsBefore)
                << "a successful loop cut inserts new vertices";
            EXPECT_EQ(undo->stack()->index(), undoBefore + 1)
                << "loop cut pushes exactly one undo command";
            succeeded = true;
            break;
        }
    }

    ASSERT_TRUE(succeeded)
        << "at least one boundary edge of the quad cube must seed a loop cut";
    EXPECT_GT(inserted, 0);
}

// ===========================================================================
// subdivideCatmullClarkAll() — vertex/face growth AND undo-command push.
//
// The sibling test asserts growth only. Here we additionally assert that the
// op pushes exactly one undo command (the "Catmull-Clark Subdivide" command),
// that the result is reversible via undo, and that selection is cleared.
// ===========================================================================

TEST_F(EditModeControllerOpsCoverage, CatmullClarkAllPushesUndoAndGrowsMesh)
{
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());

    // Pre-select something so we can assert the post-op clear.
    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->selectVertex(0);
    EXPECT_EQ(ctrl->selectedVertexCount(), 1);

    const int vertsBefore = ctrl->vertexCount();
    const int trisBefore = ctrl->triangleCount();
    ASSERT_GT(vertsBefore, 0);

    auto* undo = UndoManager::getSingleton();
    const int undoBefore = undo->stack()->index();

    const int added = ctrl->subdivideCatmullClarkAll();
    EXPECT_GT(added, 0);
    EXPECT_GT(ctrl->vertexCount(), vertsBefore);
    EXPECT_GT(ctrl->triangleCount(), trisBefore);

    // Selection is cleared after the op (new face/edge points have no stable
    // analogue in the pre-op selection set).
    EXPECT_EQ(ctrl->selectedVertexCount(), 0);

    // Exactly one undo command was pushed and it is undoable.
    EXPECT_EQ(undo->stack()->index(), undoBefore + 1)
        << "Catmull-Clark pushes exactly one undo command";
    ASSERT_TRUE(undo->canUndo());

    // Undo restores the original vertex count (reversibility of the command).
    undo->undo();
    EXPECT_EQ(ctrl->vertexCount(), vertsBefore)
        << "undo of Catmull-Clark restores the pre-op vertex count";
}

TEST_F(EditModeControllerOpsCoverage, CatmullClarkAllOnQuadMeshAlsoGrows)
{
    // Same op but on an already-quad-based mesh: every quad becomes 4 quads.
    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    makeQuadBased(ctrl);

    const int vertsBefore = ctrl->vertexCount();
    const int added = ctrl->subdivideCatmullClarkAll();
    EXPECT_GT(added, 0);
    EXPECT_GT(ctrl->vertexCount(), vertsBefore);
    // C-C output stays all-quads.
    EXPECT_TRUE(ctrl->isMeshQuadBased());
}
