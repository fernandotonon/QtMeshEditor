#ifndef LATTICECONTROLLER_H
#define LATTICECONTROLLER_H

#include "LatticeDeformer.h"

#include <QObject>
#include <QQmlEngine>
#include <QJsonObject>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVariantList>

#include <OgreVector.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

class EditableMesh;
class OgreWidget;
namespace Ogre { class Entity; class SceneNode; class ManualObject; class Camera; }

/**
 * Lattice (free-form) deformer — the Blender "Lattice modifier" for
 * QtMeshEditor. QML_SINGLETON (`PropertiesPanel 1.0`), Object mode.
 *
 * Workflow: select a mesh → **Add Lattice** boxes it with an `nx×ny×nz` grid of
 * control points (default 3×3×3) → click/drag points in the viewport (Shift
 * adds to the point selection; a drag moves every selected point in the
 * camera plane) → the enclosed vertices follow live → **Apply** bakes the
 * deformation into the mesh as ONE undo step, **Cancel** restores the mesh.
 *
 * The deformation is always recomputed from the REST positions captured when
 * the session began (`Lattice::Grid::deformAll(rest)`), so it never
 * accumulates and resetting the points restores the mesh bit-exactly.
 * Interactive edits are also undoable inside the session
 * (`LatticePointsCommand`); the bake is `LatticeApplyCommand`.
 *
 * Viewport hooks (`sessionActive`/`beginDrag`/`updateDrag`/`endDrag`/
 * `updateHover`) are called by `TransformOperator` — the SkinWeightController
 * "brush owns the drag" idiom. The cage is drawn on a dedicated child node of
 * the entity's scene node (mesh-local geometry, follows the transform), never
 * attached to the entity node itself (`ObjectItemModel` casts every attached
 * object to `Entity*`).
 *
 * Headless parity: `latticeJson()`/`setLatticeJson()` expose the
 * `qtmesh-lattice-v1` document the CLI (`qtmesh lattice`) and MCP
 * (`lattice_*`) consume, and `deformEntityWithGrid` is the shared one-shot
 * bake used by both.
 */
