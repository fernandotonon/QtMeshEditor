/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

#include "EditModeController.h"
#include "EditableMesh.h"
#include "HalfEdgeMesh.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/TransformCommands.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "NormalVisualizer.h"
#include "BevelGizmo.h"
#include "mainwindow.h"
#include "OgreWidget.h"
#include "SpaceCamera.h"
#include <Ogre.h>
#include <OgreRTShaderSystem.h>
#include <ProceduralSphereGenerator.h>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <limits>

namespace {

// Walk-guard budget: a manifold vertex's fan will close well under this.
constexpr int kHERotateMaxSteps = 1024;

// Rotate one step around a vertex along its outgoing half-edge fan:
// `he` → prev → twin. Returns -1 when the rotation hits a boundary
// (no twin) or when the walker loops back to `startHE`. Callers use
// the -1 sentinel as a "stop iterating" signal, which flattens what
// would otherwise be a nested prev/twin/sentinel triple in every
// half-edge fan walker.
int nextAroundVertex(const HalfEdgeMesh& hm, int he, int startHE)
{
    if (he < 0) return -1;
    int prev = hm.halfEdge(he).prev;
    if (prev < 0) return -1;
    int twin = hm.halfEdge(prev).twin;
    if (twin < 0 || twin == startHE) return -1;
    return twin;
}

// Per-vertex max-width for a prospective multi-vertex bevel. Mirrors
// HalfEdgeMesh::bevelVertices's pre-budget formula so the drag gizmo
// caps at the same scalar the algorithm itself will apply:
//   - shared edges (both endpoints selected) → 0.999 × edgeLen × 0.5
//   - unshared edges → 0.499 × edgeLen (the single-vertex safety clamp)
// The min across all incident edges of a target is that vertex's cap;
// the min across all targets is the session cap.
float computeVertexBevelCap(const HalfEdgeMesh& hm,
                            const std::vector<int>& targets)
{
    float cap = std::numeric_limits<float>::infinity();
    std::set<int> selected(targets.begin(), targets.end());

    for (int v : targets) {
        if (v < 0 || v >= static_cast<int>(hm.vertexCount())) continue;
        int startHE = hm.vertex(v).halfEdge;
        if (startHE < 0) continue;

        int he = startHE;
        for (int guard = 0; he >= 0 && guard < kHERotateMaxSteps; ++guard) {
            const int n = hm.halfEdge(he).vertex;
            const float edgeLen =
                hm.vertex(v).position.distance(hm.vertex(n).position);
            const float budget = selected.count(n)
                                 ? edgeLen * 0.999f * 0.5f
                                 : edgeLen * 0.499f;
            if (budget < cap) cap = budget;
            he = nextAroundVertex(hm, he, startHE);
        }
    }
    return cap;
}

// Shrink `shortestAdj` against one of the two faces adjacent to the
// (va, vb) edge. Walks outgoing half-edges from `va` looking for one
// where `.vertex == vb`; when found, picks the face's opposite
// vertex (the third of a triangle) and clamps `shortestAdj` by both
// va→opp and vb→opp. Returns immediately after a hit — call this
// twice, once from each endpoint, to cover both adjacent faces.
void shrinkEdgeBevelAdjacency(const HalfEdgeMesh& hm,
                              int va, int vb,
                              float& shortestAdj)
{
    int startHE = hm.vertex(va).halfEdge;
    if (startHE < 0) return;

    int he = startHE;
    for (int guard = 0; he >= 0 && guard < kHERotateMaxSteps; ++guard) {
        if (hm.halfEdge(he).vertex == vb) {
            // Found the va→vb HE; scan the face's other HEs for the
            // opposite vertex (the non-va, non-vb one).
            for (int loopHE = hm.halfEdge(he).next;
                 loopHE != he;
                 loopHE = hm.halfEdge(loopHE).next) {
                const int target = hm.halfEdge(loopHE).vertex;
                if (target == va || target == vb) continue;
                shortestAdj = std::min(shortestAdj,
                    hm.vertex(va).position.distance(hm.vertex(target).position));
                shortestAdj = std::min(shortestAdj,
                    hm.vertex(vb).position.distance(hm.vertex(target).position));
                break;
            }
            return;
        }
        he = nextAroundVertex(hm, he, startHE);
    }
}

// Per-edge max-width for a prospective multi-edge bevel. Mirrors
// HalfEdgeMesh::bevelEdges::effectiveWidth — 0.4 × shortestAdj, where
// shortestAdj is the min of (edge length, va→opp, vb→opp) across both
// adjacent faces.
float computeEdgeBevelCap(const HalfEdgeMesh& hm,
                          const std::vector<std::pair<int,int>>& targets)
{
    float cap = std::numeric_limits<float>::infinity();

    for (const auto& [v1, v2] : targets) {
        if (v1 < 0 || v2 < 0) continue;
        if (v1 >= static_cast<int>(hm.vertexCount())) continue;
        if (v2 >= static_cast<int>(hm.vertexCount())) continue;

        float shortestAdj = hm.vertex(v1).position.distance(hm.vertex(v2).position);
        // v1 → v2 finds f1; v2 → v1 finds f2.
        shrinkEdgeBevelAdjacency(hm, v1, v2, shortestAdj);
        shrinkEdgeBevelAdjacency(hm, v2, v1, shortestAdj);

        const float budget = shortestAdj * 0.4f;
        if (budget < cap) cap = budget;
    }
    return cap;
}

} // namespace

EditModeController* EditModeController::m_pSingleton = nullptr;

EditModeController::EditModeController()
    : QObject(nullptr)
{
    // Track selection changes to update canEnterEditMode and auto-exit
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &EditModeController::onSelectionChanged);
}

EditModeController::~EditModeController()
{
    // Exit cleanly if still in edit mode. Swallow any exception: a
    // destructor must never propagate, and the process is ending anyway.
    if (m_editModeActive) {
        try {
            exitEditMode(false);
        } catch (...) {
            // Best-effort cleanup during shutdown.
        }
    }
}

EditModeController* EditModeController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new EditModeController();
    return m_pSingleton;
}

EditModeController* EditModeController::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine);
    Q_UNUSED(scriptEngine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void EditModeController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

QString EditModeController::modeLabel() const
{
    if (!m_editModeActive)
        return QStringLiteral("Object Mode");

    switch (m_selectionMode) {
    case VertexMode: return QStringLiteral("Edit Mode (Vertex)");
    case EdgeMode:   return QStringLiteral("Edit Mode (Edge)");
    case FaceMode:   return QStringLiteral("Edit Mode (Face)");
    }
    return QStringLiteral("Edit Mode");
}

bool EditModeController::canEnterEditMode() const
{
    // Edit mode requires exactly one entity selected (either directly or via node)
    auto* sel = SelectionSet::getSingleton();
    QList<Ogre::Entity*> entities = sel->getResolvedEntities();
    return entities.size() == 1;
}

int EditModeController::vertexCount() const
{
    if (!m_editableMesh) return 0;
    return static_cast<int>(m_editableMesh->totalVertexCount());
}

int EditModeController::triangleCount() const
{
    if (!m_editableMesh) return 0;
    return static_cast<int>(m_editableMesh->totalTriangleCount());
}

int EditModeController::subMeshCount() const
{
    if (!m_editableMesh) return 0;
    return static_cast<int>(m_editableMesh->subMeshCount());
}

void EditModeController::toggleEditMode()
{
    if (m_editModeActive)
        exitEditMode(true);
    else
        enterEditMode();
}

// Try the n-gon-aware re-import path: when the entity's mesh has the
// `qtme.source_path` user-binding (cached by MeshImporterExporter at
// import time AND not yet wiped by a topology mutation), re-read the
// source asset through Assimp with aiProcess_Triangulate disabled so
// source quads survive into EditableSubMesh::faces. The cached
// `qtme.source_convert_lh` and `qtme.source_up_axis` keys make the
// editable mesh share basis with the rendered buffers; passing the
// live skeleton makes bone handles match MeshProcessor's convention.
// Returns true on success (caller uses the editable mesh as-is),
// false to signal that the legacy `loadFromEntity` path should run.
static bool tryLoadEditableMeshNGonPath(
    Ogre::Entity* entity, EditableMesh* editableMesh)
{
    if (!entity || !editableMesh) return false;
    const Ogre::MeshPtr meshPtr = entity->getMesh();
    if (!meshPtr) return false;

    const auto& bindings = meshPtr->getUserObjectBindings();
    const Ogre::Any& any = bindings.getUserAny("qtme.source_path");
    if (!any.has_value()) return false;

    std::string sourcePath;
    try {
        sourcePath = Ogre::any_cast<std::string>(any);
    } catch (const Ogre::Exception&) {
        return false; // Any held the wrong type — fall back defensively.
    }
    if (sourcePath.empty()) return false;

    // Default: convert-LH=true matches AssimpToOgreImporter's behaviour
    // for unknown origins; up-axis=Y-up is the safe default if the
    // import didn't cache one.
    bool convertLH = true;
    const Ogre::Any& lhAny = bindings.getUserAny("qtme.source_convert_lh");
    if (lhAny.has_value()) {
        try { convertLH = Ogre::any_cast<bool>(lhAny); }
        catch (const Ogre::Exception&) {}
    }
    bool isZup = false;
    const Ogre::Any& upAny = bindings.getUserAny("qtme.source_up_axis");
    if (upAny.has_value()) {
        try { isZup = (Ogre::any_cast<int>(upAny) == 2); }
        catch (const Ogre::Exception&) {}
    }
    const Ogre::Skeleton* skel =
        meshPtr->hasSkeleton() ? meshPtr->getSkeleton().get() : nullptr;
    return editableMesh->loadFromAssimpFile(sourcePath, convertLH, isZup, skel);
}

bool EditModeController::enterEditMode()
{
    if (m_editModeActive)
        return true; // Already in edit mode

    if (!canEnterEditMode()) {
        SentryReporter::addBreadcrumb("edit_mode", "Cannot enter Edit Mode: no single entity selected");
        return false;
    }

    auto* sel = SelectionSet::getSingleton();
    QList<Ogre::Entity*> entities = sel->getResolvedEntities();
    m_editEntity = entities.first();

    // Decompose mesh into editable data. Prefer the n-gon path (re-
    // import via Assimp with aiProcess_Triangulate off) when the mesh
    // still carries qtme.source_path. Fall back to the legacy
    // loadFromEntity path for procedural primitives, .scene.glb sub-
    // entities, and post-edit re-entries (where commitToEntity /
    // resizeEntityBuffers wipe the cache on mutation). The n-gon path
    // is what enables Catmull-Clark subdivide / loop cut / future
    // quad-aware ops to act on real source quads. (Quad migration
    // #326, chunk 4.)
    m_editableMesh = std::make_unique<EditableMesh>();
    bool loaded = tryLoadEditableMeshNGonPath(m_editEntity, m_editableMesh.get());
    if (loaded) {
        SentryReporter::addBreadcrumb("edit_mode",
            "Edit Mode entered via n-gon import path");
    }
    if (!loaded && !m_editableMesh->loadFromEntity(m_editEntity)) {
        SentryReporter::addBreadcrumb("edit_mode", "Failed to load mesh data for Edit Mode");
        m_editableMesh.reset();
        m_editEntity = nullptr;
        return false;
    }

    m_editModeActive = true;

    // Reset selection state
    m_selectionMode = VertexMode;
    m_selectedVertices.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    // Create overlay materials and initial (empty) overlays
    createOverlayMaterials();

    // Re-apply wireframe if it was enabled in a previous session
    if (m_wireframeEnabled)
        applyWireframeMaterials();

    SentryReporter::addBreadcrumb("edit_mode",
        QString("Entered Edit Mode: %1 vertices, %2 triangles, %3 submeshes")
            .arg(m_editableMesh->totalVertexCount())
            .arg(m_editableMesh->totalTriangleCount())
            .arg(m_editableMesh->subMeshCount()));

    // Run initial validation
    validateMesh();

    emit editModeChanged();
    emit meshDataChanged();
    emit selectionModeChanged();
    emit editSelectionChanged();

    return true;
}

void EditModeController::exitEditMode(bool commitChanges)
{
    if (!m_editModeActive)
        return;

    // An active bevel session is committed before exiting (or cancelled if
    // the caller is discarding changes).
    if (m_bevelSession.active) {
        if (commitChanges) commitBevel();
        else cancelBevel();
    }
    // Knife session is always abandoned on exit — committing implicitly
    // would be surprising mid-cut.
    if (m_knifeSession.active) cancelKnife();

    if (commitChanges && m_editableMesh && m_editEntity) {
        bool ok = m_editableMesh->commitToEntity(m_editEntity);
        refreshNormalVisualizer();
        SentryReporter::addBreadcrumb("edit_mode",
            ok ? "Exited Edit Mode: changes committed"
               : "Exited Edit Mode: commit failed");
    } else {
        SentryReporter::addBreadcrumb("edit_mode", "Exited Edit Mode: changes discarded");
    }

    // Restore original materials (keep m_wireframeEnabled so it persists)
    if (!m_savedMaterials.empty())
        removeWireframeMaterials();

    // Clean up selection overlays
    destroySelectionOverlay();

    // Clear selection state
    m_selectedVertices.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    m_editableMesh.reset();
    m_editEntity = nullptr;
    m_editModeActive = false;

    // Reset validation state for next session
    m_degenerateTriangleCount = 0;

    emit editModeChanged();
    emit meshDataChanged();
    emit editSelectionChanged();
    emit validationChanged();
}

void EditModeController::onSelectionChanged()
{
    // If selection changes while in edit mode and the edited entity is no
    // longer selected, auto-exit edit mode (committing changes).
    if (m_editModeActive && m_editEntity) {
        auto* sel = SelectionSet::getSingleton();
        QList<Ogre::Entity*> entities = sel->getResolvedEntities();
        bool stillSelected = entities.contains(m_editEntity);
        if (!stillSelected) {
            exitEditMode(true);
        }
    }

    emit selectionStateChanged();
}

// ===========================================================================
// Selection mode
// ===========================================================================

void EditModeController::setSelectionMode(int mode)
{
    if (mode < 0 || mode > 2)
        return;

    auto newMode = static_cast<SelectionMode>(mode);
    if (m_selectionMode == newMode)
        return;

    m_selectionMode = newMode;

    // Clear selection from previous mode
    m_selectedVertices.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    QString modeName;
    switch (m_selectionMode) {
    case VertexMode: modeName = "Vertex"; break;
    case EdgeMode:   modeName = "Edge"; break;
    case FaceMode:   modeName = "Face"; break;
    }
    SentryReporter::addBreadcrumb("edit_mode",
        QString("Selection mode changed to %1").arg(modeName));

    // Update overlay to show the correct selection type
    updateSelectionOverlay();

    emit selectionModeChanged();
    emit editModeChanged(); // modeLabel changed
}

// ===========================================================================
// Selection operations
// ===========================================================================

void EditModeController::selectAll()
{
    if (!m_editModeActive || !m_editableMesh)
        return;

    SentryReporter::addBreadcrumb("edit_mode", "Select All");

    int totalVerts = static_cast<int>(m_editableMesh->totalVertexCount());
    int totalTris = static_cast<int>(m_editableMesh->totalTriangleCount());

    // Select all vertices
    m_selectedVertices.clear();
    for (int i = 0; i < totalVerts; ++i)
        m_selectedVertices.insert(i);

    // Select all edges (from all triangles)
    m_selectedEdges.clear();
    const auto& subMeshes = m_editableMesh->subMeshes();
    int globalTriOffset = 0;
    for (size_t si = 0; si < subMeshes.size(); ++si) {
        int vertexOffset = localToGlobal(si, 0);
        for (const auto& tri : subMeshes[si].triangles) {
            int g0 = vertexOffset + static_cast<int>(tri.indices[0]);
            int g1 = vertexOffset + static_cast<int>(tri.indices[1]);
            int g2 = vertexOffset + static_cast<int>(tri.indices[2]);
            m_selectedEdges.insert({std::min(g0, g1), std::max(g0, g1)});
            m_selectedEdges.insert({std::min(g1, g2), std::max(g1, g2)});
            m_selectedEdges.insert({std::min(g0, g2), std::max(g0, g2)});
        }
        globalTriOffset += static_cast<int>(subMeshes[si].triangles.size());
    }

    // Select all faces
    m_selectedFaces.clear();
    for (int i = 0; i < totalTris; ++i)
        m_selectedFaces.insert(i);

    updateSelectionOverlay();
    emit editSelectionChanged();
}

void EditModeController::deselectAll()
{
    if (!m_editModeActive)
        return;

    SentryReporter::addBreadcrumb("edit_mode", "Deselect All");

    m_selectedVertices.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    updateSelectionOverlay();
    emit editSelectionChanged();
}

void EditModeController::selectVertex(int globalIndex, bool addToSelection)
{
    if (!m_editModeActive || !m_editableMesh)
        return;

    if (!addToSelection)
        m_selectedVertices.clear();

    if (globalIndex < 0 || globalIndex >= static_cast<int>(m_editableMesh->totalVertexCount()))
        return;

    m_selectedVertices.insert(globalIndex);
    updateSelectionOverlay();
    emit editSelectionChanged();
}

void EditModeController::deselectVertex(int globalIndex)
{
    if (m_selectedVertices.erase(globalIndex) > 0) {
        updateSelectionOverlay();
        emit editSelectionChanged();
    }
}

void EditModeController::selectEdge(int v1, int v2, bool addToSelection)
{
    if (!m_editModeActive || !m_editableMesh)
        return;

    auto edge = std::make_pair(std::min(v1, v2), std::max(v1, v2));

    if (!addToSelection) {
        m_selectedEdges.clear();
        m_selectedVertices.clear();
    }

    m_selectedEdges.insert(edge);
    // Also select the endpoint vertices
    m_selectedVertices.insert(edge.first);
    m_selectedVertices.insert(edge.second);

    updateSelectionOverlay();
    emit editSelectionChanged();
}

void EditModeController::deselectEdge(int v1, int v2)
{
    auto edge = std::make_pair(std::min(v1, v2), std::max(v1, v2));
    if (m_selectedEdges.erase(edge) > 0) {
        // Note: we don't remove vertices here as they may be part of other selected edges
        updateSelectionOverlay();
        emit editSelectionChanged();
    }
}

void EditModeController::selectFace(int triIndex, bool addToSelection)
{
    if (!m_editModeActive || !m_editableMesh)
        return;
    if (triIndex < 0 || triIndex >= static_cast<int>(m_editableMesh->totalTriangleCount()))
        return;

    if (!addToSelection) {
        m_selectedFaces.clear();
        m_selectedVertices.clear();
        m_selectedEdges.clear();
    }

    auto [subIdx, localTri] = globalTriToLocal(triIndex);
    if (subIdx >= m_editableMesh->subMeshes().size()) return;
    const auto& sub = m_editableMesh->subMeshes()[subIdx];

    // n-gon dilation: when the clicked triangle belongs to a face that
    // has more than one fan triangle (a quad / N-gon), select EVERY
    // fan triangle of that face so the user-visible "face" is the
    // whole polygon — not just one half of a quad. Storage stays as
    // global triangle indices for backward compat with delete /
    // dissolve / undo serialization; downstream ops that need the
    // HE-level face dedup it via faceIndexForTriangle. (Chunk 4b.)
    size_t faceFirstTri = localTri, faceTriCount = 1;
    const int faceK = faceIndexForTriangle(sub, localTri,
                                           &faceFirstTri, &faceTriCount);

    const int vertOffset = localToGlobal(subIdx, 0);

    for (size_t t = 0; t < faceTriCount; ++t) {
        const size_t tri = faceFirstTri + t;
        if (tri >= sub.triangles.size()) continue;
        const int gi = localTriToGlobal(subIdx, tri);
        m_selectedFaces.insert(gi);

        const auto& triData = sub.triangles[tri];
        m_selectedVertices.insert(vertOffset + static_cast<int>(triData.indices[0]));
        m_selectedVertices.insert(vertOffset + static_cast<int>(triData.indices[1]));
        m_selectedVertices.insert(vertOffset + static_cast<int>(triData.indices[2]));
    }

    // Edge dilation: insert only the polygon's PERIMETER edges. For
    // an n-gon submesh those come from the EditableFace's index loop
    // (NOT the fan-triangulation, which would leak the artificial
    // diagonal edge into the selection). For triangle-only submeshes
    // every triangle edge IS a perimeter edge. (Chunk 4b edge fix.)
    if (faceK >= 0 && faceK < static_cast<int>(sub.faces.size())) {
        const auto& f = sub.faces[faceK];
        const size_t n = f.indices.size();
        for (size_t i = 0; i < n; ++i) {
            const int g0 = vertOffset + static_cast<int>(f.indices[i]);
            const int g1 = vertOffset + static_cast<int>(f.indices[(i + 1) % n]);
            m_selectedEdges.insert({std::min(g0, g1), std::max(g0, g1)});
        }
    } else {
        // Legacy single-triangle face: 3 edges = the triangle's edges.
        if (faceFirstTri < sub.triangles.size()) {
            const auto& triData = sub.triangles[faceFirstTri];
            const int g0 = vertOffset + static_cast<int>(triData.indices[0]);
            const int g1 = vertOffset + static_cast<int>(triData.indices[1]);
            const int g2 = vertOffset + static_cast<int>(triData.indices[2]);
            m_selectedEdges.insert({std::min(g0, g1), std::max(g0, g1)});
            m_selectedEdges.insert({std::min(g1, g2), std::max(g1, g2)});
            m_selectedEdges.insert({std::min(g0, g2), std::max(g0, g2)});
        }
    }

    updateSelectionOverlay();
    emit editSelectionChanged();
}

void EditModeController::deselectFace(int triIndex)
{
    if (!m_editableMesh) return;
    auto [subIdx, localTri] = globalTriToLocal(triIndex);
    if (subIdx >= m_editableMesh->subMeshes().size()) {
        // Out of range — try the legacy single-triangle erase as a
        // best-effort fallback so callers with stale indices don't
        // silently no-op.
        if (m_selectedFaces.erase(triIndex) > 0) {
            updateSelectionOverlay();
            emit editSelectionChanged();
        }
        return;
    }
    const auto& sub = m_editableMesh->subMeshes()[subIdx];

    // Mirror selectFace's dilation: deselect every fan triangle of
    // the clicked face so the user sees the whole face de-highlighted.
    size_t faceFirstTri = localTri, faceTriCount = 1;
    faceIndexForTriangle(sub, localTri, &faceFirstTri, &faceTriCount);

    bool anyErased = false;
    for (size_t t = 0; t < faceTriCount; ++t) {
        const size_t tri = faceFirstTri + t;
        if (tri >= sub.triangles.size()) continue;
        const int gi = localTriToGlobal(subIdx, tri);
        if (m_selectedFaces.erase(gi) > 0) anyErased = true;
    }
    if (anyErased) {
        updateSelectionOverlay();
        emit editSelectionChanged();
    }
}

// ===========================================================================
// Index conversion helpers
// ===========================================================================

std::pair<size_t, size_t> EditModeController::globalToLocal(int globalIndex) const
{
    if (!m_editableMesh)
        return {0, 0};

    int offset = 0;
    for (size_t si = 0; si < m_editableMesh->subMeshes().size(); ++si) {
        int subCount = static_cast<int>(m_editableMesh->subMeshes()[si].vertices.size());
        if (globalIndex < offset + subCount)
            return {si, static_cast<size_t>(globalIndex - offset)};
        offset += subCount;
    }
    return {0, 0};
}

int EditModeController::localToGlobal(size_t subMeshIndex, size_t localVertexIndex) const
{
    if (!m_editableMesh)
        return 0;

    int offset = 0;
    for (size_t si = 0; si < subMeshIndex && si < m_editableMesh->subMeshes().size(); ++si)
        offset += static_cast<int>(m_editableMesh->subMeshes()[si].vertices.size());

    return offset + static_cast<int>(localVertexIndex);
}

std::pair<size_t, size_t> EditModeController::globalTriToLocal(int globalTriIndex) const
{
    if (!m_editableMesh)
        return {0, 0};

    int offset = 0;
    for (size_t si = 0; si < m_editableMesh->subMeshes().size(); ++si) {
        int subCount = static_cast<int>(m_editableMesh->subMeshes()[si].triangles.size());
        if (globalTriIndex < offset + subCount)
            return {si, static_cast<size_t>(globalTriIndex - offset)};
        offset += subCount;
    }
    return {0, 0};
}

int EditModeController::localTriToGlobal(size_t subMeshIndex, size_t localTriIndex) const
{
    if (!m_editableMesh)
        return 0;

    int offset = 0;
    for (size_t si = 0; si < subMeshIndex && si < m_editableMesh->subMeshes().size(); ++si)
        offset += static_cast<int>(m_editableMesh->subMeshes()[si].triangles.size());

    return offset + static_cast<int>(localTriIndex);
}

std::vector<int> EditModeController::selectedFacesAsHEFaceIndices() const
{
    std::vector<int> result;
    if (!m_editableMesh) return result;
    if (m_selectedFaces.empty()) return result;

    // The HE face index for a given (subIdx, localTri) depends on
    // whether the submesh has a populated `faces` array. If it does,
    // the HE face index = sum of prior submeshes' face counts +
    // faceIndexForTriangle's result. If not (legacy triangle-only),
    // HE face index = sum of prior submeshes' triangle counts +
    // localTri.
    //
    // HalfEdgeMesh::buildFromEditableMesh visits submeshes in order;
    // within a submesh, when faces is non-empty it appends one HE face
    // per `EditableFace`, otherwise one per triangle. So the offsets
    // match this rule.
    const auto& subs = m_editableMesh->subMeshes();
    std::vector<int> heBaseBySub(subs.size(), 0);
    int running = 0;
    for (size_t s = 0; s < subs.size(); ++s) {
        heBaseBySub[s] = running;
        if (subs[s].faces.empty()) {
            running += static_cast<int>(subs[s].triangles.size());
        } else {
            // Match HalfEdgeMesh::buildFromEditableMesh, which only
            // appends faces that pass isValid(). Counting raw faces
            // here would over-shoot the offset and shift later
            // submeshes' HE face indices.
            int valid = 0;
            for (const auto& f : subs[s].faces) {
                if (f.isValid()) ++valid;
            }
            running += valid;
        }
    }

    // Walk the selection, dedup via a small set keyed on HE face idx.
    std::set<int> uniq;
    for (int gi : m_selectedFaces) {
        const auto [subIdx, localTri] = globalTriToLocal(gi);
        if (subIdx >= subs.size()) continue;
        const auto& sub = subs[subIdx];
        const int faceK = faceIndexForTriangle(sub, localTri, nullptr, nullptr);
        if (faceK >= 0) {
            // n-gon submesh: HE face index = base + faceK.
            uniq.insert(heBaseBySub[subIdx] + faceK);
        } else {
            // Legacy triangle-only submesh: HE face index = base +
            // localTri (one HE face per triangle).
            uniq.insert(heBaseBySub[subIdx] + static_cast<int>(localTri));
        }
    }
    result.assign(uniq.begin(), uniq.end());
    return result;
}

// ===========================================================================
// Geometry helpers
// ===========================================================================

Ogre::ColourValue EditModeController::weightToColor(float weight)
{
    weight = std::clamp(weight, 0.0f, 1.0f);

    // Heat map: blue (0.0) → cyan → green → yellow → orange → red (1.0)
    if (weight < 0.2f) {
        float t = weight / 0.2f;
        return Ogre::ColourValue(0.0f, t, 1.0f, 1.0f);             // blue → cyan
    }
    if (weight < 0.4f) {
        float t = (weight - 0.2f) / 0.2f;
        return Ogre::ColourValue(0.0f, 1.0f, 1.0f - t, 1.0f);      // cyan → green
    }
    if (weight < 0.6f) {
        float t = (weight - 0.4f) / 0.2f;
        return Ogre::ColourValue(t, 1.0f, 0.0f, 1.0f);             // green → yellow
    }
    if (weight < 0.8f) {
        float t = (weight - 0.6f) / 0.2f;
        return Ogre::ColourValue(1.0f, 1.0f - 0.6f * t, 0.0f, 1.0f); // yellow → orange
    }
    float t = (weight - 0.8f) / 0.2f;
    return Ogre::ColourValue(1.0f, 0.4f * (1.0f - t), 0.0f, 1.0f); // orange → red
}

QPoint EditModeController::worldToScreen(const Ogre::Vector3& worldPos,
                                          Ogre::Camera* camera,
                                          int viewportWidth, int viewportHeight)
{
    if (!camera)
        return QPoint(-1, -1);

    Ogre::Vector3 screenPos = camera->getProjectionMatrix() * camera->getViewMatrix() * worldPos;

    // Convert from NDC (-1..1) to pixel coordinates
    int x = static_cast<int>((screenPos.x * 0.5f + 0.5f) * viewportWidth);
    int y = static_cast<int>((1.0f - (screenPos.y * 0.5f + 0.5f)) * viewportHeight);

    return QPoint(x, y);
}

float EditModeController::pointToSegmentDistance(const QPoint& point,
                                                  const QPoint& segA,
                                                  const QPoint& segB)
{
    float px = static_cast<float>(point.x());
    float py = static_cast<float>(point.y());
    float ax = static_cast<float>(segA.x());
    float ay = static_cast<float>(segA.y());
    float bx = static_cast<float>(segB.x());
    float by = static_cast<float>(segB.y());

    float dx = bx - ax;
    float dy = by - ay;
    float lenSq = dx * dx + dy * dy;

    if (lenSq < 1e-8f) {
        // Degenerate segment (both endpoints are the same)
        float ddx = px - ax;
        float ddy = py - ay;
        return std::sqrt(ddx * ddx + ddy * ddy);
    }

    // Parameter t of closest point on the line
    float t = ((px - ax) * dx + (py - ay) * dy) / lenSq;
    t = std::max(0.0f, std::min(1.0f, t));

    float closestX = ax + t * dx;
    float closestY = ay + t * dy;

    float ddx = px - closestX;
    float ddy = py - closestY;
    return std::sqrt(ddx * ddx + ddy * ddy);
}

float EditModeController::rayTriangleIntersect(const Ogre::Vector3& rayOrigin,
                                                const Ogre::Vector3& rayDir,
                                                const Ogre::Vector3& v0,
                                                const Ogre::Vector3& v1,
                                                const Ogre::Vector3& v2)
{
    const float EPSILON = 1e-7f;

    Ogre::Vector3 edge1 = v1 - v0;
    Ogre::Vector3 edge2 = v2 - v0;
    Ogre::Vector3 h = rayDir.crossProduct(edge2);
    float a = edge1.dotProduct(h);

    if (a > -EPSILON && a < EPSILON)
        return -1.0f; // Ray is parallel to the triangle

    float f = 1.0f / a;
    Ogre::Vector3 s = rayOrigin - v0;
    float u = f * s.dotProduct(h);

    if (u < 0.0f || u > 1.0f)
        return -1.0f;

    Ogre::Vector3 q = s.crossProduct(edge1);
    float v = f * rayDir.dotProduct(q);

    if (v < 0.0f || u + v > 1.0f)
        return -1.0f;

    float t = f * edge2.dotProduct(q);

    if (t > EPSILON)
        return t;

    return -1.0f;
}

// ===========================================================================
// Hit testing
// ===========================================================================

int EditModeController::hitTestVertex(const QPoint& screenPos,
                                       Ogre::Camera* camera,
                                       int viewportWidth, int viewportHeight,
                                       float pixelRadius) const
{
    if (!m_editableMesh || !m_editEntity || !camera)
        return -1;

    Ogre::SceneNode* node = m_editEntity->getParentSceneNode();
    if (!node)
        return -1;

    // Build the front-facing vertex set so we never snap to a vertex
    // hidden behind a visible front-facing face. Mirrors the chunk-4b
    // edge / face hit-test behaviour. A polygon is front-facing when
    // its Newell normal (rotated into world space) points toward the
    // camera; a vertex is front-facing iff at least one incident
    // polygon does.
    const Ogre::Vector3 camPos = camera->getDerivedPosition();
    auto isPolygonFrontFacing = [&](const EditableSubMesh& sub,
                                    const std::vector<unsigned int>& corners) -> bool {
        if (corners.size() < 3) return false;
        Ogre::Vector3 nrm = Ogre::Vector3::ZERO;
        for (size_t i = 0; i < corners.size(); ++i) {
            if (corners[i] >= sub.vertices.size()) return false;
            const auto& a = sub.vertices[corners[i]].position;
            const auto& b = sub.vertices[corners[(i + 1) % corners.size()]].position;
            nrm.x += (a.y - b.y) * (a.z + b.z);
            nrm.y += (a.z - b.z) * (a.x + b.x);
            nrm.z += (a.x - b.x) * (a.y + b.y);
        }
        const Ogre::Vector3 worldNrm = node->_getDerivedOrientation() * nrm;
        const Ogre::Vector3 anyCornerWorld =
            node->convertLocalToWorldPosition(sub.vertices[corners[0]].position);
        return worldNrm.dotProduct(camPos - anyCornerWorld) > 0.0f;
    };
    std::set<int> frontVerts;
    {
        int vertOffset = 0;
        for (size_t si = 0; si < m_editableMesh->subMeshes().size(); ++si) {
            const auto& sub = m_editableMesh->subMeshes()[si];
            auto recordFrontPoly = [&](const std::vector<unsigned int>& corners) {
                for (unsigned int c : corners)
                    frontVerts.insert(vertOffset + static_cast<int>(c));
            };
            if (!sub.faces.empty()) {
                for (const auto& face : sub.faces) {
                    if (face.indices.size() < 3) continue;
                    if (!isPolygonFrontFacing(sub, face.indices)) continue;
                    recordFrontPoly(face.indices);
                }
            } else {
                for (const auto& tri : sub.triangles) {
                    std::vector<unsigned int> corners = {
                        tri.indices[0], tri.indices[1], tri.indices[2] };
                    if (!isPolygonFrontFacing(sub, corners)) continue;
                    recordFrontPoly(corners);
                }
            }
            vertOffset += static_cast<int>(sub.vertices.size());
        }
    }

    float bestDistSq = pixelRadius * pixelRadius;
    int bestIndex = -1;
    int globalOffset = 0;

    for (size_t si = 0; si < m_editableMesh->subMeshes().size(); ++si) {
        const auto& sub = m_editableMesh->subMeshes()[si];
        for (size_t vi = 0; vi < sub.vertices.size(); ++vi) {
            int globalIdx = globalOffset + static_cast<int>(vi);

            // Skip back-face vertices: not in any front-facing polygon.
            if (!frontVerts.count(globalIdx)) continue;

            // Transform from local space to world space
            Ogre::Vector3 worldPos = node->convertLocalToWorldPosition(sub.vertices[vi].position);

            // Check if the vertex is behind the camera
            Ogre::Vector3 camToVert = worldPos - camera->getDerivedPosition();
            if (camToVert.dotProduct(camera->getDerivedDirection()) < 0)
                continue;

            QPoint sp = worldToScreen(worldPos, camera, viewportWidth, viewportHeight);
            float dx = static_cast<float>(sp.x() - screenPos.x());
            float dy = static_cast<float>(sp.y() - screenPos.y());
            float distSq = dx * dx + dy * dy;

            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                bestIndex = globalIdx;
            }
        }
        globalOffset += static_cast<int>(sub.vertices.size());
    }

    return bestIndex;
}

