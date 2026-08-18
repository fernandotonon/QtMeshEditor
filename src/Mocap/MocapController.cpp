#include "MocapController.h"

#ifdef ENABLE_MOCAP
#include "MocapRecorder.h"
#include "OneEuroFilter.h"
#include "PoseCapPredictor.h"
#include "HandCapPredictor.h"
#include "PoseIKSolver.h"
#include "VideoFrameSource.h"
#include "MocapCameraHints.h"
#include "MocapLiveTypes.h"
#include "MocapPoseDebugOverlay.h"
#include "MocapPoseIkFk.h"
#include "MocapBodyDriveDebug.h"
#include "MocapPoseFix.h"
#include "FaceCapGeom.h"
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
#include <OgreRoot.h>
#include <OgreSkeletonInstance.h>

#include <memory>
#include <cmath>
#include <algorithm>

#include <QHash>
#include <QSettings>
#include <QBuffer>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QPainter>
#include <QPen>
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
QString MocapController::bodyCalibrationHint() const { return {}; }
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
bool MocapController::showPoseDebug() const { return false; }
void MocapController::setShowPoseDebug(bool) {}
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

struct BodyDriveBone {
    int role = -1;
    std::string boneName;
    Ogre::Quaternion bindLocal = Ogre::Quaternion::IDENTITY;
    bool wasManuallyControlled = false;
};

struct BodyManualBoneSnapshot {
    std::string boneName;
    Ogre::Quaternion bindLocal = Ogre::Quaternion::IDENTITY;
    Ogre::Vector3 bindPosition = Ogre::Vector3::ZERO;
    bool wasManuallyControlled = false;
};

struct BodyAnimMaskEntry {
    std::string animName;
    unsigned short boneHandle = 0;
    float weight = 1.f;
};
}  // namespace

namespace {

void paintMocapPreviewHud(QImage& preview, const BodyLiveFrame& body, int srcW,
                          int srcH)
{
    if (preview.isNull() || srcW <= 0 || srcH <= 0)
        return;
    if (preview.format() != QImage::Format_RGB32
        && preview.format() != QImage::Format_ARGB32)
        preview = preview.convertToFormat(QImage::Format_RGB32);
    const float sx = static_cast<float>(preview.width())
                     / static_cast<float>(srcW);
    const float sy = static_cast<float>(preview.height())
                     / static_cast<float>(srcH);
    auto toPt = [&](float x, float y) {
        return QPointF(x * sx, y * sy);
    };
    QPainter p(&preview);
    p.setRenderHint(QPainter::Antialiasing, false);

    static const int kPoseEdges[][2] = {
        {11, 12}, {11, 13}, {13, 15}, {12, 14}, {14, 16},
        {11, 23}, {12, 24}, {23, 24}, {23, 25}, {25, 27},
        {24, 26}, {26, 28}, {15, 19}, {15, 17}, {15, 21},
        {16, 20}, {16, 18}, {16, 22},
    };
    p.setPen(QPen(QColor(20, 230, 255), 1));
    for (const auto& e : kPoseEdges) {
        if (body.visibility[static_cast<size_t>(e[0])] < 0.2f
            || body.visibility[static_cast<size_t>(e[1])] < 0.2f)
            continue;
        p.drawLine(toPt(body.imageXy[static_cast<size_t>(e[0] * 2)],
                        body.imageXy[static_cast<size_t>(e[0] * 2 + 1)]),
                   toPt(body.imageXy[static_cast<size_t>(e[1] * 2)],
                        body.imageXy[static_cast<size_t>(e[1] * 2 + 1)]));
    }

    static const int kHandEdges[][2] = {
        {0, 1},  {1, 2},  {2, 3},  {3, 4},  {0, 5},  {5, 6},  {6, 7},  {7, 8},
        {0, 9},  {9, 10}, {10, 11}, {11, 12}, {0, 13}, {13, 14}, {14, 15},
        {15, 16}, {0, 17}, {17, 18}, {18, 19}, {19, 20}, {5, 9}, {9, 13},
        {13, 17},
    };
    auto drawHand = [&](const HandLandmarks& h, const QColor& col) {
        if (!h.valid)
            return;
        p.setPen(QPen(col, 2));
        for (const auto& e : kHandEdges) {
            p.drawLine(toPt(h.imageXy[static_cast<size_t>(e[0] * 2)],
                            h.imageXy[static_cast<size_t>(e[0] * 2 + 1)]),
                       toPt(h.imageXy[static_cast<size_t>(e[1] * 2)],
                            h.imageXy[static_cast<size_t>(e[1] * 2 + 1)]));
        }
    };
    drawHand(body.hands.right, QColor(255, 40, 220));
    drawHand(body.hands.left, QColor(255, 180, 40));
}

void fillFingerFlexFromHands(
    const HandsLiveFrame& hands,
    std::array<float, AnimationMerger::kFingerSlots>& out)
{
    out.fill(-1.f);
    auto pack = [&](const HandLandmarks& h, int side) {
        if (!h.valid)
            return;
        float flex[5][3];
        const bool haveWorld = std::any_of(
            h.worldXyz.begin(), h.worldXyz.end(),
            [](float v) { return std::abs(v) > 1e-6f; });
        FaceCapGeom::handFingerFlexRad(
            haveWorld ? h.worldXyz.data() : h.cropXyz.data(), flex);
        for (int fgr = 0; fgr < 5; ++fgr) {
            for (int seg = 0; seg < 3; ++seg) {
                const int slot =
                    AnimationMerger::fingerSlot(side, fgr, seg);
                if (slot >= 0)
                    out[static_cast<size_t>(slot)] = flex[fgr][seg];
            }
        }
    };
    pack(hands.right, 0);
    pack(hands.left, 1);
}

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
    std::shared_ptr<HandCapPredictor> handPredictor;
    PoseIK::Solver poseSolver;
    std::array<OneEuroQuatFilter, PoseIK::kCanonicalRoles> roleFilters;
    std::array<std::array<OneEuroFilter, 3>, PoseIK::kLandmarkCount> worldFilters;
    bool requestPoseReset = false;

public slots:
    void processPending()
    {
        if (!mailbox)
            return;
        MocapFrame frame;
        while (mailbox->take(&frame)) {
            if (requestPoseReset) {
                poseSolver.reset();
                requestPoseReset = false;
            }

            FaceSample s = predictor.predict(frame.image, frame.timeSec);
            if (smooth && s.confidence > 0.f) {
                for (int c = 0; c < 52; ++c)
                    s.weights[c] = static_cast<float>(
                        weightFilters[c].filter(s.weights[c], s.timeSec));
                s.headRotation = headFilter.filter(s.headRotation, s.timeSec);
            }

            BodyLiveFrame body;
            if (bodyEnabled && posePredictor) {
                PoseSample ps =
                    posePredictor->predict(frame.image, frame.timeSec);
                if (ps.confidence > 0.f) {
                    PoseIK::FrameResult fr = poseSolver.solveFrame(
                        ps.world.data(), ps.visibility.data());
                    if (smooth)
                        for (int r = 0; r < PoseIK::kCanonicalRoles; ++r)
                            fr.quats[r] =
                                roleFilters[r].filter(fr.quats[r], frame.timeSec);
                    body.valid = true;
                    body.timeSec = frame.timeSec;
                    body.quats = fr.quats;
                    body.resolvedMask = fr.resolvedMask;
                    body.world = ps.world;
                    body.visibility = ps.visibility;
                    body.screenCrop = ps.screenCrop;
                    body.imageXy = ps.imageXy;
                    if (handPredictor && handPredictor->isAvailable())
                        body.hands = handPredictor->predict(
                            frame.image, ps.imageXy.data(),
                            ps.visibility.data(), frame.timeSec);
                    if (smooth) {
                        for (int lm = 0; lm < PoseIK::kLandmarkCount; ++lm) {
                            // BlazePose world coords for fingertip LMs (17–22) barely
                            // move; screen-crop deltas carry the motion instead.
                            if (lm >= 17 && lm <= 22)
                                continue;
                            for (int axis = 0; axis < 3; ++axis) {
                                const size_t idx =
                                    static_cast<size_t>(lm * 3 + axis);
                                body.world[idx] = static_cast<float>(
                                    worldFilters[static_cast<size_t>(lm)]
                                                [static_cast<size_t>(axis)]
                                        .filter(body.world[idx], frame.timeSec));
                            }
                        }
                    }
                }
            }

            // HUD preview — every frame, small + smooth scale to avoid flicker
            QImage preview;
            preview = frame.image.scaledToWidth(200, Qt::SmoothTransformation);
            if (body.valid)
                paintMocapPreviewHud(preview, body, frame.image.width(),
                                     frame.image.height());
            emit sampleReady(s, body, preview);
        }
    }

signals:
    void sampleReady(const FaceSample& sample, const BodyLiveFrame& body,
                     const QImage& preview);
};

