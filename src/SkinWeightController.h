#ifndef SKIN_WEIGHT_CONTROLLER_H
#define SKIN_WEIGHT_CONTROLLER_H

#include "SkinEvaluate.h"
#include "WeightPaintOps.h"

#include <QObject>
#include <QQmlEngine>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <OgreVertexBoneAssignment.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

class AnimationWidget;
class BoneWeightOverlay;

namespace Ogre {
class Bone;
class Entity;
class Mesh;
}
class OgreWidget;

/**
 * @brief Skel Slice D (#558) — interactive skin-weight painting.
 *
 * Owns a weight-paint SESSION: the per-vertex weights are extracted once
 * (SkinEvaluate::extract), edited in memory by WeightPaintOps as the user
 * strokes, and written back to the mesh's VertexBoneAssignment lists on a
 * debounce. `BoneWeightOverlay` renders the result.
 *
 * The write path is deliberately the in-place one — `clearBoneAssignments` +
 * `addBoneAssignment`* + `_compileBoneAssignments()` per owner, with NO
 * `Entity::_initialise`. That re-packs BLEND_INDICES/WEIGHTS into the SAME
 * vertex buffer the live SkeletonInstance already references, which is what
 * makes per-stroke editing viable; swapping VertexData out from under a live
 * skeleton is what shatters the on-screen mesh (see the UV-unwrap notes).
 *
 * `_compileBoneAssignments` is O(assignments) for the whole owner, not O(dab),
 * so it is called once per debounce tick — never per dab.
 *
 * The brush SETTINGS (radius / strength / falloff / shape) are read from
 * EditModeController, the canonical owner, so the weight brush and the
 * vertex-colour brush share one set of controls.
 */
class SkinWeightController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool  weightPaintEnabled READ weightPaintEnabled WRITE setWeightPaintEnabled NOTIFY weightPaintChanged)
    Q_PROPERTY(int   brushMode          READ brushMode          WRITE setBrushMode          NOTIFY weightPaintChanged) // 0 add / 1 subtract / 2 blur
    Q_PROPERTY(int   maxInfluences      READ maxInfluences      WRITE setMaxInfluences      NOTIFY weightPaintChanged)
    Q_PROPERTY(QString activeBoneName   READ activeBoneName                                 NOTIFY weightPaintChanged)
    /// Active bone's weight under the cursor, -1 when the cursor is off-mesh.
    Q_PROPERTY(double hoverWeight       READ hoverWeight                                    NOTIFY hoverChanged)
    Q_PROPERTY(int    hoverVertex       READ hoverVertex                                    NOTIFY hoverChanged)
    Q_PROPERTY(QString status           READ status                                         NOTIFY weightPaintChanged)

public:
    static SkinWeightController* instance();
    static SkinWeightController* qmlInstance(QQmlEngine*, QJSEngine*);
    static void kill();

    bool    weightPaintEnabled() const { return m_enabled; }
    void    setWeightPaintEnabled(bool on);
    int     brushMode() const { return m_brushMode; }
    void    setBrushMode(int mode);
    int     maxInfluences() const { return m_maxInfluences; }
    void    setMaxInfluences(int n);
    QString activeBoneName() const;
    double  hoverWeight() const { return m_hoverWeight; }
    int     hoverVertex() const { return m_hoverVertex; }
    QString status() const { return m_status; }

    // --- stroke API (driven from TransformOperator's mouse pipeline) -------
    bool beginStroke(OgreWidget* widget, const QPoint& screenPos);
    void updateStroke(OgreWidget* widget, const QPoint& screenPos);
    void endStroke();
    bool strokeActive() const { return m_strokeActive; }
    /// Update the hover readout without painting (mouse-move with no button).
    void updateHover(OgreWidget* widget, const QPoint& screenPos);

    // --- utility ops (whole mesh; each is one undo step) ------------------
    Q_INVOKABLE bool normalizeAll();
    Q_INVOKABLE bool smoothAll(int iterations = 2);
    Q_INVOKABLE bool limitInfluencesAll(int maxInfluences = 4);
    /// Axis-position mirror (0=X, 1=Y, 2=Z). Slice E (#559) owns name-pair
    /// mirroring; see WeightPaintOps::mirrorByPosition for what this does and
    /// does not guarantee.
    Q_INVOKABLE bool mirrorAll(int axis = 0, double tolerance = 0.001);
    /// Flood-fill the connected island under the last hovered vertex.
    Q_INVOKABLE bool fillConnectedAtHover(int maxHops = 0);

    // --- bone locking -----------------------------------------------------
    Q_INVOKABLE bool isBoneLocked(const QString& boneName) const;
    Q_INVOKABLE void setBoneLocked(const QString& boneName, bool locked);
    Q_INVOKABLE QStringList lockedBoneNames() const;

    // --- per-vertex inspector --------------------------------------------
    /// "boneName=weight" for every influence on `vertexIndex`, weight-descending.
    Q_INVOKABLE QStringList vertexWeights(int vertexIndex) const;
    /// Set one bone's weight on one vertex numerically. Undoable.
    Q_INVOKABLE bool setVertexWeight(int vertexIndex, const QString& boneName,
                                     double weight);

    /// Bone-assignment snapshot of one owner (submeshIndex -1 == the mesh-level
    /// shared list), for undo. Mirrors ComputeSkinWeightsCommand::OwnerSnapshot.
    struct OwnerSnapshot {
        int submeshIndex = 0;
        std::multimap<size_t, Ogre::VertexBoneAssignment> assignments;
    };
    /// Capture / restore every owner. Public so the undo command can use them.
    static std::vector<OwnerSnapshot> captureSnapshot(Ogre::Mesh* mesh);
    static void restoreSnapshot(Ogre::Mesh* mesh,
                                const std::vector<OwnerSnapshot>& snapshot);
    /// Re-read weights from the mesh after an undo/redo replaced them.
    void resyncFromMesh();

    /// Test hook: the positions picking/brushing actually run against. Exposed
    /// so a test can assert that they follow the POSED surface (see
    /// pickPositions) without needing a viewport and a screen ray.
    const std::vector<float>& pickPositionsForTest() const { return pickPositions(); }

