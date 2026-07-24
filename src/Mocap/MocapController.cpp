#include "MocapController.h"

#ifdef ENABLE_MOCAP
#include "MocapRecorder.h"
#include "OneEuroFilter.h"
#include "PoseCapPredictor.h"
#include "PoseIKSolver.h"
#include "VideoFrameSource.h"
#include "MocapCameraHints.h"
#include "../AnimationMerger.h"
#include "../MotionInbetween.h"
#include "../Manager.h"
#include "../MorphAnimationManager.h"
#include "../SelectionSet.h"
#include "../SentryReporter.h"
#include "../UndoManager.h"
#include "../GamificationManager.h"
#include "../commands/RecordMocapClipCommand.h"

#include <OgreAnimationState.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSkeletonInstance.h>

#include <QBuffer>
#include <QElapsedTimer>
#include <QThread>
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>
#include <QPermissions>
#include <QWidget>

namespace {
MocapController* s_instance = nullptr;
}

MocapController* MocapController::instance()
{
    if (!s_instance)
        s_instance = new MocapController();
    return s_instance;
}

MocapController* MocapController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    MocapController* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void MocapController::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

#ifndef ENABLE_MOCAP

// -----------------------------------------------------------------------------
// Non-mocap build: the section shows a disabled label with the reason.
// -----------------------------------------------------------------------------

struct MocapController::Impl {};

MocapController::MocapController(QObject* parent) : QObject(parent) {}
MocapController::~MocapController() = default;
bool MocapController::available() const { return false; }
QString MocapController::unavailableReason() const
{
    return tr("This build has no performance-capture support — rebuild with "
              "-DENABLE_MOCAP=ON -DENABLE_ONNX=ON.");
}
int MocapController::state() const { return Idle; }
QVariantList MocapController::availableDevices() const { return {}; }
bool MocapController::faceDetected() const { return false; }
double MocapController::liveFps() const { return 0; }
double MocapController::recordingSeconds() const { return 0; }
int MocapController::sampleCount() const { return 0; }
QString MocapController::previewDataUrl() const { return {}; }
QString MocapController::statusMessage() const { return {}; }
int MocapController::matchedChannelCount() const { return 0; }
QStringList MocapController::unmatchedChannels() const { return {}; }
bool MocapController::headAvailable() const { return false; }
bool MocapController::bodyAvailable() const { return false; }
bool MocapController::bodyDetected() const { return false; }
bool MocapController::faceEnabled() const { return false; }
void MocapController::setFaceEnabled(bool) {}
bool MocapController::headEnabled() const { return false; }
void MocapController::setHeadEnabled(bool) {}
bool MocapController::bodyEnabled() const { return false; }
void MocapController::setBodyEnabled(bool) {}
QString MocapController::clipName() const { return QStringLiteral("FaceCap"); }
void MocapController::setClipName(const QString&) {}
double MocapController::smoothingCutoff() const { return 1.0; }
void MocapController::setSmoothingCutoff(double) {}
void MocapController::refreshDevices() {}
bool MocapController::startPreview(const QString&) { return false; }
bool MocapController::startPreviewFromVideo(const QString&) { return false; }
QString MocapController::openVideoDialog() { return {}; }
void MocapController::stopPreview() {}
bool MocapController::startRecording() { return false; }
void MocapController::stopRecording() {}
void MocapController::calibrateNeutral() {}

#else  // ENABLE_MOCAP

// -----------------------------------------------------------------------------
// Worker: drains the camera mailbox through the predictor off the main thread.
// -----------------------------------------------------------------------------

// One canonical-role world-quat frame from the pose solver, marshalled to the
// main thread alongside the face sample (avoids a second queued signal type).
struct BodyLiveFrame {
    bool valid = false;
    std::array<std::array<float, 4>, PoseIK::kCanonicalRoles> quats;
    uint32_t resolvedMask = 0;
};
Q_DECLARE_METATYPE(BodyLiveFrame)

// queued sampleReady(FaceSample, BodyLiveFrame, QImage) across the worker
// thread boundary needs both payload types registered.
namespace {
struct MocapMetaTypeRegistrar {
    MocapMetaTypeRegistrar()
    {
        qRegisterMetaType<FaceSample>("FaceSample");
        qRegisterMetaType<BodyLiveFrame>("BodyLiveFrame");
    }
};
const MocapMetaTypeRegistrar mocapMetaTypeRegistrar;
}  // namespace

class MocapInferenceWorker : public QObject
{
    Q_OBJECT
public:
    FrameMailbox* mailbox = nullptr;
    FaceCapPredictor predictor;
    std::array<OneEuroFilter, 52> weightFilters;
    OneEuroQuatFilter headFilter;
    bool smooth = true;

