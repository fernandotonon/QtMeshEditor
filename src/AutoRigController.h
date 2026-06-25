#ifndef AUTO_RIG_CONTROLLER_H
#define AUTO_RIG_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QVariantMap>
#include <QPoint>
#include <vector>

#include "AutoRig.h"

class OgreWidget;
namespace Ogre { class Entity; class SceneNode; }

// QML-facing singleton for native auto-rigging (issue #407).
// Wraps `AutoRig::rigEntity` (+ optional `SkinWeights::computeAndApply`)
// and exposes selection state so the Animation-Mode button can disable
// itself when the selection isn't a riggable static mesh.
//
// It also drives the Mixamo-style MARKER placement flow: the user enters
// marker mode, clicks the 10 humanoid markers on the mesh in the viewport
// (routed in via TransformOperator), and commits — the markers anchor the
// matching joints and the limb chains interpolate between them.
class AutoRigController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // True when the selected entity is a STATIC (skeleton-less) mesh —
    // the only thing auto-rig can sensibly act on. Already-rigged meshes
    // and empty selections disable the button.
    Q_PROPERTY(bool hasRiggableSelection READ hasRiggableSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // Marker-placement session state (for the QML guided UX).
    Q_PROPERTY(bool markerMode READ markerMode NOTIFY markerModeChanged)
    Q_PROPERTY(int markerCount READ markerCount NOTIFY markerCountChanged)
    Q_PROPERTY(int markerTotal READ markerTotal NOTIFY markerModeChanged)
    Q_PROPERTY(int markerPlacedCount READ markerPlacedCount NOTIFY markerCountChanged)
    Q_PROPERTY(QString currentMarkerLabel READ currentMarkerLabel NOTIFY markerCountChanged)

public:
    static AutoRigController* instance();
    static AutoRigController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool hasRiggableSelection() const;
    bool busy() const { return m_busy; }

    /// Auto-rig the first resolved selected entity with `templateName`
    /// (humanoid / biped / quadruped / generic) using `algo`
    /// ('pinocchio' default, or 'rignet' — falls back to pinocchio when the
    /// RigNet model / ONNX runtime is unavailable). When `alsoSkin` is true,
    /// chains SkinWeights::computeAndApply so the mesh deforms immediately.
    /// Returns a QVariantMap mirroring AutoRig::Report (+ a `skinned` bool).
    /// Emits `rigged(report)` on success or `error(msg)` on failure.
    Q_INVOKABLE QVariantMap autoRigSelected(const QString& templateName,
                                            const QString& upAxis,
                                            bool alsoSkin,
                                            const QString& algo = QStringLiteral("pinocchio"));

    // ---- Marker placement (Mixamo-style) -------------------------------
    bool markerMode() const { return m_markerMode; }
    int markerCount() const;                     // slots resolved (placed+skipped)
    int markerTotal() const;                     // total expected (10 for humanoid)
    int markerPlacedCount() const;               // only the placed (set) markers
    QString currentMarkerLabel() const;          // label of the next marker to place

    /// Enter marker mode for the selected static mesh. Subsequent viewport
    /// clicks place the markers (chin, L/R shoulder, L/R wrist, L/R hip,
    /// L/R knee, hips/pelvis in order).
    Q_INVOKABLE bool beginMarkerPlacement(const QString& upAxis);
    /// Leave marker mode, discarding any placed markers + their overlays.
    Q_INVOKABLE void cancelMarkerPlacement();
    /// Skip the current marker (leaves that joint at the template fit).
    Q_INVOKABLE void skipCurrentMarker();
    /// Remove the last placed marker (undo one click).
    Q_INVOKABLE void undoLastMarker();
    /// Build the rig from the placed markers (+ optional skin). Returns a
    /// QVariantMap like autoRigSelected.
    Q_INVOKABLE QVariantMap commitMarkerRig(bool alsoSkin);

    /// Called by TransformOperator when a viewport click happens while marker
    /// mode is active. Ray-casts to the mesh surface and records the marker.
    /// Returns true if the click was consumed (so the operator skips select).
    bool handleMarkerClick(OgreWidget* widget, const QPoint& screenPos);

    /// Called by AutoRigCommand after a rig/unrig (incl. undo/redo). Drops any
    /// active skeleton-debug overlay on the entity (it would dangle once the
    /// skeleton state flips) and emits selectionChanged so the Inspector
    /// re-evaluates the Rigging / Skeleton sections.
    void notifyRiggingChanged(const std::string& entityName);

signals:
    void selectionChanged();
    void busyChanged();
    void rigged(const QVariantMap& report);
    void error(const QString& message);
    void markerModeChanged();
    void markerCountChanged();
    void markerPlaced(const QString& label);

private:
    AutoRigController();
    ~AutoRigController() override = default;

    Ogre::Entity* selectedRiggableEntity() const;
    void clearMarkerOverlays();
    void refreshMarkerOverlays();

    static AutoRigController* m_pSingleton;
    bool m_busy = false;

    // Marker session. Progress is a CURSOR into m_markerOrder: slots before the
    // cursor are resolved (either placed in m_markers, or skipped — absent from
    // m_markers). The cursor — not the contents of m_markers — drives which
    // marker is "current", so Skip advances past a slot without placing it and
    // the cursor never sticks. m_markers holds only the placed (set) markers.
    bool m_markerMode = false;
    int  m_upAxis = 1;                                  // resolved at begin
    int  m_markerCursor = 0;                            // next slot to resolve
    std::vector<AutoRig::MarkerId> m_markerOrder;       // the 10, in click order
    std::vector<AutoRig::Marker>   m_markers;           // PLACED markers (set)
    std::vector<Ogre::SceneNode*>  m_markerNodes;       // viewport sphere overlays
    std::string m_markerEntityName;                     // entity being marked
};

#endif // AUTO_RIG_CONTROLLER_H
