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

#ifndef EDITMODECONTROLLER_H
#define EDITMODECONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QPoint>
#include <QRect>
#include <QVariantList>
#include <QColor>
#include <atomic>
#include <limits>
#include <memory>
#include <functional>
#include <set>
#include <map>
#include <utility>
#include <vector>
#include <OgreVector.h>
#include <OgreQuaternion.h>

#include "EditableMesh.h" // BevelSession stores std::vector<EditableSubMesh> by value

class OgreWidget;
class HalfEdgeMesh;

namespace Ogre {
    class Entity;
    class Camera;
    class SceneManager;
    class SceneNode;
    class ManualObject;
}

/**
 * @brief QML_SINGLETON that manages Object Mode / Edit Mode state.
 *
 * In Object Mode, the user manipulates entire scene nodes (translate,
 * rotate, scale). In Edit Mode, the user can manipulate individual
 * vertices, edges, and faces of a single mesh entity.
 *
 * Entering Edit Mode decomposes the selected entity's mesh into an
 * editable representation (EditableMesh). Exiting Edit Mode commits
 * the changes back to the Ogre buffers and recalculates normals/bounds.
 *
 * The Tab key toggles between modes. In edit mode, 1/2/3 keys switch
 * between Vertex/Edge/Face selection modes.
 */
class EditModeController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool editModeActive READ isEditModeActive NOTIFY editModeChanged)
    Q_PROPERTY(QString modeLabel READ modeLabel NOTIFY editModeChanged)
    Q_PROPERTY(bool canEnterEditMode READ canEnterEditMode NOTIFY selectionStateChanged)
    Q_PROPERTY(int vertexCount READ vertexCount NOTIFY meshDataChanged)
    Q_PROPERTY(int triangleCount READ triangleCount NOTIFY meshDataChanged)
    Q_PROPERTY(int subMeshCount READ subMeshCount NOTIFY meshDataChanged)
    Q_PROPERTY(int selectionMode READ selectionMode WRITE setSelectionMode NOTIFY selectionModeChanged)
    Q_PROPERTY(int selectedVertexCount READ selectedVertexCount NOTIFY editSelectionChanged)
    Q_PROPERTY(int selectedEdgeCount READ selectedEdgeCount NOTIFY editSelectionChanged)
    Q_PROPERTY(int selectedFaceCount READ selectedFaceCount NOTIFY editSelectionChanged)
    // "Select by Part" (#410) async worker progress.
    Q_PROPERTY(bool segmentBusy READ segmentBusy NOTIFY segmentProgressChanged)
    Q_PROPERTY(bool segmentDownloading READ segmentDownloading NOTIFY segmentProgressChanged)
    Q_PROPERTY(int segmentProgress READ segmentProgress NOTIFY segmentProgressChanged)
    Q_PROPERTY(int segmentTotal READ segmentTotal NOTIFY segmentProgressChanged)

    // Soft selection (proportional editing)
    Q_PROPERTY(bool softSelectionEnabled READ softSelectionEnabled WRITE setSoftSelectionEnabled NOTIFY softSelectionChanged)
    Q_PROPERTY(double softSelectionRadius READ softSelectionRadius WRITE setSoftSelectionRadius NOTIFY softSelectionChanged)
    Q_PROPERTY(int softSelectionFalloff READ softSelectionFalloff WRITE setSoftSelectionFalloff NOTIFY softSelectionChanged)

    // Normals recalculation
    Q_PROPERTY(int normalsMode READ normalsMode WRITE setNormalsMode NOTIFY normalsModeChanged)

    // Mesh validation after edits
    Q_PROPERTY(int degenerateTriangleCount READ degenerateTriangleCount NOTIFY validationChanged)
    Q_PROPERTY(bool hasValidationWarnings READ hasValidationWarnings NOTIFY validationChanged)

    // Vertex color paint (MVP)
    Q_PROPERTY(bool vertexPaintEnabled READ vertexPaintEnabled WRITE setVertexPaintEnabled NOTIFY vertexPaintChanged)
    Q_PROPERTY(QColor vertexPaintColor READ vertexPaintColor WRITE setVertexPaintColor NOTIFY vertexPaintChanged)
    Q_PROPERTY(QColor vertexPaintBackgroundColor READ vertexPaintBackgroundColor WRITE setVertexPaintBackgroundColor NOTIFY vertexPaintChanged)
    Q_PROPERTY(double vertexPaintRadius READ vertexPaintRadius WRITE setVertexPaintRadius NOTIFY vertexPaintChanged)
    Q_PROPERTY(double vertexPaintStrength READ vertexPaintStrength WRITE setVertexPaintStrength NOTIFY vertexPaintChanged)
    Q_PROPERTY(double vertexPaintFalloff READ vertexPaintFalloff WRITE setVertexPaintFalloff NOTIFY vertexPaintChanged)
    Q_PROPERTY(int vertexPaintShape READ vertexPaintShape WRITE setVertexPaintShape NOTIFY vertexPaintChanged)
    Q_PROPERTY(bool vertexColorPreviewEnabled READ vertexColorPreviewEnabled WRITE setVertexColorPreviewEnabled NOTIFY vertexColorPreviewChanged)