    // body (optional; created only when the Body channel is enabled)
    bool bodyEnabled = false;
    std::shared_ptr<PoseCapPredictor> posePredictor;
    PoseIK::Solver poseSolver;
    std::array<OneEuroQuatFilter, PoseIK::kCanonicalRoles> roleFilters;

public slots:
    void processPending()
    {
        if (!mailbox)
            return;
        MocapFrame frame;
        while (mailbox->take(&frame)) {
            FaceSample s = predictor.predict(frame.image, frame.timeSec);
            if (smooth && s.confidence > 0.f) {
                for (int c = 0; c < 52; ++c)
                    s.weights[c] = static_cast<float>(
                        weightFilters[c].filter(s.weights[c], s.timeSec));
                s.headRotation = headFilter.filter(s.headRotation, s.timeSec);
            }

            BodyLiveFrame body;
            if (bodyEnabled && posePredictor) {
                const PoseSample ps =
                    posePredictor->predict(frame.image, frame.timeSec);
                if (ps.confidence > 0.f) {
                    PoseIK::FrameResult fr = poseSolver.solveFrame(
                        ps.world.data(), ps.visibility.data());
                    if (smooth)
                        for (int r = 0; r < PoseIK::kCanonicalRoles; ++r)
                            fr.quats[r] =
                                roleFilters[r].filter(fr.quats[r], frame.timeSec);
                    body.valid = true;
                    body.quats = fr.quats;
                    body.resolvedMask = fr.resolvedMask;
                }
            }

            // downscale sparsely for the HUD preview (payload stays small)
            QImage preview;
            if (frame.frameIndex % 3 == 0)
                preview = frame.image.scaledToWidth(240, Qt::FastTransformation);
            emit sampleReady(s, body, preview);
        }
    }

signals:
    void sampleReady(const FaceSample& sample, const BodyLiveFrame& body,
                     const QImage& preview);
};

struct MocapController::Impl {
    State state = Idle;
    QString status;
    QString clipName = QStringLiteral("FaceCap");
    double smoothingCutoff = 1.0;

    // The live source feeding the worker: a CameraFrameSource (webcam) OR a
    // FileFrameSource (video-file preview, the macOS-camera-blocked path).
    // Both feed the same mailbox() via the base class.
    std::unique_ptr<VideoFrameSource> camera;
    VideoFrameSource* injectedSource = nullptr;  // test seam, not owned
    QThread workerThread;
    MocapInferenceWorker* worker = nullptr;  // owned by workerThread

    // capture-session state (all main-thread)
    std::string entityName;
    FaceCapMapper::Mapping mapping;
    QString headBone;
    bool calibrated = false;
    Ogre::Quaternion neutral = Ogre::Quaternion::IDENTITY;
    Ogre::Quaternion headBindWorld = Ogre::Quaternion::IDENTITY;
    Ogre::Quaternion headBindLocal = Ogre::Quaternion::IDENTITY;
    bool headWasManuallyControlled = false;
    QHash<QString, float> savedWeights;          // mesh target -> weight
    QStringList savedEnabledAnimations;

    // channel enables (persist across sessions; body gated on a humanoid rig)
    bool faceEnabled = true;
    bool headEnabled = true;
    bool bodyEnabled = false;
    bool bodyRigOk = false;   // selection resolved >= half the canonical roles

    // body live-drive state: one canonical role -> the rig bone it drives, plus
    // that bone's bind orientations (for the same world-delta math recordBody
    // uses) and its pre-preview manual-control flag (restore contract).
    struct BodyBone {
        int role = -1;
        std::string boneName;
        Ogre::Quaternion bindLocal = Ogre::Quaternion::IDENTITY;
        Ogre::Quaternion bindWorld = Ogre::Quaternion::IDENTITY;
        // The PARENT bone's bind-pose world orientation. A world-space rotation
        // delta must be transported through the PARENT's frame (not the bone's
        // own) to become a valid parent-relative local, matching the offline
        // applyMotionClip retarget. Using the bone's own bindWorld is only
        // correct when the local bind is identity (~head); it mis-rotates
        // bones with a non-trivial local bind (arms/shoulders).
        Ogre::Quaternion parentBindWorld = Ogre::Quaternion::IDENTITY;
        bool wasManuallyControlled = false;
    };
    std::vector<BodyBone> bodyBones;
    bool bodyCalibrated = false;
    std::array<Ogre::Quaternion, PoseIK::kCanonicalRoles> bodyNeutral;
    // Shared per-frame retargeter (same math as the recorded clip); built once
    // at preview start for the driven entity.
    std::shared_ptr<BodyRetargeter> bodyRetargeter;
    bool bodyDetected = false;

    // recording
    bool recordPending = false;
    std::vector<FaceSample> take;
    std::vector<BodyLiveFrame> bodyTake;   // parallel to `take`, when body on
    double takeStart = 0.0;
    double lastSampleTime = 0.0;

    // HUD
    bool faceDetected = false;
    double liveFps = 0.0;
    double lastArrival = -1.0;
    QElapsedTimer clock;
    QString previewDataUrl;
    int sampleCount = 0;

    Ogre::Entity* entity() const
    {
        auto* mgr = Manager::getSingletonPtr();
        if (!mgr || !mgr->getSceneMgr() || entityName.empty())
            return nullptr;
        auto* scene = mgr->getSceneMgr();
        return scene->hasEntity(entityName) ? scene->getEntity(entityName)
                                            : nullptr;
    }
};

