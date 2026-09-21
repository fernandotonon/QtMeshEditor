#include "LipsyncController.h"

#include "AudioToFace/A2FPredictor.h"
#include "AudioToFace/LipsyncApply.h"
#include "AudioToFace/WavReader.h"

#include "Manager.h"
#include "MorphAnimationManager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/LipsyncClipCommand.h"

#include <QCoreApplication>
#include <QHash>
#include <QPointer>
#include <QUndoStack>

#include <OgreEntity.h>
#include <OgrePose.h>
#include <OgreMesh.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <thread>

namespace {
LipsyncController* g_instance = nullptr;

/// Selected entity, or null. Mirrors how the other controllers resolve it.
Ogre::Entity* selectedEntity()
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return nullptr;
    for (Ogre::Entity* e : sel->getEntitiesSelectionList())
        if (e) return e;
    // A node may be selected without its entity being in the entity list.
    for (Ogre::SceneNode* n : sel->getNodesSelectionList()) {
        if (!n) continue;
        for (unsigned short i = 0; i < n->numAttachedObjects(); ++i) {
            auto* o = n->getAttachedObject(i);
            if (o && o->getMovableType() == "Entity")
                return static_cast<Ogre::Entity*>(o);
        }
    }
    return nullptr;
}
}  // namespace

struct LipsyncController::Impl {
    std::shared_ptr<std::atomic_bool> cancel = std::make_shared<std::atomic_bool>(false);
};

LipsyncController::LipsyncController(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>())
{
    if (auto* sel = SelectionSet::getSingleton())
        connect(sel, &SelectionSet::selectionChanged,
                this, &LipsyncController::selectionChanged);
}

LipsyncController::~LipsyncController() = default;

LipsyncController* LipsyncController::instance()
{
    if (!g_instance) g_instance = new LipsyncController();
    return g_instance;
}

QObject* LipsyncController::qmlInstance(QQmlEngine*, QJSEngine*)
{
    return instance();
}

void LipsyncController::kill()
{
    delete g_instance;
    g_instance = nullptr;
}

bool LipsyncController::available() const
{
    return AudioToFace::A2FPredictor::available();
}

QStringList LipsyncController::emotionNames() const
{
    QStringList out;
    for (const char* n : AudioToFace::kEmotionNames) out << QString::fromLatin1(n);
    return out;
}

bool LipsyncController::hasRiggableSelection() const
{
    Ogre::Entity* e = selectedEntity();
    if (!e) return false;
    auto* mm = MorphAnimationManager::instance();
    return mm && !mm->morphTargetsFor(e).isEmpty();
}

void LipsyncController::setStatus(const QString& s)
{
    if (m_status == s) return;
    m_status = s;
    emit statusChanged();
}

void LipsyncController::setBusy(bool b)
{
    if (m_busy == b) return;
    m_busy = b;
    emit busyChanged();
}

void LipsyncController::setProgress(int done, int total)
{
    m_progress = done;
    m_progressTotal = total;
    emit progressChanged();
}

void LipsyncController::browseForAudio() { emit browseRequested(); }

void LipsyncController::cancel()
{
    if (m_impl) m_impl->cancel->store(true);
    setStatus(QStringLiteral("Cancelling…"));
}

