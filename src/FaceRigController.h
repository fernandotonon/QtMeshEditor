#ifndef FACE_RIG_CONTROLLER_H
#define FACE_RIG_CONTROLLER_H

#include "FaceRig/FaceRigLandmarks.h"   // FaceMarker, NricpLandmark

#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantMap>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace Ogre { class SceneNode; }
namespace FaceRig { class ArkitTemplate; struct FaceRigGeometry; }

// QML-facing singleton for the face auto-rig (#889, Slice F #895). Wraps
// FaceRig::attachFaceRig* plus selection state so the Inspector's "Add ARKit
// Blendshapes" button can enable itself only on a mesh selection and run the
// (heavy) fit on a worker thread while the UI stays responsive.
//
// The pipeline is split across threads: geometry extraction + the pose attach
// touch Ogre and run on the MAIN thread; the heavy Ogre-free buildFaceRig()
// (NRICP + deformation transfer over 52 shapes) runs on a WORKER. The attach
// goes through AddMorphTargetCommand so it is undoable, and it lands the
// shapes in the same Ogre::Pose + VAT_POSE form the Vertex Morph section reads,
// so the #869 face-capture panel drives them immediately.
class FaceRigController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool hasMeshSelection READ hasMeshSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // True while the bundled ARKit template is downloading on first use.
    Q_PROPERTY(bool downloading READ downloading NOTIFY statusChanged)
    // Short human-readable status for the button label / tooltip.
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    // Worker progress for a progress bar: done / total steps (0 total = idle).
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(int progressTotal READ progressTotal NOTIFY progressChanged)
    // Face-marker editing session (auto-seed → user adjusts → rig).
    Q_PROPERTY(bool markerMode READ markerMode NOTIFY markerModeChanged)
    Q_PROPERTY(QStringList markerLabels READ markerLabels NOTIFY markersChanged)
    Q_PROPERTY(int selectedMarker READ selectedMarker NOTIFY markersChanged)
    Q_PROPERTY(bool markersSeededFromDetection READ markersSeededFromDetection
               NOTIFY markersChanged)

public:
    static FaceRigController* instance();
    static FaceRigController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool hasMeshSelection() const;
    bool busy() const { return m_busy; }
    bool downloading() const { return m_downloading; }
    QString status() const { return m_status; }
    int progress() const { return m_progress; }
    int progressTotal() const { return m_progressTotal; }

    /// Run the face auto-rig on the first selected entity and attach the ARKit
    /// blendshapes. Prepares (extracts geometry + loads the bundled template)
    /// on the main thread, runs the fit on a WORKER, commits the undoable
    /// attach back on the main thread. Returns false when it could not start
    /// (invalid selection / already busy — `error` is emitted); the real
    /// outcome arrives via completed(report) or error(msg).
    Q_INVOKABLE bool addArkitBlendshapesAsync(int maxShapes = 0,
                                              double maxResidualPct = 8.0,
                                              double amplitude = 1.0);

    /// Request cancellation of an in-flight fit (no-op otherwise). The worker
    /// stops at the next progress step; `error("cancelled")` follows.
    Q_INVOKABLE void cancel();

    // ---- Face-marker editing (auto-seed, user adjusts, then rig) --------
    bool markerMode() const { return m_markerMode; }
    QStringList markerLabels() const;
    int selectedMarker() const { return m_selMarker; }
    bool markersSeededFromDetection() const { return m_seededConfident; }

    /// Enter marker mode on the selected face mesh: loads the template, seeds
    /// the markers (auto-detect when it works, sensible defaults otherwise),
    /// and shows draggable overlays. Emits markersChanged / error. Runs the
    /// (main-thread) detection renders inline — quick for a head.
    Q_INVOKABLE bool beginFaceMarkers();
    /// Select a marker (index into markerLabels) to reposition on the next
    /// mesh click. -1 = none.
    Q_INVOKABLE void selectMarker(int index);
    /// Leave marker mode, discarding overlays (does NOT rig).
    Q_INVOKABLE void cancelFaceMarkers();
    /// Whether a marker is currently "placed" (has a position). QML dims the
    /// unplaced ones.
    Q_INVOKABLE bool markerPlaced(int index) const;

    /// Commit: build NRICP anchors from the (edited) markers and run the rig
    /// (worker thread), same flow as addArkitBlendshapesAsync but anchored to
    /// the user-corrected markers. Returns false if it couldn't start.
    Q_INVOKABLE bool rigFromMarkers(int maxShapes = 0, double maxResidualPct = 8.0,
                                    double amplitude = 1.0);

    /// Called by TransformOperator on a viewport left-click while markerMode is
    /// active: ray-casts to the mesh surface and moves the SELECTED marker
    /// there (or picks the nearest marker if none selected). Returns true if
    /// the click was consumed.
    bool handleMarkerClick(class OgreWidget* widget, const QPoint& screenPos);

signals:
    void selectionChanged();
    void busyChanged();
    void statusChanged();
    void progressChanged();
    void markerModeChanged();
    void markersChanged();
    void completed(const QVariantMap& report);
    void error(const QString& message);

private:
    FaceRigController();
    ~FaceRigController() override = default;

    void setStatus(const QString& s);

    // Shared async rig runner: extract geometry + template already loaded on the
    // main thread; `anchors` are the landmark constraints (auto or marker-based,
    // possibly empty). Runs the fit on a worker, attaches on the main thread.
    bool runRigAsync(const std::shared_ptr<class FaceRig::ArkitTemplate>& tmpl,
                     int maxShapes, double maxResidualPct, double amplitude,
                     const std::vector<FaceRig::NricpLandmark>& anchors);

    void clearMarkerOverlays();
    void refreshMarkerOverlays();

    static FaceRigController* m_pSingleton;
    bool m_busy = false;
    bool m_downloading = false;
    QString m_status;
    int m_progress = 0;
    int m_progressTotal = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;

    // Marker session.
    bool m_markerMode = false;
    int m_selMarker = -1;
    bool m_seededConfident = false;
    std::string m_markerEntityName;
    std::vector<FaceRig::FaceMarker> m_markers;
    std::vector<Ogre::SceneNode*> m_markerNodes;
    std::shared_ptr<class FaceRig::ArkitTemplate> m_markerTmpl;
    // Geometry extracted on the main thread, handed to the worker rig run.
    std::shared_ptr<FaceRig::FaceRigGeometry> m_geo;
};

#endif // FACE_RIG_CONTROLLER_H
