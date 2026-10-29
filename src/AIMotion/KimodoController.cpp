#include "KimodoController.h"
#include "KimodoNative.h"

#include "KimodoPredictor.h"
#include "MotionSceneApply.h"
#include "../AnimationControlController.h"
#include "../AppStorage.h"
#include "../GamificationManager.h"
#include "../Manager.h"
#include "../SentryReporter.h"

#include <OgreEntity.h>

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QPointer>
#include <QRegularExpression>
#include <QThread>
#include <QUrl>

#include <memory>

namespace AIMotion {

namespace {

KimodoController* s_instance = nullptr;

QString motionRoot() { return QDir(AppStorage::persistentRoot()).filePath(QStringLiteral("generated_motion")); }

QWidget* dialogParent()
{
    QApplication::processEvents();
    QWidget* p = QApplication::activeWindow();
    if (p) { p->raise(); p->activateWindow(); }
    return p;
}

} // namespace

KimodoController* KimodoController::instance()
{
    if (!s_instance) s_instance = new KimodoController();
    return s_instance;
}

KimodoController* KimodoController::qmlInstance(QQmlEngine*, QJSEngine*)
{
    KimodoController* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void KimodoController::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

KimodoController::KimodoController(QObject* parent) : QObject(parent)
{
    refreshRuntime();
}

KimodoController::~KimodoController()
{
    if (m_worker) {
        m_cancel = true;   // predict() terminates the sidecar on its next poll
        m_worker->wait();
        delete m_worker;
    }
}

void KimodoController::refreshRuntime()
{
    const bool avail = KimodoPredictor::runtimeAvailable();
    const QString desc = KimodoPredictor::runtimeDescription();
    if (avail == m_runtimeAvailable && desc == m_runtimeDescription) return;
    m_runtimeAvailable = avail;
    m_runtimeDescription = desc;
    emit runtimeChanged();
}

bool KimodoController::nativeDownloadable() const
{
    return KimodoNative::buildSupported() && !KimodoNative::modelsInstalled();
}

QString KimodoController::nativeDownloadSize() const
{
    return QStringLiteral("%1 GB").arg(double(KimodoNative::missingBytes()) / 1e9, 0, 'f', 1);
}

void KimodoController::downloadNativeModels()
{
    if (m_busy || !nativeDownloadable()) return;
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.kimodo"), QStringLiteral("download native models"));
    m_cancel = false;
    setBusy(true);
    QString err;
    const bool ok = KimodoNative::downloadModels(&err, [this](const QString&, int done, int total) {
        m_stage = QStringLiteral("downloading models (%1/%2 files)").arg(done).arg(total);
        m_progress = total > 0 ? double(done) / double(total) : -1.0;
        emit progressChanged();
        return !m_cancel.load();
    });
    setBusy(false);
    m_stage.clear();
    m_progress = -1.0;
    emit progressChanged();
    refreshRuntime();
    emit runtimeChanged();   // nativeDownloadable/size change even when availability does not
    setStatus(ok ? QStringLiteral("Kimodo models downloaded: generation now runs natively, without Python.")
                 : QStringLiteral("Kimodo model download failed: %1").arg(err), ok);
}

void KimodoController::setStatus(const QString& s, bool ok)
{
    m_status = s;
    m_lastOk = ok;
    emit statusChanged();
}

void KimodoController::setBusy(bool b)
{
    if (m_busy == b) return;
    m_busy = b;
    emit busyChanged();
}

Ogre::Entity* KimodoController::entityByName(const std::string& name) const
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    for (auto* e : mgr->getEntities())
        if (e && e->getMovableType() == "Entity" && e->getName() == name && e->hasSkeleton()) return e;
    return nullptr;
}

Ogre::Entity* KimodoController::targetEntity() const
{
    if (auto* acc = AnimationControlController::instance())
        if (Ogre::Entity* e = acc->selectedEntity(); e && e->hasSkeleton()) return e;
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    for (auto* e : mgr->getEntities())
        if (e && e->getMovableType() == "Entity" && e->hasSkeleton()) return e;
    return nullptr;
}

bool KimodoController::parsePromptSegments(const QString& prompt, double defaultDuration,
                                           QStringList* texts, QList<double>* durations, QString* error)
{
    static const QRegularExpression trailing(QStringLiteral("^(.*?)\\s*@\\s*([0-9]+(?:\\.[0-9]+)?)\\s*s?\\s*$"));
    texts->clear();
    durations->clear();
    for (const QString& raw : prompt.split(QLatin1Char('\n'))) {
        QString line = raw.trimmed();
        if (line.isEmpty()) continue;
        double d = defaultDuration;
        if (const auto m = trailing.match(line); m.hasMatch()) {
            line = m.captured(1).trimmed();
            d = m.captured(2).toDouble();
        }
        if (line.isEmpty() || !(d > 0.0) || d > 600.0) {
            if (error) *error = QStringLiteral("'%1': each line needs text and a duration in (0, 600] seconds").arg(raw.trimmed());
            return false;
        }
        *texts << line;
        *durations << d;
    }
    if (texts->isEmpty()) {
        if (error) *error = QStringLiteral("Enter a prompt (e.g. \"a person walks forward and waves\").");
        return false;
    }
    return true;
}

bool KimodoController::generate(const QString& prompt, double duration, int variations, int seed, bool rootMotion,
                                bool footPin)
{
    if (m_busy) return false;
    refreshRuntime();
    Ogre::Entity* entity = targetEntity();
    if (!entity) { setStatus(QStringLiteral("Select a rigged mesh first: Kimodo motion needs a skeleton."), false); return false; }

    KimodoPredictor::Options o;
    QStringList texts;
    QList<double> durations;
    QString err;
    if (!parsePromptSegments(prompt, duration > 0.0 ? duration : 5.0, &texts, &durations, &err)) { setStatus(err, false); return false; }
    for (int i = 0; i < texts.size(); ++i) o.segments.push_back({texts[i], durations[i]});
    o.numSamples = qBound(1, variations, 16);
    o.hasSeed = seed >= 0;
    o.seed = qMax(0, seed);
    o.rootMotion = rootMotion;
    if (!KimodoPredictor::mockRequested(o) && !m_runtimeAvailable) { setStatus(m_runtimeDescription, false); return false; }

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.kimodo"),
                                  QStringLiteral("GUI generate segments=%1 variations=%2").arg(o.segments.size()).arg(o.numSamples));
    GamificationManager::noteFeature(QStringLiteral("animation_blend"));