// LCOV_EXCL_START — requires active render window + camera for ray casting
int EditModeController::hitTestFace(const QPoint& screenPos,
                                     Ogre::Camera* camera,
                                     int viewportWidth, int viewportHeight) const
{
    if (!m_editableMesh || !m_editEntity || !camera)
        return -1;

    Ogre::SceneNode* node = m_editEntity->getParentSceneNode();
    if (!node)
        return -1;

    // Cast a ray from the camera through the click point
    Ogre::Real nx = static_cast<Ogre::Real>(screenPos.x()) / static_cast<Ogre::Real>(viewportWidth);
    Ogre::Real ny = static_cast<Ogre::Real>(screenPos.y()) / static_cast<Ogre::Real>(viewportHeight);
    Ogre::Ray ray = camera->getCameraToViewportRay(nx, ny);

    // Transform ray into entity's local space
    Ogre::Affine3 worldToLocal = node->_getFullTransform().inverse();
    Ogre::Vector3 localOrigin = worldToLocal * ray.getOrigin();
    Ogre::Vector3 localDir = worldToLocal.linear() * ray.getDirection();
    localDir.normalise();

    float bestT = std::numeric_limits<float>::max();
    int bestTriIndex = -1;
    int globalTriOffset = 0;

    for (size_t si = 0; si < m_editableMesh->subMeshes().size(); ++si) {
        const auto& sub = m_editableMesh->subMeshes()[si];
        for (size_t ti = 0; ti < sub.triangles.size(); ++ti) {
            const auto& tri = sub.triangles[ti];

            if (tri.indices[0] >= sub.vertices.size() ||
                tri.indices[1] >= sub.vertices.size() ||
                tri.indices[2] >= sub.vertices.size())
            {
                continue;
            }

            const Ogre::Vector3& v0 = sub.vertices[tri.indices[0]].position;
            const Ogre::Vector3& v1 = sub.vertices[tri.indices[1]].position;
            const Ogre::Vector3& v2 = sub.vertices[tri.indices[2]].position;

            float t = rayTriangleIntersect(localOrigin, localDir, v0, v1, v2);
            if (t >= 0.0f && t < bestT) {
                bestT = t;
                bestTriIndex = globalTriOffset + static_cast<int>(ti);
            }
        }
        globalTriOffset += static_cast<int>(sub.triangles.size());
    }

    return bestTriIndex;
}
// LCOV_EXCL_STOP

std::pair<int,int> EditModeController::hitTestEdge(const QPoint& screenPos,
                                                    Ogre::Camera* camera,
                                                    int viewportWidth, int viewportHeight,
                                                    float pixelRadius) const
{
    if (!m_editableMesh || !m_editEntity || !camera)
        return {-1, -1};

    Ogre::SceneNode* node = m_editEntity->getParentSceneNode();
    if (!node)
        return {-1, -1};

    float bestDist = pixelRadius;
    std::pair<int,int> bestEdge = {-1, -1};

    const Ogre::Vector3 camPos = camera->getDerivedPosition();

    auto considerEdge = [&](int li0, int li1, const EditableSubMesh& sub,
                            int vertOffset) {
        if (li0 >= static_cast<int>(sub.vertices.size()) ||
            li1 >= static_cast<int>(sub.vertices.size()))
            return;

        Ogre::Vector3 wp0 = node->convertLocalToWorldPosition(sub.vertices[li0].position);
        Ogre::Vector3 wp1 = node->convertLocalToWorldPosition(sub.vertices[li1].position);

        // Skip edges where both endpoints are behind the camera
        Ogre::Vector3 camDir = camera->getDerivedDirection();
        bool behind0 = (wp0 - camPos).dotProduct(camDir) < 0;
        bool behind1 = (wp1 - camPos).dotProduct(camDir) < 0;
        if (behind0 && behind1) return;

        QPoint sp0 = worldToScreen(wp0, camera, viewportWidth, viewportHeight);
        QPoint sp1 = worldToScreen(wp1, camera, viewportWidth, viewportHeight);

        float dist = pointToSegmentDistance(screenPos, sp0, sp1);
        if (dist < bestDist) {
            bestDist = dist;
            int g0 = vertOffset + li0;
            int g1 = vertOffset + li1;
            bestEdge = {std::min(g0, g1), std::max(g0, g1)};
        }
    };

    // Compute per-face front-facing flags so we can skip edges whose
    // both adjacent polygons face away from the camera. Per-face
    // (rather than per-edge) so a quad's perimeter edges all share
    // the same culling decision once. Boundary edges (only one
    // incident face) are kept if that one face faces the camera.
    // (Chunk 4b: edge selection mirrors face selection's front-only
    // behaviour.)
    auto isPolygonFrontFacing = [&](const EditableSubMesh& sub,
                                    const std::vector<unsigned int>& corners) -> bool {
        if (corners.size() < 3) return false;
        Ogre::Vector3 nrm = Ogre::Vector3::ZERO;
        for (size_t i = 0; i < corners.size(); ++i) {
            if (corners[i] >= sub.vertices.size()) return false;
            const auto& a = sub.vertices[corners[i]].position;
            const auto& b = sub.vertices[corners[(i + 1) % corners.size()]].position;
            nrm.x += (a.y - b.y) * (a.z + b.z);
            nrm.y += (a.z - b.z) * (a.x + b.x);
            nrm.z += (a.x - b.x) * (a.y + b.y);
        }
        // Newell normal in local space → world space (rotate via node
        // orientation, ignore translation since direction).
        const Ogre::Vector3 worldNrm = node->_getDerivedOrientation() * nrm;
        const Ogre::Vector3 anyCornerWorld =
            node->convertLocalToWorldPosition(sub.vertices[corners[0]].position);
        const Ogre::Vector3 toCam = camPos - anyCornerWorld;
        return worldNrm.dotProduct(toCam) > 0.0f;
    };

    for (size_t si = 0; si < m_editableMesh->subMeshes().size(); ++si) {
        const auto& sub = m_editableMesh->subMeshes()[si];
        int vertOffset = localToGlobal(si, 0);

        // n-gon path: iterate the polygon's perimeter edges only,
        // skipping triangle-fan diagonals (which are NOT real polygon
        // edges) and back-facing polygons. (Chunk 4b edge fix.)
        if (!sub.faces.empty()) {
            for (const auto& face : sub.faces) {
                const size_t n = face.indices.size();
                if (n < 3) continue;
                if (!isPolygonFrontFacing(sub, face.indices)) continue;
                for (size_t i = 0; i < n; ++i) {
                    const int li0 = static_cast<int>(face.indices[i]);
                    const int li1 = static_cast<int>(face.indices[(i + 1) % n]);
                    considerEdge(li0, li1, sub, vertOffset);
                }
            }
        } else {
            // Legacy triangle-only submesh: every triangle edge IS a
            // polygon edge. Apply the same front-facing filter using
            // the triangle's own corners.
            for (const auto& tri : sub.triangles) {
                std::vector<unsigned int> corners = {
                    tri.indices[0], tri.indices[1], tri.indices[2] };
                if (!isPolygonFrontFacing(sub, corners)) continue;
                considerEdge(static_cast<int>(tri.indices[0]),
                             static_cast<int>(tri.indices[1]), sub, vertOffset);
                considerEdge(static_cast<int>(tri.indices[1]),
                             static_cast<int>(tri.indices[2]), sub, vertOffset);
                considerEdge(static_cast<int>(tri.indices[0]),
                             static_cast<int>(tri.indices[2]), sub, vertOffset);
            }
        }
    }

    return bestEdge;
}