bool LipsyncController::generateAsync(const QString& audioPath,
                                      const QString& clipName,
                                      double fps,
                                      const QList<double>& emotion)
{
    if (m_busy) return false;
    if (!available()) {
        emit finished(false, QStringLiteral(
            "Audio2Face needs ONNX Runtime; this build was made without it."));
        return false;
    }
    Ogre::Entity* entity = selectedEntity();
    if (!entity) {
        emit finished(false, QStringLiteral("Select a head mesh first."));
        return false;
    }
    auto* mm = MorphAnimationManager::instance();
    const QStringList targets = mm ? mm->morphTargetsFor(entity) : QStringList{};
    if (targets.isEmpty()) {
        // The actionable part is where to GET the targets; this is the most
        // likely way a first attempt fails.
        emit finished(false, QStringLiteral(
            "This mesh has no morph targets. Add ARKit blendshapes first "
            "(Vertex Morph Animation → Add ARKit Blendshapes)."));
        return false;
    }

    QString err;
    auto wav = std::make_shared<AudioToFace::WavData>(
        AudioToFace::readWav(audioPath, &err));
    if (!wav->valid()) {
        emit finished(false, err.isEmpty()
            ? QStringLiteral("Could not read %1").arg(audioPath) : err);
        return false;
    }

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.audio2face"),
        QStringLiteral("gui lipsync %1s @%2fps")
            .arg(wav->durationSec(), 0, 'f', 1).arg(fps));

    m_impl->cancel->store(false);
    setBusy(true);
    setProgress(0, 0);

    // MAIN thread: make sure the models are on disk BEFORE the worker starts.
    // ModelFetch drives the singleton ModelDownloader, its QNetworkAccessManager
    // and its timers, all of which live on this thread — reaching them from a
    // detached std::thread is a Qt thread-affinity violation, and on a first
    // run it would also give the singleton worker affinity for every later
    // GUI use. Same order FaceRigController uses, and it lets the status line
    // say "Downloading…" only when something really is being fetched.
    const bool needsDownload = !AudioToFace::A2FPredictor::present();
    setStatus(needsDownload ? QStringLiteral("Downloading model (~320 MB)…")
                            : QStringLiteral("Preparing…"));
    if (needsDownload) QCoreApplication::processEvents();
    QString fetchErr;
    if (AudioToFace::A2FPredictor::ensureModelBlocking(&fetchErr).isEmpty()) {
        setBusy(false);
        setStatus(QString());
        emit finished(false, fetchErr.isEmpty()
            ? QStringLiteral("Audio2Face models are unavailable.")
            : fetchErr);
        return false;
    }
    setStatus(QStringLiteral("Preparing…"));

    auto emo = std::make_shared<std::vector<float>>();
    for (double v : emotion) emo->push_back(float(qBound(0.0, v, 1.0)));

    auto cancelFlag = m_impl->cancel;
    const std::string entName = entity->getName();
    auto targetsCopy = std::make_shared<QStringList>(targets);
    const QString clip = clipName;
    QPointer<LipsyncController> self(this);

    // WORKER: model load (possibly a ~320 MB first-use download) plus one
    // inference and solve per frame. Ogre is never touched here.
    std::thread([self, wav, emo, fps, clip, entName, cancelFlag, targetsCopy]() {
        // The files are already on disk (fetched on the main thread above),
        // so this only parses them — no network, no Qt singletons.
        auto predictor = std::make_shared<AudioToFace::A2FPredictor>();
        QString loadErr;
        QMetaObject::invokeMethod(qApp, [self]() {
            if (self) self->setStatus(QStringLiteral("Loading model…"));
        }, Qt::QueuedConnection);

        if (!predictor->load(&loadErr)) {
            QMetaObject::invokeMethod(qApp, [self, loadErr]() {
                if (!self) return;
                self->setBusy(false);
                self->setStatus(QString());
                emit self->finished(false, loadErr);
            }, Qt::QueuedConnection);
            return;
        }

        AudioToFace::PredictOptions opts;
        opts.fps = fps;
        opts.emotion = *emo;
        opts.progress = [self, cancelFlag](int done, int total) {
            if (cancelFlag->load()) return false;
            QMetaObject::invokeMethod(qApp, [self, done, total]() {
                if (!self) return;
                self->setProgress(done, total);
                self->setStatus(QStringLiteral("Solving %1/%2…").arg(done).arg(total));
            }, Qt::QueuedConnection);
            return true;
        };

        auto result = std::make_shared<AudioToFace::PredictResult>(
            predictor->predict(wav->samples, wav->sampleRate, wav->channels, opts));
        auto poses = std::make_shared<QStringList>(predictor->poseNames());

        // MAIN: writing keyframes touches Ogre and the undo stack.
        QMetaObject::invokeMethod(qApp,
            [self, result, poses, entName, clip, targetsCopy, cancelFlag]() {
            if (!self) return;
            self->setBusy(false);
            self->setProgress(0, 0);
            self->setStatus(QString());

            // A cancelled run reports !ok() even though it may hold frames:
            // those are a truncated take and must never be committed as if
            // the run had finished.
            if (!result->ok()) {
                emit self->finished(false,
                    (result->cancelled || cancelFlag->load())
                        ? QStringLiteral("Lipsync cancelled.")
                        : result->error);
                return;
            }
            // Re-resolve by NAME: the selection may have changed while the
            // worker ran, and a cached pointer could be dangling.
            Ogre::Entity* e = nullptr;
            for (Ogre::Entity* cand : Manager::getSingleton()->getEntities())
                if (cand && cand->getName() == entName) { e = cand; break; }
            if (!e) {
                emit self->finished(false,
                    QStringLiteral("The mesh is no longer in the scene."));
                return;
            }

            // Name binding and key-time selection come from LipsyncApply,
            // shared with the CLI and the MCP tool. Only the WRITE differs
            // here: the GUI turns the take into pose-index keys for one
            // undoable command instead of calling writeWeightKeyOn directly.
            const AudioToFace::NameBinding binding =
                AudioToFace::bindPoseNames(*poses, *targetsCopy);
            if (!binding.ok()) {
                emit self->finished(false, binding.error.isEmpty()
                    ? QStringLiteral("No ARKit channels matched this mesh.")
                    : binding.error);
                return;
            }
            const int matched = int(binding.matched.size());
            const std::vector<size_t> keyTimes =
                AudioToFace::selectKeyFrames(*result, binding);

            // Build the take as pose-index keys and commit it through ONE
            // undoable command. A `beginMacro` around direct writeWeightKeyOn
            // calls would NOT be undoable at all: a macro groups commands
            // pushed while it is open, and nothing was ever pushed.
            Ogre::MeshPtr mesh = e->getMesh();
            if (!mesh) {
                emit self->finished(false,
                    QStringLiteral("The mesh is no longer in the scene."));
                return;
            }
            QHash<QString, unsigned short> poseIndexByName;
            const auto& poseList = mesh->getPoseList();
            for (unsigned short pi = 0; pi < poseList.size(); ++pi)
                poseIndexByName.insert(
                    QString::fromStdString(poseList[pi]->getName()), pi);

            std::vector<LipsyncClipCommand::Key> cmdKeys;
            cmdKeys.reserve(keyTimes.size());
            int keys = 0;
            for (size_t fi : keyTimes) {
                const auto& f = result->frames[fi];
                LipsyncClipCommand::Key k;
                k.time = float(f.timeSec);
                for (int i = 0; i < poses->size() && i < int(f.weights.size()); ++i) {
                    if (binding.poseTarget[size_t(i)].isEmpty()) continue;
                    const auto it =
                        poseIndexByName.constFind(binding.poseTarget[size_t(i)]);
                    if (it == poseIndexByName.constEnd()) continue;
                    k.poseRefs.emplace_back(
                        *it, std::clamp(f.weights[size_t(i)], 0.0f, 1.0f));
                }
                if (k.poseRefs.empty()) continue;
                keys += int(k.poseRefs.size());
                cmdKeys.push_back(std::move(k));
            }

            if (!cmdKeys.empty()) {
                auto* cmd = new LipsyncClipCommand(entName, clip,
                                                   std::move(cmdKeys));
                if (auto* undo = UndoManager::getSingleton(); undo && undo->stack())
                    undo->stack()->push(cmd);   // push() runs redo()
                else { cmd->redo(); delete cmd; }
            }

            if (keys == 0) {
                emit self->finished(false, QStringLiteral(
                    "The solve produced no motion — the audio may be silent."));
                return;
            }
            SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.audio2face"),
                QStringLiteral("gui lipsync ok: %1 keys").arg(keys));
            emit self->finished(true, QStringLiteral(
                "Lipsync '%1': %2 frames, %3 keyframes on %4 channels.")
                .arg(clip).arg(result->frames.size()).arg(keys).arg(matched));
        }, Qt::QueuedConnection);
    }).detach();

    return true;
}