namespace {

int countFingerScreen2dSlots(
    const std::array<std::array<float, 2>, AnimationMerger::kFingerSlots>& dirs)
{
    int n = 0;
    for (const auto& dir : dirs) {
        if (dir[0] != 0.f || dir[1] != 0.f)
            ++n;
    }
    return n;
}

bool tryCaptureFingerNeutralScreen(
    const BodyLiveFrame& body,
    std::array<std::array<float, 2>, AnimationMerger::kFingerSlots>& out)
{
    out.fill({0.f, 0.f});
    AnimationMerger::captureFingerNeutralScreenDirs(
        body.screenCrop.data(), out);
    return countFingerScreen2dSlots(out) >= 4;
}

bool tryCaptureFingerNeutralFlex(
    const BodyLiveFrame& body,
    std::array<float, AnimationMerger::kFingerSlots>& out)
{
    fillFingerFlexFromHands(body.hands, out);
    int n = 0;
    for (float v : out) {
        if (v >= 0.f)
            ++n;
    }
    return n >= 6;
}

void collectLiveFingerScreen2d(
    const BodyLiveFrame& body,
    std::array<std::array<float, 2>, AnimationMerger::kFingerSlots>& out)
{
    AnimationMerger::captureFingerNeutralScreenDirs(body.screenCrop.data(), out);
}

void smoothFingerScreen2d(
    std::array<std::array<float, 2>, AnimationMerger::kFingerSlots>& dirs,
    std::array<std::array<OneEuroFilter, 2>, AnimationMerger::kFingerSlots>&
        filters,
    double timeSec)
{
    for (int s = 0; s < AnimationMerger::kFingerSlots; ++s) {
        auto& d = dirs[static_cast<size_t>(s)];
        if (d[0] == 0.f && d[1] == 0.f)
            continue;
        for (int ax = 0; ax < 2; ++ax)
            d[static_cast<size_t>(ax)] = static_cast<float>(
                filters[static_cast<size_t>(s)][static_cast<size_t>(ax)].filter(
                    d[static_cast<size_t>(ax)], timeSec));
    }
}

void smoothFingerLandmarkDirs(
    std::array<std::array<float, 3>, AnimationMerger::kFingerSlots>& dirs,
    std::array<std::array<OneEuroFilter, 3>, AnimationMerger::kFingerSlots>&
        filters,
    double timeSec)
{
    for (int s = 0; s < AnimationMerger::kFingerSlots; ++s) {
        auto& d = dirs[static_cast<size_t>(s)];
        if (d[0] == 0.f && d[1] == 0.f && d[2] == 0.f)
            continue;
        for (int ax = 0; ax < 3; ++ax)
            d[static_cast<size_t>(ax)] = static_cast<float>(
                filters[static_cast<size_t>(s)][static_cast<size_t>(ax)].filter(
                    d[static_cast<size_t>(ax)], timeSec));
        Ogre::Vector3 v(d[0], d[1], d[2]);
        if (v.squaredLength() > 1e-9f) {
            v.normalise();
            d = {v.x, v.y, v.z};
        } else {
            d = {0.f, 0.f, 0.f};
        }
    }
}

}  // namespace