class LatticeController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool sessionActive READ sessionActive NOTIFY sessionChanged)
    Q_PROPERTY(QString entityName READ entityName NOTIFY sessionChanged)
    Q_PROPERTY(int resolutionX READ resolutionX WRITE setResolutionX NOTIFY latticeChanged)
    Q_PROPERTY(int resolutionY READ resolutionY WRITE setResolutionY NOTIFY latticeChanged)
    Q_PROPERTY(int resolutionZ READ resolutionZ WRITE setResolutionZ NOTIFY latticeChanged)
    Q_PROPERTY(int interpolation READ interpolation WRITE setInterpolation NOTIFY latticeChanged)
    Q_PROPERTY(bool isDeformed READ isDeformed NOTIFY latticeChanged)
    Q_PROPERTY(int pointCount READ pointCount NOTIFY latticeChanged)
    Q_PROPERTY(int selectedPointCount READ selectedPointCount NOTIFY pointSelectionChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(bool statusIsError READ statusIsError NOTIFY statusChanged)

public:
    static LatticeController* instance();
    static LatticeController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();
    ~LatticeController() override;

    // --- state -------------------------------------------------------------
    /** Exactly one mesh entity selected and Edit Mode off — a lattice target. */
    bool hasSelection() const;
    bool sessionActive() const { return m_entity != nullptr; }
    QString entityName() const { return QString::fromStdString(m_entityName); }
    int resolutionX() const { return m_resX; }
    int resolutionY() const { return m_resY; }
    int resolutionZ() const { return m_resZ; }
    /** 0 = linear, 1 = smooth (Catmull-Rom), 2 = Bézier — `Lattice::Interpolation`. */
    int interpolation() const { return static_cast<int>(m_grid.interpolation); }
    bool isDeformed() const { return sessionActive() && !m_grid.isAtRest(); }
    int pointCount() const { return sessionActive() ? m_grid.pointCount() : 0; }
    int selectedPointCount() const { return static_cast<int>(m_selected.size()); }
    QString statusText() const { return m_status; }
    bool statusIsError() const { return m_statusIsError; }

    // --- session -----------------------------------------------------------
    /** Box the selected entity with a rest lattice. Returns false (with a
     *  status message) without a valid selection. */
    Q_INVOKABLE bool beginSession();
    /** Begin on a specific entity (MCP / tests). */
    bool beginSessionOn(Ogre::Entity* entity);
    /** Bake the current deformation into the mesh as one undo step and close. */
    Q_INVOKABLE bool applySession();
    /** Restore the rest mesh and close. */
    Q_INVOKABLE void cancelSession();

    // --- lattice edits (all undoable inside the session) ------------------
    /** Changing the resolution rebuilds a REST lattice (the mesh snaps back
     *  to its rest shape — the old control points have no meaning on the new
     *  grid, exactly as in Blender). Clamped to [2, 16]. */
    void setResolutionX(int n);
    void setResolutionY(int n);
    void setResolutionZ(int n);
    Q_INVOKABLE void setResolution(int nx, int ny, int nz);
    void setInterpolation(int mode);
    /** Snap every control point back to its rest position. */
    Q_INVOKABLE void resetPoints();
    Q_INVOKABLE void selectAllPoints();
    Q_INVOKABLE void clearPointSelection();
    /** Translate the selected control points by a mesh-local delta. */
    Q_INVOKABLE void moveSelectedPoints(double dx, double dy, double dz);
    /** Set one control point (flat index) to a mesh-local position. */
    Q_INVOKABLE bool setPoint(int index, double x, double y, double z);
    /** Set the point selection to the given flat indices. */
    Q_INVOKABLE void selectPoints(const QVariantList& indices);
    Q_INVOKABLE QVariantList selectedPoints() const;
    /** [x, y, z] of a control point (mesh-local), empty when out of range. */
    Q_INVOKABLE QVariantList pointPosition(int index) const;
    /** [x, y, z] rest position of a control point. */
    Q_INVOKABLE QVariantList pointRestPosition(int index) const;

    // --- persistence -------------------------------------------------------
    QJsonObject latticeJson() const;
    /** Replace the live lattice (resolution/box/points) — undoable. */
    bool setLatticeJson(const QJsonObject& obj, QString* error = nullptr);
    Q_INVOKABLE bool saveLatticeToFile(const QString& path);
    Q_INVOKABLE bool loadLatticeFromFile(const QString& path);
    /** Ask the host window for a save/open dialog (QML can't parent one). */
    Q_INVOKABLE void requestSaveDialog() { emit saveLatticeRequested(); }
    Q_INVOKABLE void requestLoadDialog() { emit loadLatticeRequested(); }

    const Lattice::Grid& grid() const { return m_grid; }

    /** One-shot headless bake: deform `entity`'s vertices with `grid` from
     *  their CURRENT positions and write them back (no session, no undo).
     *  Shared by the CLI and the MCP one-shot tool. */
    static bool deformEntityWithGrid(Ogre::Entity* entity, const Lattice::Grid& grid, QString* error = nullptr);

    // --- viewport hooks (TransformOperator) --------------------------------
    /** Flat index of the control point under the cursor, or -1. */
    int hitTestPoint(OgreWidget* widget, const QPoint& screenPos) const;
    /** Press: hit-test, update the point selection (`additive` = Shift) and
     *  arm a camera-plane drag. Returns true when a point was grabbed; a miss
     *  has no side effect (the caller decides between a click-to-deselect and
     *  a rubber-band box select). */
    bool beginDrag(OgreWidget* widget, const QPoint& screenPos, bool additive);
    /** Rubber-band: select every control point whose screen projection lies
     *  inside `screenRect` (viewport pixels). `additive` keeps the current
     *  selection; otherwise it is replaced. Returns the number of points hit. */
    int selectPointsInRect(OgreWidget* widget, const QRect& screenRect, bool additive);
    void updateDrag(OgreWidget* widget, const QPoint& screenPos);
    void endDrag();
    bool dragActive() const { return m_dragActive; }
    void updateHover(OgreWidget* widget, const QPoint& screenPos);

    /** Undo plumbing (LatticePointsCommand). No-op unless the session is on
     *  `entityName`. */
    void restorePointsFromUndo(const std::string& entityName, const std::vector<Ogre::Vector3>& points);

signals:
    void selectionChanged();
    void sessionChanged();
    void latticeChanged();
    void pointSelectionChanged();
    void statusChanged();
    void saveLatticeRequested();
    void loadLatticeRequested();

private:
    LatticeController();
    static LatticeController* m_pSingleton;

    Ogre::Entity* resolveEntity() const;
    Ogre::SceneNode* entityNode() const;
    void endSession();
    void setStatus(const QString& text, bool isError = false);

    void rebuildRestLattice();
    void captureRest();
    /** Recompute every vertex from `m_rest` through `m_grid` and schedule a GPU flush. */
    void applyDeform();
    void flushToEntity();
    void refreshOverlay();
    void destroyOverlay();
    void pushPointsUndo(const std::vector<Ogre::Vector3>& before, const QString& description);
    bool restoreRestToEntity();

    bool screenToRay(OgreWidget* widget, const QPoint& p, Ogre::Vector3& origin, Ogre::Vector3& dir,
                     Ogre::Camera** camOut = nullptr) const;
    bool worldToScreen(const Ogre::Camera* cam, OgreWidget* widget, const Ogre::Vector3& world,
                       float& sx, float& sy) const;

    // session
    Ogre::Entity* m_entity = nullptr;
    std::string m_entityName;
    std::unique_ptr<EditableMesh> m_mesh;
    std::vector<std::vector<Ogre::Vector3>> m_rest; ///< [submesh][vertex] rest positions
    Lattice::Grid m_grid;
    int m_resX = 3, m_resY = 3, m_resZ = 3;
    std::set<int> m_selected;
    int m_hover = -1;
    bool m_flushPending = false;

    // drag
    bool m_dragActive = false;
    Ogre::Vector3 m_dragAnchorWorld = Ogre::Vector3::ZERO;
    Ogre::Vector3 m_dragPlaneNormal = Ogre::Vector3::UNIT_Z;
    std::vector<Ogre::Vector3> m_dragStartPoints;
    bool m_dragMoved = false;

    // overlay
    Ogre::SceneNode* m_overlayNode = nullptr;
    Ogre::ManualObject* m_wireObj = nullptr;
    Ogre::ManualObject* m_pointObj = nullptr;

    QString m_status;
    bool m_statusIsError = false;
};

#endif // LATTICECONTROLLER_H