    const std::string entityName = entity->getName();
    auto result = std::make_shared<KimodoPredictor::Result>();
    m_cancel = false;
    m_stage = QStringLiteral("starting");
    m_progress = -1.0;
    emit progressChanged();
    setStatus(QStringLiteral("Generating with Kimodo… (the first run downloads the models)"), true);
    setBusy(true);

    QPointer<KimodoController> self(this);
    m_worker = QThread::create([this, o, result]() {
        *result = KimodoPredictor::predict(o, [this](const QString& stage, int done, int total) {
            const double p = total > 0 ? double(done) / double(total) : -1.0;
            QMetaObject::invokeMethod(this, [this, stage, p]() {
                m_stage = stage;
                m_progress = p;
                emit progressChanged();
            }, Qt::QueuedConnection);
            return !m_cancel.load();
        });
    });
    connect(m_worker, &QThread::finished, this, [this, self, result, entityName, rootMotion, footPin]() {
        if (!self) return;
        m_worker->deleteLater();
        m_worker = nullptr;
        setBusy(false);
        m_stage.clear();
        m_progress = -1.0;
        emit progressChanged();
        if (!result->ok) {
            SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.kimodo"), QStringLiteral("GUI generate failed: %1").arg(result->error));
            setStatus(result->error == QLatin1String("cancelled") ? QStringLiteral("Cancelled.") : result->error, false);
            return;
        }
        m_lastMotionDir = result->outputDir;
        Ogre::Entity* target = entityByName(entityName);
        if (!target) { setStatus(QStringLiteral("The rig was removed while Kimodo ran; the motion is saved in %1.").arg(result->outputDir), false); return; }
        applyFiles(target, result->files, rootMotion, footPin);
    });
    m_worker->start();
    return true;
}

void KimodoController::cancel()
{
    if (!m_busy) return;
    m_cancel = true;
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("Kimodo generation cancelled"));
}

bool KimodoController::applyFiles(Ogre::Entity* entity, const QStringList& files, bool rootMotion, bool footPin)
{
    std::vector<Motion> motions;
    QString err;
    if (!loadMotionFiles(files, &motions, &err)) { setStatus(err, false); return false; }
    SceneApplyOptions so;
    so.rootMotion = rootMotion;
    so.footPin = footPin;
    const SceneApplyResult r = applyMotionsToEntity(entity, motions, so);
    if (!r.ok) { setStatus(r.error, false); return false; }
    if (auto* acc = AnimationControlController::instance()) acc->updateAnimationTree();
    m_lastClips = r.clips;
    const ApplyOutcome& first = r.outcomes.front();
    setStatus(QStringLiteral("Added %1 to %2: %3 bones driven (%4 fingers), %5 rig bones undriven.")
                  .arg(r.clips.join(QStringLiteral(", ")), QString::fromStdString(entity->getName()))
                  .arg(first.mappedBones).arg(first.fingerBones).arg(first.unmappedTarget.size()),
              true);
    emit applied(QString::fromStdString(entity->getName()), r.clips);
    return true;
}

bool KimodoController::applyMotionFiles(const QStringList& files, bool rootMotion, bool footPin)
{
    if (m_busy) return false;
    Ogre::Entity* entity = targetEntity();
    if (!entity) { setStatus(QStringLiteral("Select a rigged mesh first."), false); return false; }
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.kimodo"), QStringLiteral("GUI apply saved motion files=%1").arg(files.size()));
    return applyFiles(entity, files, rootMotion, footPin);
}

bool KimodoController::applyMotionFile(bool rootMotion, bool footPin)
{
    const QString start = m_lastMotionDir.isEmpty() ? motionRoot() : m_lastMotionDir;
    const QStringList files = QFileDialog::getOpenFileNames(dialogParent(), QStringLiteral("Apply saved motion"), start,
                                                            QStringLiteral("QtMeshEditor motion (*.npz)"), nullptr,
                                                            QFileDialog::DontUseNativeDialog);
    return !files.isEmpty() && applyMotionFiles(files, rootMotion, footPin);
}

void KimodoController::openMotionFolder()
{
    const QString dir = m_lastMotionDir.isEmpty() ? motionRoot() : m_lastMotionDir;
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

} // namespace AIMotion
