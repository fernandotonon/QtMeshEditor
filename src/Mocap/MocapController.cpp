#include "MocapController.h"

#ifdef ENABLE_MOCAP
#include "MocapRecorder.h"
#include "OneEuroFilter.h"
#include "VideoFrameSource.h"
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

#include <QCoreApplication>

namespace {
MocapController* s_instance = nullptr;
#ifdef ENABLE_MOCAP
// queued sampleReady(FaceSample, QImage) needs the metatype
struct FaceSampleMetaTypeRegistrar {
    FaceSampleMetaTypeRegistrar() { qRegisterMetaType<FaceSample>("FaceSample"); }
};
const FaceSampleMetaTypeRegistrar faceSampleRegistrar;
#endif
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
QString MocapController::clipName() const { return QStringLiteral("FaceCap"); }
void MocapController::setClipName(const QString&) {}
double MocapController::smoothingCutoff() const { return 1.0; }
void MocapController::setSmoothingCutoff(double) {}
void MocapController::refreshDevices() {}
bool MocapController::startPreview(const QString&) { return false; }
void MocapController::stopPreview() {}
bool MocapController::startRecording() { return false; }
void MocapController::stopRecording() {}
void MocapController::calibrateNeutral() {}

#else  // ENABLE_MOCAP

// -----------------------------------------------------------------------------
// Worker: drains the camera mailbox through the predictor off the main thread.
// -----------------------------------------------------------------------------

class MocapInferenceWorker : public QObject
{
    Q_OBJECT
public:
    FrameMailbox* mailbox = nullptr;
    FaceCapPredictor predictor;
    std::array<OneEuroFilter, 52> weightFilters;
    OneEuroQuatFilter headFilter;
    bool smooth = true;

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
            // downscale sparsely for the HUD preview (payload stays small)
            QImage preview;
            if (frame.frameIndex % 3 == 0)
                preview = frame.image.scaledToWidth(240, Qt::FastTransformation);
            emit sampleReady(s, preview);
        }
    }

signals:
    void sampleReady(const FaceSample& sample, const QImage& preview);
};

struct MocapController::Impl {
    State state = Idle;
    QString status;
    QString clipName = QStringLiteral("FaceCap");
    double smoothingCutoff = 1.0;

    std::unique_ptr<CameraFrameSource> camera;
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

    // recording
    bool recordPending = false;
    std::vector<FaceSample> take;
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
}

MocapController::~MocapController()
{
    stopPreview();
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

    // models first (blocking download with a visible status)
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

    d->entityName = entity->getName();
    d->mapping = FaceCapMapper::build(
        MorphAnimationManager::instance()->morphTargetsFor(entity));
    d->headBone = MocapRecorder::resolveHeadBone(entity);
    emit mappingChanged();
    if (d->mapping.channels.isEmpty() && d->headBone.isEmpty()) {
        setStatusMessage(
                  tr("The selection has no ARKit-style morph targets and no "
                     "Head bone — nothing to drive."));
        emit errorOccurred(d->status);
        return false;
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
    if (!d->headBone.isEmpty() && entity->hasSkeleton()) {
        Ogre::SkeletonInstance* skel = entity->getSkeleton();
        Ogre::Bone* bone = skel->getBone(d->headBone.toStdString());
        d->headWasManuallyControlled = bone->isManuallyControlled();
        d->headBindLocal = bone->getOrientation();
        d->headBindWorld = bone->_getDerivedOrientation();
        bone->setManuallyControlled(true);
    }
    d->calibrated = false;
    d->faceDetected = false;
    d->liveFps = 0;
    d->lastArrival = -1;
    d->sampleCount = 0;
    d->clock.start();

    // camera + worker
    d->camera = std::make_unique<CameraFrameSource>(deviceId);
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
            &MocapController::onSample, Qt::QueuedConnection);
    d->workerThread.start();

    d->state = CameraStarting;
    emit stateChanged();
    setStatusMessage(tr("Starting camera…"));
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
    if (d->state == Idle)
        return;
    if (d->state == CameraStarting) {
        d->state = Previewing;
        emit stateChanged();
        setStatusMessage(tr("Live — driving the selection."));
    }

    // HUD
    d->faceDetected = sample.confidence > 0.f;
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

    // live drive — head
    if (!d->headBone.isEmpty() && entity->hasSkeleton()) {
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

    if (d->state == Recording) {
        d->take.push_back(sample);
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
    MocapRecorder::FaceRecordOptions options;
    options.clipName = d->clipName;
    options.head = !d->headBone.isEmpty();
    auto* cmd = new RecordMocapClipCommand(d->entityName, d->take, d->mapping,
                                           options);
    UndoManager::getSingleton()->push(cmd);
    const auto& report = cmd->report();
    if (!report.ok()) {
        setStatusMessage(report.error);
        return;
    }
    setStatusMessage(
              tr("Recorded %1s → clip '%2' (%3 keyframes) — Ctrl+Z to discard.")
                  .arg(report.clipLength, 0, 'f', 1)
                  .arg(report.clipName)
                  .arg(report.keyframesWritten + report.headKeyframesWritten));
    GamificationManager::noteOperation(
        QStringLiteral("mocap_face"),
        {{QStringLiteral("frames"), static_cast<qint64>(report.framesProcessed)},
         {QStringLiteral("keyframes"),
          static_cast<qint64>(report.keyframesWritten
                              + report.headKeyframesWritten)}},
        GamificationManager::Surface::Gui);
}

void MocapController::restoreEntityState()
{
    Ogre::Entity* entity = d->entity();
    if (!entity)
        return;
    auto* morphMgr = MorphAnimationManager::instance();
    for (auto it = d->savedWeights.begin(); it != d->savedWeights.end(); ++it)
        morphMgr->setWeight(entity, it.key(), it.value());
    if (!d->headBone.isEmpty() && entity->hasSkeleton()) {
        Ogre::SkeletonInstance* skel = entity->getSkeleton();
        Ogre::Bone* bone = skel->getBone(d->headBone.toStdString());
        bone->setOrientation(d->headBindLocal);
        bone->setManuallyControlled(d->headWasManuallyControlled);
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
        d->workerThread.wait(2000);
    }
    d->worker = nullptr;  // deleted via QThread::finished
    d->camera.reset();
    d->injectedSource = nullptr;

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