MocapController::MocapController(QObject* parent)
    : QObject(parent), d(new Impl)
{
    // Keep the channel availability (Face/Head/Body checkboxes) in sync with
    // whatever is selected, WITHOUT running a preview — otherwise the boxes
    // stay disabled until the first Preview builds the mapping, and never
    // update when the user picks a different mesh.
    if (auto* sel = SelectionSet::getSingleton())
        connect(sel, &SelectionSet::selectionChanged, this,
                [this]() { refreshMappingForSelection(); });
    refreshMappingForSelection();
}

MocapController::~MocapController()
{
    // stopPreview() touches Ogre (snapshot/restore of bone + morph state) and
    // could throw; an exception escaping a destructor calls std::terminate.
    // Swallow it — we're tearing down anyway.
    try {
        stopPreview();
    } catch (...) {
    }
}

bool MocapController::available() const { return true; }
QString MocapController::unavailableReason() const { return {}; }
int MocapController::state() const { return d->state; }

QVariantList MocapController::availableDevices() const
{
    QVariantList out;
    for (const auto& dev : CameraFrameSource::availableDevices()) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), dev.id);
        m.insert(QStringLiteral("description"), dev.description);
        out.append(m);
    }
    return out;
}

bool MocapController::faceDetected() const { return d->faceDetected; }
double MocapController::liveFps() const { return d->liveFps; }
double MocapController::recordingSeconds() const
{
    return d->state == Recording ? d->lastSampleTime - d->takeStart : 0.0;
}
int MocapController::sampleCount() const { return d->sampleCount; }
QString MocapController::previewDataUrl() const { return d->previewDataUrl; }
QString MocapController::statusMessage() const { return d->status; }
int MocapController::matchedChannelCount() const
{
    return d->mapping.channels.size();
}
QStringList MocapController::unmatchedChannels() const
{
    return d->mapping.unmatchedCanonical;
}
bool MocapController::headAvailable() const { return !d->headBone.isEmpty(); }
bool MocapController::bodyAvailable() const { return d->bodyRigOk; }
bool MocapController::bodyDetected() const { return d->bodyDetected; }
bool MocapController::faceEnabled() const { return d->faceEnabled; }
void MocapController::setFaceEnabled(bool on)
{
    if (on == d->faceEnabled) return;
    d->faceEnabled = on;
    emit channelsChanged();
}
bool MocapController::headEnabled() const { return d->headEnabled; }
void MocapController::setHeadEnabled(bool on)
{
    if (on == d->headEnabled) return;
    d->headEnabled = on;
    emit channelsChanged();
}
bool MocapController::bodyEnabled() const { return d->bodyEnabled; }
void MocapController::setBodyEnabled(bool on)
{
    if (on == d->bodyEnabled) return;
    d->bodyEnabled = on;
    emit channelsChanged();
}
QString MocapController::clipName() const { return d->clipName; }
void MocapController::setClipName(const QString& name)
{
    if (name == d->clipName || name.isEmpty())
        return;
    d->clipName = name;
    emit clipNameChanged();
}
double MocapController::smoothingCutoff() const { return d->smoothingCutoff; }
void MocapController::setSmoothingCutoff(double hz)
{
    if (hz <= 0 || hz == d->smoothingCutoff)
        return;
    d->smoothingCutoff = hz;
    emit smoothingChanged();
}

void MocapController::refreshDevices() { emit devicesChanged(); }

void MocapController::setStatusMessage(const QString& message)
{
    d->status = message;
    emit statusChanged();
}

bool MocapController::startPreview(const QString& deviceId)
{
    if (d->state != Idle)
        return false;

    // A drive target must exist before we bother the user with a camera
    // permission prompt.
    auto* sel = SelectionSet::getSingleton();
    bool haveEntity = false;
    if (sel && !sel->getResolvedEntities().isEmpty())
        haveEntity = sel->getResolvedEntities().first() != nullptr;
    if (!haveEntity) {
        setStatusMessage(tr("Select an entity to drive first."));
        emit errorOccurred(d->status);
        return false;
    }

    // Camera permission (Qt 6.6+). QCamera::start() delivers NO frames on
    // macOS until the user has granted access — without this request the
    // preview would sit at "Starting camera…" forever (there is no error, the
    // sink is simply silent). requestPermission is async; we continue in
    // beginPreview() once granted. checkPermission() short-circuits the prompt
    // on repeat runs.
    QCameraPermission camPerm;
    if (qApp->checkPermission(camPerm) == Qt::PermissionStatus::Granted)
        return beginPreview(deviceId);

    // Not granted yet — ALWAYS go through requestPermission(). It is the only
    // call that shows the OS dialog (checkPermission never prompts); on the
    // first run macOS presents it, and if the user previously denied it the
    // request returns Denied without a dialog, which we turn into the
    // open-Settings hint. We do NOT short-circuit on a pre-checked Denied,
    // so clicking Preview always makes a real attempt at the prompt.
    setStatusMessage(tr("Requesting camera access…"));
    qApp->requestPermission(camPerm, this,
        [this, deviceId](const QPermission& result) {
            if (result.status() == Qt::PermissionStatus::Granted) {
                beginPreview(deviceId);
            } else {
                setStatusMessage(MocapCameraHints::permissionDeniedMessage());
                emit errorOccurred(d->status);
            }
        });
    return true;  // async; the callback finishes the job
}