OneEuroFilter::Params faceSmoothParams(double cutoffHz)
{
    OneEuroFilter::Params p;
    p.minCutoff = cutoffHz;
    p.beta = 0.05;
    return p;
}

OneEuroFilter::Params landmarkSmoothParams(double cutoffHz)
{
    OneEuroFilter::Params p;
    p.minCutoff = std::max(0.15, cutoffHz * 0.45);
    p.beta = 0.02;
    p.dCutoff = 0.8;
    return p;
}

OneEuroFilter::Params boneOutputSmoothParams(double cutoffHz)
{
    OneEuroFilter::Params p;
    p.minCutoff = std::max(0.15, cutoffHz * 0.65);
    p.beta = 0.025;
    return p;
}

void configureWorkerSmoothing(MocapInferenceWorker* worker, double cutoffHz)
{
    if (!worker)
        return;
    const auto face = faceSmoothParams(cutoffHz);
    const auto landmarks = landmarkSmoothParams(cutoffHz);
    for (auto& f : worker->weightFilters)
        f = OneEuroFilter(face);
    worker->headFilter = OneEuroQuatFilter(face);
    for (auto& f : worker->roleFilters)
        f = OneEuroQuatFilter(face);
    for (auto& lm : worker->worldFilters)
        for (auto& axis : lm)
            axis = OneEuroFilter(landmarks);
}

struct MocapController::Impl {
    State state = Idle;
    QString status;
    QString clipName = QStringLiteral("FaceCap");
    double smoothingCutoff = 0.5;
    QVariantList cachedDevices;  // [{id, description}] — populated by refreshDevices()

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
    Ogre::Quaternion headRestoreLocal = Ogre::Quaternion::IDENTITY;
    bool headWasManuallyControlled = false;
    bool savedHeadSnapshot = false;
    bool mirroredLivePreview = false;
    QHash<QString, float> savedWeights;          // mesh target -> weight
    QStringList savedEnabledAnimations;
    bool savedSkipAnimStateUpdate = false;
    bool savedAlwaysUpdateMainSkeleton = false;

    // channel enables (persist across sessions; body gated on a humanoid rig)
    bool faceEnabled = true;
    bool headEnabled = true;
    bool bodyEnabled = false;
    bool bodyRigOk = false;   // selection resolved >= half the canonical roles

    // body live-drive: landmark-direction retarget (BodyRetargeter) + restore list.
    std::unique_ptr<BodyRetargeter> bodyRetargeter;
    std::vector<BodyDriveBone> bodyBones;
    QHash<unsigned short, OneEuroQuatFilter> bodyBoneFilters;
    std::vector<BodyManualBoneSnapshot> bodyManualRestore;
    std::vector<BodyAnimMaskEntry> bodyAnimMaskRestore;
    bool bodyDetected = false;
    int bodyTorsoStableFrames = 0;
    bool bodyNeutralReady = false;
    uint32_t bodyNeutralCapturedMask = 0;
    static constexpr int kBodyTorsoStableFrames = 3;
    static constexpr uint32_t kTorsoResolvedMask =
        (1u << 0) | (1u << 1) | (1u << 2);  // hip, abdomen, chest
    // Live vertical shift on the entity node (world Y), not hip bone local Y —
    // hip rotation must not skew the translation axis.
    Ogre::Vector3 entityBindPosition = Ogre::Vector3::ZERO;
    bool haveEntityBindPosition = false;
    float bodyRigLegLen = 0.f;
    float bodyNeutralLegSpan = -1.f;
    OneEuroFilter bodyHipHeightFilter;
    std::array<std::array<float, 2>, AnimationMerger::kFingerSlots>
        fingerNeutralScreen2d{};
    std::array<float, AnimationMerger::kFingerSlots> fingerNeutralFlex{};
    bool haveFingerNeutralFlex = false;
    bool haveFingerNeutralScreen = false;
    AnimationMerger::FingerLiveDriveContext fingerLiveCtx;
    std::array<std::array<OneEuroFilter, 2>, AnimationMerger::kFingerSlots>
        fingerScreenFilters{};

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
    QTimer* cameraStartupTimer = nullptr;
    MocapPoseDebugOverlay poseDebugOverlay;
    bool showPoseDebug = false;

    // Ogre only recomputes GPU bone matrices / software-skinned vertex buffers when
    // AnimationStateSet or manual bones are dirty. Mocap samples arrive on the Qt
    // event loop (~30 Hz) while the render loop runs faster — without a per-frame
    // refresh the mesh stays frozen in bind pose even though Bone::setOrientation
    // succeeded (debug overlay can move while skin does not).
    struct SkinningFrameListener : public Ogre::FrameListener {
        Impl* impl = nullptr;

