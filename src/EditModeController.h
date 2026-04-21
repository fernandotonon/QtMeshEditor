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
#include <memory>
#include <set>
#include <map>
#include <utility>
#include <vector>
#include <OgreVector.h>
#include <OgreQuaternion.h>

#include "EditableMesh.h" // BevelSession stores std::vector<EditableSubMesh> by value

class OgreWidget;

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

    // Soft selection (proportional editing)
    Q_PROPERTY(bool softSelectionEnabled READ softSelectionEnabled WRITE setSoftSelectionEnabled NOTIFY softSelectionChanged)
    Q_PROPERTY(double softSelectionRadius READ softSelectionRadius WRITE setSoftSelectionRadius NOTIFY softSelectionChanged)
    Q_PROPERTY(int softSelectionFalloff READ softSelectionFalloff WRITE setSoftSelectionFalloff NOTIFY softSelectionChanged)

    // Normals recalculation
    Q_PROPERTY(int normalsMode READ normalsMode WRITE setNormalsMode NOTIFY normalsModeChanged)

    // Mesh validation after edits
    Q_PROPERTY(int degenerateTriangleCount READ degenerateTriangleCount NOTIFY validationChanged)
    Q_PROPERTY(bool hasValidationWarnings READ hasValidationWarnings NOTIFY validationChanged)

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
    Q_INVOKABLE bool bevelSessionActive() const { return m_bevelSession.active; }

    /// @brief Gizmo pivot position in local mesh space (chamfer region center).
    Ogre::Vector3 bevelGizmoOrigin() const { return m_bevelSession.pivot; }

    /// @brief Gizmo axis direction in local mesh space (averaged surface normal).
    Ogre::Vector3 bevelGizmoAxis() const { return m_bevelSession.axis; }

    /// @brief Currently-applied width (starts at 0.005, grows/shrinks via drag).
    float bevelGizmoWidth() const { return m_bevelSession.width; }

    /// @brief Currently-applied segment count (1 = single-strip chamfer).
    Q_INVOKABLE int bevelSegments() const { return m_bevelSession.segments; }

    /// @brief Currently-applied profile shape (0.5 = flat, 1.0 = convex).
    Q_INVOKABLE float bevelProfile() const { return m_bevelSession.profile; }

    /// @brief Re-run the active bevel with new segments (>=1). No-op if
    ///        no session is active or if the value didn't change.
    Q_INVOKABLE void updateBevelSegments(int segments);

    /// @brief Re-run the active bevel with a new profile in [0, 1].
    ///        0.5 = flat, >0.5 = convex (fillet), <0.5 = concave (groove).
    Q_INVOKABLE void updateBevelProfile(float profile);
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
    void selectFace(int triIndex, bool addToSelection = false);

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

    /// Convert a global vertex index to (subMeshIndex, localVertexIndex) pair.
    std::pair<size_t, size_t> globalToLocal(int globalIndex) const;

    /// Convert (subMeshIndex, localVertexIndex) to a global vertex index.
    int localToGlobal(size_t subMeshIndex, size_t localVertexIndex) const;

    /// Convert a global triangle index to (subMeshIndex, localTriangleIndex) pair.
    std::pair<size_t, size_t> globalTriToLocal(int globalTriIndex) const;

    /// Convert (subMeshIndex, localTriangleIndex) to a global triangle index.
    int localTriToGlobal(size_t subMeshIndex, size_t localTriIndex) const;
    /// @}

signals:
    /// Emitted when entering or exiting edit mode.
    void editModeChanged();
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

private slots:
    void onSelectionChanged();

private:
    EditModeController();
    ~EditModeController() override;

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

    void refreshNormalVisualizer();

    // Component selection state
    SelectionMode m_selectionMode = VertexMode;
    std::set<int> m_selectedVertices;              ///< Global vertex indices
    std::set<std::pair<int,int>> m_selectedEdges;  ///< Edges as (min, max) vertex pairs
    std::set<int> m_selectedFaces;                 ///< Global triangle indices

    // Selection overlay
    Ogre::ManualObject* m_overlayVertices = nullptr;
    Ogre::ManualObject* m_overlayEdges = nullptr;
    Ogre::ManualObject* m_overlayFaces = nullptr;
    Ogre::SceneNode* m_overlayNode = nullptr;

    // Bevel session state — populated on beginBevel, consumed on commit/cancel.
    struct BevelSession {
        bool active = false;
        // Snapshot of submeshes before any bevel was applied. Used to restart
        // the bevel at a new width and to restore on cancel or undo.
        std::vector<EditableSubMesh> originalSubMeshes;
        // Selection sets at begin-time, restored on cancel.
        std::set<int> origSelectedVertices;
        std::set<std::pair<int,int>> origSelectedEdges;
        std::set<int> origSelectedFaces;
        // The edges targeted by the bevel, captured so updateBevelWidth can
        // re-run against the same topology on each drag tick.
        std::vector<std::pair<int,int>> targetEdges;
        Ogre::Vector3 pivot = Ogre::Vector3::ZERO; ///< Gizmo pivot (chamfer region center).
        Ogre::Vector3 axis = Ogre::Vector3::UNIT_Y; ///< Gizmo axis (averaged surface normal).
        float width = 0.0f;                         ///< Currently-applied width.
        int segments = 1;                           ///< Chamfer-strip segment count.
        float profile = 0.5f;                       ///< Profile shape (0.5 = flat).
    };
    BevelSession m_bevelSession;
    std::unique_ptr<class BevelGizmo> m_bevelGizmo;

    /// Apply a bevel at `width` to `edges` assuming the mesh is at its
    /// pre-bevel snapshot state. Updates selection to the new chamfer verts.
    /// Internal helper shared by beginBevel / updateBevelWidth.
    bool applyBevelTopology(const std::vector<std::pair<int,int>>& edges,
                            float width,
                            int segments = 1,
                            float profile = 0.5f);

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

    // Wireframe mode
    bool m_wireframeEnabled = false;
    std::map<unsigned int, Ogre::String> m_savedMaterials; ///< SubEntity index → original material name

    // Normals mode: 0=smooth (default), 1=flat
    int m_normalsMode = 0;

    // Mesh validation
    int m_degenerateTriangleCount = 0;
};

#endif // EDITMODECONTROLLER_H