void MocapController::refreshMappingForSelection()
{
    // Never disturb a running session — the mapping is fixed once preview starts.
    if (d->state != Idle)
        return;

    auto* sel = SelectionSet::getSingleton();
    Ogre::Entity* entity = nullptr;
    if (sel) {
        const auto ents = sel->getResolvedEntities();
        if (!ents.isEmpty())
            entity = ents.first();
    }
    if (!entity) {
        // Nothing selected: clear so the channel checkboxes disable.
        d->entityName.clear();
        d->mapping = FaceCapMapper::Mapping{};
        d->headBone.clear();
        d->bodyBones.clear();
        d->bodyRigOk = false;
        emit mappingChanged();
        return;
    }

    d->entityName = entity->getName();
    d->mapping = FaceCapMapper::build(
        MorphAnimationManager::instance()->morphTargetsFor(entity));
    d->headBone = MocapRecorder::resolveHeadBone(entity);

    // body rig mapping: canonical role -> the rig bone that plays it (the same
    // MotionInbetween::canonicalIndexForBone the retarget uses). Requires a
    // skinned mesh resolving >= half of the 22 roles (the humanoid gate).
    d->bodyBones.clear();
    d->bodyRigOk = false;
    if (entity->hasSkeleton()) {
        Ogre::SkeletonInstance* skel = entity->getSkeleton();
        std::array<bool, PoseIK::kCanonicalRoles> roleSeen{};
        for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
            Ogre::Bone* bone = skel->getBone(i);
            const int role = MotionInbetween::canonicalIndexForBone(
                QString::fromStdString(bone->getName()));
            if (role < 0 || role >= PoseIK::kCanonicalRoles || roleSeen[role])
                continue;
            roleSeen[role] = true;
            Impl::BodyBone bb;
            bb.role = role;
            bb.boneName = bone->getName();
            d->bodyBones.push_back(bb);
        }
        int resolved = 0;
        for (bool s : roleSeen) resolved += s ? 1 : 0;
        d->bodyRigOk = resolved * 2 >= PoseIK::kCanonicalRoles;  // >= half
        if (!d->bodyRigOk)
            d->bodyBones.clear();
    }
    if (!d->bodyRigOk)
        d->bodyEnabled = false;  // can't drive body on a non-humanoid rig

    emit mappingChanged();
}

bool MocapController::beginPreview(const QString& deviceId)
{
    // Webcam path: build the camera source and run the shared preview.
    return beginPreviewWithLiveSource(
        std::make_unique<CameraFrameSource>(deviceId),
        tr("Starting camera…"));
}

bool MocapController::startPreviewFromVideo(const QString& filePath)
{
    if (d->state != Idle)
        return false;
    if (filePath.isEmpty()) {
        setStatusMessage(tr("No video file selected."));
        emit errorOccurred(d->status);
        return false;
    }
    // Video-file path: no camera permission needed (the macOS-blocked-camera
    // fallback). FileFrameSource plays at real time and drops frames
    // latest-wins into the mailbox, same as the webcam preview.
    return beginPreviewWithLiveSource(
        std::make_unique<FileFrameSource>(filePath),
        tr("Playing video…"));
}

QString MocapController::openVideoDialog()
{
    // Non-native dialog (matches the codebase convention — avoids the macOS
    // native panel stealing focus from the Ogre window).
    if (QWidget* activeWin = QApplication::activeWindow()) {
        activeWin->raise();
        activeWin->activateWindow();
    }
    QApplication::processEvents();
    const QString file = QFileDialog::getOpenFileName(
        QApplication::activeWindow(),
        tr("Select a video file"),
        QDir::currentPath(),
        tr("Video files (*.mp4 *.mov *.m4v *.avi *.mkv *.webm);;All files (*)"),
        nullptr,
        QFileDialog::DontUseNativeDialog);
    return file;
}