        bool frameRenderingQueued(const Ogre::FrameEvent&) override
        {
            if (!impl || impl->state == MocapController::Idle)
                return true;
            Ogre::Entity* entity = impl->entity();
            if (!entity || !entity->hasSkeleton())
                return true;
            Ogre::SkeletonInstance* skel = entity->getSkeleton();
            skel->_notifyManualBonesDirty();
            if (auto* states = entity->getAllAnimationStates())
                states->_notifyDirty();
            entity->_updateAnimation();
            return true;
        }
    };

    std::unique_ptr<SkinningFrameListener> skinningListener;
    bool skinningListenerRegistered = false;
    bool addedSoftwareAnimRequest = false;

    Ogre::Entity* entity() const
    {
        auto* mgr = Manager::getSingletonPtr();
        if (!mgr || !mgr->getSceneMgr() || entityName.empty())
            return nullptr;
        auto* scene = mgr->getSceneMgr();
        return scene->hasEntity(entityName) ? scene->getEntity(entityName)
                                            : nullptr;
    }

    void unregisterSkinningListener()
    {
        if (!skinningListener || !skinningListenerRegistered)
            return;
        Ogre::Root::getSingleton().removeFrameListener(skinningListener.get());
        skinningListener->impl = nullptr;
        skinningListenerRegistered = false;
    }

    void registerSkinningListener()
    {
        if (!skinningListener)
            skinningListener = std::make_unique<SkinningFrameListener>();
        skinningListener->impl = this;
        if (!skinningListenerRegistered) {
            Ogre::Root::getSingleton().addFrameListener(skinningListener.get());
            skinningListenerRegistered = true;
        }
    }
};

MocapController::MocapController(QObject* parent)
    : QObject(parent), d(new Impl)
{
    QSettings settings;
    d->smoothingCutoff =
        settings.value(QStringLiteral("mocap/smoothingCutoff"), 0.5).toDouble();
    if (d->smoothingCutoff <= 0.0)
        d->smoothingCutoff = 0.5;
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
    return d->cachedDevices;
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
QString MocapController::bodyCalibrationHint() const
{
    if (d->state < Previewing || !d->bodyEnabled || !d->bodyRetargeter)
        return {};
    if (d->bodyNeutralReady)
        return {};
    if (!d->bodyDetected)
        return tr("Stand in frame so your hips and shoulders are visible…");
    return tr("Calibrating body drive — stand relaxed, arms at your sides, "
              "feet visible for sit/stand height…");
}
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
    QSettings settings;
    settings.setValue(QStringLiteral("mocap/smoothingCutoff"), hz);
    configureWorkerSmoothing(d->worker, d->smoothingCutoff);
    d->bodyBoneFilters.clear();
    emit smoothingChanged();
}

bool MocapController::showPoseDebug() const
{
    return d->showPoseDebug;
}

void MocapController::setShowPoseDebug(bool on)
{
    if (on == d->showPoseDebug)
        return;
    d->showPoseDebug = on;
    if (on && d->state != Idle) {
        if (Ogre::Entity* entity = d->entity()) {
            auto* mgr = Manager::getSingletonPtr();
            if (mgr && mgr->getSceneMgr())
                d->poseDebugOverlay.attach(mgr->getSceneMgr(),
                                           entity->getParentSceneNode());
        }
    } else {
        d->poseDebugOverlay.detach();
    }
    emit previewSettingsChanged();
}

void MocapController::resetLiveCaptureCalibration()
{
    d->calibrated = false;
    d->bodyTorsoStableFrames = 0;
    d->bodyNeutralReady = false;
    d->bodyNeutralCapturedMask = 0;
    d->bodyNeutralLegSpan = -1.f;
    d->bodyHipHeightFilter = OneEuroFilter();
    d->haveFingerNeutralScreen = false;
    d->haveFingerNeutralFlex = false;
    d->fingerNeutralScreen2d.fill({0.f, 0.f});
    d->fingerNeutralFlex.fill(-1.f);
    d->fingerLiveCtx = {};
    d->fingerScreenFilters = {};
    if (d->bodyRetargeter)
        d->bodyRetargeter->resetLiveNeutral();
}