void EditModeController::boxSelectVertices(const QRect& rect,
                                            Ogre::Camera* camera,
                                            int viewportWidth, int viewportHeight,
                                            bool addToSelection)
{
    if (!m_editableMesh || !m_editEntity || !camera)
        return;

    Ogre::SceneNode* node = m_editEntity->getParentSceneNode();
    if (!node)
        return;

    if (!addToSelection)
        m_selectedVertices.clear();

    int globalOffset = 0;
    for (size_t si = 0; si < m_editableMesh->subMeshes().size(); ++si) {
        const auto& sub = m_editableMesh->subMeshes()[si];
        for (size_t vi = 0; vi < sub.vertices.size(); ++vi) {
            Ogre::Vector3 worldPos = node->convertLocalToWorldPosition(sub.vertices[vi].position);

            // Skip vertices behind camera
            Ogre::Vector3 camToVert = worldPos - camera->getDerivedPosition();
            if (camToVert.dotProduct(camera->getDerivedDirection()) < 0)
                continue;

            QPoint sp = worldToScreen(worldPos, camera, viewportWidth, viewportHeight);
            if (rect.contains(sp))
                m_selectedVertices.insert(globalOffset + static_cast<int>(vi));
        }
        globalOffset += static_cast<int>(sub.vertices.size());
    }

    updateSelectionOverlay();
    emit editSelectionChanged();
}

// ===========================================================================
// Mouse handling
// ===========================================================================

// LCOV_EXCL_START — mouse interaction requires active render window + camera
void EditModeController::handleMouseClick(const QPoint& screenPos, OgreWidget* widget,
                                           bool shiftHeld, bool ctrlHeld)
{
    if (!m_editModeActive || !widget)
        return;

    auto* cam = widget->getSpaceCamera();
    if (!cam || !cam->getCamera())
        return;

    auto* camera = cam->getCamera();
    auto* vp = camera->getViewport();
    if (!vp)
        return;

#ifdef Q_OS_MACOS
    int widthMod = 2;
#else
    int widthMod = 1;
#endif
    int vpWidth = vp->getActualWidth() / widthMod;
    int vpHeight = vp->getActualHeight() / widthMod;

    bool addToSelection = shiftHeld;
    bool removeFromSelection = ctrlHeld;

    switch (m_selectionMode) {
    case VertexMode: {
        int vi = hitTestVertex(screenPos, camera, vpWidth, vpHeight);
        if (vi >= 0) {
            if (removeFromSelection) {
                deselectVertex(vi);
            } else {
                selectVertex(vi, addToSelection);
            }
        } else if (!addToSelection && !removeFromSelection) {
            deselectAll();
        }
        break;
    }
    case EdgeMode: {
        auto edge = hitTestEdge(screenPos, camera, vpWidth, vpHeight);
        if (edge.first >= 0) {
            if (removeFromSelection) {
                deselectEdge(edge.first, edge.second);
            } else {
                selectEdge(edge.first, edge.second, addToSelection);
            }
        } else if (!addToSelection && !removeFromSelection) {
            deselectAll();
        }
        break;
    }
    case FaceMode: {
        int fi = hitTestFace(screenPos, camera, vpWidth, vpHeight);
        if (fi >= 0) {
            if (removeFromSelection) {
                deselectFace(fi);
            } else {
                selectFace(fi, addToSelection);
            }
        } else if (!addToSelection && !removeFromSelection) {
            deselectAll();
        }
        break;
    }
    }
}

void EditModeController::handleBoxSelect(const QPoint& startPos, const QPoint& endPos,
                                           OgreWidget* widget, bool shiftHeld)
{
    if (!m_editModeActive || !widget)
        return;

    auto* cam = widget->getSpaceCamera();
    if (!cam || !cam->getCamera())
        return;

    auto* camera = cam->getCamera();
    auto* vp = camera->getViewport();
    if (!vp)
        return;

#ifdef Q_OS_MACOS
    int widthMod = 2;
#else
    int widthMod = 1;
#endif
    int vpWidth = vp->getActualWidth() / widthMod;
    int vpHeight = vp->getActualHeight() / widthMod;

    QRect rect(startPos, endPos);
    rect = rect.normalized();

    boxSelectVertices(rect, camera, vpWidth, vpHeight, shiftHeld);
}
// LCOV_EXCL_STOP

// ===========================================================================
// Soft selection
// ===========================================================================

void EditModeController::setSoftSelectionEnabled(bool enabled)
{
    if (m_softSelectionEnabled != enabled) {
        m_softSelectionEnabled = enabled;
        SentryReporter::addBreadcrumb("edit_mode",
            QString("Soft selection %1").arg(enabled ? "enabled" : "disabled"));
        emit softSelectionChanged();
        updateSelectionOverlay();
    }
}

void EditModeController::setSoftSelectionRadius(double radius)
{
    if (radius > 0.0 && m_softSelectionRadius != radius) {
        m_softSelectionRadius = radius;
        emit softSelectionChanged();
        updateSelectionOverlay();
    }
}

void EditModeController::setSoftSelectionFalloff(int falloff)
{
    if (falloff >= 0 && falloff <= 1 && m_softSelectionFalloff != falloff) {
        m_softSelectionFalloff = falloff;
        emit softSelectionChanged();
        updateSelectionOverlay();
    }
}

std::map<int, float> EditModeController::getSoftSelectionWeights() const
{
    std::map<int, float> weights;
    if (!m_editableMesh || m_selectedVertices.empty())
        return weights;

    // All selected vertices get weight 1.0
    for (int gi : m_selectedVertices)
        weights[gi] = 1.0f;

    if (!m_softSelectionEnabled || m_softSelectionRadius <= 0.0)
        return weights;

    // Collect positions of selected vertices
    std::vector<Ogre::Vector3> selectedPositions;
    selectedPositions.reserve(m_selectedVertices.size());
    for (int gi : m_selectedVertices) {
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx < m_editableMesh->subMeshes().size() &&
            localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
        {
            selectedPositions.push_back(
                m_editableMesh->subMeshes()[subIdx].vertices[localIdx].position);
        }
    }

    float radius = static_cast<float>(m_softSelectionRadius);
    int totalVerts = static_cast<int>(m_editableMesh->totalVertexCount());

    for (int gi = 0; gi < totalVerts; ++gi) {
        if (m_selectedVertices.count(gi))
            continue; // Already weight 1.0

        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx >= m_editableMesh->subMeshes().size())
            continue;
        const auto& pos = m_editableMesh->subMeshes()[subIdx].vertices[localIdx].position;

        // Find minimum distance to any selected vertex
        float minDist = std::numeric_limits<float>::max();
        for (const auto& selPos : selectedPositions) {
            float dist = pos.distance(selPos);
            if (dist < minDist)
                minDist = dist;
        }

        if (minDist < radius) {
            float t = minDist / radius; // 0 at selected vertex, 1 at radius boundary
            float weight = 0.0f;
            if (m_softSelectionFalloff == 0) {
                // Linear falloff
                weight = 1.0f - t;
            } else {
                // Smooth (cosine) falloff
                weight = 0.5f * (1.0f + std::cos(t * static_cast<float>(M_PI)));
            }
            if (weight > 0.001f)
                weights[gi] = weight;
        }
    }

    return weights;
}

std::map<int, float> EditModeController::computeSoftSelectionWeightsFromPositions(
    const std::map<int, Ogre::Vector3>& positions) const
{
    std::map<int, float> weights;
    if (!m_editableMesh || m_selectedVertices.empty())
        return weights;

    // Selected vertices always get weight 1.0.
    for (int gi : m_selectedVertices)
        weights[gi] = 1.0f;

    if (!m_softSelectionEnabled || m_softSelectionRadius <= 0.0)
        return weights;

    // Position of each selected vertex — prefer the supplied map, fall back
    // to the live mesh for any selected vertex not represented in it.
    std::vector<Ogre::Vector3> selectedPositions;
    selectedPositions.reserve(m_selectedVertices.size());
    for (int gi : m_selectedVertices) {
        auto it = positions.find(gi);
        if (it != positions.end()) {
            selectedPositions.push_back(it->second);
            continue;
        }
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx < m_editableMesh->subMeshes().size() &&
            localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
        {
            selectedPositions.push_back(
                m_editableMesh->subMeshes()[subIdx].vertices[localIdx].position);
        }
    }

    const float radius = static_cast<float>(m_softSelectionRadius);
    for (const auto& [gi, pos] : positions) {
        if (m_selectedVertices.count(gi))
            continue;

        float minDist = std::numeric_limits<float>::max();
        for (const auto& selPos : selectedPositions) {
            float dist = pos.distance(selPos);
            if (dist < minDist)
                minDist = dist;
        }
        if (minDist >= radius)
            continue;

        float t = minDist / radius;
        float weight = 0.0f;
        if (m_softSelectionFalloff == 0) {
            weight = 1.0f - t;
        } else {
            weight = 0.5f * (1.0f + std::cos(t * static_cast<float>(M_PI)));
        }
        if (weight > 0.001f)
            weights[gi] = weight;
    }
    return weights;
}

// ===========================================================================
// Normals recalculation
// ===========================================================================

void EditModeController::setNormalsMode(int mode)
{
    if (mode >= 0 && mode <= 1 && m_normalsMode != mode) {
        m_normalsMode = mode;
        SentryReporter::addBreadcrumb("edit_mode",
            QString("Normals mode changed to %1").arg(mode == 0 ? "smooth" : "flat"));
        emit normalsModeChanged();
    }
}

void EditModeController::recalculateNormals(bool smooth)
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity)
        return;

    SentryReporter::addBreadcrumb("edit_mode",
        QString("Recalculate normals (%1)").arg(smooth ? "smooth" : "flat"));

    if (smooth)
        m_editableMesh->recalculateNormals();
    else
        m_editableMesh->recalculateNormalsFlat();

    // Write normals back to the GPU buffers for immediate visual feedback
    m_editableMesh->commitToEntity(m_editEntity);
    refreshNormalVisualizer();

    emit meshDataChanged();
}

// ===========================================================================
// Mesh validation after edits
// ===========================================================================

void EditModeController::validateMesh()
{
    if (!m_editableMesh) {
        m_degenerateTriangleCount = 0;
        emit validationChanged();
        return;
    }

    int oldCount = m_degenerateTriangleCount;
    m_degenerateTriangleCount = m_editableMesh->countDegenerateTriangles();
    if (m_degenerateTriangleCount != oldCount)
        emit validationChanged();
}

void EditModeController::removeDegenerateTriangles()
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity)
        return;

    SentryReporter::addBreadcrumb("edit_mode", "Remove degenerate triangles");

    int removed = m_editableMesh->removeDegenerateTriangles();

    if (removed > 0) {
        // recalculateNormals() already calls commitToEntity + refreshNormalVisualizer
        recalculateNormals(m_normalsMode == 0);
        updateSelectionOverlay();
    }

    // Re-validate
    validateMesh();
}

// ===========================================================================
// Topology operations
// ===========================================================================

bool EditModeController::extrudeSelection()
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity)
        return false;

    // Only face extrude is supported for now. Edge extrude is implemented in
    // HalfEdgeMesh but not yet wired up through the full pipeline.
    if (m_selectionMode != FaceMode)
        return false;

    SentryReporter::addBreadcrumb("edit_mode",
        QString("Extrude selection (faces=%1)").arg(m_selectedFaces.size()));

    // Snapshot for undo
    EditableMesh oldMesh;
    oldMesh.subMeshes() = m_editableMesh->subMeshes();
    auto oldSelectedVertices = m_selectedVertices;
    auto oldSelectedEdges = m_selectedEdges;
    auto oldSelectedFaces = m_selectedFaces;

    // Build half-edge structure
    HalfEdgeMesh heMesh;
    if (!heMesh.buildFromEditableMesh(*m_editableMesh))
        return false;

    std::vector<int> newHEVertices;

    if (m_selectionMode == FaceMode && !m_selectedFaces.empty()) {
        // Triangle indices → unique HE face indices (chunk 4b). On
        // n-gon submeshes one quad selection produces multiple
        // triangle entries that all map to the same HE face;
        // dedup is critical so extrudeFaces doesn't double-process.
        const std::vector<int> faceIndices = selectedFacesAsHEFaceIndices();
        newHEVertices = heMesh.extrudeFaces(faceIndices);
    } else if (m_selectionMode == EdgeMode && !m_selectedEdges.empty()) {
        // Convert (min,max) vertex-pair edge selections to HE edge indices
        std::vector<int> edgeIndices;
        for (const auto& [v1, v2] : m_selectedEdges) {
            for (size_t e = 0; e < heMesh.edgeCount(); ++e) {
                auto [ev1, ev2] = heMesh.edgeVertices(e);
                int eMin = std::min(ev1, ev2);
                int eMax = std::max(ev1, ev2);
                if (eMin == v1 && eMax == v2) {
                    edgeIndices.push_back(static_cast<int>(e));
                    break;
                }
            }
        }
        newHEVertices = heMesh.extrudeEdges(edgeIndices);
    } else {
        return false;
    }

    if (newHEVertices.empty())
        return false;

    // Offset new vertices slightly along the average normal of the
    // top faces (the ones whose vertices are ALL new — i.e. the
    // extruded caps). Side-wall faces, which mix old and new verts,
    // are skipped so the offset only pushes the cap outward. Without
    // this, the side-wall faces have zero area and shading goes bad.
    //
    // n-gon-aware: use Newell's method instead of the triangle-only
    // cross product so quad / pentagon caps (the result of extruding
    // an n-gon face) contribute correctly. The triangle path was a
    // strict `verts.size() != 3` skip, which made every offset zero
    // on quad-imported assets — and selection-by-position then
    // matched the OLD un-offset vertex coords, leaving the user with
    // the pre-extrude vertices selected.
    const float EXTRUDE_OFFSET = 0.01f;
    std::set<int> newVertSet(newHEVertices.begin(), newHEVertices.end());
    auto newellNormal = [&](const std::vector<int>& verts) {
        Ogre::Vector3 nrm = Ogre::Vector3::ZERO;
        const size_t N = verts.size();
        for (size_t i = 0; i < N; ++i) {
            const auto& a = heMesh.vertex(verts[i]).position;
            const auto& b = heMesh.vertex(verts[(i + 1) % N]).position;
            nrm.x += (a.y - b.y) * (a.z + b.z);
            nrm.y += (a.z - b.z) * (a.x + b.x);
            nrm.z += (a.x - b.x) * (a.y + b.y);
        }
        return nrm;
    };
    std::map<int, Ogre::Vector3> offsets;
    for (int heVert : newHEVertices) {
        auto adjFaces = heMesh.facesAroundVertex(heVert);
        Ogre::Vector3 avgNormal(0, 0, 0);
        int count = 0;
        for (int fi : adjFaces) {
            auto verts = heMesh.faceVertices(fi);
            if (verts.size() < 3) continue;
            // Top face = every vertex is in the new-vertex set.
            bool allNew = true;
            for (int v : verts) {
                if (!newVertSet.count(v)) { allNew = false; break; }
            }
            if (!allNew) continue;
            Ogre::Vector3 n = newellNormal(verts);
            if (n.length() > 1e-8f) {
                n.normalise();
                avgNormal += n;
                ++count;
            }
        }
        if (count > 0) {
            avgNormal /= static_cast<float>(count);
            if (avgNormal.length() > 1e-6f) {
                avgNormal.normalise();
                offsets[heVert] = avgNormal * EXTRUDE_OFFSET;
            }
        }
    }
    for (const auto& [heVert, offset] : offsets)
        heMesh.vertex(heVert).position += offset;

    EditableMesh newMesh;
    if (!heMesh.toEditableMesh(newMesh))
        return false;

    m_editableMesh->subMeshes() = std::move(newMesh.subMeshes());

    // Collect the offset positions of the new vertices. These will be used
    // to identify which vertices in the new EditableMesh are "new" and need
    // fresh normals (side-wall triangles have new geometry that affects
    // adjacent vertex normals too).
    std::vector<Ogre::Vector3> newVertPositions;
    newVertPositions.reserve(newHEVertices.size());
    for (int heVert : newHEVertices)
        newVertPositions.push_back(heMesh.vertex(heVert).position);

    // Selective normal recompute: only update normals for vertices whose
    // position matches a new vertex AND for vertices used by triangles that
    // include any of those new vertices (these are the vertices whose
    // neighborhood changed).
    //
    // All OTHER vertices keep their ORIGINAL normals (preserved through the
    // HE round-trip). This avoids a lighting shift on the untouched parts.
    {
        const float TOL2 = 1e-8f; // squared distance tolerance

        auto positionMatchesNew = [&](const Ogre::Vector3& p) {
            for (const auto& np : newVertPositions) {
                if (p.squaredDistance(np) < TOL2)
                    return true;
            }
            return false;
        };

        for (auto& sub : m_editableMesh->subMeshes()) {
            // Mark vertices at new positions
            std::vector<bool> isNew(sub.vertices.size(), false);
            for (size_t v = 0; v < sub.vertices.size(); ++v) {
                if (positionMatchesNew(sub.vertices[v].position))
                    isNew[v] = true;
            }

            // Expand the dirty set: any vertex sharing a triangle with a
            // new vertex also needs its normal updated.
            std::vector<bool> isDirty = isNew;
            for (const auto& tri : sub.triangles) {
                if (tri.indices[0] >= sub.vertices.size() ||
                    tri.indices[1] >= sub.vertices.size() ||
                    tri.indices[2] >= sub.vertices.size())
                    continue;
                bool anyNew = isNew[tri.indices[0]] || isNew[tri.indices[1]] || isNew[tri.indices[2]];
                if (anyNew) {
                    isDirty[tri.indices[0]] = true;
                    isDirty[tri.indices[1]] = true;
                    isDirty[tri.indices[2]] = true;
                }
            }

            // Zero the normals of dirty vertices (we'll accumulate below)
            for (size_t v = 0; v < sub.vertices.size(); ++v) {
                if (isDirty[v]) {
                    sub.vertices[v].normal = Ogre::Vector3::ZERO;
                    sub.vertices[v].hasNormal = true;
                }
            }

            // Accumulate area-weighted face normals ONLY onto dirty vertices
            for (const auto& tri : sub.triangles) {
                if (tri.indices[0] >= sub.vertices.size() ||
                    tri.indices[1] >= sub.vertices.size() ||
                    tri.indices[2] >= sub.vertices.size())
                    continue;
                bool anyDirty = isDirty[tri.indices[0]] || isDirty[tri.indices[1]] || isDirty[tri.indices[2]];
                if (!anyDirty) continue;

                const auto& v0 = sub.vertices[tri.indices[0]].position;
                const auto& v1 = sub.vertices[tri.indices[1]].position;
                const auto& v2 = sub.vertices[tri.indices[2]].position;
                Ogre::Vector3 faceN = (v1 - v0).crossProduct(v2 - v0);
                for (int k = 0; k < 3; ++k) {
                    if (isDirty[tri.indices[k]])
                        sub.vertices[tri.indices[k]].normal += faceN;
                }
            }

            // Normalize the dirty vertices
            for (size_t v = 0; v < sub.vertices.size(); ++v) {
                if (isDirty[v]) {
                    float len = sub.vertices[v].normal.length();
                    if (len > 1e-8f)
                        sub.vertices[v].normal /= len;
                }
            }
        }
    }

    if (!m_editableMesh->resizeEntityBuffers(m_editEntity))
        return false;

    // Refresh Entity caches + RTSS state (incl. tangent rebuild for
    // bump-mapped materials). See `rewriteEntityAfterTopologyChange`.
    rewriteEntityAfterTopologyChange(m_editEntity);

    // Select the new (offset) vertices by position — offset ensures uniqueness
    m_selectedVertices.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    std::set<int> newGlobalVerts;
    for (const auto& pos : newVertPositions) {
        int globalIdx = 0;
        bool found = false;
        for (size_t s = 0; s < m_editableMesh->subMeshes().size() && !found; ++s) {
            const auto& verts = m_editableMesh->subMeshes()[s].vertices;
            for (size_t v = 0; v < verts.size(); ++v) {
                if (verts[v].position.squaredDistance(pos) < 1e-8f) {
                    newGlobalVerts.insert(globalIdx + static_cast<int>(v));
                    found = true;
                    break;
                }
            }
            globalIdx += static_cast<int>(verts.size());
        }
    }
    m_selectedVertices = newGlobalVerts;

    // Select the extruded top faces (all 3 vertices are new)
    if (m_selectionMode == FaceMode) {
        int globalTriIdx = 0;
        for (size_t s = 0; s < m_editableMesh->subMeshes().size(); ++s) {
            const auto& sub = m_editableMesh->subMeshes()[s];
            int subVertOffset = 0;
            for (size_t ss = 0; ss < s; ++ss)
                subVertOffset += static_cast<int>(m_editableMesh->subMeshes()[ss].vertices.size());

            for (const auto& tri : sub.triangles) {
                int g0 = subVertOffset + static_cast<int>(tri.indices[0]);
                int g1 = subVertOffset + static_cast<int>(tri.indices[1]);
                int g2 = subVertOffset + static_cast<int>(tri.indices[2]);
                if (newGlobalVerts.count(g0) && newGlobalVerts.count(g1) && newGlobalVerts.count(g2)) {
                    m_selectedFaces.insert(globalTriIdx);
                }
                ++globalTriIdx;
            }
        }
    }

    // Push undo command
    auto* cmd = new EditMeshTopologyCommand(
        std::move(oldMesh.subMeshes()),
        m_editableMesh->subMeshes(),
        oldSelectedVertices, oldSelectedEdges, oldSelectedFaces,
        m_selectedVertices, m_selectedEdges, m_selectedFaces,
        "Extrude");
    UndoManager::getSingleton()->push(cmd);

    refreshNormalVisualizer();
    updateSelectionOverlay();
    validateMesh();

    emit meshDataChanged();
    emit editSelectionChanged();
    emit selectionModeChanged();
    emit editModeChanged();

    return true;
}

