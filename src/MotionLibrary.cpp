#include "MotionLibrary.h"

#include <QRandomGenerator>
#include "ModelDownloader.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

namespace {
constexpr const char* kLibraryFile = "motion-library.json";
constexpr const char* kDefaultBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/motion/";
constexpr const char* kBaseUrlSettingsKey = "ai/motionLibraryBaseUrl";
constexpr int kCanonJoints = 22;

// Synonyms → a canonical action keyword. Maps prompt words onto the library's
// actions so e.g. "jog"/"sprint" pick "run", "stand"/"idle" pick "idle".
struct Syn { const char* word; const char* action; };
const Syn kSynonyms[] = {
    {"jog", "run"}, {"sprint", "run"}, {"running", "run"},
    {"walking", "walk"}, {"stroll", "walk"}, {"step", "walk"},
    {"jumping", "jump"}, {"hop", "jump"}, {"leap", "jump"},
    {"dancing", "dance"}, {"spin", "dance"},
    {"marching", "march"},
    {"kicking", "kick"},
    {"punching", "punch"}, {"strike", "punch"}, {"hit", "punch"},
    {"waving", "wave"}, {"greet", "wave"}, {"hello", "wave"},
    {"climbing", "climb"},
    {"idle", "idle"}, {"stand", "idle"}, {"standing", "idle"}, {"rest", "idle"},
    {"sitting", "sit"}, {"seat", "sit"},
    {"throwing", "throw"}, {"toss", "throw"}, {"pitch", "throw"},
    {"box", "boxing"}, {"fight", "boxing"}, {"spar", "boxing"},
};
} // namespace

bool MotionLibrary::loadFromFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_error = QStringLiteral("cannot open motion library: %1").arg(path);
        return false;
    }
    return parse(f.readAll());
}

bool MotionLibrary::loadFromJson(const QByteArray& json) { return parse(json); }

