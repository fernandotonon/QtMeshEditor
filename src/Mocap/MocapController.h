#ifndef MOCAPCONTROLLER_H
#define MOCAPCONTROLLER_H

// Live performance-capture controller (epic #869, Slice F #875) — the
// QML_SINGLETON behind the Animation-Mode "Performance Capture" Inspector
// section: camera preview, live drive of the selected entity's morph weights
// + Head bone, and Record writing an ordinary undoable clip.
//
// Threading (the MeshGenController pattern): CameraFrameSource lives on the
// main thread (Qt Multimedia), frames land in its latest-wins FrameMailbox;
// a dedicated worker thread drains the mailbox through FaceCapPredictor and
// emits FaceSamples QUEUED back to the main thread, where ALL Ogre mutation
// happens. Never BlockingQueuedConnection (the MCP rule applies everywhere).
//
// Live drive is preview-only state: entering preview snapshots the mapped
// morph weights and the Head bone's {manuallyControlled, orientation} plus
// which AnimationStates were enabled (they are disabled during preview so
// they don't fight the manual bone); leaving preview restores everything
// exactly — the UvUnwrap GUI-safe-restore discipline.
//
// Recording buffers samples in memory; stopRecording() runs the take through
// MocapRecorder via ONE RecordMocapClipCommand (Ctrl+Z discards the take).
// Face-only live drive in this slice; body capture stays offline (CLI/MCP).

#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantList>

#ifdef ENABLE_MOCAP
#include "FaceCapMapper.h"
#include "FaceCapPredictor.h"
#include <vector>
#endif

#include <memory>

class QJSEngine;

class MocapController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString unavailableReason READ unavailableReason CONSTANT)
    Q_PROPERTY(int state READ state NOTIFY stateChanged)
    Q_PROPERTY(QVariantList availableDevices READ availableDevices
               NOTIFY devicesChanged)
    Q_PROPERTY(bool faceDetected READ faceDetected NOTIFY liveStatsChanged)
    Q_PROPERTY(double liveFps READ liveFps NOTIFY liveStatsChanged)
    Q_PROPERTY(double recordingSeconds READ recordingSeconds NOTIFY liveStatsChanged)
    Q_PROPERTY(int sampleCount READ sampleCount NOTIFY liveStatsChanged)
    Q_PROPERTY(QString previewDataUrl READ previewDataUrl NOTIFY previewChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)
    Q_PROPERTY(int matchedChannelCount READ matchedChannelCount
               NOTIFY mappingChanged)
    Q_PROPERTY(QStringList unmatchedChannels READ unmatchedChannels
               NOTIFY mappingChanged)
    Q_PROPERTY(bool headAvailable READ headAvailable NOTIFY mappingChanged)
    // body: only offerable when the selection is a humanoid rig
    Q_PROPERTY(bool bodyAvailable READ bodyAvailable NOTIFY mappingChanged)
    Q_PROPERTY(bool bodyDetected READ bodyDetected NOTIFY liveStatsChanged)
    // channel enables (writable from the QML Face/Head/Body checkboxes)
    Q_PROPERTY(bool faceEnabled READ faceEnabled WRITE setFaceEnabled
               NOTIFY channelsChanged)
    Q_PROPERTY(bool headEnabled READ headEnabled WRITE setHeadEnabled
               NOTIFY channelsChanged)
    Q_PROPERTY(bool bodyEnabled READ bodyEnabled WRITE setBodyEnabled
               NOTIFY channelsChanged)
    Q_PROPERTY(QString clipName READ clipName WRITE setClipName
               NOTIFY clipNameChanged)
    Q_PROPERTY(double smoothingCutoff READ smoothingCutoff
               WRITE setSmoothingCutoff NOTIFY smoothingChanged)
    Q_PROPERTY(bool showPoseDebug READ showPoseDebug WRITE setShowPoseDebug
               NOTIFY previewSettingsChanged)