bool EditModeController::applyBevelTopology(
    const std::vector<std::pair<int,int>>& edges, float width,
    int segments, const std::vector<float>& profilePoints)
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity)
        return false;
    if (edges.empty() || width <= 0.0f)
        return false;

    // Bevel has two implementations:
    //   - `bevelEdges` (triangle-only): the original, with crease /
    //     coplanar-sibling detection and segment / profile support.
    //   - `bevelEdgesNgon` (n-gon-aware MVP): handles arbitrary face
    //     arity but only single-segment flat chamfers.
    //
    // Pick the right one based on whether the editable mesh actually
    // carries n-gon canonicalised faces. Triangle-only meshes
    // (procedural primitives, post-edit re-entries, .scene.glb sub-
    // entities) keep using `bevelEdges` so we don't lose its quality
    // features. Quad-imported meshes (FBX, glTF) get `bevelEdgesNgon`,
    // which produces visible chamfers without the "all-triangulated"
    // workaround that previously triangulated entire submeshes.
    bool meshHasNGons = false;
    for (const auto& sub : m_editableMesh->subMeshes()) {
        if (!sub.faces.empty()) { meshHasNGons = true; break; }
    }

    auto originalSubMeshes = m_editableMesh->subMeshes();

    HalfEdgeMesh heMesh;
    if (!heMesh.buildFromEditableMesh(*m_editableMesh))
        return false;

    // Convert (min,max) vertex-pair edges to HE edge indices.
    std::vector<int> edgeIndices;
    for (const auto& [v1, v2] : edges) {
        for (size_t e = 0; e < heMesh.edgeCount(); ++e) {
            auto [ev1, ev2] = heMesh.edgeVertices(e);
            int eMin = std::min(ev1, ev2);
            int eMax = std::max(ev1, ev2);
            if (eMin == v1 && eMax == v2) {
                edgeIndices.push_back(static_cast<int>(e));
                break;
            }
        }
    }

    std::vector<int> newHEVertices = meshHasNGons
        ? heMesh.bevelEdgesNgon(edgeIndices, width, segments, 0.5f, profilePoints)
        : heMesh.bevelEdges(edgeIndices, width, segments, 0.5f, profilePoints);
    if (newHEVertices.empty())
        return false;

    // Snapshot new-vertex positions for post-conversion normal-dirty detection
    std::vector<Ogre::Vector3> newVertPositions;
    newVertPositions.reserve(newHEVertices.size());
    for (int heVert : newHEVertices)
        newVertPositions.push_back(heMesh.vertex(heVert).position);

    EditableMesh newMesh;
    if (!heMesh.toEditableMesh(newMesh))
        return false;
    (void)originalSubMeshes; // n-gon path preserves quads natively

    m_editableMesh->subMeshes() = std::move(newMesh.subMeshes());

    // Full normal recompute — cheaper options (selective by proximity) turned
    // out to leave seams around the beveled region where the neighbor faces
    // share vertices with the new topology. A full pass is safe and cheap on
    // meshes that pass through edit mode. Respect the current normals mode
    // so flat-shaded meshes don't silently flip to smooth after bevel.
    if (m_normalsMode == 0)
        m_editableMesh->recalculateNormals();
    else
        m_editableMesh->recalculateNormalsFlat();

    if (!m_editableMesh->resizeEntityBuffers(m_editEntity))
        return false;

    // Refresh Entity caches + RTSS state (incl. tangent rebuild for
    // bump-mapped materials), preserving per-subentity material
    // overrides (wireframe variant, MaterialEditor writes).
    rewriteEntityAfterTopologyChange(m_editEntity);

    m_selectedVertices.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    std::set<int> newGlobalVerts;
    for (const auto& pos : newVertPositions) {
        int globalIdx = 0;
        bool found = false;
        for (size_t s = 0; s < m_editableMesh->subMeshes().size() && !found; ++s) {
            const auto& verts = m_editableMesh->subMeshes()[s].vertices;
            for (size_t v = 0; v < verts.size(); ++v) {
                if (verts[v].position.squaredDistance(pos) < 1e-8f) {
                    newGlobalVerts.insert(globalIdx + static_cast<int>(v));
                    found = true;
                    break;
                }
            }
            globalIdx += static_cast<int>(verts.size());
        }
    }
    m_selectedVertices = newGlobalVerts;

    refreshNormalVisualizer();
    updateSelectionOverlay();
    validateMesh();

    emit meshDataChanged();
    emit editSelectionChanged();
    emit selectionModeChanged();
    emit editModeChanged();

    return true;
}

bool EditModeController::applyBevelVertexTopology(
    const std::vector<int>& vertexIndices, float width,
    int segments, const std::vector<float>& profilePoints)
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity)
        return false;
    if (vertexIndices.empty() || width <= 0.0f)
        return false;

    // Same dispatch as applyBevelTopology: pick the n-gon path when the
    // editable mesh has n-gon canonical faces, the triangle-only path
    // otherwise. The n-gon variant is single-segment flat MVP; the
    // triangle path keeps its segments / profile features.
    bool meshHasNGons = false;
    for (const auto& sub : m_editableMesh->subMeshes()) {
        if (!sub.faces.empty()) { meshHasNGons = true; break; }
    }
    auto originalSubMeshes = m_editableMesh->subMeshes();

    HalfEdgeMesh heMesh;
    if (!heMesh.buildFromEditableMesh(*m_editableMesh))
        return false;

    // Map global selection indices to HE indices by position match.
    // HE indexing preserves (submesh, local) so a straight offset walk
    // works: global = sum of prior subs' vertex counts + local.
    // We instead just match by position because global-index plumbing
    // elsewhere (selection set) stores them that way already.
    std::vector<int> heIndices;
    heIndices.reserve(vertexIndices.size());
    {
        int globalIdx = 0;
        auto& subs = m_editableMesh->subMeshes();
        for (size_t s = 0; s < subs.size(); ++s) {
            for (size_t v = 0; v < subs[s].vertices.size(); ++v) {
                int g = globalIdx + static_cast<int>(v);
                if (std::find(vertexIndices.begin(), vertexIndices.end(), g)
                    != vertexIndices.end()) {
                    // The HE mesh indexes one HEVertex per (submesh, local)
                    // in the order subs were built — same as `globalIdx + v`.
                    heIndices.push_back(g);
                }
            }
            globalIdx += static_cast<int>(subs[s].vertices.size());
        }
    }
    if (heIndices.empty()) return false;

    std::vector<int> newHEVertices = meshHasNGons
        ? heMesh.bevelVerticesNgon(heIndices, width, segments, 0.5f, profilePoints)
        : heMesh.bevelVertices(heIndices, width, segments, 0.5f, profilePoints);
    if (newHEVertices.empty())
        return false;

    std::vector<Ogre::Vector3> newVertPositions;
    newVertPositions.reserve(newHEVertices.size());
    for (int heVert : newHEVertices)
        newVertPositions.push_back(heMesh.vertex(heVert).position);

    EditableMesh newMesh;
    if (!heMesh.toEditableMesh(newMesh))
        return false;
    (void)originalSubMeshes; // n-gon path preserves quads natively

    m_editableMesh->subMeshes() = std::move(newMesh.subMeshes());

    if (m_normalsMode == 0)
        m_editableMesh->recalculateNormals();
    else
        m_editableMesh->recalculateNormalsFlat();

    if (!m_editableMesh->resizeEntityBuffers(m_editEntity))
        return false;

    rewriteEntityAfterTopologyChange(m_editEntity);

    m_selectedVertices.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    std::set<int> newGlobalVerts;
    for (const auto& pos : newVertPositions) {
        int globalIdx = 0;
        bool found = false;
        for (size_t s = 0; s < m_editableMesh->subMeshes().size() && !found; ++s) {
            const auto& verts = m_editableMesh->subMeshes()[s].vertices;
            for (size_t v = 0; v < verts.size(); ++v) {
                if (verts[v].position.squaredDistance(pos) < 1e-8f) {
                    newGlobalVerts.insert(globalIdx + static_cast<int>(v));
                    found = true;
                    break;
                }
            }
            globalIdx += static_cast<int>(verts.size());
        }
    }
    m_selectedVertices = newGlobalVerts;

    refreshNormalVisualizer();
    updateSelectionOverlay();
    validateMesh();

    emit meshDataChanged();
    emit editSelectionChanged();
    emit selectionModeChanged();
    emit editModeChanged();

    return true;
}

bool EditModeController::reapplyActiveBevel(
    float width, int segments, const std::vector<float>& profilePoints)
{
    if (!m_bevelSession.active) return false;
    if (m_bevelSession.kind == BevelSession::Vertices)
        return applyBevelVertexTopology(m_bevelSession.targetVertices,
                                        width, segments, profilePoints);
    return applyBevelTopology(m_bevelSession.targetEdges, width,
                              segments, profilePoints);
}

bool EditModeController::beginBevel()
{
    if (m_bevelSession.active)
        return false;
    if (!m_editModeActive || !m_editableMesh || !m_editEntity)
        return false;
    // Knife and bevel are mutually exclusive — a stray knife session would
    // keep its preview overlay visible behind the bevel gizmo.
    if (m_knifeSession.active) cancelKnife();
    const bool edgeValid = (m_selectionMode == EdgeMode && !m_selectedEdges.empty());
    const bool vertValid = (m_selectionMode == VertexMode && !m_selectedVertices.empty());
    if (!edgeValid && !vertValid) return false;

    SentryReporter::addBreadcrumb("edit_mode",
        QString("Bevel: begin session (%1=%2)")
            .arg(edgeValid ? "edges" : "vertices")
            .arg(edgeValid ? m_selectedEdges.size() : m_selectedVertices.size()));

    BevelSession s;
    s.kind = edgeValid ? BevelSession::Edges : BevelSession::Vertices;
    s.originalSubMeshes = m_editableMesh->subMeshes();
    s.origSelectedVertices = m_selectedVertices;
    s.origSelectedEdges = m_selectedEdges;
    s.origSelectedFaces = m_selectedFaces;
    if (edgeValid)
        s.targetEdges.assign(m_selectedEdges.begin(), m_selectedEdges.end());
    else
        s.targetVertices.assign(m_selectedVertices.begin(), m_selectedVertices.end());

    // Compute gizmo pivot (centroid of edge endpoints) and axis (averaged
    // adjacent-face normal at those edges). Uses the pre-bevel mesh so the
    // gizmo stays put while the user drags.
    Ogre::Vector3 pivot = Ogre::Vector3::ZERO;
    Ogre::Vector3 normalSum = Ogre::Vector3::ZERO;
    int pivotCount = 0;
    {
        // Build global-index → (submesh, local) lookup by walking vertex
        // arrays once.
        const auto& subs = m_editableMesh->subMeshes();
        auto posOf = [&](int gIdx) -> Ogre::Vector3 {
            int off = 0;
            for (const auto& sub : subs) {
                int n = static_cast<int>(sub.vertices.size());
                if (gIdx < off + n)
                    return sub.vertices[gIdx - off].position;
                off += n;
            }
            return Ogre::Vector3::ZERO;
        };
        if (s.kind == BevelSession::Edges) {
            for (const auto& [v1, v2] : s.targetEdges) {
                Ogre::Vector3 a = posOf(v1);
                Ogre::Vector3 b = posOf(v2);
                pivot += (a + b) * 0.5f;
                ++pivotCount;
            }
        } else {
            for (int vIdx : s.targetVertices) {
                pivot += posOf(vIdx);
                ++pivotCount;
            }
        }
        if (pivotCount > 0) pivot /= static_cast<float>(pivotCount);

        // Sum per-face normals of triangles containing any selected
        // vertex. Gives a reasonable "outward" axis even for irregular fans.
        std::set<int> edgeVerts;
        if (s.kind == BevelSession::Edges) {
            for (const auto& [v1, v2] : s.targetEdges) {
                edgeVerts.insert(v1);
                edgeVerts.insert(v2);
            }
        } else {
            for (int vIdx : s.targetVertices) edgeVerts.insert(vIdx);
        }
        int vertOffset = 0;
        for (const auto& sub : subs) {
            for (const auto& tri : sub.triangles) {
                int g0 = vertOffset + static_cast<int>(tri.indices[0]);
                int g1 = vertOffset + static_cast<int>(tri.indices[1]);
                int g2 = vertOffset + static_cast<int>(tri.indices[2]);
                if (!edgeVerts.count(g0) && !edgeVerts.count(g1) && !edgeVerts.count(g2))
                    continue;
                const auto& p0 = sub.vertices[tri.indices[0]].position;
                const auto& p1 = sub.vertices[tri.indices[1]].position;
                const auto& p2 = sub.vertices[tri.indices[2]].position;
                Ogre::Vector3 n = (p1 - p0).crossProduct(p2 - p0);
                if (n.length() > 1e-8f) {
                    n.normalise();
                    normalSum += n;
                }
            }
            vertOffset += static_cast<int>(sub.vertices.size());
        }
    }
    s.pivot = pivot;
    s.axis = (normalSum.length() > 1e-6f) ? normalSum.normalisedCopy() : Ogre::Vector3::UNIT_Y;
    s.width = 0.05f; // 2.5% of a 2-unit cube — visible initial chamfer

    // Compute the largest width the bevel algorithm will actually apply
    // before its internal per-vertex/per-edge clamp takes over, so the
    // drag handler can freeze the gizmo at the same scalar the topology
    // op would freeze at. See the named helpers at the top of this file
    // (computeVertexBevelCap / computeEdgeBevelCap) for the exact
    // formulas, which mirror HalfEdgeMesh::bevelVertices and
    // HalfEdgeMesh::bevelEdges respectively.
    {
        HalfEdgeMesh hm;
        if (hm.buildFromEditableMesh(*m_editableMesh)) {
            const float cap = (s.kind == BevelSession::Vertices)
                              ? computeVertexBevelCap(hm, s.targetVertices)
                              : computeEdgeBevelCap(hm, s.targetEdges);
            if (cap > 0.0f && std::isfinite(cap)) s.maxWidth = cap;
        }
    }

    const bool applied = (s.kind == BevelSession::Edges)
        ? applyBevelTopology(s.targetEdges, s.width)
        : applyBevelVertexTopology(s.targetVertices, s.width);
    if (!applied) {
        // Failed — undo any partial state by restoring the snapshot.
        m_editableMesh->subMeshes() = std::move(s.originalSubMeshes);
        m_selectedVertices = std::move(s.origSelectedVertices);
        m_selectedEdges = std::move(s.origSelectedEdges);
        m_selectedFaces = std::move(s.origSelectedFaces);
        return false;
    }

    s.active = true;
    m_bevelSession = std::move(s);
    emit bevelProfilePointsChanged();

    // Spawn the gizmo. Created lazily against the edit entity's scene manager.
    if (!m_bevelGizmo && m_editEntity) {
        auto* sceneMgr = m_editEntity->_getManager();
        if (sceneMgr)
            m_bevelGizmo = std::make_unique<BevelGizmo>(sceneMgr);
    }
    if (m_bevelGizmo) {
        m_bevelGizmo->setAxis(bevelGizmoWorldOrigin(), bevelGizmoWorldAxis());
        // Start the handle at the default shaft tip (matches buildGeometry).
        m_bevelGizmo->setHandleOffset(0.4f);
        m_bevelGizmo->setVisible(true);
    }

    emit editModeChanged(); // UI hook so inspector can show "bevel active" state.
    return true;
}

Ogre::Vector3 EditModeController::bevelGizmoWorldOrigin() const
{
    if (!m_editEntity) return m_bevelSession.pivot;
    auto* node = m_editEntity->getParentSceneNode();
    if (!node) return m_bevelSession.pivot;
    return node->convertLocalToWorldPosition(m_bevelSession.pivot);
}

Ogre::Vector3 EditModeController::bevelGizmoWorldAxis() const
{
    if (!m_editEntity) return m_bevelSession.axis;
    auto* node = m_editEntity->getParentSceneNode();
    if (!node) return m_bevelSession.axis;
    return node->convertLocalToWorldOrientation(Ogre::Quaternion::IDENTITY) * m_bevelSession.axis;
}

bool EditModeController::isBevelGizmoHandle(const Ogre::MovableObject* obj) const
{
    return m_bevelGizmo && m_bevelGizmo->isHandle(obj);
}

void EditModeController::tickBevelGizmo(const Ogre::Camera* camera)
{
    if (!m_bevelSession.active || !m_bevelGizmo || !camera)
        return;
    m_bevelGizmo->updateScreenSpaceScale(camera);
}

void EditModeController::updateBevelFromDrag(const Ogre::Ray& startRay,
                                              const Ogre::Ray& dragRay,
                                              float startWidth)
{
    if (!m_bevelSession.active || !m_bevelGizmo)
        return;
    float startT = m_bevelGizmo->distanceAlongAxis(startRay);
    float curT = m_bevelGizmo->distanceAlongAxis(dragRay);
    float delta = curT - startT;
    // Map axis-distance-delta to width-delta 1:1 in local units. Positive
    // delta (drag away from the mesh) grows the bevel; negative shrinks it.
    float newWidth = startWidth + delta;
    if (newWidth < 1e-4f) newWidth = 1e-4f;

    // Cap against the pre-computed per-session maximum so the shaft/handle
    // don't slide past the point where the bevel algorithm's own clamp
    // stops updating the mesh. Without this, the gizmo visibly grows
    // while the bevel is frozen — a confusing UX signal.
    bool capped = false;
    if (std::isfinite(m_bevelSession.maxWidth) && newWidth > m_bevelSession.maxWidth) {
        newWidth = m_bevelSession.maxWidth;
        capped = true;
    }
    updateBevelWidth(newWidth);

    // Compute the handle position from the *effective* width delta so the
    // visual shaft length matches the applied bevel. When capped, delta is
    // pinned to (maxWidth - startWidth) so the handle freezes too.
    float effectiveDelta = capped ? (newWidth - startWidth) : delta;
    float handleLocalY = std::max(0.02f, 0.4f + effectiveDelta);
    m_bevelGizmo->setHandleOffset(handleLocalY);
}

void EditModeController::updateBevelWidth(float width)
{
    if (!m_bevelSession.active)
        return;
    if (width <= 0.0f)
        width = 1e-4f; // keep just enough to stay a bevel (not a collapse)

    // Restore the pre-bevel mesh state, then re-apply at the new width.
    m_editableMesh->subMeshes() = m_bevelSession.originalSubMeshes;
    m_selectedVertices = m_bevelSession.origSelectedVertices;
    m_selectedEdges = m_bevelSession.origSelectedEdges;
    m_selectedFaces = m_bevelSession.origSelectedFaces;

    if (reapplyActiveBevel(width, m_bevelSession.segments,
                           m_bevelSession.profilePoints))
        m_bevelSession.width = width;
}

QVariantList EditModeController::bevelProfilePoints() const
{
    QVariantList out;
    out.reserve(static_cast<int>(m_bevelSession.profilePoints.size()));
    for (float v : m_bevelSession.profilePoints)
        out.append(v);
    return out;
}

void EditModeController::updateBevelSegments(int segments)
{
    if (!m_bevelSession.active) return;
    if (segments < 1) segments = 1;
    if (segments == m_bevelSession.segments) return;

    // Resample existing profile points onto the new segment count so the
    // user's curve shape is preserved when the spinner moves up/down.
    std::vector<float> newPoints(segments > 1 ? segments - 1 : 0, 0.5f);
    if (!m_bevelSession.profilePoints.empty() && segments > 1) {
        const int oldN = m_bevelSession.segments;
        const int newN = segments;
        for (int i = 1; i < newN; ++i) {
            const float tNew = static_cast<float>(i) / static_cast<float>(newN);
            const float oldPos = tNew * static_cast<float>(oldN);
            const int lo = std::max(1, std::min(oldN - 1,
                                   static_cast<int>(std::floor(oldPos))));
            const int hi = std::max(1, std::min(oldN - 1,
                                   static_cast<int>(std::ceil(oldPos))));
            if (lo == hi) {
                newPoints[i - 1] = m_bevelSession.profilePoints[lo - 1];
            } else {
                const float frac = oldPos - static_cast<float>(lo);
                newPoints[i - 1] = m_bevelSession.profilePoints[lo - 1] * (1.0f - frac)
                                 + m_bevelSession.profilePoints[hi - 1] * frac;
            }
        }
    }

    m_editableMesh->subMeshes() = m_bevelSession.originalSubMeshes;
    m_selectedVertices = m_bevelSession.origSelectedVertices;
    m_selectedEdges = m_bevelSession.origSelectedEdges;
    m_selectedFaces = m_bevelSession.origSelectedFaces;

    if (reapplyActiveBevel(m_bevelSession.width, segments, newPoints)) {
        m_bevelSession.segments = segments;
        m_bevelSession.profilePoints = std::move(newPoints);
        emit bevelProfilePointsChanged();
    }
}

void EditModeController::updateBevelProfilePoint(int index, float value)
{
    if (!m_bevelSession.active) return;
    if (m_bevelSession.segments < 2) return;
    if (index < 0 || index >= static_cast<int>(m_bevelSession.profilePoints.size()))
        return;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    if (std::abs(value - m_bevelSession.profilePoints[index]) < 1e-4f) return;

    std::vector<float> newPoints = m_bevelSession.profilePoints;
    newPoints[index] = value;

    m_editableMesh->subMeshes() = m_bevelSession.originalSubMeshes;
    m_selectedVertices = m_bevelSession.origSelectedVertices;
    m_selectedEdges = m_bevelSession.origSelectedEdges;
    m_selectedFaces = m_bevelSession.origSelectedFaces;

    if (reapplyActiveBevel(m_bevelSession.width,
                           m_bevelSession.segments, newPoints)) {
        m_bevelSession.profilePoints = std::move(newPoints);
        emit bevelProfilePointsChanged();
    }
}