void MocapController::refreshDevices()
{
    QVariantList out;
    for (const auto& dev : CameraFrameSource::availableDevices()) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), dev.id);
        m.insert(QStringLiteral("description"), dev.description);
        out.append(m);
    }
    if (out == d->cachedDevices)
        return;
    d->cachedDevices = std::move(out);
    emit devicesChanged();
}

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
            BodyDriveBone bb;
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
    d->mirroredLivePreview =
        (dynamic_cast<CameraFrameSource*>(d->camera.get()) != nullptr);

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
    // Prevent the render loop's Entity::updateAnimation → setAnimationState →
    // Skeleton::reset from re-applying disabled clips (and racing our manual
    // bone writes). We drive bones ourselves and call _updateAnimation() after
    // each sample to refresh GPU skinning matrices.
    d->savedSkipAnimStateUpdate = entity->getSkipAnimationStateUpdate();
    entity->setSkipAnimationStateUpdate(true);
    d->savedAlwaysUpdateMainSkeleton = entity->getAlwaysUpdateMainSkeleton();
    entity->setAlwaysUpdateMainSkeleton(true);
    // Head-bone drive uses FaceCap (dense landmarks) even when Body is on —
    // PoseIK's head role is coarse and fights the face solve.
    const bool headBoneDrive =
        d->headEnabled && !d->headBone.isEmpty();
    const std::string headBoneStd =
        headBoneDrive ? d->headBone.toStdString() : std::string{};
    if (entity->hasSkeleton()) {
        Ogre::SkeletonInstance* skel = entity->getSkeleton();
        // Snapshot manual-bone state BEFORE reset(true) — reset clears manual
        // orientations, so post-reset snapshots cannot be restored faithfully.
        d->bodyManualRestore.clear();
        d->savedHeadSnapshot = false;
        for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
            Ogre::Bone* bone = skel->getBone(i);
            if (headBoneDrive && bone->getName() == headBoneStd) {
                d->headWasManuallyControlled = bone->isManuallyControlled();
                d->headRestoreLocal = bone->getOrientation();
                d->savedHeadSnapshot = true;
                continue;
            }
            if (bodyDrivable) {
                BodyManualBoneSnapshot snap;
                snap.boneName = bone->getName();
                snap.bindLocal = bone->getOrientation();
                snap.bindPosition = bone->getPosition();
                snap.wasManuallyControlled = bone->isManuallyControlled();
                d->bodyManualRestore.push_back(std::move(snap));
            }
        }
        skel->reset(true);
        skel->_updateTransforms();
        if (headBoneDrive) {
            Ogre::Bone* bone = skel->getBone(headBoneStd);
            d->headBindLocal = bone->getOrientation();
            d->headBindWorld = bone->_getDerivedOrientation();
            bone->setManuallyControlled(true);
        }
    }

    // body drive setup: BodyRetargeter + manual bone control for live drive.
    d->bodyRetargeter.reset();
    d->bodyBones.clear();
    d->bodyAnimMaskRestore.clear();
    d->haveEntityBindPosition = false;
    d->bodyRigLegLen = 0.f;
    d->bodyNeutralLegSpan = -1.f;
    d->bodyHipHeightFilter = OneEuroFilter();
    d->fingerLiveCtx = {};
    d->fingerScreenFilters = {};
    d->haveFingerNeutralScreen = false;
    d->haveFingerNeutralFlex = false;
    d->fingerNeutralScreen2d.fill({0.f, 0.f});
    d->fingerNeutralFlex.fill(-1.f);
    if (Ogre::SceneNode* node = entity->getParentSceneNode()) {
        d->entityBindPosition = node->getPosition();
        d->haveEntityBindPosition = true;
    }
    if (bodyDrivable && entity->hasSkeleton()) {
        Ogre::SkeletonInstance* skel = entity->getSkeleton();
        const bool yaw180 = AnimationMerger::detectBackwardFacing(entity);
        d->bodyRetargeter = std::make_unique<BodyRetargeter>(skel, yaw180);
        if (!d->bodyRetargeter->valid()) {
            d->bodyRetargeter.reset();
            d->bodyBones.clear();
        } else {
            for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
                Ogre::Bone* bone = skel->getBone(i);
                const bool isHeadBone =
                    headBoneDrive && bone->getName() == headBoneStd;
                if (!isHeadBone)
                    bone->setManuallyControlled(true);
                if (!isHeadBone) {
                    const int role = MotionInbetween::canonicalIndexForBone(
                        QString::fromStdString(bone->getName()));
                    if (role >= 0) {
                        BodyDriveBone bb;
                        bb.role = role;
                        bb.boneName = bone->getName();
                        bb.bindLocal = bone->getOrientation();
                        bb.wasManuallyControlled = bone->isManuallyControlled();
                        d->bodyBones.push_back(std::move(bb));
                    }
                }
                // Zero animation influence on every bone (incl. head) so idle
                // clips cannot fight manual mocap drive.
                if (auto* states = entity->getAllAnimationStates()) {
                    const auto nBones = static_cast<size_t>(skel->getNumBones());
                    for (const auto& [animName, st] : states->getAnimationStates()) {
                        if (!st)
                            continue;
                        if (!st->hasBlendMask())
                            st->createBlendMask(nBones, 1.0f);
                        const float before = st->getBlendMaskEntry(i);
                        d->bodyAnimMaskRestore.push_back(
                            {animName, i, before});
                        st->setBlendMaskEntry(i, 0.0f);
                    }
                }
            }
            Ogre::Bone* hipBone = nullptr;
            Ogre::Bone* footBone = nullptr;
            for (Ogre::Bone* root : skel->getRootBones())
                root->_update(true, true);
            skel->_updateTransforms();
            d->fingerLiveCtx =
                AnimationMerger::buildFingerLiveDriveContext(skel);
            for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
                Ogre::Bone* bone = skel->getBone(i);
                const int role = MotionInbetween::canonicalIndexForBone(
                    QString::fromStdString(bone->getName()));
                if (role == 0)
                    hipBone = bone;
                else if (role == 17)
                    footBone = bone;
                else if (role == 21 && !footBone)
                    footBone = bone;
            }
            if (hipBone && footBone) {
                const Ogre::Vector3 hipW = hipBone->_getDerivedPosition();
                const Ogre::Vector3 footW = footBone->_getDerivedPosition();
                d->bodyRigLegLen = hipW.y - footW.y;
                if (d->bodyRigLegLen < 1e-4f)
                    d->bodyRigLegLen = (hipW - footW).length();
            }
        }
    } else {
        d->bodyBones.clear();
    }

    d->registerSkinningListener();
    if (!d->addedSoftwareAnimRequest) {
        entity->addSoftwareAnimationRequest(true);
        d->addedSoftwareAnimRequest = true;
    }

    d->calibrated = false;
    d->bodyTorsoStableFrames = 0;
    d->bodyNeutralReady = false;
    d->bodyNeutralCapturedMask = 0;
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
        d->unregisterSkinningListener();
        restoreEntityState();
        setStatusMessage(error);
        emit errorOccurred(error);
        return false;
    }
    d->worker = new MocapInferenceWorker();
    d->worker->mailbox = &d->camera->mailbox();
    configureWorkerSmoothing(d->worker, d->smoothingCutoff);
    {
        const auto fingerSmooth = landmarkSmoothParams(d->smoothingCutoff);
        for (auto& slot : d->fingerScreenFilters)
            for (auto& ax : slot)
                ax = OneEuroFilter(fingerSmooth);
    }
    d->bodyBoneFilters.clear();
    if (!d->worker->predictor.load()) {
        const QString msg = d->worker->predictor.lastError();
        delete d->worker;
        d->worker = nullptr;
        d->camera.reset();
        d->unregisterSkinningListener();
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
            if (!HandCapPredictor::modelsPresent()) {
                setStatusMessage(tr("Downloading hand capture model…"));
                QCoreApplication::processEvents();
                HandCapPredictor::ensureModelsBlocking();
            }
            auto hands = std::make_shared<HandCapPredictor>();
            if (hands->load())
                d->worker->handPredictor = hands;
            else
                setStatusMessage(
                    tr("Hand capture model not loaded (%1) — fingers will use "
                       "the coarse pose fallback.")
                        .arg(hands->lastError()));
        } else {
            // body models unavailable: keep face/head live, drop body cleanly
            setStatusMessage(tr("Body capture unavailable (%1) — driving "
                                "face/head only.").arg(pose->lastError()));
            for (auto& bb : d->bodyBones)
                if (auto* skel = entity->getSkeleton())
                    skel->getBone(bb.boneName)->setManuallyControlled(
                        bb.wasManuallyControlled);
            d->bodyBones.clear();
            d->bodyRetargeter.reset();
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

    if (d->cameraStartupTimer) {
        d->cameraStartupTimer->stop();
        d->cameraStartupTimer->deleteLater();
    }
    d->cameraStartupTimer = new QTimer(this);
    d->cameraStartupTimer->setSingleShot(true);
    connect(d->cameraStartupTimer, &QTimer::timeout, this, [this]() {
        if (d->state != CameraStarting)
            return;
        setStatusMessage(
            tr("Camera opened but no frames arrived. Close other apps using "
               "the webcam%1, then click Preview again.")
                .arg(MocapCameraHints::snapConnectHint()));
        emit errorOccurred(d->status);
        stopPreview();
    });
    d->cameraStartupTimer->start(10000);

    SentryReporter::addBreadcrumb("ai.assist.mocap_live", "preview start");
    GamificationManager::noteFeature(QStringLiteral("mocap"),
                                     GamificationManager::Surface::Gui);
    if (d->showPoseDebug)
        d->poseDebugOverlay.attach(Manager::getSingleton()->getSceneMgr(),
                                   entity->getParentSceneNode());
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
        if (d->cameraStartupTimer) {
            d->cameraStartupTimer->stop();
            d->cameraStartupTimer->deleteLater();
            d->cameraStartupTimer = nullptr;
        }
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
    if (!entity)
        return;

    bool skeletonDriven = false;

    // live drive — morphs + head (face graph)
    if (sample.confidence > 0.f) {
        auto* morphMgr = MorphAnimationManager::instance();
        for (const auto& ch : d->mapping.channels)
            morphMgr->setWeight(entity, ch.meshTargetName,
                                sample.weights[ch.canonicalIndex]);

        if (d->headEnabled && !d->headBone.isEmpty() && entity->hasSkeleton()) {
            if (!d->calibrated) {
                d->neutral = Ogre::Quaternion(
                    sample.headRotation[3], sample.headRotation[0],
                    sample.headRotation[1], sample.headRotation[2]);
                d->calibrated = true;
            }
            const Ogre::Quaternion current(
                sample.headRotation[3], sample.headRotation[0],
                sample.headRotation[1], sample.headRotation[2]);
            Ogre::Quaternion delta = current * d->neutral.Inverse();
            delta = MocapPoseFix::invertCameraPitchDelta(delta);
            // Selfie/webcam preview is mirrored; video-file playback is not.
            if (d->mirroredLivePreview)
                delta = MocapPoseFix::invertCameraYawDelta(delta);
            const Ogre::Quaternion local =
                d->headBindWorld.Inverse() * delta * d->headBindWorld;
            Ogre::SkeletonInstance* skel = entity->getSkeleton();
            Ogre::Bone* bone = skel->getBone(d->headBone.toStdString());
            bone->setOrientation(d->headBindLocal * local);
            bone->needUpdate(true);
            skel->_notifyManualBonesDirty();
            if (auto* states = entity->getAllAnimationStates())
                states->_notifyDirty();
            skeletonDriven = true;
        }
    }

    // live drive — body (independent of face confidence). Landmark directions
    // retarget onto the selection skeleton via BodyRetargeter.
    if (body.valid && d->bodyRetargeter && d->bodyRetargeter->valid()
        && entity->hasSkeleton()) {
        std::array<std::array<float, 4>, 22> canonQuats{};
        for (int r = 0; r < PoseIK::kCanonicalRoles; ++r)
            canonQuats[static_cast<size_t>(r)] = body.quats[r];

        const bool torsoOk =
            (body.resolvedMask & Impl::kTorsoResolvedMask) == Impl::kTorsoResolvedMask;
        if (torsoOk)
            ++d->bodyTorsoStableFrames;
        else
            d->bodyTorsoStableFrames = 0;

        if (torsoOk
            && d->bodyTorsoStableFrames >= Impl::kBodyTorsoStableFrames) {
            if (!d->bodyRetargeter->hasNeutralReference()) {
                d->bodyRetargeter->setNeutralReference(
                    canonQuats, body.resolvedMask, body.world.data(),
                    body.visibility.data());
                d->bodyNeutralCapturedMask = body.resolvedMask;
                const float span = MocapPoseIkFk::canonicalHipFootVerticalSpan(
                    body.world.data(), body.visibility.data());
                if (span > 1e-4f) {
                    d->bodyNeutralLegSpan = span;
                    d->bodyHipHeightFilter =
                        OneEuroFilter(landmarkSmoothParams(d->smoothingCutoff));
                }
                tryCaptureFingerNeutralScreen(body, d->fingerNeutralScreen2d);
                d->haveFingerNeutralScreen =
                    countFingerScreen2dSlots(d->fingerNeutralScreen2d) >= 4;
                d->haveFingerNeutralFlex =
                    tryCaptureFingerNeutralFlex(body, d->fingerNeutralFlex);
            } else if ((d->bodyNeutralCapturedMask & Impl::kTorsoResolvedMask)
                       != Impl::kTorsoResolvedMask) {
                // First neutral was captured before hip/chest were visible —
                // parent-relative math was wrong (world quats as locals).
                d->bodyRetargeter->resetLiveNeutral();
                d->bodyRetargeter->setNeutralReference(
                    canonQuats, body.resolvedMask, body.world.data(),
                    body.visibility.data());
                d->bodyNeutralCapturedMask = body.resolvedMask;
                const float span = MocapPoseIkFk::canonicalHipFootVerticalSpan(
                    body.world.data(), body.visibility.data());
                if (span > 1e-4f) {
                    d->bodyNeutralLegSpan = span;
                    d->bodyHipHeightFilter =
                        OneEuroFilter(landmarkSmoothParams(d->smoothingCutoff));
                }
                tryCaptureFingerNeutralScreen(body, d->fingerNeutralScreen2d);
                d->haveFingerNeutralScreen =
                    countFingerScreen2dSlots(d->fingerNeutralScreen2d) >= 4;
                d->haveFingerNeutralFlex =
                    tryCaptureFingerNeutralFlex(body, d->fingerNeutralFlex);
            } else {
                if (!d->haveFingerNeutralScreen
                    && tryCaptureFingerNeutralScreen(body,
                                                     d->fingerNeutralScreen2d))
                    d->haveFingerNeutralScreen = true;
                if (!d->haveFingerNeutralFlex
                    && tryCaptureFingerNeutralFlex(body, d->fingerNeutralFlex))
                    d->haveFingerNeutralFlex = true;
            }
        }
        d->bodyNeutralReady = d->bodyRetargeter->hasNeutralReference();

        const uint32_t skipHead =
            (d->headEnabled && !d->headBone.isEmpty())
                ? (1u << static_cast<unsigned>(PoseIK::Head))
                : 0u;
        const auto locals = d->bodyRetargeter->evaluateFrame(
            canonQuats, body.resolvedMask, skipHead, body.world.data(),
            body.visibility.data());
        Ogre::SkeletonInstance* skel = entity->getSkeleton();
        const auto boneSmooth = boneOutputSmoothParams(d->smoothingCutoff);
        for (const auto& [handle, local] : locals) {
            Ogre::Bone* bone = skel->getBone(handle);
            bone->setManuallyControlled(true);
            std::array<float, 4> q{
                local.x, local.y, local.z, local.w};
            auto it = d->bodyBoneFilters.find(handle);
            if (it == d->bodyBoneFilters.end())
                it = d->bodyBoneFilters.insert(
                    handle, OneEuroQuatFilter(boneSmooth));
            const std::array<float, 4> smoothed =
                it->filter(q, sample.timeSec);
            bone->setOrientation(Ogre::Quaternion(
                smoothed[3], smoothed[0], smoothed[1], smoothed[2]));
            bone->needUpdate(true);
        }
        for (Ogre::Bone* root : skel->getRootBones())
            root->_update(true, false);
        skel->_updateTransforms();
        if (d->haveEntityBindPosition && d->bodyNeutralLegSpan > 1e-4f
            && d->bodyRigLegLen > 1e-4f) {
            const float span = MocapPoseIkFk::canonicalHipFootVerticalSpan(
                body.world.data(), body.visibility.data());
            if (Ogre::SceneNode* node = entity->getParentSceneNode()) {
                float offsetY = 0.f;
                if (span > 1e-4f) {
                    offsetY = (span - d->bodyNeutralLegSpan)
                              * (d->bodyRigLegLen / d->bodyNeutralLegSpan);
                    const float maxShift = d->bodyRigLegLen * 0.45f;
                    offsetY = std::clamp(offsetY, -maxShift, maxShift);
                    offsetY = static_cast<float>(d->bodyHipHeightFilter.filter(
                        static_cast<double>(offsetY), sample.timeSec));
                }
                Ogre::Vector3 pos = d->entityBindPosition;
                pos.y += offsetY;
                node->setPosition(pos);
            }
        }
        std::array<std::array<float, 2>, AnimationMerger::kFingerSlots>
            fingerLive2d{};
        int fingerDriven = 0;
        if (d->fingerLiveCtx.valid) {
            if (d->haveFingerNeutralFlex
                && (body.hands.left.valid || body.hands.right.valid)) {
                std::array<float, AnimationMerger::kFingerSlots> liveFlex{};
                fillFingerFlexFromHands(body.hands, liveFlex);
                fingerDriven = AnimationMerger::driveFingersLiveFromFlex(
                    skel, liveFlex, d->fingerNeutralFlex, d->fingerLiveCtx);
            }
            if (fingerDriven == 0 && d->haveFingerNeutralScreen) {
                collectLiveFingerScreen2d(body, fingerLive2d);
                smoothFingerScreen2d(fingerLive2d, d->fingerScreenFilters,
                                     sample.timeSec);
                fingerDriven = AnimationMerger::driveFingersLiveFromScreenCrop(
                    skel, d->fingerNeutralScreen2d, fingerLive2d,
                    d->fingerLiveCtx);
            }
            if (fingerDriven > 0)
                skeletonDriven = true;
        }
        for (Ogre::Bone* root : skel->getRootBones())
            root->_update(true, true);
        skel->_notifyManualBonesDirty();
        skel->_updateTransforms();
        if (auto* states = entity->getAllAnimationStates())
            states->_notifyDirty();
        entity->_updateAnimation();
        if (Ogre::SceneNode* node = entity->getParentSceneNode())
            node->needUpdate();
        skeletonDriven = true;

        MocapBodyDriveDebug::logFrame(
            entity, skel, body, d->bodyRetargeter.get(), sample.timeSec,
            d->bodyNeutralReady, d->bodyRetargeter->hasNeutralReference(),
            d->bodyTorsoStableFrames, Impl::kBodyTorsoStableFrames,
            locals.size());
    }

    if (d->showPoseDebug && body.valid && entity) {
        const Ogre::AxisAlignedBox box = entity->getBoundingBox();
        const float height = box.getMaximum().y - box.getMinimum().y;
        d->poseDebugOverlay.update(body, height);
    }

    if (d->state == Recording) {
        d->take.push_back(sample);
        if (d->bodyRetargeter)
            d->bodyTake.push_back(body);
        d->lastSampleTime = sample.timeSec;
    }
}