signals:
    void weightPaintChanged();
    void hoverChanged();

private:
    explicit SkinWeightController(QObject* parent = nullptr);
    ~SkinWeightController() override;

    /// Positions to pick/brush against: the SOFTWARE-SKINNED vertices when a
    /// pose is active, else the bind pose. Same owner order as
    /// SkinEvaluate::extract, so indices match m_data.weights.
    const std::vector<float>& pickPositions() const;

    /// Screen -> mesh-LOCAL hit against THIS controller's own session
    /// geometry (m_data.positions/indices).
    ///
    /// Deliberately NOT TexturePaintController::hitTestLocalPoint: that one
    /// early-returns unless a TEXTURE paint session exists (`m_paintMesh`),
    /// which weight painting never creates — so it always returned false and
    /// every dab was silently skipped. Using our own extracted geometry also
    /// keeps the hit test in the exact index space the weights use.
    bool hitTestLocalPoint(OgreWidget* widget, const QPoint& screenPos,
                           double outLocal[3]) const;
    /// Ensure a session exists for the selected skinned entity.
    bool ensureSession();
    void closeSession();
    /// Active bone HANDLE, or -1.
    int activeBoneHandle() const;
    /// Bone that absorbs weight when `forBoneHandle` is a vertex's ONLY
    /// influence (normally that bone's PARENT). Pass -1 to use the active bone.
    /// Returns -1 when no recipient exists.
    int fallbackBoneHandle(int forBoneHandle = -1) const;
    /// Per-bone-handle lock flags sized to the skeleton.
    std::vector<std::uint8_t> lockedBoneFlags() const;
    /// Adjacency over the session mesh (built lazily; Blur/smooth need it).
    const std::vector<std::vector<int>>& adjacency();
    /// Write the in-memory weights back to the mesh, in place.
    void flushToMesh();
    /// Coalesce flushes onto one event-loop tick — `_compileBoneAssignments`
    /// re-packs the whole owner, so per-dab flushing would crawl on a real mesh.
    void scheduleFlush();
    /// Push one undo command spanning `before` -> current mesh state.
    void pushUndo(const QString& label, std::vector<OwnerSnapshot> before);
    /// Run `op` on the session weights as a single undoable step.
    bool runUndoableOp(const QString& label,
                       const std::function<int()>& op);
    void refreshOverlay();
    /// Heat-map overlay for the painted entity, or nullptr when absent.
    BoneWeightOverlay* findOverlay() const;
    /// The AnimationWidget that owns the weight overlays, or nullptr.
    AnimationWidget* findAnimationWidget() const;

    bool m_enabled = false;
    int  m_brushMode = 0;
    int  m_maxInfluences = 4;
    QString m_status;

    Ogre::Entity* m_entity = nullptr;
    SkinEvaluate::EvalData m_data;      ///< the live paint buffer
    bool m_haveData = false;
    std::vector<std::vector<int>> m_adjacency;
    bool m_haveAdjacency = false;
    std::vector<QString> m_lockedBones;  ///< by NAME: survives skeleton rebinds
    /// Scratch buffer for pickPositions(); mutable so picking stays const.
    mutable std::vector<float> m_pickPositions;

    bool m_strokeActive = false;
    bool m_strokeDirty = false;
    std::vector<OwnerSnapshot> m_strokeBefore;
    bool m_flushScheduled = false;

    double m_hoverWeight = -1.0;
    int    m_hoverVertex = -1;
};

#endif // SKIN_WEIGHT_CONTROLLER_H