void EditModeController::resetBevelProfile()
{
    if (!m_bevelSession.active) return;
    if (m_bevelSession.segments < 2) return;

    std::vector<float> newPoints(m_bevelSession.segments - 1, 0.5f);
    if (newPoints == m_bevelSession.profilePoints) return;

    m_editableMesh->subMeshes() = m_bevelSession.originalSubMeshes;
    m_selectedVertices = m_bevelSession.origSelectedVertices;
    m_selectedEdges = m_bevelSession.origSelectedEdges;
    m_selectedFaces = m_bevelSession.origSelectedFaces;

    if (reapplyActiveBevel(m_bevelSession.width,
                           m_bevelSession.segments, newPoints)) {
        m_bevelSession.profilePoints = std::move(newPoints);
        emit bevelProfilePointsChanged();
    }
}

void EditModeController::commitBevel()
{
    if (!m_bevelSession.active)
        return;

    SentryReporter::addBreadcrumb("edit_mode",
        QString("Bevel: commit (width=%1, segments=%2)")
            .arg(m_bevelSession.width, 0, 'f', 4)
            .arg(m_bevelSession.segments));

    auto* cmd = new EditMeshTopologyCommand(
        std::move(m_bevelSession.originalSubMeshes),
        m_editableMesh->subMeshes(),
        m_bevelSession.origSelectedVertices, m_bevelSession.origSelectedEdges,
        m_bevelSession.origSelectedFaces,
        m_selectedVertices, m_selectedEdges, m_selectedFaces,
        "Bevel");
    UndoManager::getSingleton()->push(cmd);

    m_bevelSession = {};
    if (m_bevelGizmo) { m_bevelGizmo->setVisible(false); }
    emit bevelProfilePointsChanged();
    emit editModeChanged();
}

void EditModeController::cancelBevel()
{
    if (!m_bevelSession.active)
        return;

    SentryReporter::addBreadcrumb("edit_mode",
        QString("Bevel: cancel (width=%1, segments=%2)")
            .arg(m_bevelSession.width, 0, 'f', 4)
            .arg(m_bevelSession.segments));

    m_editableMesh->subMeshes() = std::move(m_bevelSession.originalSubMeshes);
    m_selectedVertices = std::move(m_bevelSession.origSelectedVertices);
    m_selectedEdges = std::move(m_bevelSession.origSelectedEdges);
    m_selectedFaces = std::move(m_bevelSession.origSelectedFaces);

    // Re-sync the entity with the restored mesh — preserves wireframe /
    // material-editor SubEntity overrides through the Esc path.
    m_editableMesh->resizeEntityBuffers(m_editEntity);
    rewriteEntityAfterTopologyChange(m_editEntity);

    m_bevelSession = {};
    if (m_bevelGizmo) m_bevelGizmo->setVisible(false);

    refreshNormalVisualizer();
    updateSelectionOverlay();
    validateMesh();
    emit bevelProfilePointsChanged();
    emit meshDataChanged();
    emit editSelectionChanged();
    emit editModeChanged();
}

bool EditModeController::bevelSelection()
{
    // Interactive bevel: opens a session with the gizmo visible. User commits
    // with a click outside the gizmo, cancels with Esc, or the session ends
    // automatically on tool-switch / edit-mode exit. Pressing Cmd+B again
    // while a session is active commits the current one.
    if (m_bevelSession.active) {
        commitBevel();
        return true;
    }
    return beginBevel();
}

// ===========================================================================
// Knife tool
// ===========================================================================

bool EditModeController::beginKnife()
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity) return false;
    if (m_knifeSession.active) return true;
    // Knife lives alongside bevel but they shouldn't both be active.
    if (m_bevelSession.active) cancelBevel();

    m_knifeSession = {};
    m_knifeSession.active = true;

    SentryReporter::addBreadcrumb("edit_mode", "Knife: begin session");
    emit knifeSessionChanged();
    return true;
}

bool EditModeController::addKnifePoint(OgreWidget* widget, int screenX, int screenY)
{
    if (!m_knifeSession.active || !widget) return false;

    KnifePoint pt;
    if (!knifeHitTest(QPoint(screenX, screenY), widget, pt)) return false;

    m_knifeSession.points.push_back(pt);
    m_knifeSession.hoverValid = false;
    updateKnifePreviewOverlay();

    SentryReporter::addBreadcrumb("edit_mode",
        QString("Knife: point added (n=%1)").arg(m_knifeSession.points.size()));
    emit knifeSessionChanged();
    return true;
}

bool EditModeController::addKnifePointOnEdge(int heEdgeIndex, float t)
{
    if (!m_knifeSession.active || !m_editableMesh) return false;

    // Resolve the edge's endpoint vertices from a fresh HE build; we need
    // them to place the KnifePoint's localPosition for the preview overlay
    // and to expose the same data the widget-based path produces.
    HalfEdgeMesh hm;
    if (!hm.buildFromEditableMesh(*m_editableMesh)) return false;
    if (heEdgeIndex < 0 || heEdgeIndex >= static_cast<int>(hm.edgeCount())) return false;
    const auto [ga, gb] = hm.edgeVertices(heEdgeIndex);
    if (ga < 0 || gb < 0) return false;

    auto [subA, locA] = globalToLocal(ga);
    auto [subB, locB] = globalToLocal(gb);
    const auto& subs = m_editableMesh->subMeshes();
    if (subA >= subs.size() || subB >= subs.size()) return false;
    if (locA >= subs[subA].vertices.size() || locB >= subs[subB].vertices.size()) return false;

    const auto& p0 = subs[subA].vertices[locA].position;
    const auto& p1 = subs[subB].vertices[locB].position;
    const float clampT = std::clamp(t, 0.0f, 1.0f);

    KnifePoint pt;
    pt.kind = KnifePoint::OnEdge;
    pt.edgeIndex = heEdgeIndex;
    pt.edgeT = clampT;
    pt.localPosition = p0 + (p1 - p0) * clampT;

    m_knifeSession.points.push_back(pt);
    m_knifeSession.hoverValid = false;
    updateKnifePreviewOverlay();

    SentryReporter::addBreadcrumb("edit_mode",
        QString("Knife: point added programmatically (n=%1)").arg(m_knifeSession.points.size()));
    emit knifeSessionChanged();
    return true;
}

void EditModeController::updateKnifeHover(OgreWidget* widget, int screenX, int screenY)
{
    if (!m_knifeSession.active || !widget) return;
    m_knifeSession.hoverValid =
        knifeHitTest(QPoint(screenX, screenY), widget, m_knifeSession.hover);
    updateKnifePreviewOverlay();
}

bool EditModeController::commitKnife()
{
    if (!m_knifeSession.active) return false;
    if (m_knifeSession.points.size() < 2) {
        SentryReporter::addBreadcrumb("edit_mode",
            "Knife: commit rejected (fewer than 2 points)");
        cancelKnife();
        return false;
    }

    // Snapshot for undo.
    EditableMesh snapshot;
    std::vector<EditableSubMesh> originalSubMeshes = m_editableMesh->subMeshes();

    // Build an HE view once and hand the confirmed-click list to cutPath,
    // which runs the walk-and-cut algorithm: splits each click endpoint
    // and every interior edge crossed by the straight-line cut between
    // consecutive endpoints, so the visible preview becomes a real chain
    // of mesh edges. OnFace/OnVertex clicks aren't yet in scope — the
    // commit skips them and proceeds on the OnEdge subset.
    //
    // splitEdge is now n-gon-aware: it inserts vMid into each adjacent
    // face's loop without introducing a fan diagonal. cutPath's walk
    // calls splitFace explicitly to materialise the cut between
    // consecutive vertices. So we can build the HE directly from the
    // (possibly n-gon) editable mesh — no triangle-mode workaround.
    HalfEdgeMesh hm;
    if (!hm.buildFromEditableMesh(*m_editableMesh)) {
        cancelKnife();
        return false;
    }

    // Build the CutPoint list, translating OnVertex clicks into edge
    // clicks: pick any incident edge to the vertex and use t=0 or t=1
    // depending on which endpoint the vertex sits at. `cutPath` only
    // accepts edge inputs (its `splitEdge` primitive is edge-keyed),
    // and `splitEdge` clamps t away from 0/1 by 1e-4 to avoid sliver
    // faces — so the resulting vertex sits a hair off the user's click
    // but topology stays clean. OnFace clicks are still rejected: the
    // commit can't represent them with the current edge-walk algorithm.
    std::vector<HalfEdgeMesh::CutPoint> cpts;
    cpts.reserve(m_knifeSession.points.size());
    for (const auto& p : m_knifeSession.points) {
        if (p.kind == KnifePoint::OnEdge) {
            cpts.push_back({p.edgeIndex, p.edgeT});
            continue;
        }
        if (p.kind != KnifePoint::OnVertex) {
            SentryReporter::addBreadcrumb("edit_mode",
                "Knife: commit rejected (OnFace point — only edge / vertex supported)");
            cancelKnife();
            return false;
        }
        // OnVertex → find any incident edge in the triangle-mode HE
        // and pick the t that puts the new split-vertex closest to the
        // clicked vertex.
        int incidentEdge = -1;
        float incidentT = 0.0f;
        for (size_t e = 0; e < hm.edgeCount(); ++e) {
            const auto [ev0, ev1] = hm.edgeVertices(static_cast<int>(e));
            if (ev0 == p.vertexIndex) { incidentEdge = static_cast<int>(e); incidentT = 0.0f; break; }
            if (ev1 == p.vertexIndex) { incidentEdge = static_cast<int>(e); incidentT = 1.0f; break; }
        }
        if (incidentEdge < 0) {
            SentryReporter::addBreadcrumb("edit_mode",
                "Knife: commit rejected (OnVertex point — no incident edge)");
            cancelKnife();
            return false;
        }
        cpts.push_back({incidentEdge, incidentT});
    }
    if (cpts.size() < 2) {
        SentryReporter::addBreadcrumb("edit_mode",
            "Knife: commit rejected (fewer than 2 edge clicks)");
        cancelKnife();
        return false;
    }
    const auto cutVerts = hm.cutPath(cpts);
    if (cutVerts.empty()) {
        SentryReporter::addBreadcrumb("edit_mode",
            "Knife: commit rejected (cutPath failed)");
        cancelKnife();
        return false;
    }
    const int inserted = static_cast<int>(cutVerts.size());

    // Write the modified mesh back. Topology changed (new verts and
    // tris), so we must resize the Ogre buffers and re-initialise the
    // entity's subentity caches — same sequence applyBevelTopology
    // uses. Capture per-subentity material names first so the wireframe
    // / MaterialEditor overrides survive the deinit/init round-trip;
    // invalidate RTSS afterwards so its shader cache picks up the new
    // vertex layout.
    EditableMesh updated;
    if (!hm.toEditableMesh(updated)) {
        cancelKnife();
        return false;
    }
    m_editableMesh->subMeshes() = std::move(updated.subMeshes());
    m_editableMesh->resizeEntityBuffers(m_editEntity);

    rewriteEntityAfterTopologyChange(m_editEntity);

    // Clear edge/face selection — pre-cut IDs refer to retired topology
    // slots and the walk added new vertices that aren't in any existing
    // set. Leaving stale indices risks crashes in later overlay updates.
    m_selectedVertices.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    auto* cmd = new EditMeshTopologyCommand(
        std::move(originalSubMeshes),
        m_editableMesh->subMeshes(),
        m_selectedVertices, m_selectedEdges, m_selectedFaces,
        m_selectedVertices, m_selectedEdges, m_selectedFaces,
        "Knife");
    UndoManager::getSingleton()->push(cmd);

    SentryReporter::addBreadcrumb("edit_mode",
        QString("Knife: commit (points=%1, cuts=%2)")
            .arg(m_knifeSession.points.size()).arg(inserted));

    m_knifeSession = {};
    destroyKnifePreviewOverlay();
    emit knifeSessionChanged();
    emit meshDataChanged();
    return true;
}

// ---------------------------------------------------------------------------
// Merge vertices
//
// All four public entry points share the same plumbing: take the current
// VertexMode selection, run an HEMesh transform, write the result back. The
// only thing that changes between them is which target position we hand to
// the HE primitive (centroid / first / last / per-cluster centroid for
// by-distance). `applyMergeOp` factors that boilerplate.
// ---------------------------------------------------------------------------
// Shared post-mesh-mutation hook: rewrite the entity's Ogre buffers,
// save / restore per-subentity material overrides, and tell RTSS to
// re-link shaders against the new vertex layout. Used by every Edit-
// Mode topology op AND by EditMeshTopologyCommand::applyMeshState so
// undo/redo preserves bump map / per-pixel lighting state.
//
// Bump-map / per-pixel lighting handling: after a topology op the new
// vertices written by EditableMesh::buildSubMeshBuffers have zero-valued
// tangents (EditableVertex defaults), and the declaration may even drop
// the VES_TANGENT slot entirely if `vertices[0].hasTangent == false`
// (which is the case on the n-gon import path that omits
// aiProcess_CalcTangentSpace). Either way RTSS's SRS_NORMALMAP TBN math
// collapses, dropping bump mapping and (depending on the shader path)
// basic per-pixel lighting. Fix: force `Mesh::buildTangentVectors`
// BEFORE `_deinitialise/_initialise` so the SubEntity vertex-decl cache
// reads the corrected layout, then re-run `applyNormalMapsToEntity` so
// RTSS re-attaches SRS_NORMALMAP against the fresh tangents.
namespace {
// Returns true if any sub-entity of `ent` has a normal-map TUS — the
// signal we use to decide whether RTSS will need tangents on the next
// render. Materials that fail to load are skipped so a single broken
// resource doesn't abort the topology op.
bool entityWantsTangents(Ogre::Entity* ent) {
    for (unsigned int i = 0; i < ent->getNumSubEntities(); ++i) {
        auto mat = ent->getSubEntity(i)->getMaterial();
        if (!mat) continue;
        if (!mat->isLoaded()) {
            try { mat->load(); }
            catch (const Ogre::Exception&) { continue; }
        }
        if (mat->getNumTechniques() == 0) continue;
        auto* pass = mat->getTechnique(0)->getPass(0);
        if (!pass) continue;
        for (unsigned short t = 0; t < pass->getNumTextureUnitStates(); ++t) {
            const auto& tusName = pass->getTextureUnitState(t)->getName();
            if (tusName == "normal_map" || tusName == "NormalMap") return true;
        }
    }
    return false;
}

// Force-rebuild tangent vectors on `mesh`. Logs (rather than throws) on
// failure since the caller runs from inside an entity-refresh hook.
void rebuildMeshTangents(const Ogre::MeshPtr& mesh) {
    if (!mesh) return;
    try {
        // storeParityInW=true → VET_FLOAT4, matching what
        // RTShaderHelper::applyNormalMap and the import path use.
        mesh->buildTangentVectors(/*sourceTexCoordSet=*/0,
                                  /*splitMirrored=*/false,
                                  /*splitRotated=*/false,
                                  /*storeParityInW=*/true);
    } catch (const Ogre::Exception& e) {
        Ogre::LogManager::getSingleton().logMessage(
            "rewriteEntityAfterTopologyChange: buildTangentVectors "
            "failed for '" + mesh->getName() + "': " + e.getDescription());
    }
}

// Drop the cached RTSS shader programs for every sub-entity material
// so the next render regenerates them against the post-topology-op
// vertex declaration. No-op when the ShaderGenerator is absent.
void invalidateEntityRtssMaterials(Ogre::Entity* ent) {
    auto* sg = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    if (!sg) return;
    for (unsigned int i = 0; i < ent->getNumSubEntities(); ++i) {
        const std::string& m = ent->getSubEntity(i)->getMaterialName();
        if (!m.empty())
            sg->invalidateMaterial(
                Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME, m);
    }
}
} // namespace

void EditModeController::rewriteEntityAfterTopologyChange(Ogre::Entity* ent) {
    if (!ent) return;

    // Snapshot per-subentity material overrides — _deinitialise/_initialise
    // resets them to the SubMesh default, but the wireframe overlay and
    // MaterialEditor write to the SubEntity, not the SubMesh, so both
    // must survive the rebuild.
    std::vector<std::string> preMats;
    preMats.reserve(ent->getNumSubEntities());
    for (unsigned int i = 0; i < ent->getNumSubEntities(); ++i)
        preMats.push_back(ent->getSubEntity(i)->getMaterialName());

    // Rebuild tangents BEFORE `_deinitialise/_initialise` so the
    // SubEntity vertex-decl cache captured during `_initialise` already
    // sees the VES_TANGENT element. Calling `buildTangentVectors` AFTER
    // `_initialise` is too late: the SubEntity has already linked
    // against the old (no-tangent) declaration and the next render
    // compiles RTSS shaders against that stale layout, so the
    // SRS_NORMALMAP code path collapses to a no-op even though the
    // tangents physically exist in the buffer.
    //
    // We rebuild whenever any subentity is bump-mapped: new vertices
    // written by `EditableMesh::buildSubMeshBuffers` start with zero-
    // valued tangents (EditableVertex defaults), and on the n-gon
    // import path — which omits `aiProcess_CalcTangentSpace` because
    // that flag forces triangulation — the declaration drops VES_TANGENT
    // entirely. `Mesh::buildTangentVectors` re-adds the element if
    // missing and recomputes values from positions/normals/UVs.
    if (entityWantsTangents(ent))
        rebuildMeshTangents(ent->getMesh());

    // Re-init the entity so its SubEntity caches (vertex / index
    // counts, skeleton anim buffers) sync to the resized mesh.
    // Without this the next render uses stale draw-call params and
    // the topology change appears as holes / broken geometry.
    ent->_deinitialise();
    ent->_initialise(true);

    for (unsigned int i = 0;
         i < ent->getNumSubEntities() && i < preMats.size(); ++i) {
        if (!preMats[i].empty())
            ent->getSubEntity(i)->setMaterialName(preMats[i]);
    }

    // Re-attach the RTSS SRS_NORMALMAP sub-render-state per material
    // that has a normal-map TUS, then drop any cached shader programs
    // so the next render regenerates against the new vertex layout.
    // `applyNormalMapsToEntity` uses `removeShaderBasedTechnique +
    // createShaderBasedTechnique` to start from a clean slate (some
    // tangent-related state doesn't survive a buffer-format change),
    // so it must run AFTER the tangent rebuild + _initialise.
    MeshImporterExporter::applyNormalMapsToEntity(ent);
    invalidateEntityRtssMaterials(ent);
}

// Find global vertex indices of the survivor positions after toEditableMesh
// re-packed the per-submesh arrays. Each survivor is the unique vertex at
// `pos` (within `eps`) in the post-merge mesh. Mirrors the pattern bevel
// uses to re-select its newly-created vertices.
static std::set<int> findGlobalVertsByPosition(
    const EditableMesh* mesh, const std::vector<Ogre::Vector3>& targets)
{
    std::set<int> result;
    if (!mesh || targets.empty()) return result;

    constexpr float eps2 = 1e-8f;
    int globalIdx = 0;
    for (const auto& sub : mesh->subMeshes()) {
        for (size_t v = 0; v < sub.vertices.size(); ++v) {
            for (const auto& pos : targets) {
                if (sub.vertices[v].position.squaredDistance(pos) < eps2) {
                    result.insert(globalIdx + static_cast<int>(v));
                    break;
                }
            }
        }
        globalIdx += static_cast<int>(sub.vertices.size());
    }
    return result;
}

// Shared post-merge plumbing for all four merge ops. Captures pre-merge
// selection for undo, runs the supplied HE-side merge, writes the new
// submeshes back, recomputes normals, refreshes the entity, and re-selects
// the survivor vertex (or vertices, for By Distance) by hunting the target
// position(s) in the re-packed mesh.
//
// `mergeFn` does the actual HE mutation; it gets the prepared HEMesh and
// must return the number of retired vertices (0 = no-op, abort).
//
// `survivorTargets` is the world-space position(s) the user expected the
// survivor to land at — used to re-select after toEditableMesh re-packs.
int EditModeController::applyMergeAndRefresh(
    const QString& opLabel,
    const std::function<int(HalfEdgeMesh&)>& mergeFn,
    const std::vector<Ogre::Vector3>& survivorTargets)
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity) return 0;
    if (m_selectionMode != VertexMode) return 0;
    if (m_selectedVertices.size() < 2) return 0;

    HalfEdgeMesh hm;
    if (!hm.buildFromEditableMesh(*m_editableMesh)) return 0;

    // Snapshot mesh + selection BEFORE the mutation so undo restores both
    // (CodeRabbit critical: previously the cleared selection was passed
    // for both old and new state, leaving undo with an empty selection).
    auto originalSubMeshes = m_editableMesh->subMeshes();
    const auto preSelectedVerts = m_selectedVertices;
    const auto preSelectedEdges = m_selectedEdges;
    const auto preSelectedFaces = m_selectedFaces;

    const int retired = mergeFn(hm);
    if (retired == 0) return 0;

    EditableMesh updated;
    if (!hm.toEditableMesh(updated)) return 0;
    m_editableMesh->subMeshes() = std::move(updated.subMeshes());

    // Recompute normals so shading stays right across the new topology —
    // bevel/extrude do this for the same reason. Respect the current
    // smooth/flat mode so the user doesn't lose their setting on merge.
    if (m_normalsMode == 0)
        m_editableMesh->recalculateNormals();
    else
        m_editableMesh->recalculateNormalsFlat();

    m_editableMesh->resizeEntityBuffers(m_editEntity);
    rewriteEntityAfterTopologyChange(m_editEntity);

    // Re-select survivor(s) by position. toEditableMesh re-packs per
    // submesh so old indices are dead, but the position is stable —
    // matches the bevel post-op refresh pattern.
    m_selectedVertices = findGlobalVertsByPosition(m_editableMesh.get(),
                                                   survivorTargets);
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    auto* cmd = new EditMeshTopologyCommand(
        std::move(originalSubMeshes),
        m_editableMesh->subMeshes(),
        preSelectedVerts, preSelectedEdges, preSelectedFaces,
        m_selectedVertices, m_selectedEdges, m_selectedFaces,
        opLabel);
    UndoManager::getSingleton()->push(cmd);

    SentryReporter::addBreadcrumb("edit_mode",
        QString("%1 (removed=%2)").arg(opLabel).arg(retired));

    updateSelectionOverlay();
    refreshNormalVisualizer();
    emit editSelectionChanged();
    emit meshDataChanged();
    return retired;
}

