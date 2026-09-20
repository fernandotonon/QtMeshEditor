#include "LipsyncController.h"

#include "AudioToFace/A2FPredictor.h"
#include "AudioToFace/WavReader.h"

#include "Manager.h"
#include "MorphAnimationManager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"

#include <QCoreApplication>
#include <QHash>
#include <QPointer>
#include <QUndoStack>

#include <OgreEntity.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <thread>

namespace {
LipsyncController* g_instance = nullptr;

const char* const kEmotionNames[] = {
    "amazement", "anger", "cheekiness", "disgust", "fear",
    "grief", "joy", "outofbreath", "pain", "sadness",
};

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
    for (const char* n : kEmotionNames) out << QString::fromLatin1(n);
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
    setStatus(QStringLiteral("Preparing…"));
    setProgress(0, 0);

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

            if (!result->ok()) {
                emit self->finished(false, cancelFlag->load()
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

            // Match pose names to the mesh's own spelling, case-insensitively
            // and ignoring a leading underscore.
            auto canonical = [](QString n) {
                while (n.startsWith(QLatin1Char('_'))) n.remove(0, 1);
                return n.toLower();
            };
            QHash<QString, QString> byName;
            for (const QString& t : *targetsCopy) byName.insert(canonical(t), t);

            auto* undo = UndoManager::getSingleton();
            auto* stack = undo ? undo->stack() : nullptr;
            if (stack) stack->beginMacro(QStringLiteral("Lipsync"));

            constexpr float kEps = 0.01f;
            std::vector<float> last(size_t(poses->size()), -1.0f);
            int keys = 0, matched = 0;
            const std::string clipStd = clip.toStdString();
            for (size_t fi = 0; fi < result->frames.size(); ++fi) {
                const auto& f = result->frames[fi];
                const bool isLast = (fi + 1 == result->frames.size());
                for (int i = 0; i < poses->size() && i < int(f.weights.size()); ++i) {
                    const QString target = byName.value(canonical(poses->at(i)));
                    if (target.isEmpty()) continue;
                    if (fi == 0) ++matched;
                    const float w = f.weights[size_t(i)];
                    const bool changed = std::abs(w - last[size_t(i)]) >= kEps;
                    if (!changed && !isLast && last[size_t(i)] >= 0.0f) continue;
                    if (MorphAnimationManager::writeWeightKeyOn(
                            e, clipStd, target.toStdString(), float(f.timeSec), w)) {
                        last[size_t(i)] = w;
                        ++keys;
                    }
                }
            }
            if (stack) stack->endMacro();
            e->refreshAvailableAnimationState();

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