bool MocapController::beginPreviewWithLiveSource(
    std::unique_ptr<VideoFrameSource> source, const QString& startingMessage)
{
    if (d->state != Idle)
        return false;

    auto* sel = SelectionSet::getSingleton();
    Ogre::Entity* entity = nullptr;
    if (sel) {
        const auto ents = sel->getResolvedEntities();
        if (!ents.isEmpty())
            entity = ents.first();
    }
    if (!entity) {
        setStatusMessage(tr("Select an entity to drive first."));
        emit errorOccurred(d->status);
        return false;
    }
    d->camera = std::move(source);

    // Determine what the selection can be driven with BEFORE downloading any
    // models — a ~30 MB face-model fetch is wasted if the mesh has no ARKit
    // morph targets and no head bone (field-reported: clicking Preview on a
    // plain mesh triggered the download, then failed the drivability check).
    // (Also keeps the channel checkboxes honest — see refreshMappingForSelection.)
    refreshMappingForSelection();

    const bool faceDrivable = d->faceEnabled && !d->mapping.channels.isEmpty();
    const bool headDrivable = d->headEnabled && !d->headBone.isEmpty();
    const bool bodyDrivable = d->bodyEnabled && d->bodyRigOk;
    if (!faceDrivable && !headDrivable && !bodyDrivable) {
        setStatusMessage(
                  tr("Nothing to drive: enable a channel the selection "
                     "supports (morph targets for Face, a Head bone, or a "
                     "humanoid rig for Body)."));
        emit errorOccurred(d->status);
        return false;
    }

    // NOW that we know there is something to drive, fetch the models (blocking
    // download with a visible status). Face models feed both face + head drive
    // (the head rotation comes from the face graph); the pose models are
    // fetched lazily in the worker only when body drive is active.
    if (!FaceCapPredictor::modelsPresent()) {
        setStatusMessage(tr("Downloading face capture models…"));
        QCoreApplication::processEvents();
        if (FaceCapPredictor::ensureModelsBlocking().isEmpty()) {
            setStatusMessage(
                      tr("Face capture models are not available (offline, or "
                         "not hosted yet)."));
            emit errorOccurred(d->status);
            return false;
        }
    }

    // snapshot live state (the restore contract)
    d->savedWeights.clear();
    auto* morphMgr = MorphAnimationManager::instance();
    for (const auto& ch : d->mapping.channels)
        d->savedWeights.insert(ch.meshTargetName,
                               morphMgr->weight(entity, ch.meshTargetName));
    d->savedEnabledAnimations.clear();
    if (auto* states = entity->getAllAnimationStates()) {
        for (const auto& [name, st] : states->getAnimationStates()) {
            if (st->getEnabled()) {
                d->savedEnabledAnimations << QString::fromStdString(name);
                st->setEnabled(false);
            }
        }
    }
    // Head-bone drive is used only when body is NOT driving (body owns the
    // whole skeleton incl. the head when enabled, so they never fight).
    const bool headBoneDrive =
        d->headEnabled && !d->headBone.isEmpty() && !bodyDrivable;
    if (headBoneDrive && entity->hasSkeleton()) {
        Ogre::SkeletonInstance* skel = entity->getSkeleton();
        Ogre::Bone* bone = skel->getBone(d->headBone.toStdString());
        d->headWasManuallyControlled = bone->isManuallyControlled();
        d->headBindLocal = bone->getOrientation();
        d->headBindWorld = bone->_getDerivedOrientation();
        bone->setManuallyControlled(true);
    } else {
        d->headBone.clear();  // signals onSample to skip the head-bone path
    }

    // body drive setup: build the SHARED retargeter (same math as the recorded
    // clip) from the rig's bind pose, and put the driven bones under manual
    // control so we can write their orientations each frame.
    d->bodyRetargeter.reset();
    if (bodyDrivable && entity->hasSkeleton()) {
        Ogre::SkeletonInstance* skel = entity->getSkeleton();
        // The retargeter reads the bind frame off the skeleton, so capture it
        // BEFORE any bone goes manual (reset() gives the bind pose).
        d->bodyRetargeter =
            std::make_shared<BodyRetargeter>(entity->getMesh()->getSkeleton().get());
        for (auto& bb : d->bodyBones) {
            Ogre::Bone* bone = skel->getBone(bb.boneName);
            bb.wasManuallyControlled = bone->isManuallyControlled();
            bb.bindLocal = bone->getOrientation();  // for restore-on-stop
            bone->setManuallyControlled(true);
        }
        if (!d->bodyRetargeter->valid())
            d->bodyRetargeter.reset();  // non-humanoid — skip body drive
    } else {
        d->bodyBones.clear();  // not driving this session
    }

    d->calibrated = false;
    d->bodyCalibrated = false;
    d->bodyDetected = false;
    d->faceDetected = false;
    d->liveFps = 0;
    d->lastArrival = -1;
    d->sampleCount = 0;
    d->clock.start();

    // camera + worker
    QString error;
    if (!d->camera->open(&error)) {
        d->camera.reset();
        restoreEntityState();
        setStatusMessage(error);
        emit errorOccurred(error);
        return false;
    }
    d->worker = new MocapInferenceWorker();
    d->worker->mailbox = &d->camera->mailbox();
    OneEuroFilter::Params params;
    params.minCutoff = d->smoothingCutoff;
    for (auto& f : d->worker->weightFilters)
        f = OneEuroFilter(params);
    d->worker->headFilter = OneEuroQuatFilter(params);
    for (auto& f : d->worker->roleFilters)
        f = OneEuroQuatFilter(params);
    if (!d->worker->predictor.load()) {
        const QString msg = d->worker->predictor.lastError();
        delete d->worker;
        d->worker = nullptr;
        d->camera.reset();
        restoreEntityState();
        setStatusMessage(msg);
        emit errorOccurred(msg);
        return false;
    }
    // body predictor (only when driving body this session — the mesh is skinned
    // + humanoid and the Body channel is on)
    if (!d->bodyBones.empty()) {
        if (!PoseCapPredictor::modelsPresent()) {
            setStatusMessage(tr("Downloading pose capture models…"));
            QCoreApplication::processEvents();
            PoseCapPredictor::ensureModelsBlocking();
        }
        auto pose = std::make_shared<PoseCapPredictor>();
        if (pose->load()) {
            d->worker->posePredictor = pose;
            d->worker->bodyEnabled = true;
        } else {
            // body models unavailable: keep face/head live, drop body cleanly
            setStatusMessage(tr("Body capture unavailable (%1) — driving "
                                "face/head only.").arg(pose->lastError()));
            for (auto& bb : d->bodyBones)
                if (auto* skel = entity->getSkeleton())
                    skel->getBone(bb.boneName)->setManuallyControlled(
                        bb.wasManuallyControlled);
            d->bodyBones.clear();
        }
    }
    d->worker->moveToThread(&d->workerThread);
    connect(&d->workerThread, &QThread::finished, d->worker,
            &QObject::deleteLater);
    connect(d->camera.get(), &VideoFrameSource::frameReady, d->worker,
            [w = d->worker](const MocapFrame&) { w->processPending(); },
            Qt::QueuedConnection);
    connect(d->camera.get(), &VideoFrameSource::errorOccurred, this,
            [this](const QString& message) {
                setStatusMessage(message);
                emit errorOccurred(message);
                stopPreview();
            });
    connect(d->worker, &MocapInferenceWorker::sampleReady, this,
            [this](const FaceSample& s, const BodyLiveFrame& b,
                   const QImage& p) { onSample(s, b, p); },
            Qt::QueuedConnection);
    d->workerThread.start();

    d->state = CameraStarting;
    emit stateChanged();
    setStatusMessage(startingMessage);
    d->camera->start();

    SentryReporter::addBreadcrumb("ai.assist.mocap_live", "preview start");
    GamificationManager::noteFeature(QStringLiteral("mocap"),
                                     GamificationManager::Surface::Gui);
    return true;
}