public:
    enum State { Idle = 0, CameraStarting, Previewing, Recording };
    Q_ENUM(State)

    static MocapController* instance();
    static MocapController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool available() const;
    QString unavailableReason() const;
    int state() const;
    QVariantList availableDevices() const;   // [{id, description}]
    bool faceDetected() const;
    double liveFps() const;
    double recordingSeconds() const;
    int sampleCount() const;
    QString previewDataUrl() const;
    QString statusMessage() const;
    int matchedChannelCount() const;
    QStringList unmatchedChannels() const;
    bool headAvailable() const;
    bool bodyAvailable() const;
    bool bodyDetected() const;
    bool faceEnabled() const;
    void setFaceEnabled(bool on);
    bool headEnabled() const;
    void setHeadEnabled(bool on);
    bool bodyEnabled() const;
    void setBodyEnabled(bool on);
    QString clipName() const;
    void setClipName(const QString& name);
    double smoothingCutoff() const;
    void setSmoothingCutoff(double hz);
    bool showPoseDebug() const;
    void setShowPoseDebug(bool on);

    Q_INVOKABLE void refreshDevices();
    // Start the camera + live drive of the SELECTED entity. Empty deviceId =
    // default camera.
    Q_INVOKABLE bool startPreview(const QString& deviceId = {});
    // Start live drive from a VIDEO FILE instead of the camera — the path for
    // macOS where camera access is blocked/unavailable. No permission prompt;
    // playback-driven at real time (frames dropped latest-wins, same as the
    // webcam preview — recording re-runs the full clip). filePath is a local
    // file path (not a URL).
    Q_INVOKABLE bool startPreviewFromVideo(const QString& filePath);
    // Native open-file dialog filtered to video files; returns the chosen
    // local path or "" if cancelled. Convenience for the GUI "Load Video…"
    // button (the codebase's C++-side dialog convention).
    Q_INVOKABLE QString openVideoDialog();
    Q_INVOKABLE void stopPreview();
    Q_INVOKABLE bool startRecording();
    Q_INVOKABLE void stopRecording();
    // Re-base the head-pose neutral on the current sample ("hold a neutral
    // face"). Auto-applied on the first confident sample of a preview.
    Q_INVOKABLE void calibrateNeutral();

#ifdef ENABLE_MOCAP
    // Test seam: enter Previewing without a camera/worker (no camera in CI).
    // Ownership stays with the caller; feed samples through onSample().
    bool startPreviewWithSource(class VideoFrameSource* source);
    // Receives one predicted sample on the MAIN thread (queued from the
    // inference worker in live mode; called directly by tests). The
    // face-only overload forwards with no body frame.
    void onSample(const FaceSample& sample, const QImage& preview);
    void onSample(const FaceSample& sample, const struct BodyLiveFrame& body,
                  const QImage& preview);
#endif

signals:
    void stateChanged();
    void devicesChanged();
    void liveStatsChanged();
    void previewChanged();
    void statusChanged();
    void mappingChanged();
    void channelsChanged();
    void clipNameChanged();
    void smoothingChanged();
    void previewSettingsChanged();
    void errorOccurred(const QString& message);

private:
    explicit MocapController(QObject* parent = nullptr);
    ~MocapController() override;

#ifdef ENABLE_MOCAP
    // startPreview() gates on camera permission (async on first run), then
    // hands off here once granted.
    bool beginPreview(const QString& deviceId);
    // Shared live-preview runner: drivability check, model download, worker
    // thread + connections, all driven by `source` (a webcam or a video file).
    // Takes ownership of `source`. `startingMessage` is the status shown while
    // the source spins up. Returns false (and restores state) on any failure.
    bool beginPreviewWithLiveSource(std::unique_ptr<class VideoFrameSource> source,
                                    const QString& startingMessage);
    void resetLiveCaptureCalibration();
    // Inspect the currently-selected entity and (re)build the face mapping,
    // head bone, and body-rig role set — so matchedChannelCount / headAvailable
    // / bodyAvailable reflect the selection BEFORE a preview runs (the channel
    // checkboxes gate on them). Called on selectionChanged and at construction;
    // beginPreviewWithLiveSource also calls it. No-op / clears when nothing
    // suitable is selected. Never touches live-session state.
    void refreshMappingForSelection();
    void restoreEntityState();
    void setStatusMessage(const QString& message);
#endif

    struct Impl;
    std::unique_ptr<Impl> d;
};

#endif  // MOCAPCONTROLLER_H