public:
    /// Selection component mode for edit mode.
    enum SelectionMode { VertexMode = 0, EdgeMode = 1, FaceMode = 2 };
    Q_ENUM(SelectionMode)

    static EditModeController* instance();
    static EditModeController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /// @name Mode state
    /// @{
    bool isEditModeActive() const { return m_editModeActive; }
    QString modeLabel() const;

    /// Returns true if the current selection allows entering edit mode
    /// (exactly one mesh entity selected).
    bool canEnterEditMode() const;

    /// Wireframe overlay toggle for edit mode
    Q_PROPERTY(bool wireframeEnabled READ wireframeEnabled WRITE setWireframeEnabled NOTIFY wireframeChanged)
    bool wireframeEnabled() const { return m_wireframeEnabled; }
    void setWireframeEnabled(bool enabled);

    /// True if any submesh of the current edit mesh has non-empty
    /// n-gon `.faces` — i.e. quad-based already.
    Q_INVOKABLE bool isMeshQuadBased() const;

    /// True when at least one submesh still has triangle-only
    /// representation (`.faces` empty). Drives the "Convert to Quads"
    /// toolbar button: a fully n-gon mesh has nothing to do, but a
    /// MIXED mesh (some submeshes already n-gon, others tri-only)
    /// still benefits — a less precise `isMeshQuadBased()` check
    /// would wrongly disable the action on those. (CodeRabbit follow-
    /// up on PR #347.)
    Q_INVOKABLE bool canConvertToQuads() const;
    /// @}

    /// @name Mesh info (only valid when in edit mode)
    /// @{
    int vertexCount() const;
    int triangleCount() const;
    int subMeshCount() const;
    /// @}

    /// @name Mode transitions
    /// @{
    Q_INVOKABLE void toggleEditMode();
    Q_INVOKABLE bool enterEditMode();
    Q_INVOKABLE void exitEditMode(bool commitChanges = true);
    /// Notify listeners that editable mesh UV/attribute data changed (issue #461).
    Q_INVOKABLE void notifyMeshDataChanged();
    /// @}

    /// @name Morph sculpt session (Blender-style non-destructive authoring, #519)
    /// @{
    /// True while a base-preserving morph sculpt session is active. While on,
    /// vertex edits are treated as sculpt work for a morph target: `+ Add`
    /// captures the delta vs the base, and ending the session (or exiting Edit
    /// Mode) RESTORES the base mesh — the base is never permanently changed,
    /// matching Blender shape keys / Maya blend shapes.
    Q_PROPERTY(bool morphSculptActive READ morphSculptActive NOTIFY morphSculptChanged)
    bool morphSculptActive() const { return m_morphSculptActive; }
    /// Begin a morph sculpt session (must already be in Edit Mode).
    Q_INVOKABLE bool beginMorphSculpt();
    /// End the session. Always restores the base mesh from the entry snapshot
    /// (the captured target already lives on the mesh as a Pose + weight).
    Q_INVOKABLE void endMorphSculpt();
    /// @}

    /// @name Component selection mode (Vertex/Edge/Face)
    /// @{
    int selectionMode() const { return static_cast<int>(m_selectionMode); }
    void setSelectionMode(int mode);

    int selectedVertexCount() const { return static_cast<int>(m_selectedVertices.size()); }
    int selectedEdgeCount() const { return static_cast<int>(m_selectedEdges.size()); }
    int selectedFaceCount() const { return static_cast<int>(m_selectedFaces.size()); }

    const std::set<int>& selectedVertices() const { return m_selectedVertices; }
    const std::set<std::pair<int,int>>& selectedEdges() const { return m_selectedEdges; }
    const std::set<int>& selectedFaces() const { return m_selectedFaces; }
    /// @}

    /// @name Soft selection (proportional editing)
    /// @{
    bool softSelectionEnabled() const { return m_softSelectionEnabled; }
    void setSoftSelectionEnabled(bool enabled);
    double softSelectionRadius() const { return m_softSelectionRadius; }
    void setSoftSelectionRadius(double radius);
    int softSelectionFalloff() const { return m_softSelectionFalloff; }
    void setSoftSelectionFalloff(int falloff);

    /// Compute soft selection weights for all vertices.
    /// Returns a map of global vertex index -> weight (0.0 to 1.0).
    /// Selected vertices get weight 1.0; nearby vertices get falloff weight.
    std::map<int, float> getSoftSelectionWeights() const;

    /// Compute soft-selection weights using a caller-supplied position map
    /// instead of the live mesh. Used by baseline-based operations (e.g.
    /// scaleFromSnapshot) to freeze weights at drag-start, so a vertex near
    /// the radius boundary can't drift out of the soft zone mid-drag.
    std::map<int, float> computeSoftSelectionWeightsFromPositions(
        const std::map<int, Ogre::Vector3>& positions) const;
    /// @}

    /// @name Normals recalculation
    /// @{
    int normalsMode() const { return m_normalsMode; }
    void setNormalsMode(int mode);
    Q_INVOKABLE void recalculateNormals(bool smooth = true);
    /// @}

    /// @name Mesh validation after edits
    /// @{
    int degenerateTriangleCount() const { return m_degenerateTriangleCount; }
    bool hasValidationWarnings() const { return m_degenerateTriangleCount > 0; }
    Q_INVOKABLE void validateMesh();
    Q_INVOKABLE void removeDegenerateTriangles();
    /// @}

    /// @name Vertex paint settings
    /// @{
    bool vertexPaintEnabled() const { return m_vertexPaintEnabled; }
    void setVertexPaintEnabled(bool enabled);
    QColor vertexPaintColor() const { return m_vertexPaintColor; }
    void setVertexPaintColor(const QColor& c);
    /// Preferred from QML: parses "#RRGGBB" / CSS names reliably (avoids QVariant QColor edge cases).
    Q_INVOKABLE void setVertexPaintBrushColor(const QString& cssColor);

    /// Secondary "background" color. Used by:
    /// - texture paint erase (replaces pixels with this color instead of
    ///   transparent, so the user gets a solid fill)
    /// - smart-select "fill with BG" action
    /// The two-color FG/BG model mirrors Photoshop / GIMP / Krita.
    QColor vertexPaintBackgroundColor() const { return m_vertexPaintBackgroundColor; }
    void setVertexPaintBackgroundColor(const QColor& c);
    Q_INVOKABLE void setVertexPaintBackgroundBrushColor(const QString& cssColor);
    /// Swap foreground and background colors. Standard "X" shortcut in
    /// image editors.
    Q_INVOKABLE void swapPaintColors();
    /// Reset to canonical defaults: FG = Fern green (#71BC78),
    /// BG = black. Mirrors the member-initialiser defaults near
    /// the bottom of the class so the documentation cannot drift
    /// from the actual reset behaviour.
    Q_INVOKABLE void resetPaintColors();

    /// Brush footprint shape. Round = circular falloff (the default);
    /// Square = constant-strength axis-aligned rectangle, no falloff.
    /// Falloff slider has no effect in Square mode — the brush stays
    /// crisp like a pixel-art tool. Exposed as an int property to QML.
    enum BrushShape { ShapeRound = 0, ShapeSquare = 1 };
    Q_ENUM(BrushShape)
    int vertexPaintShape() const { return static_cast<int>(m_vertexPaintShape); }
    void setVertexPaintShape(int shape);
    double vertexPaintRadius() const { return m_vertexPaintRadius; }
    void setVertexPaintRadius(double r);
    double vertexPaintStrength() const { return m_vertexPaintStrength; }
    void setVertexPaintStrength(double s);
    /// Brush falloff [0..1]. Internally mapped to an exponent curve.
    double vertexPaintFalloff() const { return m_vertexPaintFalloff; }
    void setVertexPaintFalloff(double f);

    /// Render-time material override to preview vertex colors (Edit Mode only).
    bool vertexColorPreviewEnabled() const { return m_vertexColorPreviewEnabled; }
    void setVertexColorPreviewEnabled(bool enabled);

    /**
     * @brief Commits deferred vertex paint from EditableMesh into GPU vertex buffers.
     * @param entity Must be the active edit target; otherwise this is a no-op.
     * @note Call before mesh/FBX/glTF export so coalesced paint is not still pending on the event loop.
     */
    void flushPendingVertexPaintForEntity(Ogre::Entity* entity);
    /// @}

    /// @name Topology operations
    /// @{
    /**
     * @brief Extrude the current selection (faces or edges depending on mode).
     *
     * In Face mode: duplicates selected faces, creates side walls, selects
     * the new (top) vertices for immediate translation.
     * In Edge mode: creates new faces connecting selected edges to new vertices.
     * In Vertex mode: no-op.
     *
     * @return true if extrude succeeded.
     */
    Q_INVOKABLE bool extrudeSelection();

    /**
     * @brief Bevel the current selection (Edge mode only, fixed-width for now).
     *
     * Replaces each selected interior edge with a flat chamfer quad at a
     * starting width of 0.005 local units. Skips boundary edges, edges with
     * one-face adjacency, and edges that share endpoints with other selected
     * edges (chained bevels need special handling, planned for a follow-up).
     *
     * Pushes a single undo command capturing the full mesh state.
     *
     * @return true if bevel succeeded (at least one edge was beveled).
     */
    Q_INVOKABLE bool bevelSelection();

    /**
     * @brief Begin an interactive bevel session at default width 0.005.
     *
     * Captures a snapshot for Esc-cancel and future undo; applies an
     * initial bevel and positions a bevel gizmo on the chamfered region.
     * Returns true if at least one edge was beveled.
     *
     * While a session is active, the Inspector's Bevel button and Cmd+B
     * are the same as clicking the gizmo drag handle — width updates
     * happen through updateBevelWidth. The session ends only on
     * commitBevel or cancelBevel.
     */
    Q_INVOKABLE bool beginBevel();

    /**
     * @brief Re-run bevel at a new width using the session's snapshot.
     *
     * No-op if no session is active. Does not push an undo command —
     * intermediate widths are not undoable. Call commitBevel to finalize.
     */
    Q_INVOKABLE void updateBevelWidth(float width);

    /// @brief Finalize the current bevel session: push a single undo command.
    Q_INVOKABLE void commitBevel();

    /// @brief Abandon the current bevel session: restore mesh + selection.
    Q_INVOKABLE void cancelBevel();

    /// @brief Whether a bevel session is active (gizmo should be visible).
    ///        Exposed as a Q_PROPERTY so QML `visible:` bindings get a
    ///        real bool (not a function reference, which is always truthy).
    Q_PROPERTY(bool bevelSessionActiveValue READ bevelSessionActive
               NOTIFY bevelProfilePointsChanged)
    bool bevelSessionActive() const { return m_bevelSession.active; }

    /// @brief Gizmo pivot position in local mesh space (chamfer region center).
    Ogre::Vector3 bevelGizmoOrigin() const { return m_bevelSession.pivot; }

    /// @brief Gizmo axis direction in local mesh space (averaged surface normal).
    Ogre::Vector3 bevelGizmoAxis() const { return m_bevelSession.axis; }

    /// @brief Currently-applied width (starts at 0.005, grows/shrinks via drag).
    float bevelGizmoWidth() const { return m_bevelSession.width; }

    /// @brief Currently-applied segment count (1 = single-strip chamfer).
    ///        Exposed as a Q_PROPERTY so QML bindings (e.g. a segments
    ///        SpinBox) get a real int, not a function reference.
    Q_PROPERTY(int bevelSegmentsValue READ bevelSegments
               NOTIFY bevelProfilePointsChanged)
    int bevelSegments() const { return m_bevelSession.segments; }

    /// @brief Currently-applied profile points (size = segments-1).
    ///        Each value in [0, 1]: 0.5 = flat, 1 = max outward bulge,
    ///        0 = max inward bulge. Empty when segments == 1.
    ///        Exposed as a Q_PROPERTY so QML bindings re-evaluate when it
    ///        changes; updated on every segments/point-value change.
    Q_PROPERTY(QVariantList bevelProfilePointsList READ bevelProfilePoints
               NOTIFY bevelProfilePointsChanged)
    QVariantList bevelProfilePoints() const;

    /// @brief Re-run the active bevel with new segments (>=1). No-op if
    ///        no session is active or if the value didn't change.
    ///        Resizes the profile-points vector to segments-1, preserving
    ///        existing values where possible.
    Q_INVOKABLE void updateBevelSegments(int segments);

    /// @brief Re-run the active bevel with a new value at one profile
    ///        point index. `index` in [0, segments-2], `value` in [0, 1].
    Q_INVOKABLE void updateBevelProfilePoint(int index, float value);

    /// @brief Reset all profile points to 0.5 (flat chamfer).
    Q_INVOKABLE void resetBevelProfile();
    /// @}

    /// @name Knife tool
    /// @{
    /**
     * @brief Enter knife mode. The user places cut points along mesh
     *        surface (left-click), commits with commitKnife (Enter or
     *        double-click) or cancels with cancelKnife (Esc). While
     *        active, knifeSessionActive() returns true and the viewport
     *        draws a live preview of the pending cut line.
     */
    Q_INVOKABLE bool beginKnife();

    /**
     * @brief Record a cut point from a viewport click. The hit-test
     *        priority is vertex → edge → face (so snaps are sticky at
     *        geometry boundaries). Called from TransformOperator's
     *        mouse handler when the knife session is active.
     */
    bool addKnifePoint(OgreWidget* widget, int screenX, int screenY);

    /**
     * @brief Update the hover preview. Called from the mouse-move path
     *        while knife is active; redraws the provisional segment
     *        between the last confirmed point and the cursor.
     */
    void updateKnifeHover(OgreWidget* widget, int screenX, int screenY);

    /**
     * @brief Programmatic cut-point entry: append a knife click at
     *        parametric position `t` on an existing HE edge, resolved
     *        against the current mesh. Used by scripted/automated knife
     *        flows and by unit tests that can't run the widget-based
     *        hit-test (headless CI, macOS without plugins).
     */
    bool addKnifePointOnEdge(int heEdgeIndex, float t);

    /**
     * @brief Apply the current cut point list as splitEdge operations and
     *        push one undo command. Clears the session afterwards.
     *        No-op (returns false) if fewer than 2 points are confirmed.
     */
    Q_INVOKABLE bool commitKnife();

    /// @brief Abandon the current knife session without mutating the mesh.
    Q_INVOKABLE void cancelKnife();

    /// @brief Whether a knife session is active. Exposed as a Q_PROPERTY
    ///        so QML bindings get a real bool.
    Q_PROPERTY(bool knifeSessionActiveValue READ knifeSessionActive
               NOTIFY knifeSessionChanged)
    bool knifeSessionActive() const { return m_knifeSession.active; }

    /// @brief Count of confirmed cut points. QML uses this to decide
    ///        whether Enter would commit or is a no-op.
    Q_PROPERTY(int knifePointCountValue READ knifePointCount
               NOTIFY knifeSessionChanged)
    int knifePointCount() const {
        return static_cast<int>(m_knifeSession.points.size());
    }
    /// @}

    /// @name Merge vertices
    /// @{
    /**
     * @brief Collapse the current vertex selection into a single survivor at
     *        their centroid. No-op if fewer than 2 vertices are selected or
     *        the selection spans submeshes (HE refuses cross-submesh merges
     *        to preserve UV seams). Pushes one undo command labeled
     *        "Merge At Center". Returns the count of removed vertices
     *        (selection size minus 1) on success, 0 otherwise.
     */
    Q_INVOKABLE int mergeAtCenter();

    /**
     * @brief Collapse selected vertices to the position of the first
     *        (lowest-index) vertex in the selection. Same constraints and
     *        behavior as mergeAtCenter() otherwise.
     */
    Q_INVOKABLE int mergeAtFirst();

    /**
     * @brief Collapse selected vertices to the position of the last
     *        (highest-index) vertex in the selection.
     */
    Q_INVOKABLE int mergeAtLast();

    /**
     * @brief Fuse any pair of selected vertices that lie within `threshold`
     *        of each other (default 1e-4 ≈ 0.1 mm at meter scale). Each
     *        cluster collapses to its centroid. Useful for cleaning up
     *        duplicate vertices left by extrudes / mirror operations.
     *
     * @return Total vertices retired across all clusters.
     */
    Q_INVOKABLE int mergeByDistance(float threshold = 1e-4f);
    /// @}

    /// @name Delete / Dissolve
    /// @{
    /**
     * @brief Delete the current edit-mode selection.
     *
     * Dispatches by selection mode:
     *   - VertexMode → HEMesh::deleteVertices on the selected vertex set
     *   - EdgeMode   → HEMesh::deleteEdges on the selected edges
     *   - FaceMode   → HEMesh::deleteFaces on the selected triangles
     *
     * Pushes one undo command labeled "Delete <Mode>". Returns the
     * number of elements actually retired, or 0 on no-op.
     */
    Q_INVOKABLE int deleteSelection();

    /**
     * @brief Dissolve the current edit-mode selection.
     *
     * Dispatches by selection mode:
     *   - VertexMode → HEMesh::dissolveVertices
     *   - EdgeMode   → HEMesh::dissolveEdges
     *   - FaceMode   → HEMesh::deleteFaces (same result as Delete Faces
     *                  on a pure triangle mesh — there are no coplanar
     *                  neighbors to merge into an n-gon)
     *
     * Pushes one undo command labeled "Dissolve <Mode>". Returns the
     * number of elements actually dissolved, or 0 on no-op.
     */
    Q_INVOKABLE int dissolveSelection();
    /// @}

    /// @name Subdivide / Fill
    /// @{
    /**
     * @brief Subdivide selected faces (1-to-4 triangle split).
     *
     * Face mode: subdivides the selected triangles. Adjacent non-selected
     * triangles are retriangulated as needed to avoid T-junctions.
     *
     * Edge mode: subdivides the union of triangles incident to any
     * selected edge (Blender convention — selecting an edge subdivides
     * the surrounding faces).
     *
     * Vertex mode: no-op (vertex selection alone doesn't define faces
     * to split).
     *
     * After the operation, the new midpoint vertices are selected so
     * the user can immediately translate them. Pushes one undo command
     * labeled "Subdivide".
     *
     * @return Number of triangles whose topology changed (0 on no-op).
     */
    Q_INVOKABLE int subdivideSelection();

    /**
     * @brief Insert a loop cut starting from the first selected edge.
     *
     * Walks the chain of quads adjacent to the start edge via the
     * "opposite edge" relation. Each quad in the ring is bisected by
     * splitting two parallel edges at their midpoints and connecting
     * the new midpoints with a new edge. The walk terminates at
     * boundaries, non-quad faces, or when it loops back to the start.
     *
     * Requires Edge selection mode and at least one selected edge.
     * Uses the FIRST selected edge as the start; multi-edge loop cuts
     * are out of scope for the MVP (each cut is independent).
     *
     * Pushes one undo command labeled "Loop Cut".
     *
     * @return Number of new vertices inserted (0 on no-op / failure).
     */
    Q_INVOKABLE int loopCutSelection();

    /**
     * @brief Subdivide the entire mesh by one Catmull-Clark step.
     *
     * Unlike `subdivideSelection` (which does a 1-to-4 triangle split
     * on the selected faces only), this op operates on the whole mesh
     * at once and produces an all-quad output regardless of input
     * topology — a triangle becomes 3 quads, a quad becomes 4. Output
     * geometry is smoothed via the classic Catmull-Clark rule (face
     * points, edge points, smoothed vertex positions) so the surface
     * approaches a C¹-continuous limit on closed manifolds.
     *
     * Selection is cleared after the op (the new face/edge points
     * don't have stable analogues in the pre-op selection set, and
     * partial-mesh CC needs a more sophisticated boundary blend that
     * isn't in this MVP).
     *
     * Pushes one undo command labeled "Catmull-Clark Subdivide".
     *
     * @return Number of vertices added (0 on no-op).
     */
    Q_INVOKABLE int subdivideCatmullClarkAll();

    /**
     * @brief Fill the current selection with new face(s).
     *
     * Vertex mode: 3 selected vertices → emit a triangle. 4 selected →
     * emit two triangles (fan from the first selected vertex). 5+ → fan-
     * triangulate (N-2 triangles).
     *
     * Edge mode: detects a closed boundary edge loop in the selection
     * and fan-triangulates it from the lowest-index loop vertex. The
     * loop must be closed (every selected edge endpoint shared with
     * exactly two selected edges).
     *
     * Face mode: no-op.
     *
     * Pushes one undo command labeled "Fill".
     *
     * @return Number of triangles created (0 on no-op or rejection).
     */
    Q_INVOKABLE int fillSelection();

    /**
     * @brief Merge coplanar adjacent triangle pairs into quads.
     *
     * Whole-mesh operation: walks every submesh, looks for triangle pairs
     * that share an edge and are within `angleThresholdDeg` of coplanar
     * (default 1°), and merges each qualifying pair into a single quad
     * face. Promotes the legacy triangle-only representation into the
     * n-gon canonical form. After the operation, downstream features
     * that branch on n-gon `.faces` (loop cut, n-gon-aware bevel,
     * quad-aware wireframe) start working.
     *
     * No-op when nothing qualifies (all tris are non-coplanar, or the
     * mesh is already quad-dominant). Pushes one undo command labeled
     * "Convert to Quads" iff merges happened.
     *
     * @param angleThresholdDeg Maximum dihedral angle (deg) to treat as
     *        coplanar. 0 = strict, ~5 = forgiving for float-quantised
     *        imports. Defaults to 1°.
     * @return Number of triangle pairs merged across all submeshes.
     */
    Q_INVOKABLE int convertToQuads(float angleThresholdDeg = 1.0f);
    /// @}

    /// @name Vertex transform support
    /// @{
    /// Get the centroid of selected vertices in local mesh space.
    Ogre::Vector3 getSelectedVerticesCentroid() const;

    /// Apply a translation delta to selected vertices (with optional soft selection).
    void translateSelectedVertices(const Ogre::Vector3& delta);

    /// Apply a rotation to selected vertices around the centroid (with optional soft selection).
    void rotateSelectedVertices(const Ogre::Quaternion& rotation);

    /// Apply a scale to selected vertices around the centroid (with optional soft selection).
    void scaleSelectedVertices(const Ogre::Vector3& scaleFactor);

    /// Scale vertices to a target configuration computed from a frozen
    /// snapshot + pivot. The snapshot provides the pre-drag positions; the
    /// pivot provides the center of scaling captured at drag start. Avoids
    /// the centroid-drift that compounds when scale is applied incrementally
    /// over many small frames (trackpad issue).
    void scaleFromSnapshot(const std::map<int, Ogre::Vector3>& snapshot,
                           const Ogre::Vector3& pivot,
                           const Ogre::Vector3& scaleFactor);

    /// Snapshot current positions of all affected vertices (selected + soft selection).
    /// Call before starting a transform drag.
    std::map<int, Ogre::Vector3> snapshotVertexPositions() const;

    /// Restore vertex positions from a snapshot.
    void restoreVertexPositions(const std::map<int, Ogre::Vector3>& snapshot);
    /// @}

    /// @name Selection operations
    /// @{
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void deselectAll();

    /// Mark / clear UV seam flags on the current edge selection (issue #462).
    Q_INVOKABLE void markSeamOnSelection();
    Q_INVOKABLE void clearSeamOnSelection();

    /// AI "Select by part" (#410): predict a semantic part label (head/torso/
    /// arm/leg) per face via MeshSegmenter, then add every face whose label
    /// matches any currently-selected face's label to the selection. With NO
    /// face selected, selects the largest predicted part. Switches to Face mode.
    /// Uses the ONNX model when available, else the geometric fallback.
    ///
    /// ASYNC: gathers geometry on the main thread, then runs the (potentially
    /// slow) first-use model download + ONNX inference on a WORKER thread so the
    /// UI stays responsive, and finally applies the face selection back on the
    /// main thread. Progress surfaces via segmentBusy / segmentDownloading /
    /// segmentProgress(/Total) and the segmentFinished(status) signal. Returns a
    /// short immediate status ("Segmenting…" / a guard message); the final
    /// result arrives via segmentFinished. No-op if already running.
    ///
    /// `upAxis` ("x"/"y"/"z", default "y") matches the CLI `--up-axis` / MCP
    /// `up_axis` parity — it drives the geometric/fallback head-vs-leg heuristic
    /// for meshes that are not +Y up. Case-insensitive; an empty/unknown value
    /// keeps the +Y default.
    ///
    /// `category` matches the CLI `--category` / MCP `category` parity (#818):
    /// "auto" (default) runs the point-cloud category classifier; an explicit
    /// "body"/"vegetation"/"vehicle"/"building" forces that label set — the
    /// manual override for meshes the classifier gets wrong (e.g. a car with
    /// detached wheels). An explicit NON-body category also skips the
    /// rig-prior fast path (rig labels are body parts).
    Q_INVOKABLE QString selectByPart(const QString& upAxis = QStringLiteral("y"),
                                     const QString& category = QStringLiteral("auto"));

    /// Cancel an in-flight selectByPart worker (no-op otherwise).
    Q_INVOKABLE void cancelSegment();

    bool segmentBusy() const { return m_segmentBusy; }
    bool segmentDownloading() const { return m_segmentDownloading; }
    int  segmentProgress() const { return m_segmentProgress; }   // 0..segmentTotal
    int  segmentTotal() const { return m_segmentTotal; }

    /// Select a vertex by global index. If addToSelection is false, clears
    /// prior selection first.
    void selectVertex(int globalIndex, bool addToSelection = false);

    /// Deselect a vertex by global index.
    void deselectVertex(int globalIndex);

    /// Select an edge (pair of global vertex indices, stored min-first).
    void selectEdge(int v1, int v2, bool addToSelection = false);

    /// Deselect an edge by vertex pair.
    void deselectEdge(int v1, int v2);

    /// Select a face (triangle) by global triangle index.
    // `notify` controls whether the overlay is rebuilt + editSelectionChanged()
    // is emitted at the end (default true). Batch callers (e.g. select-by-part)
    // pass false for every call and fire the update ONCE afterward.
    void selectFace(int triIndex, bool addToSelection = false, bool notify = true);

    /// Deselect a face by global triangle index.
    void deselectFace(int triIndex);
    /// @}

    /// @name Hit testing
    /// @{
    /**
     * @brief Find the closest vertex to a screen-space point.
     *
     * Projects each vertex from local to world to screen space and returns
     * the global vertex index of the nearest within a pixel radius, or -1.
     *
     * @param screenPos Click position in widget coordinates.
     * @param camera The Ogre camera used for projection.
     * @param viewportWidth Width of the viewport in pixels.
     * @param viewportHeight Height of the viewport in pixels.
     * @param pixelRadius Maximum screen-space distance in pixels (default 10).
     * @return Global vertex index or -1 if none within radius.
     */
    int hitTestVertex(const QPoint& screenPos, Ogre::Camera* camera,
                      int viewportWidth, int viewportHeight,
                      float pixelRadius = 10.0f) const;

    /**
     * @brief Find the closest face (triangle) to a screen-space point via ray intersection.
     *
     * Casts a ray from the camera through the click point and tests against
     * each triangle in the EditableMesh (in local space, transformed by entity node).
     *
     * @return Global triangle index or -1 if no hit.
     */
    int hitTestFace(const QPoint& screenPos, Ogre::Camera* camera,
                    int viewportWidth, int viewportHeight) const;

    /**
     * @brief Find the closest edge to a screen-space point.
     *
     * Projects edge endpoints to screen space and returns the edge with
     * minimum screen-space distance below threshold. The edge is returned
     * as a pair (min vertex index, max vertex index).
     *
     * @return Vertex pair or (-1, -1) if no edge within threshold.
     */
    std::pair<int,int> hitTestEdge(const QPoint& screenPos, Ogre::Camera* camera,
                                   int viewportWidth, int viewportHeight,
                                   float pixelRadius = 10.0f) const;

    /**
     * @brief Select vertices within a screen-space rectangle (box select).
     *
     * @param rect Screen-space rectangle.
     * @param camera The Ogre camera used for projection.
     * @param viewportWidth Width of the viewport in pixels.
     * @param viewportHeight Height of the viewport in pixels.
     * @param addToSelection If true, adds to existing selection.
     */
    void boxSelectVertices(const QRect& rect, Ogre::Camera* camera,
                           int viewportWidth, int viewportHeight,
                           bool addToSelection = false);
    /// @}

    /// @name Mouse handling (called from TransformOperator when in edit mode)
    /// @{
    /**
     * @brief Handle a mouse click in edit mode.
     *
     * Performs hit testing based on current selection mode and updates
     * the selection accordingly.
     *
     * @param screenPos Click position in widget coordinates.
     * @param widget The active OgreWidget.
     * @param shiftHeld True if Shift modifier is held (add to selection).
     * @param ctrlHeld True if Ctrl modifier is held (remove from selection).
     */
    void handleMouseClick(const QPoint& screenPos, OgreWidget* widget,
                          bool shiftHeld, bool ctrlHeld);

    /**
     * @brief Handle box selection completion in edit mode.
     *
     * @param startPos Starting corner of the selection box.
     * @param endPos Ending corner of the selection box.
     * @param widget The active OgreWidget.
     * @param shiftHeld True if Shift modifier is held.
     */
    void handleBoxSelect(const QPoint& startPos, const QPoint& endPos,
                         OgreWidget* widget, bool shiftHeld);
    /// @}

    /// @name Vertex paint stroke (called from TransformOperator)
    /// @{
    bool beginVertexPaintStroke(OgreWidget* widget, const QPoint& screenPos);
    void updateVertexPaintStroke(OgreWidget* widget, const QPoint& screenPos);
    void updateVertexPaintPreview(OgreWidget* widget, const QPoint& screenPos);
    void endVertexPaintStroke(bool commitUndo = true);
    void clearVertexPaintPreview();
    bool vertexPaintStrokeActive() const { return m_vertexPaintStrokeActive; }
    /// @}

    /// Access the current editable mesh (nullptr if not in edit mode).
    EditableMesh* currentMesh() const { return m_editableMesh.get(); }

    /// Returns the entity being edited (nullptr if not in edit mode).
    Ogre::Entity* editEntity() const { return m_editEntity; }

    /// @name Geometry helpers (public for testing)
    /// @{

    /// Convert a local-space vertex position to screen-space using entity transform and camera.
    static QPoint worldToScreen(const Ogre::Vector3& worldPos, Ogre::Camera* camera,
                                int viewportWidth, int viewportHeight);

    /// Compute the distance from a point to a line segment in 2D.
    static float pointToSegmentDistance(const QPoint& point,
                                        const QPoint& segA, const QPoint& segB);

    /// Ray-triangle intersection (Moller-Trumbore algorithm).
    /// Returns the distance t along the ray, or -1 if no intersection.
    static float rayTriangleIntersect(const Ogre::Vector3& rayOrigin,
                                      const Ogre::Vector3& rayDir,
                                      const Ogre::Vector3& v0,
                                      const Ogre::Vector3& v1,
                                      const Ogre::Vector3& v2);

    /// Map a soft selection weight (0.0–1.0) to a heat map colour.
    /// Red (1.0) → orange → yellow → green → cyan → blue (0.0).
    static Ogre::ColourValue weightToColor(float weight);

    /// Apply a vertex color brush in local space. Returns true if any vertex changed.
    /// `square` switches the footprint from a sphere (default) to an
    /// axis-aligned cube of side 2*radius with constant strength (no
    /// falloff). Matches the 2D TexturePaintBuffer::BrushShape::Square
    /// model in 3D.
    static bool applyVertexColorBrush(EditableMesh& mesh,
                                      const Ogre::Vector3& localCenter,
                                      float radius,
                                      const Ogre::ColourValue& color,
                                      float strength,
                                      float falloff,
                                      bool square = false);

    /// Convert a global vertex index to (subMeshIndex, localVertexIndex) pair.
    std::pair<size_t, size_t> globalToLocal(int globalIndex) const;

    /// Convert (subMeshIndex, localVertexIndex) to a global vertex index.
    int localToGlobal(size_t subMeshIndex, size_t localVertexIndex) const;

    /// Convert a global triangle index to (subMeshIndex, localTriangleIndex) pair.
    std::pair<size_t, size_t> globalTriToLocal(int globalTriIndex) const;

    /// Convert (subMeshIndex, localTriangleIndex) to a global triangle index.
    int localTriToGlobal(size_t subMeshIndex, size_t localTriIndex) const;

    /// @brief Convert the current `m_selectedFaces` set (global triangle
    /// indices) into a deduplicated list of HE face indices that
    /// HalfEdgeMesh ops accept. Each unique HE face is reported once
    /// regardless of how many of its fan-triangulated children appear
    /// in the selection — so a quad selected via either of its
    /// triangles maps to a single HE face.
    ///
    /// HE face indexing matches `HalfEdgeMesh::buildFromEditableMesh`
    /// order: submesh 0's faces first (or its triangles, in legacy
    /// triangle-only submeshes), then submesh 1's, etc.
    std::vector<int> selectedFacesAsHEFaceIndices() const;
    /// @}