int EditModeController::mergeAtCenter()
{
    if (m_selectedVertices.empty()) return 0;
    HalfEdgeMesh probe; // peek at positions before the real run
    if (!probe.buildFromEditableMesh(*m_editableMesh)) return 0;

    std::vector<int> verts(m_selectedVertices.begin(), m_selectedVertices.end());
    Ogre::Vector3 centroid = Ogre::Vector3::ZERO;
    for (int v : verts) centroid += probe.vertex(v).position;
    centroid /= static_cast<float>(verts.size());

    return applyMergeAndRefresh("Merge At Center",
        [&verts, centroid](HalfEdgeMesh& hm) {
            return hm.mergeVertices(verts, centroid);
        },
        {centroid});
}

int EditModeController::mergeAtFirst()
{
    if (m_selectedVertices.empty()) return 0;
    HalfEdgeMesh probe;
    if (!probe.buildFromEditableMesh(*m_editableMesh)) return 0;

    std::vector<int> verts(m_selectedVertices.begin(), m_selectedVertices.end());
    // std::set iteration is sorted ascending — verts[0] is the lowest index.
    const Ogre::Vector3 target = probe.vertex(verts.front()).position;

    return applyMergeAndRefresh("Merge At First",
        [&verts, target](HalfEdgeMesh& hm) {
            return hm.mergeVertices(verts, target);
        },
        {target});
}

int EditModeController::mergeAtLast()
{
    if (m_selectedVertices.empty()) return 0;
    HalfEdgeMesh probe;
    if (!probe.buildFromEditableMesh(*m_editableMesh)) return 0;

    std::vector<int> verts(m_selectedVertices.begin(), m_selectedVertices.end());
    // Move the highest-index vert to the front so HEMesh::mergeVertices
    // picks it as the survivor. Without this swap, the survivor is the
    // lowest index (verts[0]) and only its *position* would be retargeted
    // — semantically that's "Merge At First, with last's position", not
    // "Merge At Last". Caught by Codex P1 + CodeRabbit.
    std::swap(verts.front(), verts.back());
    const Ogre::Vector3 target = probe.vertex(verts.front()).position;

    return applyMergeAndRefresh("Merge At Last",
        [&verts, target](HalfEdgeMesh& hm) {
            return hm.mergeVertices(verts, target);
        },
        {target});
}

int EditModeController::mergeByDistance(float threshold)
{
    if (m_selectedVertices.empty()) return 0;

    // Defensive clamp: very large thresholds (or finite-but-massive caller
    // input) make t² overflow to +∞ in HEMesh, collapsing the entire
    // selection into one cluster. 1e6 world units is well past anything
    // realistic on a meter-scale mesh and stays safely under sqrt(FLT_MAX).
    if (threshold <= 0.0f) return 0;
    threshold = std::min(threshold, 1.0e6f);

    HalfEdgeMesh probe;
    if (!probe.buildFromEditableMesh(*m_editableMesh)) return 0;

    std::vector<int> verts(m_selectedVertices.begin(), m_selectedVertices.end());

    // Pre-compute the cluster centroids so we know what positions to
    // re-select after the operation. Mirrors HEMesh's internal grouping
    // (union-find by distance) but stays per-submesh because cross-submesh
    // pairs are never fused — including them in the union step inflates
    // clusters and causes the whole cluster to be rejected at HE level.
    // Codex P2.
    std::vector<Ogre::Vector3> survivorTargets;
    {
        // Group verts by submesh first.
        std::map<int, std::vector<int>> bySubmesh;
        for (int v : verts) {
            const auto faces = probe.facesAroundVertex(v);
            if (faces.empty()) continue;
            int sub = probe.face(faces.front()).subMeshIndex;
            bySubmesh[sub].push_back(v);
        }
        // Within each submesh, union by distance.
        const float t2 = threshold * threshold;
        for (auto& [sub, members] : bySubmesh) {
            std::vector<int> parent(members.size());
            std::iota(parent.begin(), parent.end(), 0);
            std::function<int(int)> find = [&](int i) {
                while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
                return i;
            };
            for (size_t i = 0; i < members.size(); ++i) {
                for (size_t j = i + 1; j < members.size(); ++j) {
                    if (probe.vertex(members[i]).position.squaredDistance(
                            probe.vertex(members[j]).position) <= t2) {
                        int ri = find(static_cast<int>(i));
                        int rj = find(static_cast<int>(j));
                        if (ri != rj) parent[std::max(ri, rj)] = std::min(ri, rj);
                    }
                }
            }
            std::map<int, std::vector<int>> clusters;
            for (size_t i = 0; i < members.size(); ++i)
                clusters[find(static_cast<int>(i))].push_back(members[i]);
            for (auto& [_, clusterMembers] : clusters) {
                if (clusterMembers.size() < 2) continue;
                Ogre::Vector3 c = Ogre::Vector3::ZERO;
                for (int m : clusterMembers) c += probe.vertex(m).position;
                c /= static_cast<float>(clusterMembers.size());
                survivorTargets.push_back(c);
            }
        }
    }

    return applyMergeAndRefresh("Merge By Distance",
        [&verts, threshold](HalfEdgeMesh& hm) {
            return hm.mergeVerticesByDistance(verts, threshold);
        },
        survivorTargets);
}

// ---------------------------------------------------------------------------
// Delete / Dissolve dispatchers
//
// Both ops follow the same plumbing as merge (snapshot mesh + selection,
// run a HE mutation, write back, recompute normals, refresh entity, push
// undo) but they don't have a survivor position to re-select on, so they
// share a separate helper that simply clears the selection on success.
// ---------------------------------------------------------------------------
namespace {
// Run an HE mutation that does not need to re-select survivors (delete /
// dissolve). Returns the count produced by the mutation. Wraps the same
// snapshot/undo/normals/refresh sequence as applyMergeAndRefresh.
int applyTopologyMutationNoSurvivor(
    EditModeController* self,
    EditableMesh* editableMesh,
    Ogre::Entity* editEntity,
    std::set<int>& selVerts,
    std::set<std::pair<int,int>>& selEdges,
    std::set<int>& selFaces,
    int normalsMode,
    const QString& opLabel,
    const std::function<int(HalfEdgeMesh&)>& mutate)
{
    if (!editableMesh || !editEntity) return 0;

    HalfEdgeMesh hm;
    if (!hm.buildFromEditableMesh(*editableMesh)) return 0;

    auto originalSubMeshes = editableMesh->subMeshes();
    const auto preSelectedVerts = selVerts;
    const auto preSelectedEdges = selEdges;
    const auto preSelectedFaces = selFaces;

    const int affected = mutate(hm);
    if (affected == 0) return 0;

    EditableMesh updated;
    if (!hm.toEditableMesh(updated)) return 0;
    editableMesh->subMeshes() = std::move(updated.subMeshes());

    if (normalsMode == 0)
        editableMesh->recalculateNormals();
    else
        editableMesh->recalculateNormalsFlat();

    editableMesh->resizeEntityBuffers(editEntity);
    EditModeController::rewriteEntityAfterTopologyChange(editEntity);

    selVerts.clear();
    selEdges.clear();
    selFaces.clear();

    auto* cmd = new EditMeshTopologyCommand(
        std::move(originalSubMeshes),
        editableMesh->subMeshes(),
        preSelectedVerts, preSelectedEdges, preSelectedFaces,
        selVerts, selEdges, selFaces,
        opLabel);
    UndoManager::getSingleton()->push(cmd);

    // Refresh degenerate-triangle bookkeeping so the Inspector's
    // validation badge doesn't carry a stale count past a delete /
    // dissolve. Other topology paths (extrude, bevel, knife, merge)
    // do this; CodeRabbit Minor flagged the omission.
    if (self) self->validateMesh();

    SentryReporter::addBreadcrumb("edit_mode",
        QString("%1 (count=%2)").arg(opLabel).arg(affected));

    return affected;
}
} // namespace

int EditModeController::deleteSelection()
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity) return 0;

    // Cancel any active interactive preview before mutating topology.
    // A live bevel snapshot or knife point list would otherwise replay
    // against the post-delete mesh on the next commit/cancel and either
    // overwrite the delete result or crash on stale indices.
    // (CodeRabbit Major)
    if (m_bevelSession.active) cancelBevel();
    if (m_knifeSession.active) cancelKnife();

    QString opLabel;
    std::function<int(HalfEdgeMesh&)> mutate;

    if (m_selectionMode == VertexMode) {
        if (m_selectedVertices.empty()) return 0;
        std::vector<int> verts(m_selectedVertices.begin(), m_selectedVertices.end());
        opLabel = "Delete Vertices";
        mutate = [verts](HalfEdgeMesh& hm) { return hm.deleteVertices(verts); };
    } else if (m_selectionMode == EdgeMode) {
        if (m_selectedEdges.empty()) return 0;
        // Resolve selected-edge global vertex pairs against the LIVE hm
        // inside the mutate lambda, not against a separate probe build.
        // applyTopologyMutationNoSurvivor builds its own HalfEdgeMesh, and
        // probe-vs-real edge ordering can drift if HE construction is not
        // deterministic against shared input. Keep both lookups on the
        // same instance. (CodeRabbit Major)
        const std::vector<std::pair<int,int>> selectedEdges(
            m_selectedEdges.begin(), m_selectedEdges.end());
        opLabel = "Delete Edges";
        mutate = [selectedEdges](HalfEdgeMesh& hm) {
            std::vector<int> edgeIdxs;
            edgeIdxs.reserve(selectedEdges.size());
            for (const auto& [a, b] : selectedEdges) {
                const int target = std::min(a, b), other = std::max(a, b);
                for (size_t e = 0; e < hm.edgeCount(); ++e) {
                    auto [ev1, ev2] = hm.edgeVertices(static_cast<int>(e));
                    if (std::min(ev1, ev2) == target
                        && std::max(ev1, ev2) == other) {
                        edgeIdxs.push_back(static_cast<int>(e));
                        break;
                    }
                }
            }
            return edgeIdxs.empty() ? 0 : hm.deleteEdges(edgeIdxs);
        };
    } else { // FaceMode
        if (m_selectedFaces.empty()) return 0;
        const std::vector<int> faces = selectedFacesAsHEFaceIndices();
        opLabel = "Delete Faces";
        mutate = [faces](HalfEdgeMesh& hm) { return hm.deleteFaces(faces); };
    }

    const int affected = applyTopologyMutationNoSurvivor(
        this, m_editableMesh.get(), m_editEntity,
        m_selectedVertices, m_selectedEdges, m_selectedFaces,
        m_normalsMode, opLabel, mutate);

    if (affected == 0) return 0;

    updateSelectionOverlay();
    refreshNormalVisualizer();
    emit editSelectionChanged();
    emit meshDataChanged();
    return affected;
}

int EditModeController::dissolveSelection()
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity) return 0;

    // Cancel any active interactive preview before mutating topology —
    // see deleteSelection() for the rationale. (CodeRabbit Major)
    if (m_bevelSession.active) cancelBevel();
    if (m_knifeSession.active) cancelKnife();

    QString opLabel;
    std::function<int(HalfEdgeMesh&)> mutate;

    if (m_selectionMode == VertexMode) {
        if (m_selectedVertices.empty()) return 0;
        std::vector<int> verts(m_selectedVertices.begin(), m_selectedVertices.end());
        opLabel = "Dissolve Vertices";
        mutate = [verts](HalfEdgeMesh& hm) { return hm.dissolveVertices(verts); };
    } else if (m_selectionMode == EdgeMode) {
        if (m_selectedEdges.empty()) return 0;
        const std::vector<std::pair<int,int>> selectedEdges(
            m_selectedEdges.begin(), m_selectedEdges.end());
        opLabel = "Dissolve Edges";
        mutate = [selectedEdges](HalfEdgeMesh& hm) {
            std::vector<int> edgeIdxs;
            edgeIdxs.reserve(selectedEdges.size());
            for (const auto& [a, b] : selectedEdges) {
                const int target = std::min(a, b), other = std::max(a, b);
                for (size_t e = 0; e < hm.edgeCount(); ++e) {
                    auto [ev1, ev2] = hm.edgeVertices(static_cast<int>(e));
                    if (std::min(ev1, ev2) == target
                        && std::max(ev1, ev2) == other) {
                        edgeIdxs.push_back(static_cast<int>(e));
                        break;
                    }
                }
            }
            return edgeIdxs.empty() ? 0 : hm.dissolveEdges(edgeIdxs);
        };
    } else { // FaceMode
        if (m_selectedFaces.empty()) return 0;
        // On a pure triangle mesh, face dissolve and face delete are
        // identical — there are no coplanar neighbors to merge into a
        // single n-gon, so the hole boundary is the same in both cases.
        // Wire face dissolve to deleteFaces so the menu entry stays
        // active and predictable; an n-gon-aware variant can replace
        // this once the rest of the pipeline supports n-gons.
        const std::vector<int> faces = selectedFacesAsHEFaceIndices();
        opLabel = "Dissolve Faces";
        mutate = [faces](HalfEdgeMesh& hm) { return hm.deleteFaces(faces); };
    }

    const int affected = applyTopologyMutationNoSurvivor(
        this, m_editableMesh.get(), m_editEntity,
        m_selectedVertices, m_selectedEdges, m_selectedFaces,
        m_normalsMode, opLabel, mutate);

    if (affected == 0) return 0;

    updateSelectionOverlay();
    refreshNormalVisualizer();
    emit editSelectionChanged();
    emit meshDataChanged();
    return affected;
}

int EditModeController::subdivideSelection()
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity) return 0;

    // Cancel any active interactive preview before mutating topology —
    // see deleteSelection() for the rationale.
    if (m_bevelSession.active) cancelBevel();
    if (m_knifeSession.active) cancelKnife();

    // Resolve the target face set from the current selection. Face mode
    // uses the selected triangles directly. Edge mode subdivides every
    // triangle incident to a selected edge (Blender convention). Vertex
    // mode is a no-op — vertex selection alone doesn't define faces.
    std::vector<int> targetFaces;
    if (m_selectionMode == FaceMode) {
        if (m_selectedFaces.empty()) return 0;
        // Triangle indices → unique HE face indices (chunk 4b).
        targetFaces = selectedFacesAsHEFaceIndices();
    } else if (m_selectionMode == EdgeMode) {
        if (m_selectedEdges.empty()) return 0;
        // Convert global vertex pairs → HE edge indices → incident faces.
        // Build a probe HE structure to resolve. The actual mutation runs
        // on a fresh HE inside the lambda below; we only need the face
        // set here, and triangle indices map 1:1 between probe and live
        // since both are built from the same EditableMesh.
        HalfEdgeMesh probe;
        if (!probe.buildFromEditableMesh(*m_editableMesh)) return 0;
        std::set<int> faceSet;
        for (const auto& [a, b] : m_selectedEdges) {
            const int target = std::min(a, b), other = std::max(a, b);
            for (size_t e = 0; e < probe.edgeCount(); ++e) {
                auto [ev1, ev2] = probe.edgeVertices(static_cast<int>(e));
                if (std::min(ev1, ev2) == target && std::max(ev1, ev2) == other) {
                    auto [fA, fB] = probe.edgeFaces(static_cast<int>(e));
                    if (fA >= 0) faceSet.insert(fA);
                    if (fB >= 0) faceSet.insert(fB);
                    break;
                }
            }
        }
        targetFaces.assign(faceSet.begin(), faceSet.end());
    } else {
        return 0; // VertexMode: nothing sensible to subdivide
    }
    if (targetFaces.empty()) return 0;

    // Snapshot original mesh state for the undo command.
    HalfEdgeMesh hm;
    if (!hm.buildFromEditableMesh(*m_editableMesh)) return 0;

    auto originalSubMeshes = m_editableMesh->subMeshes();
    const auto preSelectedVerts = m_selectedVertices;
    const auto preSelectedEdges = m_selectedEdges;
    const auto preSelectedFaces = m_selectedFaces;

    // Split target faces by arity (chunk 4b): triangles take the
    // 1-to-4 split via `subdivideFaces`, n-gons take the 1-to-N quad
    // split via `subdivideFacesToQuads`. Without this dispatch, the
    // existing `subdivideFaces` would silently skip non-triangles
    // (its triangle-only MVP), making clicks on quads no-op.
    std::vector<int> triFaces, ngonFaces;
    for (int f : targetFaces) {
        if (f < 0 || f >= static_cast<int>(hm.faceCount())) continue;
        const auto verts = hm.faceVertices(f);
        if (verts.size() == 3) triFaces.push_back(f);
        else if (verts.size() > 3) ngonFaces.push_back(f);
    }

    // Capture target positions of the new midpoints BEFORE the mesh is
    // re-packed by toEditableMesh — the indices change after the round
    // trip, so we re-find the survivors by position (matches the merge
    // pipeline pattern). HE-vertex positions are stable across the call.
    std::vector<int> newVertHE;
    if (!triFaces.empty()) {
        auto v = hm.subdivideFaces(triFaces);
        newVertHE.insert(newVertHE.end(), v.begin(), v.end());
    }
    if (!ngonFaces.empty()) {
        // subdivideFacesToQuads must run AFTER subdivideFaces because
        // both rebuild the edge tables; running tris first means n-gon
        // edge lookups happen against the post-tri-split topology.
        // Fortunately tri-split doesn't touch n-gon faces.
        auto v = hm.subdivideFacesToQuads(ngonFaces);
        newVertHE.insert(newVertHE.end(), v.begin(), v.end());
    }
    if (newVertHE.empty()) return 0;

    EditableMesh updated;
    if (!hm.toEditableMesh(updated)) return 0;
    m_editableMesh->subMeshes() = std::move(updated.subMeshes());

    if (m_normalsMode == 0) m_editableMesh->recalculateNormals();
    else                     m_editableMesh->recalculateNormalsFlat();

    m_editableMesh->resizeEntityBuffers(m_editEntity);
    rewriteEntityAfterTopologyChange(m_editEntity);

    // Clear the entire selection. The pre-op selectFace dilation
    // populated vertex / edge sets that are now stale (vertex indices
    // shifted on re-pack, and the inserted midpoints don't have
    // stable analogues in the pre-op selection set). Re-finding new
    // midpoints by position used to leave a partial vertex selection
    // around — confusing for the user and inconsistent with delete /
    // dissolve / Catmull-Clark which all clear after the op. Match
    // that pattern here. (Chunk 4b polish.)
    m_selectedVertices.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    auto* cmd = new EditMeshTopologyCommand(
        std::move(originalSubMeshes),
        m_editableMesh->subMeshes(),
        preSelectedVerts, preSelectedEdges, preSelectedFaces,
        m_selectedVertices, m_selectedEdges, m_selectedFaces,
        QStringLiteral("Subdivide"));
    UndoManager::getSingleton()->push(cmd);

    validateMesh();
    SentryReporter::addBreadcrumb("edit_mode",
        QString("Subdivide (faces=%1, midpoints=%2)")
            .arg(targetFaces.size()).arg(newVertHE.size()));

    updateSelectionOverlay();
    refreshNormalVisualizer();
    emit editSelectionChanged();
    emit meshDataChanged();
    return static_cast<int>(targetFaces.size());
}

int EditModeController::loopCutSelection()
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity) return 0;
    if (m_selectionMode != EdgeMode) return 0;
    if (m_selectedEdges.empty()) return 0;

    // Cancel any active interactive preview before mutating topology.
    if (m_bevelSession.active) cancelBevel();
    if (m_knifeSession.active) cancelKnife();

    // Loop cut takes a SINGLE start edge — the first one in the
    // selection if multiple are selected. Convert (min, max) global
    // vertex pair to an HE edge index against the live mesh.
    const auto firstEdge = *m_selectedEdges.begin();
    const int targetMinV = firstEdge.first;
    const int targetMaxV = firstEdge.second;

    HalfEdgeMesh hm;
    if (!hm.buildFromEditableMesh(*m_editableMesh)) return 0;
    int startEdge = -1;
    for (size_t e = 0; e < hm.edgeCount(); ++e) {
        const auto [a, b] = hm.edgeVertices(static_cast<int>(e));
        if (std::min(a, b) == targetMinV && std::max(a, b) == targetMaxV) {
            startEdge = static_cast<int>(e);
            break;
        }
    }
    if (startEdge < 0) return 0;

    auto originalSubMeshes = m_editableMesh->subMeshes();
    const auto preSelectedVerts = m_selectedVertices;
    const auto preSelectedEdges = m_selectedEdges;
    const auto preSelectedFaces = m_selectedFaces;

    const auto newHE = hm.loopCut(startEdge);
    if (newHE.empty()) return 0;

    EditableMesh updated;
    if (!hm.toEditableMesh(updated)) return 0;
    m_editableMesh->subMeshes() = std::move(updated.subMeshes());

    if (m_normalsMode == 0) m_editableMesh->recalculateNormals();
    else                     m_editableMesh->recalculateNormalsFlat();

    m_editableMesh->resizeEntityBuffers(m_editEntity);
    rewriteEntityAfterTopologyChange(m_editEntity);

    // Clear selection — pre-op edge IDs are stale after the topology
    // mutation. The user can hit-test the new loop directly if they
    // want to chain operations.
    m_selectedVertices.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    auto* cmd = new EditMeshTopologyCommand(
        std::move(originalSubMeshes),
        m_editableMesh->subMeshes(),
        preSelectedVerts, preSelectedEdges, preSelectedFaces,
        m_selectedVertices, m_selectedEdges, m_selectedFaces,
        QStringLiteral("Loop Cut"));
    UndoManager::getSingleton()->push(cmd);

    validateMesh();
    SentryReporter::addBreadcrumb("edit_mode",
        QString("Loop Cut (midpoints=%1)").arg(newHE.size()));

    updateSelectionOverlay();
    refreshNormalVisualizer();
    emit editSelectionChanged();
    emit meshDataChanged();
    return static_cast<int>(newHE.size());
}