bool MocapController::startPreviewWithSource(VideoFrameSource* source)
{
    // Test seam: synchronous drive, no camera/worker thread. Samples are fed
    // manually through onSample on THIS thread; source may be null.
    if (d->state != Idle)
        return false;
    auto* sel = SelectionSet::getSingleton();
    Ogre::Entity* entity = nullptr;
    if (sel) {
        const auto ents = sel->getResolvedEntities();
        if (!ents.isEmpty())
            entity = ents.first();
    }
    if (!entity)
        return false;
    d->entityName = entity->getName();
    d->mapping = FaceCapMapper::build(
        MorphAnimationManager::instance()->morphTargetsFor(entity));
    d->headBone = MocapRecorder::resolveHeadBone(entity);
    emit mappingChanged();

    d->savedWeights.clear();
    auto* morphMgr = MorphAnimationManager::instance();
    for (const auto& ch : d->mapping.channels)
        d->savedWeights.insert(ch.meshTargetName,
                               morphMgr->weight(entity, ch.meshTargetName));
    d->savedEnabledAnimations.clear();
    if (auto* states = entity->getAllAnimationStates()) {
        for (const auto& [name, st] : states->getAnimationStates()) {
            if (st->getEnabled()) {
                d->savedEnabledAnimations << QString::fromStdString(name);
                st->setEnabled(false);
            }
        }
    }
    if (!d->headBone.isEmpty() && entity->hasSkeleton()) {
        Ogre::Bone* bone =
            entity->getSkeleton()->getBone(d->headBone.toStdString());
        d->headWasManuallyControlled = bone->isManuallyControlled();
        d->headBindLocal = bone->getOrientation();
        d->headBindWorld = bone->_getDerivedOrientation();
        bone->setManuallyControlled(true);
    }
    d->calibrated = false;
    d->sampleCount = 0;
    d->clock.start();
    d->injectedSource = source;
    d->state = Previewing;
    emit stateChanged();
    return true;
}

// Test seam companion: feed a sample as if it came from the worker.
void MocapController::onSample(const FaceSample& sample, const QImage& preview)
{
    onSample(sample, BodyLiveFrame{}, preview);
}

