#include "MotionLibrary.h"

#include "MotionInbetween.h"   // canonicalJointCountV2() for schema v4
#include <QRandomGenerator>
#include <algorithm>
#include <cmath>
#include "ModelDownloader.h"
#include "SentryReporter.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

namespace {
constexpr const char* kLibraryFile = "motion-library.json";        // V1 (v3 schema)
constexpr const char* kLibraryFileV2 = "motion-library-v2.json";   // V2 (v4 schema, fingers)
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
    // "march"/"marching" resolve to WALK, not to a `march` action. The curated
    // library has never contained a march clip, and the t2m model's march was
    // trained on leftover CMU corpus whose ankle folds ~106 deg (the foot
    // perpendicular to the shin in 100% of windows) — it rendered as a twisted
    // mess and was dropped from the model vocab in #837. With no clip and no
    // vocab entry, "marching" resolved to nothing and returned a hard error, so
    // point it at the nearest good action instead: a marching gait IS a walk.
    {"marching", "walk"}, {"march", "walk"},
    {"kicking", "kick"},
    {"punching", "punch"}, {"strike", "punch"}, {"hit", "punch"},
    {"waving", "wave"}, {"greet", "wave"}, {"hello", "wave"},
    {"climbing", "climb"},
    {"idle", "idle"}, {"stand", "idle"}, {"standing", "idle"}, {"rest", "idle"},
    {"sitting", "sit"}, {"seat", "sit"},
    {"throwing", "throw"}, {"toss", "throw"}, {"pitch", "throw"},
    {"box", "boxing"}, {"fight", "boxing"}, {"spar", "boxing"},
    // #837 v5 corpus actions
    {"die", "death"}, {"dying", "death"}, {"killed", "death"}, {"dead", "death"},
    {"attacking", "attack"}, {"swing", "attack"}, {"slash", "attack"},
    {"crawling", "crawl"},
    {"rolling", "roll"}, {"dodge", "roll"},
    {"swimming", "swim"},
    {"praying", "pray"}, {"kneel", "pray"},
    {"grab", "pickup"}, {"pick", "pickup"}, {"lift", "pickup"},
    {"handshake", "shake"}, {"shakehand", "shake"},
    {"sing", "sing"}, {"singing", "sing"},
    {"fly", "fly"}, {"flying", "fly"},
    {"strafe", "strafeleft"},
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
        schema != QLatin1String("qtmesh-motion-library-v3") &&
        schema != QLatin1String("qtmesh-motion-library-v4")) {
        m_error = QStringLiteral("unexpected library schema");
        return false;
    }
    // v4 folds the fingers INTO the canonical skeleton (52 joints: 22 body + 30
    // finger); v1..v3 are 22-joint body-only (fingers ride the separate
    // `fingers` side-channel). The joint count per pose depends on the schema.
    m_jointCount = (schema == QLatin1String("qtmesh-motion-library-v4"))
        ? MotionInbetween::canonicalJointCountV2()   // 52
        : kCanonJoints;                               // 22
    const int nJoints = m_jointCount;
    // v3/v4 store WORLD-space joint orientations (marked "frame":"world"); v1/v2
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
            if (joints.size() != nJoints) {   // schema guard (22 v1..v3 / 52 v4)
                m_error = QStringLiteral("clip '%1' has %2 joints (expected %3)")
                              .arg(clip.action).arg(joints.size()).arg(nJoints);
                m_clips.clear();
                return false;
            }
            std::vector<std::array<float, 4>> pose(nJoints);
            for (int j = 0; j < nJoints; ++j) {
                const QJsonArray q = joints[j].toArray();
                pose[j] = { static_cast<float>(q.at(0).toDouble()),
                            static_cast<float>(q.at(1).toDouble()),
                            static_cast<float>(q.at(2).toDouble()),
                            static_cast<float>(q.at(3).toDouble(1.0)) };
            }
            clip.quats.push_back(std::move(pose));
        }
        clip.frames = static_cast<int>(clip.quats.size());
        // Optional per-clip source-bind orientations (bind-referenced
        // retarget). Malformed/absent → empty, the standing-pose path runs.
        const QJsonArray rest = co.value("restWorld").toArray();
        if (rest.size() == nJoints) {
            clip.restWorld.reserve(nJoints);
            for (int j = 0; j < nJoints; ++j) {
                const QJsonArray q = rest[j].toArray();
                clip.restWorld.push_back({
                    static_cast<float>(q.at(0).toDouble()),
                    static_cast<float>(q.at(1).toDouble()),
                    static_cast<float>(q.at(2).toDouble()),
                    static_cast<float>(q.at(3).toDouble(1.0))});
            }
        }
        const QJsonArray rdir = co.value("restDir").toArray();
        if (rdir.size() == nJoints) {
            clip.restDir.reserve(nJoints);
            for (int j = 0; j < nJoints; ++j) {
                const QJsonArray v = rdir[j].toArray();
                clip.restDir.push_back({
                    static_cast<float>(v.at(0).toDouble()),
                    static_cast<float>(v.at(1).toDouble()),
                    static_cast<float>(v.at(2).toDouble())});
            }
        }
        // #954: bind-anchored roll baseline (22 body roles).
        const QJsonArray rrArr = co.value("refRoll").toArray();
        if (rrArr.size() >= 22) {
            clip.refRoll.reserve(rrArr.size());
            for (const auto& v : rrArr)
                clip.refRoll.push_back(static_cast<float>(v.toDouble()));
        }
        const QJsonArray rootYArr = co.value("rootY").toArray();
        if (rootYArr.size() == clip.frames) {
            clip.rootY.reserve(clip.frames);
            for (const auto& yv : rootYArr)
                clip.rootY.push_back(static_cast<float>(yv.toDouble()));
        }
        // #838 finger animation: frames × 30 × [x,y,z,w] local curl.
        // 30 = AnimationMerger::kFingerSlots (2 sides × 5 fingers × 3 segments;
        // literal here because this core stays Ogre-free). Rows are indexed
        // with kFingerSlots offsets downstream, so enforce the exact width at
        // parse time like the other joint arrays.
        constexpr int kFingerSlots = 30;
        const QJsonArray fingersArr = co.value("fingers").toArray();
        if (fingersArr.size() == clip.frames) {
            clip.fingers.reserve(clip.frames);
            bool ok = true;
            for (const auto& fv : fingersArr) {
                const QJsonArray slotArr = fv.toArray();
                if (slotArr.size() != kFingerSlots) { ok = false; break; }
                std::vector<std::array<float, 4>> row;
                row.reserve(slotArr.size());
                for (const auto& qv : slotArr) {
                    const QJsonArray q = qv.toArray();
                    if (q.size() != 4) { ok = false; break; }
                    row.push_back({static_cast<float>(q.at(0).toDouble()),
                                   static_cast<float>(q.at(1).toDouble()),
                                   static_cast<float>(q.at(2).toDouble()),
                                   static_cast<float>(q.at(3).toDouble(1.0))});
                }
                if (!ok) break;
                clip.fingers.push_back(std::move(row));
            }
            if (!ok) clip.fingers.clear();
        }
        // #838 finger REST directions (kFingerSlots × [x,y,z]) — enables the
        // relative-bend finger retarget (avoids the over-bend from differing
        // rig rest conventions). Optional; absent → legacy absolute aim.
        const QJsonArray fRest = co.value("fingerRestDir").toArray();
        if (fRest.size() == kFingerSlots) {
            clip.fingerRestDir.reserve(fRest.size());
            for (const auto& dv : fRest) {
                const QJsonArray d = dv.toArray();
                if (d.size() != 3) { clip.fingerRestDir.clear(); break; }
                clip.fingerRestDir.push_back({
                    static_cast<float>(d.at(0).toDouble()),
                    static_cast<float>(d.at(1).toDouble()),
                    static_cast<float>(d.at(2).toDouble())});
            }
        }
        clip.quality = static_cast<float>(
            std::clamp(co.value("quality").toDouble(1.0), 0.0, 1.0));
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
        // Quality-weighted sampling (P ∝ quality², #855): keeps take variety
        // but one weak take no longer poisons its whole action. Libraries
        // without curation scores (quality defaults to 1) stay uniform.
        double total = 0.0;
        QList<double> weights;
        weights.reserve(hits.size());
        for (int i : hits) {
            const double q = m_clips[static_cast<size_t>(i)].quality;
            weights.append(q * q);
            total += q * q;
        }
        if (total <= 1e-9)
            return hits.at(QRandomGenerator::global()->bounded(hits.size()));
        double r = QRandomGenerator::global()->generateDouble() * total;
        for (int k = 0; k < hits.size(); ++k) {
            r -= weights.at(k);
            if (r <= 0.0) return hits.at(k);
        }
        return hits.last();
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
    const QDir dir(QDir(dataPath).filePath(QStringLiteral("ai_models/motion")));
    // Prefer the V2 library (schema v4, fingers folded in) when present; fall
    // back to the V1 file. Old app builds only know `motion-library.json`, so
    // shipping a `-v2` file alongside never breaks them — this is opt-in.
    const QString v2 = dir.filePath(QString::fromLatin1(kLibraryFileV2));
    if (QFileInfo::exists(v2)) return v2;
    return dir.filePath(QString::fromLatin1(kLibraryFile));
}