int EditModeController::subdivideCatmullClarkAll()
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity) return 0;

    // Cancel interactive previews — same rationale as deleteSelection
    // (a stale bevel/knife snapshot would replay against post-CC topology).
    if (m_bevelSession.active) cancelBevel();
    if (m_knifeSession.active) cancelKnife();

    HalfEdgeMesh hm;
    if (!hm.buildFromEditableMesh(*m_editableMesh)) return 0;

    auto originalSubMeshes = m_editableMesh->subMeshes();
    const auto preSelectedVerts = m_selectedVertices;
    const auto preSelectedEdges = m_selectedEdges;
    const auto preSelectedFaces = m_selectedFaces;

    const auto newVerts = hm.subdivideCatmullClark();
    if (newVerts.empty()) return 0;

    EditableMesh updated;
    if (!hm.toEditableMesh(updated)) return 0;
    m_editableMesh->subMeshes() = std::move(updated.subMeshes());

    if (m_normalsMode == 0) m_editableMesh->recalculateNormals();
    else                     m_editableMesh->recalculateNormalsFlat();

    m_editableMesh->resizeEntityBuffers(m_editEntity);
    rewriteEntityAfterTopologyChange(m_editEntity);

    // Selection clears: post-CC topology has no stable mapping back to
    // the pre-op selection (face/edge points didn't exist, vertex
    // indices may have shifted on re-pack). Fresh start is the safe
    // default; partial-CC with selection preservation is a follow-up.
    m_selectedVertices.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    auto* cmd = new EditMeshTopologyCommand(
        std::move(originalSubMeshes),
        m_editableMesh->subMeshes(),
        preSelectedVerts, preSelectedEdges, preSelectedFaces,
        m_selectedVertices, m_selectedEdges, m_selectedFaces,
        QStringLiteral("Catmull-Clark Subdivide"));
    UndoManager::getSingleton()->push(cmd);

    validateMesh();
    SentryReporter::addBreadcrumb("edit_mode",
        QString("Catmull-Clark Subdivide (newVerts=%1)").arg(newVerts.size()));

    updateSelectionOverlay();
    refreshNormalVisualizer();
    emit editSelectionChanged();
    emit meshDataChanged();
    return static_cast<int>(newVerts.size());
}

namespace {
// Build the closed BOUNDARY-EDGE loop implied by a set of selected edges.
// Returns vertex indices in winding order, or an empty vector if any
// selected edge is interior (touches two faces) or the selection doesn't
// form a single closed cycle.
//
// Boundary check rationale (Codex P1): a closed cycle of interior edges
// is the perimeter of an already-triangulated region; filling it would
// stack overlapping triangles on top of existing geometry. Hole-fill is
// the intended use case — every selected edge must therefore have
// exactly one incident face in the live mesh.
//
// "Closed" means: every endpoint is shared by exactly two selected
// edges, the loop traversal returns to the starting vertex, and the
// loop visits every input edge.
std::vector<int> buildClosedEdgeLoop(
    const std::set<std::pair<int,int>>& selectedEdges,
    const HalfEdgeMesh& hm)
{
    if (selectedEdges.size() < 3) return {};

    // 1. Every selected edge must be a boundary edge (one incident face)
    //    in the live mesh. Resolve each (vA, vB) pair against hm and
    //    refuse if any interior edge slips in.
    for (const auto& [a, b] : selectedEdges) {
        const int target = std::min(a, b), other = std::max(a, b);
        bool resolved = false;
        for (size_t e = 0; e < hm.edgeCount(); ++e) {
            auto [ev1, ev2] = hm.edgeVertices(static_cast<int>(e));
            if (std::min(ev1, ev2) != target
                || std::max(ev1, ev2) != other) continue;
            resolved = true;
            if (!hm.isEdgeBoundary(static_cast<int>(e))) return {};
            break;
        }
        if (!resolved) return {}; // edge no longer present in mesh
    }

    // 2. adjacency[v] = vertices connected to v via selected edges
    std::map<int, std::vector<int>> adj;
    for (const auto& [a, b] : selectedEdges) {
        adj[a].push_back(b);
        adj[b].push_back(a);
    }
    // Every vertex on a closed loop has degree 2.
    for (const auto& [v, nbrs] : adj) {
        if (nbrs.size() != 2) return {};
    }

    // 3. Walk the loop starting at the lowest-index vertex.
    const int start = adj.begin()->first;
    std::vector<int> loop;
    loop.push_back(start);
    int prev = -1;
    int cur = start;
    while (true) {
        const auto& nbrs = adj.at(cur);
        const int next = (nbrs[0] == prev) ? nbrs[1] : nbrs[0];
        if (next == start) {
            // Closed. Ensure we visited every selected edge by checking
            // loop length matches the edge count.
            if (loop.size() != selectedEdges.size()) return {};
            return loop;
        }
        // Detect non-loop selections (a chain that revisits a vertex
        // without closing). The degree-2 invariant above mostly catches
        // these, but bail defensively.
        if (loop.size() > selectedEdges.size()) return {};
        loop.push_back(next);
        prev = cur;
        cur = next;
    }
}
} // namespace

int EditModeController::fillSelection()
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity) return 0;

    if (m_bevelSession.active) cancelBevel();
    if (m_knifeSession.active) cancelKnife();

    HalfEdgeMesh hm;
    if (!hm.buildFromEditableMesh(*m_editableMesh)) return 0;

    std::vector<int> targetVerts;
    if (m_selectionMode == VertexMode) {
        if (m_selectedVertices.size() < 3) return 0;
        // Use the user-supplied selection order. std::set iterates in
        // ascending order — the resulting fan winding may not match the
        // user's intent for non-convex inputs, but for the MVP we accept
        // the std::set order. (The test suite documents this.)
        targetVerts.assign(m_selectedVertices.begin(), m_selectedVertices.end());
    } else if (m_selectionMode == EdgeMode) {
        targetVerts = buildClosedEdgeLoop(m_selectedEdges, hm);
        if (targetVerts.size() < 3) return 0;
    } else {
        return 0; // FaceMode: nothing to fill
    }

    auto originalSubMeshes = m_editableMesh->subMeshes();
    const auto preSelectedVerts = m_selectedVertices;
    const auto preSelectedEdges = m_selectedEdges;
    const auto preSelectedFaces = m_selectedFaces;

    const int created = hm.fillSelection(targetVerts);
    if (created == 0) return 0;

    EditableMesh updated;
    if (!hm.toEditableMesh(updated)) return 0;
    m_editableMesh->subMeshes() = std::move(updated.subMeshes());

    if (m_normalsMode == 0) m_editableMesh->recalculateNormals();
    else                     m_editableMesh->recalculateNormalsFlat();

    m_editableMesh->resizeEntityBuffers(m_editEntity);
    rewriteEntityAfterTopologyChange(m_editEntity);

    // Selection changes: clear edge selection (the loop edges are now
    // interior or don't exist post-pack); leave vertex selection alone
    // for vertex-mode fill so the user can chain operations. Faces stay
    // empty.
    m_selectedEdges.clear();

    auto* cmd = new EditMeshTopologyCommand(
        std::move(originalSubMeshes),
        m_editableMesh->subMeshes(),
        preSelectedVerts, preSelectedEdges, preSelectedFaces,
        m_selectedVertices, m_selectedEdges, m_selectedFaces,
        QStringLiteral("Fill"));
    UndoManager::getSingleton()->push(cmd);

    validateMesh();
    SentryReporter::addBreadcrumb("edit_mode",
        QString("Fill (verts=%1, faces=%2)")
            .arg(targetVerts.size()).arg(created));

    updateSelectionOverlay();
    refreshNormalVisualizer();
    emit editSelectionChanged();
    emit meshDataChanged();
    return created;
}

void EditModeController::cancelKnife()
{
    if (!m_knifeSession.active) return;
    SentryReporter::addBreadcrumb("edit_mode", "Knife: cancel");
    m_knifeSession = {};
    destroyKnifePreviewOverlay();
    emit knifeSessionChanged();
}

bool EditModeController::knifeHitTest(const QPoint& screenPos, OgreWidget* widget,
                                      KnifePoint& out) const
{
    if (!m_editableMesh || !m_editEntity || !widget) return false;
    auto* spaceCam = widget->getSpaceCamera();
    auto* camera = spaceCam ? spaceCam->getCamera() : nullptr;
    if (!camera) return false;

    const int vw = widget->width();
    const int vh = widget->height();

    // Build the front-facing vertex / edge sets once (mirrors the
    // chunk-4b behaviour of regular hitTestEdge / face selection): on
    // a dense mesh, snapping to back-face geometry that's hidden behind
    // the visible surface is impossible to control. Front-facing means
    // the polygon's Newell normal points toward the camera. A vertex is
    // front-facing if at least one incident polygon faces the camera;
    // an edge is front-facing if at least one of its two adjacent
    // polygons does. Edge keys use (min,max) global vertex indices so
    // they line up with the triangle-mode HE used below.
    Ogre::SceneNode* node = m_editEntity->getParentSceneNode();
    const Ogre::Vector3 camPos = camera->getDerivedPosition();
    auto isPolygonFrontFacing = [&](const EditableSubMesh& sub,
                                    const std::vector<unsigned int>& corners) -> bool {
        if (corners.size() < 3 || !node) return false;
        Ogre::Vector3 nrm = Ogre::Vector3::ZERO;
        for (size_t i = 0; i < corners.size(); ++i) {
            if (corners[i] >= sub.vertices.size()) return false;
            const auto& a = sub.vertices[corners[i]].position;
            const auto& b = sub.vertices[corners[(i + 1) % corners.size()]].position;
            nrm.x += (a.y - b.y) * (a.z + b.z);
            nrm.y += (a.z - b.z) * (a.x + b.x);
            nrm.z += (a.x - b.x) * (a.y + b.y);
        }
        const Ogre::Vector3 worldNrm = node->_getDerivedOrientation() * nrm;
        const Ogre::Vector3 anyCornerWorld =
            node->convertLocalToWorldPosition(sub.vertices[corners[0]].position);
        return worldNrm.dotProduct(camPos - anyCornerWorld) > 0.0f;
    };

    std::set<int> frontVerts;
    std::set<std::pair<int,int>> frontEdges;
    for (size_t si = 0; si < m_editableMesh->subMeshes().size(); ++si) {
        const auto& sub = m_editableMesh->subMeshes()[si];
        const int vertOffset = localToGlobal(static_cast<unsigned int>(si), 0);
        auto recordFrontPoly = [&](const std::vector<unsigned int>& corners) {
            for (size_t i = 0; i < corners.size(); ++i) {
                const int g0 = vertOffset + static_cast<int>(corners[i]);
                const int g1 = vertOffset + static_cast<int>(corners[(i + 1) % corners.size()]);
                frontVerts.insert(g0);
                frontEdges.emplace(std::min(g0, g1), std::max(g0, g1));
            }
        };
        if (!sub.faces.empty()) {
            for (const auto& face : sub.faces) {
                if (face.indices.size() < 3) continue;
                if (!isPolygonFrontFacing(sub, face.indices)) continue;
                recordFrontPoly(face.indices);
            }
        } else {
            for (const auto& tri : sub.triangles) {
                std::vector<unsigned int> corners = {
                    tri.indices[0], tri.indices[1], tri.indices[2] };
                if (!isPolygonFrontFacing(sub, corners)) continue;
                recordFrontPoly(corners);
            }
        }
    }

    // Priority 1: vertex snap. Uses the same radius as regular edit-mode
    // vertex picking so the user's eye can predict the snap. Skip
    // back-face vertices (not in `frontVerts`) so dense meshes don't
    // pull clicks to vertices the user can't see.
    const int snapVert = hitTestVertex(screenPos, camera, vw, vh, 10.0f);
    if (snapVert >= 0 && frontVerts.count(snapVert)) {
        auto [subIdx, localIdx] = globalToLocal(snapVert);
        if (subIdx < m_editableMesh->subMeshes().size()
         && localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size()) {
            out = {};
            out.kind = KnifePoint::OnVertex;
            out.vertexIndex = snapVert;
            out.localPosition = m_editableMesh->subMeshes()[subIdx].vertices[localIdx].position;
            return true;
        }
    }

    // Priority 2: edge snap. Knife needs a depth-aware variant of the
    // regular edge hit-test: when front and back edges of a cube overlap
    // in screen space, the generic hitTestEdge can pick either (iteration
    // order decides). That lands the cut on an edge facing away from the
    // camera, which the user experiences as "the vertex drops somewhere
    // random on the edge I couldn't even see." Here we walk all edges,
    // keep only those within the screen-space pixel radius, then pick the
    // one whose closest-point sits nearest the camera.
    {
        const Ogre::Real nx = static_cast<Ogre::Real>(screenPos.x()) / vw;
        const Ogre::Real ny = static_cast<Ogre::Real>(screenPos.y()) / vh;
        const Ogre::Ray ray = camera->getCameraToViewportRay(nx, ny);

        Ogre::Vector3 rO = ray.getOrigin();
        Ogre::Vector3 rD = ray.getDirection();
        if (node) {
            const Ogre::Affine3 worldToLocal = node->_getFullTransform().inverse();
            rO = worldToLocal * rO;
            rD = worldToLocal.linear() * rD;
        }

        // Build the HE directly from the (possibly n-gon) editable mesh.
        // splitEdge is now n-gon-aware so commitKnife uses the same
        // build, and edge indices line up between hit-test and commit.
        HalfEdgeMesh tmp;
        if (tmp.buildFromEditableMesh(*m_editableMesh)) {
            constexpr float kPixelRadius = 10.0f;
            float bestDepth = std::numeric_limits<float>::infinity();
            int bestEdgeIdx = -1;
            float bestT = 0.5f;
            Ogre::Vector3 bestLocal = Ogre::Vector3::ZERO;

            for (size_t e = 0; e < tmp.edgeCount(); ++e) {
                auto [gv0, gv1] = tmp.edgeVertices(static_cast<int>(e));
                if (gv0 < 0 || gv1 < 0) continue;
                // Skip back-face edges. `frontEdges` was computed from
                // the n-gon `m_editableMesh`, keyed by global vertex
                // pair; the triangle-mode `tmp` indices use the same
                // global numbering, so the lookup is direct. Note that
                // `frontEdges` only contains polygon-perimeter edges:
                // a fan-triangulation diagonal between two front-facing
                // verts is rejected here, which is correct — the user
                // shouldn't be able to click on a fake interior edge.
                {
                    const std::pair<int,int> key{std::min(gv0, gv1), std::max(gv0, gv1)};
                    if (!frontEdges.count(key)) continue;
                }
                auto [sub0, loc0] = globalToLocal(gv0);
                auto [sub1, loc1] = globalToLocal(gv1);
                if (sub0 >= m_editableMesh->subMeshes().size()) continue;
                if (sub1 >= m_editableMesh->subMeshes().size()) continue;
                const auto& sA = m_editableMesh->subMeshes()[sub0];
                const auto& sB = m_editableMesh->subMeshes()[sub1];
                if (loc0 >= sA.vertices.size() || loc1 >= sB.vertices.size()) continue;

                const auto& p0 = sA.vertices[loc0].position;
                const auto& p1 = sB.vertices[loc1].position;

                // Screen-space pixel distance: reject edges outside the
                // snap radius outright.
                if (!node) continue;
                const Ogre::Vector3 wp0 = node->convertLocalToWorldPosition(p0);
                const Ogre::Vector3 wp1 = node->convertLocalToWorldPosition(p1);
                const QPoint sp0 = worldToScreen(wp0, camera, vw, vh);
                const QPoint sp1 = worldToScreen(wp1, camera, vw, vh);
                if (pointToSegmentDistance(screenPos, sp0, sp1) > kPixelRadius)
                    continue;

                // Closest point on the edge segment to the click ray, in
                // local mesh space. Solves the 2x2 system for the edge
                // parameter s and ray parameter u that minimize |P(s)-R(u)|².
                const Ogre::Vector3 d = p1 - p0;
                const float a = d.dotProduct(d);
                const float b = d.dotProduct(rD);
                const float c = rD.dotProduct(rD);
                const float det = a * c - b * b;
                float t = 0.5f;
                if (a > 1e-10f && det > 1e-10f) {
                    const Ogre::Vector3 w0 = p0 - rO;
                    const float dW = d.dotProduct(w0);
                    const float rW = rD.dotProduct(w0);
                    t = std::clamp((b * rW - c * dW) / det, 0.0f, 1.0f);
                }
                const Ogre::Vector3 local = p0 + d * t;
                const Ogre::Vector3 world = node->convertLocalToWorldPosition(local);
                const float depth = (world - camPos).dotProduct(camera->getDerivedDirection());
                if (depth <= 0.0f) continue; // behind camera
                if (depth < bestDepth) {
                    bestDepth = depth;
                    bestEdgeIdx = static_cast<int>(e);
                    bestT = t;
                    bestLocal = local;
                }
            }

            if (bestEdgeIdx >= 0) {
                out = {};
                out.kind = KnifePoint::OnEdge;
                out.edgeIndex = bestEdgeIdx;
                out.edgeT = bestT;
                out.localPosition = bestLocal;
                return true;
            }
        }
    }

    // Priority 3: face ray-cast. Used for preview only in the MVP —
    // commit pipeline doesn't yet handle on-face points.
    const int triHit = hitTestFace(screenPos, camera, vw, vh);
    if (triHit >= 0) {
        out = {};
        out.kind = KnifePoint::OnFace;
        out.triangleIndex = triHit;
        // Re-intersect the ray to get the local hit position for preview.
        const Ogre::Real nx = static_cast<Ogre::Real>(screenPos.x()) / vw;
        const Ogre::Real ny = static_cast<Ogre::Real>(screenPos.y()) / vh;
        const Ogre::Ray ray = camera->getCameraToViewportRay(nx, ny);
        Ogre::SceneNode* node = m_editEntity->getParentSceneNode();
        Ogre::Affine3 worldToLocal = node ? node->_getFullTransform().inverse() : Ogre::Affine3::IDENTITY;
        Ogre::Vector3 localOrigin = worldToLocal * ray.getOrigin();
        Ogre::Vector3 localDir = worldToLocal.linear() * ray.getDirection();
        localDir.normalise();

        // Walk the mesh to find the tri and intersect.
        int globalTriOffset = 0;
        for (const auto& sub : m_editableMesh->subMeshes()) {
            for (size_t ti = 0; ti < sub.triangles.size(); ++ti) {
                if (globalTriOffset + static_cast<int>(ti) != triHit) continue;
                const auto& tri = sub.triangles[ti];
                const auto& v0 = sub.vertices[tri.indices[0]].position;
                const auto& v1 = sub.vertices[tri.indices[1]].position;
                const auto& v2 = sub.vertices[tri.indices[2]].position;
                const float t = rayTriangleIntersect(localOrigin, localDir, v0, v1, v2);
                if (t >= 0.0f) out.localPosition = localOrigin + localDir * t;
            }
            globalTriOffset += static_cast<int>(sub.triangles.size());
        }
        return true;
    }

    return false;
}

void EditModeController::updateKnifePreviewOverlay()
{
    if (!m_editModeActive || !m_editEntity) return;
    auto* sceneMgr = m_editEntity->_getManager();
    if (!sceneMgr) return;

    // Knife preview owns its own scene node so updating its entity-
    // mirror transform doesn't stomp on the selection overlay node's
    // origin-parked / local-space geometry. Selection overlays live on
    // m_overlayNode; knife on m_overlayKnifeNode.
    if (!m_overlayKnifeNode) {
        m_overlayKnifeNode = sceneMgr->getRootSceneNode()->createChildSceneNode();
    }
    if (!m_overlayKnife) {
        m_overlayKnife = sceneMgr->createManualObject("EditMode_KnifeOverlay");
        m_overlayKnife->setDynamic(true);
        m_overlayKnife->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY);
        m_overlayKnifeNode->attachObject(m_overlayKnife);
    }

    // Mirror the entity's world transform so knife points drawn in local
    // space sit on the mesh.
    if (auto* entNode = m_editEntity->getParentSceneNode()) {
        m_overlayKnifeNode->setPosition(entNode->_getDerivedPosition());
        m_overlayKnifeNode->setOrientation(entNode->_getDerivedOrientation());
        m_overlayKnifeNode->setScale(entNode->_getDerivedScale());
    }

    m_overlayKnife->clear();
    if (m_knifeSession.points.empty() && !m_knifeSession.hoverValid) return;

    const Ogre::ColourValue solid(1.0f, 0.85f, 0.2f, 1.0f);  // yellow for confirmed
    const Ogre::ColourValue ghost(1.0f, 0.85f, 0.2f, 0.45f); // dimmer for hover

    m_overlayKnife->begin("EditMode/EdgeSelection", Ogre::RenderOperation::OT_LINE_LIST);

    // Confirmed polyline.
    for (size_t i = 1; i < m_knifeSession.points.size(); ++i) {
        m_overlayKnife->position(m_knifeSession.points[i - 1].localPosition);
        m_overlayKnife->colour(solid);
        m_overlayKnife->position(m_knifeSession.points[i].localPosition);
        m_overlayKnife->colour(solid);
    }
    // Hover segment from last confirmed to cursor.
    if (!m_knifeSession.points.empty() && m_knifeSession.hoverValid) {
        m_overlayKnife->position(m_knifeSession.points.back().localPosition);
        m_overlayKnife->colour(ghost);
        m_overlayKnife->position(m_knifeSession.hover.localPosition);
        m_overlayKnife->colour(ghost);
    }

    m_overlayKnife->end();
}