signals:
    /// "Select by Part" worker progress (segmentBusy/Downloading/Progress/Total).
    void segmentProgressChanged();
    /// Final result of an async selectByPart() (status string; isError flag).
    void segmentFinished(const QString& status, bool isError);
    /// Emitted when entering or exiting edit mode.
    void editModeChanged();
    /// Emitted when a morph sculpt session starts/ends (#519).
    void morphSculptChanged();
    /// Emitted when the mesh data is modified during edit mode.
    void meshDataChanged();
    /// Emitted when the selection changes (to update canEnterEditMode).
    void selectionStateChanged();
    /// Emitted when the component selection mode changes (Vertex/Edge/Face).
    void selectionModeChanged();
    /// Emitted when wireframe mode is toggled.
    void wireframeChanged();
    /// Emitted when the edit-mode selection (vertices/edges/faces) changes.
    void editSelectionChanged();
    /// Emitted when soft selection settings change.
    void softSelectionChanged();
    /// Emitted when normals mode changes.
    void normalsModeChanged();
    /// Emitted when mesh validation results change.
    void validationChanged();
    /// Emitted when the bevel profile points vector changes (size or value).
    void bevelProfilePointsChanged();
    /// Emitted whenever the knife session starts, gains a point, or ends —
    /// so QML toolbar state and preview overlay refresh together.
    void knifeSessionChanged();
    void vertexPaintChanged();
    void vertexColorPreviewChanged();

    /// Emitted when an edit-mode op short-circuits and wants to surface
    /// a one-line explanation to the user (e.g. "Loop cut requires a
    /// quad mesh — try Mesh → Convert to Quads"). QML overlays /
    /// status-bar widgets can subscribe.
    void editHintMessage(const QString& message);