void MocapController::onSample(const FaceSample& sample,
                               const BodyLiveFrame& body, const QImage& preview)
{
    if (d->state == Idle)
        return;
    if (d->state == CameraStarting) {
        d->state = Previewing;
        emit stateChanged();
        setStatusMessage(tr("Live — driving the selection."));
    }

    // HUD
    d->faceDetected = sample.confidence > 0.f;
    d->bodyDetected = body.valid;
    if (d->lastArrival >= 0.0) {
        const double dt = sample.timeSec - d->lastArrival;
        if (dt > 0)
            d->liveFps = 0.8 * d->liveFps + 0.2 * (1.0 / dt);
    }
    d->lastArrival = sample.timeSec;
    ++d->sampleCount;
    emit liveStatsChanged();
    if (!preview.isNull()) {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        preview.save(&buffer, "PNG");
        d->previewDataUrl =
            QStringLiteral("data:image/png;base64,") + png.toBase64();
        emit previewChanged();
    }

    Ogre::Entity* entity = d->entity();
    if (!entity || sample.confidence <= 0.f)
        return;

    // live drive — morphs
    auto* morphMgr = MorphAnimationManager::instance();
    for (const auto& ch : d->mapping.channels)
        morphMgr->setWeight(entity, ch.meshTargetName,
                            sample.weights[ch.canonicalIndex]);

    // live drive — head (skipped when body owns the skeleton: the body
    // retargeter already drives the Head bone from the pose landmarks, and
    // letting both write the same bone makes them fight frame-to-frame).
    const bool bodyOwnsSkeleton =
        body.valid && d->bodyRetargeter && d->bodyRetargeter->valid();
    if (!d->headBone.isEmpty() && entity->hasSkeleton() && !bodyOwnsSkeleton) {
        if (!d->calibrated) {
            d->neutral = Ogre::Quaternion(
                sample.headRotation[3], sample.headRotation[0],
                sample.headRotation[1], sample.headRotation[2]);
            d->calibrated = true;
        }
        const Ogre::Quaternion current(
            sample.headRotation[3], sample.headRotation[0],
            sample.headRotation[1], sample.headRotation[2]);
        const Ogre::Quaternion delta = current * d->neutral.Inverse();
        const Ogre::Quaternion local =
            d->headBindWorld.Inverse() * delta * d->headBindWorld;
        Ogre::SkeletonInstance* skel = entity->getSkeleton();
        Ogre::Bone* bone = skel->getBone(d->headBone.toStdString());
        bone->setOrientation(d->headBindLocal * local);
        skel->_notifyManualBonesDirty();
    }

    // live drive — body. Uses the SHARED BodyRetargeter, i.e. the EXACT same
    // legacy-transport math applyMotionClip bakes into the recorded clip, so
    // live Preview and Record can't diverge: each joint's parent-relative
    // articulation delta (vs the first frame) composed onto the rig's harvested
    // standing pose. setOrientation takes the absolute local the retargeter
    // returns (Ogre node keys are absolute, not deltas).
    if (body.valid && d->bodyRetargeter && d->bodyRetargeter->valid()
        && entity->hasSkeleton()) {
        const auto locals =
            d->bodyRetargeter->evaluateFrame(body.quats, body.resolvedMask);
        Ogre::SkeletonInstance* skel = entity->getSkeleton();
        for (const auto& [handle, local] : locals)
            skel->getBone(handle)->setOrientation(local);
        skel->_notifyManualBonesDirty();
    }

    if (d->state == Recording) {
        d->take.push_back(sample);
        if (!d->bodyBones.empty())
            d->bodyTake.push_back(body);
        d->lastSampleTime = sample.timeSec;
    }
}

void MocapController::calibrateNeutral()
{
    d->calibrated = false;  // the next confident sample becomes neutral
    setStatusMessage(tr("Hold a neutral face…"));
}

bool MocapController::startRecording()
{
    if (d->state != Previewing)
        return false;
    d->take.clear();
    d->bodyTake.clear();
    d->takeStart = d->lastArrival;
    d->lastSampleTime = d->takeStart;
    d->state = Recording;
    emit stateChanged();
    setStatusMessage(tr("Recording…"));
    SentryReporter::addBreadcrumb("ai.assist.mocap_live", "record start");
    return true;
}