void MocapController::calibrateNeutral()
{
    resetLiveCaptureCalibration();
    // Next frame with a stable full torso will capture neutral immediately.
    d->bodyTorsoStableFrames = Impl::kBodyTorsoStableFrames;
    setStatusMessage(
        tr("Hold a relaxed neutral pose — face the camera, arms at your sides, "
           "hands open with fingers visible…"));
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
    if (!d->mapping.channels.isEmpty()
        || (d->headEnabled && !d->headBone.isEmpty())) {
        MocapRecorder::FaceRecordOptions options;
        options.clipName = d->clipName;
        options.head = d->headEnabled && !d->headBone.isEmpty();
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

    // body clip: bake the buffered live frames with landmark-direction retarget
    // (same BodyRetargeter path as preview) into a separate undo command.
    if (d->bodyRetargeter && d->bodyTake.size() >= 2) {
        std::vector<BodyLiveFrame> valid;
        valid.reserve(d->bodyTake.size());
        for (const auto& bf : d->bodyTake) {
            if (bf.valid)
                valid.push_back(bf);
        }
        if (valid.size() >= 2) {
            MocapRecorder::BodyRecordOptions bopts;
            bopts.clipName = d->clipName + QStringLiteral("_Body");
            bopts.algorithmUsed = QStringLiteral("pose-ik-landmarks");
            if (d->headEnabled && !d->headBone.isEmpty())
                bopts.skipRolesMask =
                    (1u << static_cast<unsigned>(PoseIK::Head));
            const int fps =
                (d->liveFps >= 5.0)
                    ? std::max(1, static_cast<int>(std::lround(d->liveFps)))
                    : 30;
            auto* bcmd = new RecordBodyClipCommand(d->entityName, std::move(valid),
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
    if (d->haveEntityBindPosition) {
        if (Ogre::SceneNode* node = entity->getParentSceneNode())
            node->setPosition(d->entityBindPosition);
        d->haveEntityBindPosition = false;
    }
    auto* morphMgr = MorphAnimationManager::instance();
    for (auto it = d->savedWeights.begin(); it != d->savedWeights.end(); ++it)
        morphMgr->setWeight(entity, it.key(), it.value());
    entity->setSkipAnimationStateUpdate(d->savedSkipAnimStateUpdate);
    entity->setAlwaysUpdateMainSkeleton(d->savedAlwaysUpdateMainSkeleton);
    if (d->addedSoftwareAnimRequest) {
        entity->removeSoftwareAnimationRequest(true);
        d->addedSoftwareAnimRequest = false;
    }
    if (entity->hasSkeleton()) {
        Ogre::SkeletonInstance* skel = entity->getSkeleton();
        if (d->savedHeadSnapshot && !d->headBone.isEmpty()) {
            Ogre::Bone* bone = skel->getBone(d->headBone.toStdString());
            bone->setOrientation(d->headRestoreLocal);
            bone->setManuallyControlled(d->headWasManuallyControlled);
            d->savedHeadSnapshot = false;
        }
        for (const auto& snap : d->bodyManualRestore) {
            Ogre::Bone* bone = skel->getBone(snap.boneName);
            bone->setOrientation(snap.bindLocal);
            bone->setPosition(snap.bindPosition);
            bone->setManuallyControlled(snap.wasManuallyControlled);
        }
        if (auto* states = entity->getAllAnimationStates()) {
            for (const auto& entry : d->bodyAnimMaskRestore) {
                if (!states->hasAnimationState(entry.animName))
                    continue;
                Ogre::AnimationState* st =
                    states->getAnimationState(entry.animName);
                if (st && st->hasBlendMask())
                    st->setBlendMaskEntry(entry.boneHandle, entry.weight);
            }
        }
        d->bodyAnimMaskRestore.clear();
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
    if (d->cameraStartupTimer) {
        d->cameraStartupTimer->stop();
        d->cameraStartupTimer->deleteLater();
        d->cameraStartupTimer = nullptr;
    }
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
    d->poseDebugOverlay.detach();

    if (d->skinningListener)
        d->unregisterSkinningListener();

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