private slots:
    void onSelectionChanged();

private:
    EditModeController();
    ~EditModeController() override;

    /// Shared post-merge plumbing: snapshot mesh + selection, run the
    /// HE-side `mergeFn`, write back, recompute normals, refresh entity,
    /// re-select the survivor(s) by hunting `survivorTargets` positions
    /// in the re-packed mesh, and push one EditMeshTopologyCommand.
    /// Returns the retired-vertex count (0 on no-op / refusal).
    int applyMergeAndRefresh(
        const QString& opLabel,
        const std::function<int(HalfEdgeMesh&)>& mergeFn,
        const std::vector<Ogre::Vector3>& survivorTargets);

    /// Build or rebuild the selection overlay ManualObject.
    void updateSelectionOverlay();
    /// Destroy the selection overlay ManualObject and scene node.
    void destroySelectionOverlay();
    /// Create the materials used for selection overlays.
    void createOverlayMaterials();

    static EditModeController* m_pSingleton;

    bool m_editModeActive = false;
    std::unique_ptr<EditableMesh> m_editableMesh;
    Ogre::Entity* m_editEntity = nullptr;

    // Morph sculpt session (#519): pristine base positions captured at
    // beginMorphSculpt(), restored on endMorphSculpt() so the base mesh is
    // never permanently altered by morph-target authoring. One entry per
    // submesh, flat xyz — matches EditableSubMesh vertex ordering.
    bool m_morphSculptActive = false;
    std::vector<std::vector<Ogre::Vector3>> m_morphBaseSnapshot;

    void refreshNormalVisualizer();

    // Component selection state
    SelectionMode m_selectionMode = VertexMode;
    std::set<int> m_selectedVertices;              ///< Global vertex indices
    std::set<std::pair<int,int>> m_selectedEdges;  ///< Edges as (min, max) vertex pairs
    std::set<int> m_selectedFaces;                 ///< Global triangle indices

    // "Select by Part" (#410) async worker state. The ONNX download + inference
    // run on a worker thread; these mirror progress back to the UI (guarded:
    // written on the main thread via QueuedConnection from the worker).
    bool m_segmentBusy = false;
    bool m_segmentDownloading = false;
    int  m_segmentProgress = 0;
    int  m_segmentTotal = 0;
    std::shared_ptr<std::atomic_bool> m_segmentCancel;
    // Applies a finished segmentation Result's face labels to the selection
    // (main thread); emits segmentFinished. Declared here, defined in the .cpp.
    void finishSegmentOnMain(const std::vector<int>& faceLabels,
                             bool usedModel, const QString& predictError);

    // Selection overlay
    Ogre::ManualObject* m_overlayVertices = nullptr;
    Ogre::ManualObject* m_overlayEdges = nullptr;
    Ogre::ManualObject* m_overlaySeamEdges = nullptr;
    Ogre::ManualObject* m_overlayFaces = nullptr;
    /// Quad-aware wireframe: lines along n-gon face boundaries only,
    /// hiding the diagonals introduced by `triangulateFaces()`. Active
    /// when `m_wireframeEnabled` AND any submesh has non-empty `.faces`.
    /// On legacy triangle-only meshes this stays empty and the
    /// PM_WIREFRAME material override does the work instead.
    Ogre::ManualObject* m_overlayBoundaryEdges = nullptr;
    Ogre::SceneNode* m_overlayNode = nullptr;

    // Bevel session state — populated on beginBevel, consumed on commit/cancel.
    struct BevelSession {
        bool active = false;
        // What kind of bevel this session is operating on. Decides which
        // HalfEdgeMesh API applyBevelTopology calls.
        enum Kind { Edges, Vertices };
        Kind kind = Edges;
        // Snapshot of submeshes before any bevel was applied. Used to restart
        // the bevel at a new width and to restore on cancel or undo.
        std::vector<EditableSubMesh> originalSubMeshes;
        // Selection sets at begin-time, restored on cancel.
        std::set<int> origSelectedVertices;
        std::set<std::pair<int,int>> origSelectedEdges;
        std::set<int> origSelectedFaces;
        // Kind == Edges: the edges targeted by the bevel.
        std::vector<std::pair<int,int>> targetEdges;
        // Kind == Vertices: the vertex indices targeted by the bevel.
        std::vector<int> targetVertices;
        Ogre::Vector3 pivot = Ogre::Vector3::ZERO; ///< Gizmo pivot (chamfer region center).
        Ogre::Vector3 axis = Ogre::Vector3::UNIT_Y; ///< Gizmo axis (averaged surface normal).
        float width = 0.0f;                         ///< Currently-applied width.
        // Computed at session start: the maximum width the bevel algorithm
        // will actually use before its internal clamp kicks in. The drag
        // handler clamps `width` — and the gizmo shaft/handle — against
        // this so the visible shaft stops growing the instant the bevel
        // caps, instead of the handle drifting past the capped bevel.
        float maxWidth = std::numeric_limits<float>::infinity();
        int segments = 1;                           ///< Chamfer-strip segment count.
        /// Per-interior-point profile values (size = segments-1, each in
        /// [0, 1], 0.5 = flat). Empty when segments == 1.
        std::vector<float> profilePoints;
    };
    BevelSession m_bevelSession;
    std::unique_ptr<class BevelGizmo> m_bevelGizmo;

    // Knife session state — populated on beginKnife, mutated by each
    // addKnifePoint / updateKnifeHover, consumed on commitKnife.
    struct KnifePoint {
        enum Kind { OnVertex, OnEdge, OnFace };
        Kind kind = OnFace;
        // OnVertex: vertexIndex. OnEdge: edgeIndex + edgeT. OnFace:
        // triangleIndex + world-space position (no splitEdge needed for
        // on-face points — they land inside a face and the commit
        // pipeline handles them separately).
        int vertexIndex = -1;
        int edgeIndex = -1;
        float edgeT = 0.5f;
        int triangleIndex = -1;
        // Local-space position of the point (for preview rendering and
        // on-face fallback placement).
        Ogre::Vector3 localPosition = Ogre::Vector3::ZERO;
    };

    struct KnifeSession {
        bool active = false;
        std::vector<KnifePoint> points;      ///< Confirmed points in click order.
        bool hoverValid = false;             ///< True while cursor hit-test is hitting the mesh.
        KnifePoint hover;                    ///< Preview point under the cursor.
    };
    KnifeSession m_knifeSession;

    // Vertex paint state. Default FG = Fern (Qt "Fern" CSS color =
    // #71BC78 = 113,188,120) so paint strokes are immediately visible
    // on either dark or light textures, and BG = black so the Erase
    // tool produces a recognisable hole.
    bool m_vertexPaintEnabled = false;
    QColor m_vertexPaintColor = QColor(113, 188, 120);
    QColor m_vertexPaintBackgroundColor = QColor(0, 0, 0);
    // Brush radius in local mesh units. Default 0.02 produces a small
    // crisp dot on most meshes; users can scale up to 2.0 for broad
    // washes or down to 0.001 for pixel-level precision via the
    // toolbar slider / brush popup.
    double m_vertexPaintRadius = 0.02;
    // Brush footprint shape. Default Round = circular falloff,
    // Square = axis-aligned constant-strength rectangle (pixel-art).
    BrushShape m_vertexPaintShape = ShapeRound;
    double m_vertexPaintStrength = 0.5;  // 0..1
    double m_vertexPaintFalloff = 0.5;   // 0..1
    bool m_vertexPaintStrokeActive = false;
    OgreWidget* m_vertexPaintWidget = nullptr;
    Ogre::Vector3 m_vertexPaintLastLocal = Ogre::Vector3::ZERO;
    bool m_vertexPaintHaveLastLocal = false;
    bool m_vertexPaintStrokeDirty = false;
    bool m_vertexPaintFlushScheduled = false;
    bool m_vertexPaintFlushPending = false;
    std::vector<EditableSubMesh> m_vertexPaintStrokeOriginalSubMeshes;

    // Vertex color preview material override (Edit Mode only)
    bool m_vertexColorPreviewEnabled = false;
    std::map<unsigned int, Ogre::String> m_vertexColorPreviewSavedMaterials;
    void applyVertexColorPreviewMaterials();
    void removeVertexColorPreviewMaterials();

    /// Hit-test a screen-space point for knife placement. Priority:
    /// snap to existing vertex within pixelRadius, else snap to edge
    /// within pixelRadius, else ray-cast to a face. Writes the result
    /// into `out` and returns true on a successful hit.
    bool knifeHitTest(const QPoint& screenPos, OgreWidget* widget,
                      KnifePoint& out) const;

    bool hitTestLocalPointOnMesh(const QPoint& screenPos, OgreWidget* widget,
                                 Ogre::Vector3& outLocal,
                                 Ogre::Vector3* outNormal = nullptr) const;

    /// Rebuild the knife preview overlay from the current session
    /// (confirmed points + hover), creating it on first use.
    void updateKnifePreviewOverlay();

    /// Destroy the knife preview overlay at session end.
    void destroyKnifePreviewOverlay();

    Ogre::ManualObject* m_overlayKnife = nullptr;
    // Independent scene node for the knife preview so its entity-mirror
    // transform doesn't fight with selection overlays, which expect
    // m_overlayNode parked at the origin with local-space geometry.
    Ogre::SceneNode* m_overlayKnifeNode = nullptr;
    Ogre::ManualObject* m_overlayPaint = nullptr;
    Ogre::SceneNode* m_overlayPaintNode = nullptr;

    /// Apply a bevel at `width` to `edges` assuming the mesh is at its
    /// pre-bevel snapshot state. Updates selection to the new chamfer verts.
    /// Internal helper shared by beginBevel / updateBevelWidth.
    bool applyBevelTopology(const std::vector<std::pair<int,int>>& edges,
                            float width,
                            int segments = 1,
                            const std::vector<float>& profilePoints = {});

    /// Vertex-bevel variant: applies bevelVertices on the given vertex
    /// indices and propagates the result through the same mesh-rebuild
    /// pipeline as applyBevelTopology.
    bool applyBevelVertexTopology(const std::vector<int>& vertexIndices,
                                  float width,
                                  int segments = 1,
                                  const std::vector<float>& profilePoints = {});

    /// Dispatcher used by updateBevelWidth/Segments/Profile. Reads
    /// m_bevelSession.kind and calls the right applyBevel*Topology.
    bool reapplyActiveBevel(float width,
                            int segments,
                            const std::vector<float>& profilePoints);

    /// World-space pivot (entity transform applied to session.pivot).
    Ogre::Vector3 bevelGizmoWorldOrigin() const;
    /// World-space axis (entity rotation applied to session.axis).
    Ogre::Vector3 bevelGizmoWorldAxis() const;