void MocapController::stopRecording()
{
    if (d->state != Recording)
        return;
    d->state = Previewing;
    emit stateChanged();

    if (d->take.size() < 2) {
        setStatusMessage(tr("Nothing recorded (no confident frames)."));
        return;
    }

    QStringList summary;
    int faceKeys = 0, bodyTracks = 0;
    double clipLen = 0.0;

    // face + head clip (only when a face/head channel actually drove)
    if (!d->mapping.channels.isEmpty() || !d->headBone.isEmpty()) {
        MocapRecorder::FaceRecordOptions options;
        options.clipName = d->clipName;
        options.head = !d->headBone.isEmpty();
        auto* cmd = new RecordMocapClipCommand(d->entityName, d->take,
                                               d->mapping, options);
        UndoManager::getSingleton()->push(cmd);
        const auto& report = cmd->report();
        if (report.ok()) {
            faceKeys = report.keyframesWritten + report.headKeyframesWritten;
            clipLen = report.clipLength;
            summary << tr("'%1' (%2 keys)").arg(report.clipName).arg(faceKeys);
        } else {
            setStatusMessage(report.error);
        }
        GamificationManager::noteOperation(
            QStringLiteral("mocap_face"),
            {{QStringLiteral("frames"),
              static_cast<qint64>(report.framesProcessed)},
             {QStringLiteral("keyframes"), static_cast<qint64>(faceKeys)}},
            GamificationManager::Surface::Gui);
    }

    // body clip: convert the buffered live frames to the [frame][22] world-quat
    // stream recordBody expects (identity for roles unresolved that frame), and
    // push a SEPARATE undo command so face + body each undo cleanly.
    if (!d->bodyBones.empty() && d->bodyTake.size() >= 2) {
        std::vector<std::vector<std::array<float, 4>>> clipQuats;
        clipQuats.reserve(d->bodyTake.size());
        for (const auto& bf : d->bodyTake) {
            if (!bf.valid)
                continue;
            clipQuats.emplace_back(bf.quats.begin(), bf.quats.end());
        }
        if (clipQuats.size() >= 2) {
            MocapRecorder::BodyRecordOptions bopts;
            bopts.clipName = d->clipName + QStringLiteral("_Body");
            bopts.algorithmUsed = QStringLiteral("pose-ik");
            const int fps = 30;
            auto* bcmd = new RecordBodyClipCommand(d->entityName, clipQuats,
                                                   fps, bopts);
            UndoManager::getSingleton()->push(bcmd);
            const auto& br = bcmd->report();
            if (br.ok()) {
                bodyTracks = br.tracksWritten;
                clipLen = std::max(clipLen, br.clipLength);
                summary << tr("'%1' (%2 tracks)")
                               .arg(br.clipName).arg(bodyTracks);
            }
            GamificationManager::noteOperation(
                QStringLiteral("mocap_body"),
                {{QStringLiteral("frames"),
                  static_cast<qint64>(br.framesProcessed)},
                 {QStringLiteral("tracks"), static_cast<qint64>(bodyTracks)}},
                GamificationManager::Surface::Gui);
        }
    }

    if (!summary.isEmpty())
        setStatusMessage(tr("Recorded %1s → %2 — Ctrl+Z to discard.")
                             .arg(clipLen, 0, 'f', 1)
                             .arg(summary.join(QStringLiteral(" + "))));
}

void MocapController::restoreEntityState()
{
    Ogre::Entity* entity = d->entity();
    if (!entity)
        return;
    auto* morphMgr = MorphAnimationManager::instance();
    for (auto it = d->savedWeights.begin(); it != d->savedWeights.end(); ++it)
        morphMgr->setWeight(entity, it.key(), it.value());
    if (entity->hasSkeleton()) {
        Ogre::SkeletonInstance* skel = entity->getSkeleton();
        if (!d->headBone.isEmpty()) {
            Ogre::Bone* bone = skel->getBone(d->headBone.toStdString());
            bone->setOrientation(d->headBindLocal);
            bone->setManuallyControlled(d->headWasManuallyControlled);
        }
        // restore every body-driven bone to its pre-preview state exactly
        for (const auto& bb : d->bodyBones) {
            Ogre::Bone* bone = skel->getBone(bb.boneName);
            bone->setOrientation(bb.bindLocal);
            bone->setManuallyControlled(bb.wasManuallyControlled);
        }
        skel->_notifyManualBonesDirty();
    }
    if (auto* states = entity->getAllAnimationStates()) {
        for (const QString& name : d->savedEnabledAnimations) {
            const std::string n = name.toStdString();
            if (states->hasAnimationState(n))
                states->getAnimationState(n)->setEnabled(true);
        }
    }
}

void MocapController::stopPreview()
{
    if (d->state == Idle)
        return;
    if (d->state == Recording)
        stopRecording();  // commit the take rather than dropping it

    if (d->camera)
        d->camera->stop();
    if (d->workerThread.isRunning()) {
        d->workerThread.quit();
        // Block until the worker thread has actually exited before we free
        // anything it touches — the worker drains d->camera->mailbox() inside
        // processPending(), so releasing the camera (and its mailbox) while
        // the thread is still running is a use-after-free. A single wait(2000)
        // could time out mid-inference and fall through; keep waiting (a long
        // ONNX step is finite) and log if it's pathologically slow.
        int waited = 0;
        while (!d->workerThread.wait(2000)) {
            waited += 2000;
            SentryReporter::addBreadcrumb("ai.assist.mocap_live",
                QStringLiteral("preview stop: worker still running after %1ms")
                    .arg(waited));
        }
    }
    d->worker = nullptr;  // deleted via QThread::finished
    d->camera.reset();
    d->injectedSource = nullptr;
    d->bodyRetargeter.reset();

    restoreEntityState();

    d->state = Idle;
    d->faceDetected = false;
    d->liveFps = 0;
    d->previewDataUrl.clear();
    emit stateChanged();
    emit liveStatsChanged();
    emit previewChanged();
    if (d->status.isEmpty() || d->status.startsWith(tr("Live")))
        setStatusMessage(tr("Preview stopped."));
    SentryReporter::addBreadcrumb("ai.assist.mocap_live", "preview stop");
}

#include "MocapController.moc"

#endif  // ENABLE_MOCAP