void EditModeController::destroyKnifePreviewOverlay()
{
    auto* sceneMgr = m_editEntity ? m_editEntity->_getManager() : nullptr;
    if (m_overlayKnife) {
        if (m_overlayKnifeNode) m_overlayKnifeNode->detachObject(m_overlayKnife);
        if (sceneMgr) sceneMgr->destroyManualObject(m_overlayKnife);
        m_overlayKnife = nullptr;
    }
    if (m_overlayKnifeNode) {
        if (sceneMgr) sceneMgr->destroySceneNode(m_overlayKnifeNode);
        m_overlayKnifeNode = nullptr;
    }
}

// ===========================================================================
// Vertex transform support
// ===========================================================================

Ogre::Vector3 EditModeController::getSelectedVerticesCentroid() const
{
    if (!m_editableMesh || m_selectedVertices.empty())
        return Ogre::Vector3::ZERO;

    Ogre::Vector3 centroid = Ogre::Vector3::ZERO;
    int count = 0;

    for (int gi : m_selectedVertices) {
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx < m_editableMesh->subMeshes().size() &&
            localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
        {
            centroid += m_editableMesh->subMeshes()[subIdx].vertices[localIdx].position;
            ++count;
        }
    }

    if (count > 0)
        centroid /= static_cast<Ogre::Real>(count);

    return centroid;
}

void EditModeController::translateSelectedVertices(const Ogre::Vector3& delta)
{
    if (!m_editableMesh || m_selectedVertices.empty())
        return;

    auto weights = getSoftSelectionWeights();

    for (const auto& [gi, weight] : weights) {
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx < m_editableMesh->subMeshes().size() &&
            localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
        {
            Ogre::Vector3 pos = m_editableMesh->getVertexPosition(subIdx, localIdx);
            pos += delta * weight;
            m_editableMesh->setVertexPosition(subIdx, localIdx, pos);
        }
    }

    // Recalculate normals and commit to GPU
    if (m_normalsMode == 0)
        m_editableMesh->recalculateNormals();
    else
        m_editableMesh->recalculateNormalsFlat();

    m_editableMesh->commitToEntity(m_editEntity);
    refreshNormalVisualizer();
    updateSelectionOverlay();
    emit meshDataChanged();
}

void EditModeController::rotateSelectedVertices(const Ogre::Quaternion& rotation)
{
    if (!m_editableMesh || m_selectedVertices.empty())
        return;

    Ogre::Vector3 pivot = getSelectedVerticesCentroid();
    auto weights = getSoftSelectionWeights();

    for (const auto& [gi, weight] : weights) {
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx < m_editableMesh->subMeshes().size() &&
            localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
        {
            Ogre::Vector3 pos = m_editableMesh->getVertexPosition(subIdx, localIdx);
            Ogre::Vector3 offset = pos - pivot;
            // Interpolate rotation by weight
            Ogre::Quaternion weightedRot = Ogre::Quaternion::Slerp(weight, Ogre::Quaternion::IDENTITY, rotation);
            Ogre::Vector3 rotatedOffset = weightedRot * offset;
            m_editableMesh->setVertexPosition(subIdx, localIdx, pivot + rotatedOffset);
        }
    }

    if (m_normalsMode == 0)
        m_editableMesh->recalculateNormals();
    else
        m_editableMesh->recalculateNormalsFlat();

    m_editableMesh->commitToEntity(m_editEntity);
    refreshNormalVisualizer();
    updateSelectionOverlay();
    emit meshDataChanged();
}

void EditModeController::scaleFromSnapshot(
    const std::map<int, Ogre::Vector3>& snapshot,
    const Ogre::Vector3& pivot,
    const Ogre::Vector3& scaleFactor)
{
    if (!m_editableMesh || snapshot.empty())
        return;

    // Compute soft-selection weights from the PRESS-TIME snapshot positions,
    // not the live (already-scaled) mesh. Otherwise a vertex near the radius
    // boundary can drift out of the soft zone mid-drag, flipping its weight
    // from its press-time value back to the default 1.0 and causing a visible
    // jump. This is only a concern for baseline-based operations like scale;
    // the incremental translate/rotate paths re-read the live mesh each call
    // and their deltas are tiny, so the drift is negligible there.
    auto weights = computeSoftSelectionWeightsFromPositions(snapshot);

    for (const auto& [gi, originalPos] : snapshot) {
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx >= m_editableMesh->subMeshes().size() ||
            localIdx >= m_editableMesh->subMeshes()[subIdx].vertices.size())
            continue;
        // Weight 0 (not in snapshot map from soft-selection) means the vertex
        // was captured but fell outside the radius, so it must not move.
        float weight = 0.0f;
        auto wit = weights.find(gi);
        if (wit != weights.end()) weight = wit->second;

        Ogre::Vector3 offset = originalPos - pivot;
        Ogre::Vector3 weightedScale = Ogre::Vector3::UNIT_SCALE +
            (scaleFactor - Ogre::Vector3::UNIT_SCALE) * weight;
        Ogre::Vector3 scaledOffset = offset * weightedScale;
        m_editableMesh->setVertexPosition(subIdx, localIdx, pivot + scaledOffset);
    }

    if (m_normalsMode == 0)
        m_editableMesh->recalculateNormals();
    else
        m_editableMesh->recalculateNormalsFlat();

    m_editableMesh->commitToEntity(m_editEntity);
    refreshNormalVisualizer();
    updateSelectionOverlay();
    emit meshDataChanged();
}

void EditModeController::scaleSelectedVertices(const Ogre::Vector3& scaleFactor)
{
    if (!m_editableMesh || m_selectedVertices.empty())
        return;

    Ogre::Vector3 pivot = getSelectedVerticesCentroid();
    auto weights = getSoftSelectionWeights();

    for (const auto& [gi, weight] : weights) {
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx < m_editableMesh->subMeshes().size() &&
            localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
        {
            Ogre::Vector3 pos = m_editableMesh->getVertexPosition(subIdx, localIdx);
            Ogre::Vector3 offset = pos - pivot;
            // Interpolate scale by weight
            Ogre::Vector3 weightedScale = Ogre::Vector3::UNIT_SCALE +
                (scaleFactor - Ogre::Vector3::UNIT_SCALE) * weight;
            Ogre::Vector3 scaledOffset = offset * weightedScale;
            m_editableMesh->setVertexPosition(subIdx, localIdx, pivot + scaledOffset);
        }
    }

    if (m_normalsMode == 0)
        m_editableMesh->recalculateNormals();
    else
        m_editableMesh->recalculateNormalsFlat();

    m_editableMesh->commitToEntity(m_editEntity);
    refreshNormalVisualizer();
    updateSelectionOverlay();
    emit meshDataChanged();
}

std::map<int, Ogre::Vector3> EditModeController::snapshotVertexPositions() const
{
    std::map<int, Ogre::Vector3> snapshot;
    if (!m_editableMesh)
        return snapshot;

    auto weights = getSoftSelectionWeights();
    for (const auto& [gi, weight] : weights) {
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx < m_editableMesh->subMeshes().size() &&
            localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
        {
            snapshot[gi] = m_editableMesh->getVertexPosition(subIdx, localIdx);
        }
    }
    return snapshot;
}

void EditModeController::restoreVertexPositions(const std::map<int, Ogre::Vector3>& snapshot)
{
    if (!m_editableMesh)
        return;

    for (const auto& [gi, pos] : snapshot) {
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx < m_editableMesh->subMeshes().size() &&
            localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
        {
            m_editableMesh->setVertexPosition(subIdx, localIdx, pos);
        }
    }

    if (m_editEntity) {
        m_editableMesh->commitToEntity(m_editEntity);
    refreshNormalVisualizer();
        updateSelectionOverlay();
        emit meshDataChanged();
    }
}

// ===========================================================================
// Selection overlay
// ===========================================================================

void EditModeController::createOverlayMaterials()
{
    auto& matMgr = Ogre::MaterialManager::getSingleton();

    // Vertex selection material (orange points, no lighting, no depth test)
    if (!matMgr.getByName("EditMode/VertexSelection"))
    {
        auto mat = matMgr.create("EditMode/VertexSelection",
                                 Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        auto* pass = mat->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
        pass->setDepthCheckEnabled(false);
        pass->setDepthWriteEnabled(false);
        pass->setPointSize(6.0f);
        pass->setPointSpritesEnabled(false);
    }

    // Edge selection material (colored lines, no lighting, no depth test)
    if (!matMgr.getByName("EditMode/EdgeSelection"))
    {
        auto mat = matMgr.create("EditMode/EdgeSelection",
                                 Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        auto* pass = mat->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
        pass->setDepthCheckEnabled(false);
        pass->setDepthWriteEnabled(false);
    }

    // Face selection material (semi-transparent overlay, no lighting)
    if (!matMgr.getByName("EditMode/FaceSelection"))
    {
        auto mat = matMgr.create("EditMode/FaceSelection",
                                 Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        auto* pass = mat->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
        pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        pass->setDepthCheckEnabled(true);
        pass->setDepthWriteEnabled(false);
        pass->setCullingMode(Ogre::CULL_NONE);
    }
}

void EditModeController::updateSelectionOverlay()
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity)
        return;

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    if (!sceneMgr)
        return;

    Ogre::SceneNode* parentNode = m_editEntity->getParentSceneNode();
    if (!parentNode)
        return;

    // Create the overlay scene node if needed (child of entity's node)
    if (!m_overlayNode)
    {
        m_overlayNode = parentNode->createChildSceneNode();
    }

    // -- Vertex overlay --
    if (!m_overlayVertices)
    {
        m_overlayVertices = sceneMgr->createManualObject("EditMode_VertexOverlay");
        m_overlayVertices->setDynamic(true);
        m_overlayVertices->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY);
        m_overlayNode->attachObject(m_overlayVertices);
    }

    m_overlayVertices->clear();
    if (!m_selectedVertices.empty())
    {
        m_overlayVertices->begin("EditMode/VertexSelection", Ogre::RenderOperation::OT_POINT_LIST);

        if (m_softSelectionEnabled) {
            // Heat map: render all influenced vertices with weight-based colors
            auto weights = getSoftSelectionWeights();
            for (const auto& [gi, weight] : weights) {
                auto [subIdx, localIdx] = globalToLocal(gi);
                if (subIdx < m_editableMesh->subMeshes().size() &&
                    localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
                {
                    const auto& pos = m_editableMesh->subMeshes()[subIdx].vertices[localIdx].position;
                    m_overlayVertices->position(pos);
                    m_overlayVertices->colour(weightToColor(weight));
                }
            }
        } else {
            // Fixed orange for selected vertices only
            Ogre::ColourValue vertColor(1.0f, 0.6f, 0.0f, 1.0f);
            for (int gi : m_selectedVertices) {
                auto [subIdx, localIdx] = globalToLocal(gi);
                if (subIdx < m_editableMesh->subMeshes().size() &&
                    localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
                {
                    const auto& pos = m_editableMesh->subMeshes()[subIdx].vertices[localIdx].position;
                    m_overlayVertices->position(pos);
                    m_overlayVertices->colour(vertColor);
                }
            }
        }

        m_overlayVertices->end();
    }

    // -- Edge overlay --
    if (!m_overlayEdges)
    {
        m_overlayEdges = sceneMgr->createManualObject("EditMode_EdgeOverlay");
        m_overlayEdges->setDynamic(true);
        m_overlayEdges->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY);
        m_overlayNode->attachObject(m_overlayEdges);
    }

    m_overlayEdges->clear();
    if (!m_selectedEdges.empty())
    {
        m_overlayEdges->begin("EditMode/EdgeSelection", Ogre::RenderOperation::OT_LINE_LIST);

        // Cyan color for selected edges
        Ogre::ColourValue edgeColor(0.0f, 1.0f, 1.0f, 1.0f);

        for (const auto& [g0, g1] : m_selectedEdges) {
            auto [sub0, loc0] = globalToLocal(g0);
            auto [sub1, loc1] = globalToLocal(g1);

            if (sub0 < m_editableMesh->subMeshes().size() &&
                loc0 < m_editableMesh->subMeshes()[sub0].vertices.size() &&
                sub1 < m_editableMesh->subMeshes().size() &&
                loc1 < m_editableMesh->subMeshes()[sub1].vertices.size())
            {
                const auto& p0 = m_editableMesh->subMeshes()[sub0].vertices[loc0].position;
                const auto& p1 = m_editableMesh->subMeshes()[sub1].vertices[loc1].position;
                m_overlayEdges->position(p0);
                m_overlayEdges->colour(edgeColor);
                m_overlayEdges->position(p1);
                m_overlayEdges->colour(edgeColor);
            }
        }

        m_overlayEdges->end();
    }

    // -- Face overlay --
    if (!m_overlayFaces)
    {
        m_overlayFaces = sceneMgr->createManualObject("EditMode_FaceOverlay");
        m_overlayFaces->setDynamic(true);
        m_overlayFaces->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY);
        m_overlayNode->attachObject(m_overlayFaces);
    }

    m_overlayFaces->clear();
    if (!m_selectedFaces.empty())
    {
        m_overlayFaces->begin("EditMode/FaceSelection", Ogre::RenderOperation::OT_TRIANGLE_LIST);

        // Semi-transparent blue tint for selected faces
        Ogre::ColourValue faceColor(0.2f, 0.4f, 1.0f, 0.35f);

        for (int gi : m_selectedFaces) {
            auto [subIdx, localTri] = globalTriToLocal(gi);
            if (subIdx < m_editableMesh->subMeshes().size() &&
                localTri < m_editableMesh->subMeshes()[subIdx].triangles.size())
            {
                const auto& tri = m_editableMesh->subMeshes()[subIdx].triangles[localTri];
                const auto& verts = m_editableMesh->subMeshes()[subIdx].vertices;

                if (tri.indices[0] < verts.size() &&
                    tri.indices[1] < verts.size() &&
                    tri.indices[2] < verts.size())
                {
                    // Offset slightly toward camera to avoid z-fighting
                    Ogre::Vector3 faceNormal =
                        (verts[tri.indices[1]].position - verts[tri.indices[0]].position)
                            .crossProduct(verts[tri.indices[2]].position - verts[tri.indices[0]].position);
                    faceNormal.normalise();
                    Ogre::Vector3 offset = faceNormal * 0.001f;

                    m_overlayFaces->position(verts[tri.indices[0]].position + offset);
                    m_overlayFaces->colour(faceColor);
                    m_overlayFaces->position(verts[tri.indices[1]].position + offset);
                    m_overlayFaces->colour(faceColor);
                    m_overlayFaces->position(verts[tri.indices[2]].position + offset);
                    m_overlayFaces->colour(faceColor);
                }
            }
        }

        m_overlayFaces->end();
    }

    // -- Soft selection radius sphere --
    if (m_softSelectionEnabled && !m_selectedVertices.empty()) {
        Ogre::Vector3 centroid = getSelectedVerticesCentroid();

        // Create sphere entity on first use
        if (!m_softSelSphere) {
            const std::string meshName = "__SoftSelSphere__";
            if (!Ogre::MeshManager::getSingleton().resourceExists(meshName)) {
                Procedural::SphereGenerator()
                    .setRadius(1.0f)
                    .setNumRings(16)
                    .setNumSegments(16)
                    .realizeMesh(meshName);
            }

            // Create a transparent glass-like material
            const std::string matName = "EditMode/SoftSelRadius";
            auto& matMgr = Ogre::MaterialManager::getSingleton();
            if (!matMgr.resourceExists(matName)) {
                auto mat = matMgr.create(matName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
                auto* pass = mat->getTechnique(0)->getPass(0);
                pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
                pass->setDepthWriteEnabled(false);
                pass->setDiffuse(Ogre::ColourValue(0.3f, 0.6f, 1.0f, 0.08f));
                pass->setAmbient(Ogre::ColourValue(0.2f, 0.4f, 0.8f, 0.08f));
                pass->setSpecular(Ogre::ColourValue(1.0f, 1.0f, 1.0f, 0.15f));
                pass->setShininess(60.0f);
                pass->setCullingMode(Ogre::CULL_NONE);
            }

            m_softSelSphereNode = m_overlayNode->createChildSceneNode();
            m_softSelSphere = sceneMgr->createEntity("EditMode_SoftSelSphere", meshName);
            m_softSelSphere->setMaterialName(matName);
            m_softSelSphere->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY - 1);
            m_softSelSphereNode->attachObject(m_softSelSphere);
        }

        m_softSelSphereNode->setPosition(centroid);
        float r = static_cast<float>(m_softSelectionRadius);
        m_softSelSphereNode->setScale(r, r, r);
        m_softSelSphereNode->setVisible(true);
    } else if (m_softSelSphereNode) {
        m_softSelSphereNode->setVisible(false);
    }
}

void EditModeController::destroySelectionOverlay()
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr)
        return;
    auto* sceneMgr = mgr->getSceneMgr();
    if (!sceneMgr)
        return;

    if (m_overlayNode)
    {
        m_overlayNode->detachAllObjects();

        if (m_overlayVertices) {
            sceneMgr->destroyManualObject(m_overlayVertices);
            m_overlayVertices = nullptr;
        }
        if (m_overlayEdges) {
            sceneMgr->destroyManualObject(m_overlayEdges);
            m_overlayEdges = nullptr;
        }
        if (m_overlayFaces) {
            sceneMgr->destroyManualObject(m_overlayFaces);
            m_overlayFaces = nullptr;
        }
        if (m_softSelSphere) {
            if (m_softSelSphereNode)
                m_softSelSphereNode->detachAllObjects();
            sceneMgr->destroyEntity(m_softSelSphere);
            m_softSelSphere = nullptr;
            if (m_softSelSphereNode) {
                sceneMgr->destroySceneNode(m_softSelSphereNode);
                m_softSelSphereNode = nullptr;
            }
        }

        sceneMgr->destroySceneNode(m_overlayNode);
        m_overlayNode = nullptr;
    }
}


// ===========================================================================
// Wireframe toggle
// ===========================================================================

void EditModeController::applyWireframeMaterials()
{
    if (!m_editEntity)
        return;

    m_savedMaterials.clear();
    for (unsigned int i = 0; i < m_editEntity->getNumSubEntities(); ++i) {
        auto* subEnt = m_editEntity->getSubEntity(i);
        m_savedMaterials[i] = subEnt->getMaterialName();

        Ogre::String wireName = subEnt->getMaterialName() + "_EditWireframe";
        auto& matMgr = Ogre::MaterialManager::getSingleton();
        auto wireMat = matMgr.getByName(wireName);
        if (!wireMat)
            wireMat = subEnt->getMaterial()->clone(wireName);

        for (unsigned short ti = 0; ti < wireMat->getNumTechniques(); ++ti) {
            for (unsigned short pi = 0; pi < wireMat->getTechnique(ti)->getNumPasses(); ++pi) {
                wireMat->getTechnique(ti)->getPass(pi)->setPolygonMode(Ogre::PM_WIREFRAME);
            }
        }
        subEnt->setMaterialName(wireName);
    }
}

void EditModeController::removeWireframeMaterials()
{
    if (!m_editEntity)
        return;

    for (const auto& [idx, matName] : m_savedMaterials) {
        if (idx < m_editEntity->getNumSubEntities()) {
            m_editEntity->getSubEntity(idx)->setMaterialName(matName);
            Ogre::String wireName = matName + "_EditWireframe";
            auto& matMgr = Ogre::MaterialManager::getSingleton();
            if (matMgr.resourceExists(wireName))
                matMgr.remove(wireName);
        }
    }
    m_savedMaterials.clear();
}

void EditModeController::setWireframeEnabled(bool enabled)
{
    if (m_wireframeEnabled == enabled)
        return;

    m_wireframeEnabled = enabled;

    if (m_editModeActive && m_editEntity) {
        if (enabled)
            applyWireframeMaterials();
        else
            removeWireframeMaterials();
    }

    SentryReporter::addBreadcrumb("edit_mode",
        QString("Wireframe %1").arg(enabled ? "enabled" : "disabled"));
    emit wireframeChanged();
}

void EditModeController::refreshNormalVisualizer()
{
    if (!m_editEntity) return;
    auto* mainWin = Manager::getSingletonPtr() ? Manager::getSingleton()->getMainWindow() : nullptr;
    if (!mainWin) return;
    auto* nv = mainWin->findChild<NormalVisualizer*>();
    if (nv) nv->refreshEntity(m_editEntity);
}