public:
    /// Pick test — does this MovableObject belong to the active bevel gizmo?
    Q_INVOKABLE bool isBevelGizmoHandle(const Ogre::MovableObject* obj) const;

    /// Per-frame hook: keeps the bevel gizmo at a constant pixel size by
    /// scaling it based on the provided camera's distance.
    void tickBevelGizmo(const Ogre::Camera* camera);

    /// During drag: translate a world-space ray into a new bevel width and
    /// apply it via updateBevelWidth. `startRay` is the ray captured at
    /// mouse-press (so we can compute a delta from drag start). `dragRay` is
    /// the ray at the current mouse position. `startWidth` is the width when
    /// the drag began.
    Q_INVOKABLE void updateBevelFromDrag(const Ogre::Ray& startRay,
                                         const Ogre::Ray& dragRay,
                                         float startWidth);
private:

    // Soft selection (proportional editing)
    bool m_softSelectionEnabled = false;
    double m_softSelectionRadius = 2.0;
    int m_softSelectionFalloff = 0; ///< 0=linear, 1=smooth (cosine)
    Ogre::Entity* m_softSelSphere = nullptr;
    Ogre::SceneNode* m_softSelSphereNode = nullptr;

    // Wireframe helpers
    void applyWireframeMaterials();
    void removeWireframeMaterials();
    /// True if any submesh has non-empty `.faces` (n-gon canonical).
    /// Drives the choice between PM_WIREFRAME (all submeshes pure tris)
    /// and the boundary-edge overlay (any n-gon faces present).
    bool meshHasNgonFaces() const;
    /// (Re)build the n-gon boundary-edge overlay from `m_editableMesh`.
    /// Active when `m_wireframeEnabled` AND `meshHasNgonFaces()` —
    /// otherwise clears the overlay so it draws nothing.
    void updateBoundaryEdgeOverlay();

public:
    /// Refresh an entity after a topology mutation: rebuild tangents
    /// (when any material is bump-mapped), `_deinitialise/_initialise`
    /// the entity, restore per-subentity material overrides, re-attach
    /// RTSS SRS_NORMALMAP, and invalidate cached shader programs. Used
    /// by every Edit-Mode topology op (subdivide, extrude, bevel, …)
    /// AND by `EditMeshTopologyCommand::applyMeshState` so undo/redo
    /// preserves bump map / per-pixel lighting state. Static so
    /// command code can invoke it without a controller instance.
    static void rewriteEntityAfterTopologyChange(Ogre::Entity* ent);

private:

    // Wireframe mode
    bool m_wireframeEnabled = false;
    std::map<unsigned int, Ogre::String> m_savedMaterials; ///< SubEntity index → original material name

    // Normals mode: 0=smooth (default), 1=flat
    int m_normalsMode = 0;

    // Mesh validation
    int m_degenerateTriangleCount = 0;
};

#endif // EDITMODECONTROLLER_H