bool MotionLibrary::parse(const QByteArray& json)
{
    m_clips.clear();
    m_error.clear();
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (doc.isNull() || !doc.isObject()) {
        m_error = QStringLiteral("invalid library JSON: %1").arg(perr.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    const QString schema = root.value("schema").toString();
    if (schema != QLatin1String("qtmesh-motion-library-v1") &&
        schema != QLatin1String("qtmesh-motion-library-v2") &&
        schema != QLatin1String("qtmesh-motion-library-v3")) {
        m_error = QStringLiteral("unexpected library schema");
        return false;
    }
    // v3 stores WORLD-space joint orientations (marked "frame":"world"); v1/v2
    // store LOCAL parent-relative rotations. The retarget branches on this.
    m_worldFrame = (root.value("frame").toString() == QLatin1String("world"));
    const int fps = root.value("fps").toInt(30);

    // CMU per-joint world-rest orientation (schema v2+; optional). When present
    // and exactly 22 entries, the retarget uses it for the exact change-of-basis.
    m_cmuRestWorld.clear();
    if (root.contains("cmuRestWorld")) {
        const QJsonArray rest = root.value("cmuRestWorld").toArray();
        if (rest.size() == kCanonJoints) {
            m_cmuRestWorld.reserve(kCanonJoints);
            for (const QJsonValue& qv : rest) {
                const QJsonArray q = qv.toArray();
                m_cmuRestWorld.push_back({ static_cast<float>(q.at(0).toDouble()),
                                           static_cast<float>(q.at(1).toDouble()),
                                           static_cast<float>(q.at(2).toDouble()),
                                           static_cast<float>(q.at(3).toDouble(1.0)) });
            }
        }
    }
    const QJsonArray clips = root.value("clips").toArray();
    for (const QJsonValue& cv : clips) {
        const QJsonObject co = cv.toObject();
        Clip clip;
        clip.action = co.value("action").toString();
        clip.source = co.value("source").toString();
        clip.fps = fps;
        const QJsonArray frames = co.value("quats").toArray();
        clip.quats.reserve(frames.size());
        for (const QJsonValue& fv : frames) {
            const QJsonArray joints = fv.toArray();
            if (joints.size() != kCanonJoints) {   // schema guard
                m_error = QStringLiteral("clip '%1' has %2 joints (expected %3)")
                              .arg(clip.action).arg(joints.size()).arg(kCanonJoints);
                m_clips.clear();
                return false;
            }
            std::vector<std::array<float, 4>> pose(kCanonJoints);
            for (int j = 0; j < kCanonJoints; ++j) {
                const QJsonArray q = joints[j].toArray();
                pose[j] = { static_cast<float>(q.at(0).toDouble()),
                            static_cast<float>(q.at(1).toDouble()),
                            static_cast<float>(q.at(2).toDouble()),
                            static_cast<float>(q.at(3).toDouble(1.0)) };
            }
            clip.quats.push_back(std::move(pose));
        }
        clip.frames = static_cast<int>(clip.quats.size());
        if (clip.frames > 0 && !clip.action.isEmpty())
            m_clips.push_back(std::move(clip));
    }
    if (m_clips.empty()) {
        m_error = QStringLiteral("library contained no usable clips");
        return false;
    }
    return true;
}

std::vector<QString> MotionLibrary::actions() const
{
    std::vector<QString> out;
    for (const auto& c : m_clips) out.push_back(c.action);
    return out;
}

int MotionLibrary::matchPrompt(const QString& prompt, QString* matchedAction) const
{
    const QString p = prompt.toLower();
    auto findAction = [&](const QString& action) -> int {
        for (int i = 0; i < static_cast<int>(m_clips.size()); ++i)
            if (m_clips[i].action.compare(action, Qt::CaseInsensitive) == 0) return i;
        return -1;
    };

    // The v4 library carries SEVERAL takes per action — pick one at random so
    // repeat generates give variety (real-mocap quality is the draw; variety
    // is what the generative path was chasing).
    auto pickAmong = [&](const QString& action) -> int {
        QList<int> hits;
        for (int i = 0; i < static_cast<int>(m_clips.size()); ++i)
            if (m_clips[i].action.compare(action, Qt::CaseInsensitive) == 0)
                hits.append(i);
        if (hits.isEmpty()) return -1;
        if (hits.size() == 1) return hits.first();
        return hits.at(QRandomGenerator::global()->bounded(hits.size()));
    };

    // 1. Direct: a library action name appears in the prompt.
    for (int i = 0; i < static_cast<int>(m_clips.size()); ++i) {
        if (p.contains(m_clips[i].action.toLower())) {
            if (matchedAction) *matchedAction = m_clips[i].action;
            return pickAmong(m_clips[i].action);
        }
    }
    // 2. Synonyms → action.
    for (const auto& s : kSynonyms) {
        if (p.contains(QLatin1String(s.word))) {
            const int idx = findAction(QString::fromLatin1(s.action));
            if (idx >= 0) {
                if (matchedAction) *matchedAction = m_clips[idx].action;
                return pickAmong(m_clips[idx].action);
            }
        }
    }
    return -1;   // caller decides the fallback (e.g. "idle" or an error)
}

// ---------------------------------------------------------------------------
// Download / cache
// ---------------------------------------------------------------------------

QString MotionLibrary::libraryPath()
{
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath(QStringLiteral("ai_models/motion/")
                                   + QString::fromLatin1(kLibraryFile));
}

bool MotionLibrary::libraryPresent() { return QFileInfo::exists(libraryPath()); }

QString MotionLibrary::ensureLibraryBlocking()
{
    const QString dest = libraryPath();
    if (QFileInfo::exists(dest)) return dest;
    if (!qEnvironmentVariableIsEmpty("QTMESH_MOTION_NO_DOWNLOAD"))
        return {};

    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_MOTION_LIBRARY_BASE_URL");
            base = env.isEmpty() ? QString::fromLatin1(kDefaultBaseUrl)
                                 : QString::fromUtf8(env);
        }
    }
    if (base.isEmpty()) return {};
    if (!base.endsWith('/')) base += '/';

    auto* dl = ModelDownloader::instance();
    if (!dl) return {};

    QDir().mkpath(QFileInfo(dest).absolutePath());
    const QString url = base + QString::fromLatin1(kLibraryFile);
    const QString label = QStringLiteral("Motion library");

    QEventLoop loop;
    bool ok = false, timedOut = false;
    auto onDone = QObject::connect(dl, &ModelDownloader::downloadCompleted, &loop,
        [&](const QString& name, const QString&) { if (name == label) { ok = true; loop.quit(); } });
    auto onErr = QObject::connect(dl, &ModelDownloader::downloadError, &loop,
        [&](const QString& name, const QString&) { if (name == label) { ok = false; loop.quit(); } });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() { timedOut = true; loop.quit(); });
    timeout.start(300000);   // 5 min — the library is small (~1 MB)

    dl->startDownload(url, dest, label);
    loop.exec();

    QObject::disconnect(onDone);
    QObject::disconnect(onErr);
    if (timedOut && dl) dl->cancelDownload();

    return (ok && !timedOut && QFileInfo::exists(dest)) ? dest : QString();
}