bool MotionLibrary::libraryPresent() { return QFileInfo::exists(libraryPath()); }

QString MotionLibrary::curationPath()
{
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath(
        QStringLiteral("ai_models/motion/curation.json"));
}

QSet<QString> MotionLibrary::loadCuration()
{
    QSet<QString> out;
    QFile f(curationPath());
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue& v : root.value(QStringLiteral("approved")).toArray())
        if (v.isString()) out.insert(v.toString());
    return out;
}

bool MotionLibrary::saveCuration(const QSet<QString>& approved)
{
    QJsonObject root;
    root[QStringLiteral("schema")] =
        QStringLiteral("qtmesh-motion-curation-v1");
    QJsonArray arr;
    // stable file diffs: sorted
    QStringList sorted(approved.begin(), approved.end());
    sorted.sort();
    for (const QString& s : sorted) arr.append(s);
    root[QStringLiteral("approved")] = arr;
    QDir().mkpath(QFileInfo(curationPath()).absolutePath());
    QFile f(curationPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

QString MotionLibrary::ensureLibraryBlocking()
{
    const QString dest = libraryPath();
    const bool haveLocal = QFileInfo::exists(dest);
    // Already on the V2 library (libraryPath prefers it when present) — done.
    if (haveLocal && dest.endsWith(QLatin1String(kLibraryFileV2)))
        return dest;
    // UPGRADE GAP: a pre-V2 install has only the legacy V1 file, and the old
    // early-return here meant the V2 (52-joint, curated) library NEVER
    // downloaded — those users kept the V1 side-channel finger path and its
    // artifacts. When only V1 exists, still attempt the V2 download (once per
    // process — see below); offline/404 keeps the local V1 working.
    if (!qEnvironmentVariableIsEmpty("QTMESH_MOTION_NO_DOWNLOAD"))
        return haveLocal ? dest : QString();

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
    if (base.isEmpty()) return haveLocal ? dest : QString();
    if (!base.endsWith('/')) base += '/';

    auto* dl = ModelDownloader::instance();
    if (!dl) return haveLocal ? dest : QString();

    // One V2-upgrade attempt per process: a v1-only install must not re-hit
    // the network for every generate call in a session when offline.
    static bool s_v2UpgradeAttempted = false;
    if (haveLocal && s_v2UpgradeAttempted)
        return dest;
    s_v2UpgradeAttempted = true;
    SentryReporter::addBreadcrumb(
        QStringLiteral("ai.motion_library"),
        haveLocal ? QStringLiteral("attempt V2 upgrade from local V1 library")
                  : QStringLiteral("download V2 motion library"));

    // Try the V2 library (schema v4, fingers-as-joints — the curated shipped
    // set) FIRST; fall back to the V1 file so overridden base URLs / older
    // hosted repos keep working. Old app builds only request the V1 name and
    // their loader rejects the v4 schema, so hosting both is non-breaking.
    bool stalled = false;
    auto tryDownload = [&](const char* fileName, int timeoutMs) -> QString {
        const QDir dir(QFileInfo(libraryPath()).absolutePath());
        const QString fdest = dir.filePath(QString::fromLatin1(fileName));
        const QString url = base + QString::fromLatin1(fileName);
        const QString label = QStringLiteral("Motion library (%1)")
                                  .arg(QString::fromLatin1(fileName));
        QDir().mkpath(dir.absolutePath());
        QEventLoop loop;
        bool ok = false, timedOut = false;
        auto onDone = QObject::connect(dl, &ModelDownloader::downloadCompleted,
            &loop, [&](const QString& name, const QString&) {
                if (name == label) { ok = true; loop.quit(); } });
        auto onErr = QObject::connect(dl, &ModelDownloader::downloadError,
            &loop, [&](const QString& name, const QString&) {
                if (name == label) { ok = false; loop.quit(); } });
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop,
                         [&]() { timedOut = true; loop.quit(); });
        timeout.start(timeoutMs);
        dl->startDownload(url, fdest, label);
        loop.exec();
        QObject::disconnect(onDone);
        QObject::disconnect(onErr);
        if (timedOut && dl) dl->cancelDownload();
        stalled = timedOut;
        if (ok && !timedOut && QFileInfo::exists(fdest)) return fdest;
        QFile::remove(fdest);   // don't leave a partial/404 body behind
        return QString();
    };
    // The UPGRADE attempt (a working V1 exists) uses a SHORT timeout: the
    // caller blocks synchronously (GUI generate / listMotionClips), and a
    // stalled connection must not freeze a functional V1 user for the full
    // 5 minutes — 20s covers the ~8 MB file on a sane connection, and the
    // attempt runs only once per process. Fresh installs (no local library)
    // keep the long timeout since there is nothing to fall back to.
    const QString v2 = tryDownload(kLibraryFileV2, haveLocal ? 20000 : 300000);
    if (!v2.isEmpty()) return v2;
    // V2 unavailable: keep the legacy local V1 when we have one.
    if (haveLocal) {
        SentryReporter::addBreadcrumb(
            QStringLiteral("ai.motion_library"),
            QStringLiteral("V2 upgrade unavailable — keeping local V1"));
        return dest;
    }
    // A TIMEOUT means the connection stalled, not that the V2 file is absent —
    // retrying the V1 name would block the (GUI/CLI) caller for up to another
    // 5 minutes for nothing. Only fall back on a fast failure (404 / older
    // hosted repo without the V2 file).
    if (stalled) return {};
    return tryDownload(kLibraryFile, 300000);
}

std::vector<std::array<float, 3>> MotionLibrary::referenceDirsForPrompt(
    const QString& prompt)
{
    MotionLibrary lib;
    if (!libraryPresent() || !lib.loadFromFile(libraryPath()))
        return {};
    // Deterministically pick the most COMPLETE direction set (some scraped
    // clips resolve fewer roles — an incomplete reference leaves those bones
    // at the raw T-pose). Prefer clips matching the prompt's action.
    QString action;
    const int matched = lib.matchPrompt(prompt, &action);
    auto score = [](const Clip& c) {
        int n = 0;
        for (const auto& d : c.restDir)
            if (std::abs(d[0]) > 1e-6f || std::abs(d[1]) > 1e-6f
                || std::abs(d[2]) > 1e-6f)
                ++n;
        return n;
    };
    const Clip* best = nullptr;
    int bestScore = 0;
    bool bestMatchesAction = false;
    for (const Clip& c : lib.m_clips) {
        const int n = score(c);
        if (n == 0) continue;
        const bool m = matched >= 0
            && c.action.compare(action, Qt::CaseInsensitive) == 0;
        if (!best || (m && !bestMatchesAction)
            || (m == bestMatchesAction && n > bestScore)) {
            best = &c; bestScore = n; bestMatchesAction = m;
        }
    }
    return best ? best->restDir : std::vector<std::array<float, 3>>{};
}

bool MotionLibrary::isVerticalDescentAction(const QString& action)
{
    // Non-locomotion actions whose motion lowers the torso. Walk/run/jump/
    // dance/etc. keep a flat root (the retarget locks the hip to the stand
    // pose). Kept deliberately small + explicit — matched on the canonical
    // action label the library builder folds prompts down to.
    static const QSet<QString> kDescent = {
        QStringLiteral("pickup"), QStringLiteral("working"),
        QStringLiteral("sit"),    QStringLiteral("crawl"),
        QStringLiteral("crouch"), QStringLiteral("death"),
        QStringLiteral("pray"),
        // Ground-work actions (Gregório worker set, #838): build/cut/farm/
        // fruit/gather all crouch or kneel at ground level — they must sink,
        // not float in a folded-leg pose. Covers the "*loop" variants too.
        QStringLiteral("build"),  QStringLiteral("buildloop"),
        QStringLiteral("cut"),    QStringLiteral("cutloop"),
        QStringLiteral("farm"),   QStringLiteral("farmloop"),
        QStringLiteral("fruit"),  QStringLiteral("fruitloop"),
        QStringLiteral("gather"),
        QStringLiteral("cartgive"),  QStringLiteral("cartgiveloop"),
        QStringLiteral("stonegive"), QStringLiteral("stonegiveloop"),
        QStringLiteral("woodgive"),  QStringLiteral("woodgiveloop"),
    };
    return kDescent.contains(action.trimmed().toLower());
}
