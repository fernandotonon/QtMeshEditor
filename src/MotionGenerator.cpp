#include "MotionGenerator.h"

#include "ModelDownloader.h"
#include "OnnxRuntimeSettings.h"

#include <QByteArray>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QRandomGenerator>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#include <random>
#include <array>
#include <vector>
#endif

namespace {
// Default hosting + override keys mirror the other AI models (#404/#408/#409).
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/motion/";
constexpr const char* kBaseUrlSettingsKey = "ai/t2mModelBaseUrl";

// Synonyms — kept in sync with MotionLibrary's matcher so --model and the
// template path resolve a prompt to the same action word.
struct Syn { const char* word; const char* action; };
const Syn kSynonyms[] = {
    {"jog", "run"}, {"running", "run"}, {"sprint", "run"},
    {"walking", "walk"}, {"stroll", "walk"},
    {"leap", "jump"}, {"hop", "jump"},
    {"dancing", "dance"}, {"punching", "punch"}, {"kicking", "kick"},
    // "marching" -> walk: the march training data was broken (ankle folded
    // ~106 deg) and was dropped from the vocab in #837, and no curated march
    // clip exists either, so route it to the nearest good gait.
    {"waving", "wave"}, {"marching", "walk"}, {"march", "walk"},
    {"climbing", "climb"},
    {"idle", "idle"}, {"stand", "idle"}, {"standing", "idle"},
    {"sitting", "sit"}, {"seat", "sit"},
    {"throwing", "throw"}, {"toss", "throw"}, {"pitch", "throw"},
    {"box", "boxing"}, {"fight", "boxing"}, {"spar", "boxing"},
};
}  // namespace

bool MotionGenerator::isModelBackendAvailable()
{
#ifdef ENABLE_ONNX
    return true;
#else
    return false;
#endif
}

QString MotionGenerator::modelPath()
{
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath(QStringLiteral("ai_models/motion/t2m.onnx"));
}

QString MotionGenerator::vocabPath()
{
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath(QStringLiteral("ai_models/motion/t2m-vocab.json"));
}

bool MotionGenerator::modelPresent()
{
    return QFileInfo::exists(modelPath()) && QFileInfo::exists(vocabPath());
}

QString MotionGenerator::ensureModelBlocking()
{
#ifndef ENABLE_ONNX
    return {};
#else
    if (modelPresent()) return modelPath();
    if (!qEnvironmentVariableIsEmpty("QTMESH_T2M_NO_DOWNLOAD")) return {};

    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_T2M_MODEL_BASE_URL");
            base = env.isEmpty() ? QString::fromLatin1(kDefaultModelBaseUrl)
                                 : QString::fromUtf8(env);
        }
    }
    if (base.isEmpty()) return {};
    if (!base.endsWith('/')) base += '/';

    auto* dl = ModelDownloader::instance();
    if (!dl) return {};

    // Download both files (model + vocab). Each blocks on its own event loop.
    auto fetch = [&](const QString& fileName, const QString& dest,
                     const QString& label) -> bool {
        if (QFileInfo::exists(dest)) return true;
        QDir().mkpath(QFileInfo(dest).absolutePath());
        QEventLoop loop;
        bool ok = false;
        auto onDone = QObject::connect(dl, &ModelDownloader::downloadCompleted, &loop,
            [&](const QString& name, const QString&) {
                if (name == label) { ok = true; loop.quit(); }
            });
        auto onErr = QObject::connect(dl, &ModelDownloader::downloadError, &loop,
            [&](const QString& name, const QString&) {
                if (name == label) { ok = false; loop.quit(); }
            });
        QTimer guard;
        guard.setSingleShot(true);
        QObject::connect(&guard, &QTimer::timeout, &loop, [&]{ ok = false; loop.quit(); });
        guard.start(180000);
        // startDownload(url, destinationPath, modelName) — same order as the
        // other consumers; completion/error signals fire under `label`.
        dl->startDownload(base + fileName, dest, label);
        loop.exec();
        QObject::disconnect(onDone); QObject::disconnect(onErr);
        return ok && QFileInfo::exists(dest);
    };

    if (!fetch(QStringLiteral("t2m.onnx"), modelPath(),
               QStringLiteral("Text-to-motion model")))
        return {};
    if (!fetch(QStringLiteral("t2m-vocab.json"), vocabPath(),
               QStringLiteral("Text-to-motion vocab")))
        return {};
    return modelPresent() ? modelPath() : QString();
#endif
}

#ifdef ENABLE_ONNX
namespace {
// Resolve a free-text prompt to a vocab action index (keyword, then synonyms),
// matching MotionLibrary::matchPrompt's logic so both surfaces agree.
int matchVocab(const QString& prompt, const QStringList& vocab, QString* matched)
{
    const QString p = prompt.toLower();
    for (int i = 0; i < vocab.size(); ++i)
        if (p.contains(vocab[i].toLower())) {
            if (matched) *matched = vocab[i];
            return i;
        }
    for (const auto& s : kSynonyms)
        if (p.contains(QLatin1String(s.word))) {
            const QString act = QString::fromLatin1(s.action);
            const int idx = vocab.indexOf(act);
            if (idx >= 0) { if (matched) *matched = act; return idx; }
        }
    return -1;
}
}  // namespace
#endif

MotionGenerator::Result MotionGenerator::generate(
        const QString& prompt, const QString& modelPathArg,
        const QString& vocabPathArg, double durationSec)
{
    Result r;
#ifndef ENABLE_ONNX
    (void)prompt; (void)modelPathArg; (void)vocabPathArg; (void)durationSec;
    r.error = QStringLiteral("text-to-motion model needs an ONNX-enabled build");
    return r;
#else
    try {
        // ---- vocab json ----
        QFile vf(vocabPathArg);
        if (!vf.open(QIODevice::ReadOnly)) {
            r.error = QStringLiteral("t2m vocab not found"); return r;
        }
        const QJsonObject vj = QJsonDocument::fromJson(vf.readAll()).object();
        QStringList vocab;
        for (const QJsonValue& v : vj.value("vocab").toArray())
            vocab << v.toString();
        const int Z = vj.value("Z").toInt(24);
        const int T = vj.value("T").toInt(40);
        const int C = vj.value("C").toInt(220);
        const int J = vj.value("J").toInt(22);
        // v4 models emit WORLD-frame quats (trained on FK world orientations,
        // consumed by the same world retarget as the v3 template library);
        // v3 vocab jsons have no "frame" key → local.
        const bool worldFrame =
            vj.value("frame").toString() == QLatin1String("world");
        const int fps = vj.value("fps").toInt(30);
        // v5 models (#858) train on CANONICALIZED quats and ship their
        // reference triple in the vocab: restWorld (identity ×22) + restDir
        // (the fixed canonical T-pose directions). With it, model clips ride
        // the SAME bind-referenced direction retarget as v5 template clips —
        // no synthetic-standing-pose shim.
        std::vector<std::array<float, 4>> vocabRestWorld;
        std::vector<std::array<float, 3>> vocabRestDir;
        {
            const QJsonArray rw = vj.value("restWorld").toArray();
            const QJsonArray rd = vj.value("restDir").toArray();
            if (rw.size() == J && rd.size() == J) {
                for (const QJsonValue& v : rw) {
                    const QJsonArray q = v.toArray();
                    if (q.size() != 4) { vocabRestWorld.clear(); break; }
                    vocabRestWorld.push_back({float(q[0].toDouble()),
                                              float(q[1].toDouble()),
                                              float(q[2].toDouble()),
                                              float(q[3].toDouble())});
                }
                for (const QJsonValue& v : rd) {
                    const QJsonArray d = v.toArray();
                    if (d.size() != 3) { vocabRestDir.clear(); break; }
                    vocabRestDir.push_back({float(d[0].toDouble()),
                                            float(d[1].toDouble()),
                                            float(d[2].toDouble())});
                }
                if (vocabRestWorld.size() != static_cast<size_t>(J)
                    || vocabRestDir.size() != static_cast<size_t>(J)) {
                    vocabRestWorld.clear();
                    vocabRestDir.clear();
                }
            }
        }
        const int V = vocab.size();
        if (V == 0 || T <= 0 || C != J * 10) {
            r.error = QStringLiteral("t2m vocab json malformed"); return r;
        }

        QString matched;
        const int act = matchVocab(prompt, vocab, &matched);
        if (act < 0) {
            r.error = QStringLiteral("prompt action not in the model vocabulary");
            return r;   // caller falls back to the template library
        }
        r.matchedAction = matched;

        // ---- run the ONNX model ----
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_t2m");
        Ort::SessionOptions so;
        OnnxRuntimeSettings::configureSessionOptions(so);
        so.SetIntraOpNumThreads(1);
#ifdef _WIN32
        const std::wstring wpath = modelPathArg.toStdWString();
        Ort::Session session(env, wpath.c_str(), so);
#else
        const std::string p = modelPathArg.toStdString();
        Ort::Session session(env, p.c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;

        // inputs: tokens[1,V] (one-hot), seed[1,Z]. The seed is SAMPLED, not
        // zero: z=0 decodes the CVAE's conditional MEAN, which averages the
        // action's phase-misaligned training windows into low-coherence
        // wiggle (renders as shaking). But single random samples are a
        // lottery — tail seeds decode to extreme poses (body folded, arms
        // flung). So: BEST-OF-N — draw N seeds, decode each (the model is
        // ~ms per run), score every candidate on motion plausibility, keep
        // the winner. Scoring uses the DERIVED LOCALS (what the retarget
        // renders): energy near real-clip statistics, temporal coherence
        // high, and no extreme articulation from the starting pose.
        std::vector<float> tokens(static_cast<size_t>(V), 0.0f);
        tokens[static_cast<size_t>(act)] = 1.0f;

        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<int64_t, 2> tShape{1, V};
        const std::array<int64_t, 2> sShape{1, Z};

        // input/output names (discover, don't hardcode the order beyond the two)
        std::vector<Ort::AllocatedStringPtr> inHolders, outHolders;
        std::vector<const char*> inNames, outNames;
        for (size_t i = 0; i < session.GetInputCount(); ++i) {
            inHolders.push_back(session.GetInputNameAllocated(i, alloc));
            inNames.push_back(inHolders.back().get());
        }
        for (size_t i = 0; i < session.GetOutputCount(); ++i) {
            outHolders.push_back(session.GetOutputNameAllocated(i, alloc));
            outNames.push_back(outHolders.back().get());
        }
        if (inNames.size() < 2 || outNames.empty()) {
            r.error = QStringLiteral("t2m model has unexpected I/O"); return r;
        }

        // quat helpers (x,y,z,w) for the plausibility score
        using Q4 = std::array<float, 4>;
        auto qConj = [](const Q4& q) { return Q4{-q[0], -q[1], -q[2], q[3]}; };
        auto qMul = [](const Q4& a, const Q4& b) {
            return Q4{ a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1],
                       a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0],
                       a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3],
                       a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2] };
        };
        auto qAngle = [](const Q4& q) {
            return 2.0f * std::acos(std::min(1.0f, std::abs(q[3])));
        };
        // canonical parent map — matches AnimationMerger's kParentCanon
        static const int kParent[22] = { -1, 0, 1, 2, 3, 4,  2, 6, 7, 8,
                                         2, 10, 11, 12,  0, 14, 15, 16,
                                         0, 18, 19, 20 };

        // NOSONAR — non-crypto: seeds latent-noise sampling for motion variety.
        std::mt19937 rng(QRandomGenerator::global()->generate());  // NOSONAR
        std::normal_distribution<float> gauss(0.0f, 0.5f);
        constexpr int kCandidates = 16;
        constexpr float kTargetStep = 0.048f;   // rad/frame — real-clip locals
        constexpr float kMaxArtic  = 1.5f;      // rad from the starting pose

        // Posture-aware ranking (#837 quality follow-up): v5+ models are
        // CANONICAL (restWorld = identity), so a bone's world direction is
        // simply q·restDir — the same measurements that curate the template
        // library rank the candidates here: spine and head must stay up,
        // and locomotion upper arms must HANG (signed Y — an abs() check
        // would pass a skyward arm).
        const bool haveCanonDirs = !vocabRestDir.empty();
        const bool locomotion = matched == QLatin1String("walk")
                                || matched == QLatin1String("run")
                                || matched == QLatin1String("march");
        auto qRotDir = [&](const Q4& q, int role) -> std::array<float, 3> {
            const auto& d = vocabRestDir[static_cast<size_t>(role)];
            const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
            const float ux = qy * d[2] - qz * d[1];
            const float uy = qz * d[0] - qx * d[2];
            const float uz = qx * d[1] - qy * d[0];
            const float vx = qy * uz - qz * uy;
            const float vy = qz * ux - qx * uz;
            const float vz = qx * uy - qy * ux;
            return { d[0] + 2.0f * (qw * ux + vx),
                     d[1] + 2.0f * (qw * uy + vy),
                     d[2] + 2.0f * (qw * uz + vz) };
        };

        std::vector<std::vector<Q4>> best;      // [T][J]
        float bestScore = -1e30f;
        for (int cand = 0; cand < kCandidates; ++cand) {
            std::vector<float> seed(static_cast<size_t>(Z), 0.0f);
            for (float& v : seed) v = gauss(rng);
            std::vector<Ort::Value> inputs;
            inputs.push_back(Ort::Value::CreateTensor<float>(
                memInfo, tokens.data(), tokens.size(), tShape.data(), tShape.size()));
            inputs.push_back(Ort::Value::CreateTensor<float>(
                memInfo, seed.data(), seed.size(), sShape.data(), sShape.size()));
            auto out = session.Run(Ort::RunOptions{nullptr}, inNames.data(),
                                   inputs.data(), inputs.size(),
                                   outNames.data(), 1);
            if (out.empty() || !out[0].IsTensor()) continue;
            const auto info = out[0].GetTensorTypeAndShapeInfo();
            if (info.GetElementCount() < static_cast<int64_t>(T) * C) continue;
            const float* m = out[0].GetTensorData<float>();

            // unpack world quats [T][J]
            std::vector<std::vector<Q4>> w(static_cast<size_t>(T),
                                           std::vector<Q4>(static_cast<size_t>(J)));
            for (int f = 0; f < T; ++f)
                for (int j = 0; j < J; ++j) {
                    const float* q = m + (static_cast<size_t>(f) * C + j * 10) + 3;
                    w[f][j] = { q[0], q[1], q[2], q[3] };
                }
            // Score on the LOCAL rotations the retarget consumes. For a
            // world-frame model these are derived (parent^-1 * child); for a
            // local-frame model (vocab has no "frame":"world") the output IS
            // already local, so deriving again would double-compose and rank
            // candidates on the wrong quantity.
            std::vector<std::vector<Q4>> loc = w;
            if (worldFrame)
                for (int f = 0; f < T; ++f)
                    for (int j = 0; j < J && j < 22; ++j)
                        if (kParent[j] >= 0)
                            loc[f][j] = qMul(qConj(w[f][kParent[j]]), w[f][j]);
            // score: energy near target, coherent step axes, bounded articulation
            double stepSum = 0.0, cohSum = 0.0; int cohN = 0;
            float maxArtic = 0.0f;
            std::vector<Q4> prevStep(static_cast<size_t>(J), Q4{0,0,0,1});
            for (int f = 0; f + 1 < T; ++f)
                for (int j = 1; j < J; ++j) {
                    const Q4 d = qMul(qConj(loc[f][j]), loc[f + 1][j]);
                    stepSum += qAngle(d);
                    const float n1 = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                    const Q4& pd = prevStep[j];
                    const float n0 = std::sqrt(pd[0]*pd[0] + pd[1]*pd[1] + pd[2]*pd[2]);
                    if (f > 0 && n1 > 1e-6f && n0 > 1e-6f) {
                        cohSum += (d[0]*pd[0] + d[1]*pd[1] + d[2]*pd[2]) / (n1 * n0);
                        ++cohN;
                    }
                    prevStep[j] = d;
                    const Q4 fromStart = qMul(qConj(loc[0][j]), loc[f + 1][j]);
                    maxArtic = std::max(maxArtic, qAngle(fromStart));
                }
            const float step = static_cast<float>(stepSum / ((T - 1) * (J - 1)));
            const float coh  = cohN ? static_cast<float>(cohSum / cohN) : 0.0f;
            float score = -std::abs(step - kTargetStep) * 40.0f
                          + coh * 2.0f
                          - std::max(0.0f, maxArtic - kMaxArtic) * 4.0f;
            // Requires the full 22-joint canonical layout — the posture roles
            // (17/21 feet, 11 larm) index up to 21. Guard J so a smaller vocab
            // can't read out of bounds.
            if (haveCanonDirs && worldFrame && J >= 22) {
                // Posture penalties DOMINATE the energy/coherence terms: a
                // bad sample (head thrown back, an arm reaching out) is worse
                // than a slightly-off-tempo good one, so the weights here are
                // ~10x the motion terms. This is the "best-of-N must never
                // pick a broken pose" guarantee — the model produces good
                // walks most of the time, and the scorer's job is to reject
                // the occasional flailing draw.
                auto meanDir = [&](int role) -> std::array<float, 3> {
                    float ax = 0.f, ay = 0.f, az = 0.f;
                    for (int f = 0; f < T; ++f) {
                        const auto d = qRotDir(w[f][role], role);
                        ax += d[0]; ay += d[1]; az += d[2];
                    }
                    const float inv = 1.0f / static_cast<float>(T);
                    return { ax * inv, ay * inv, az * inv };
                };
                const float spineY = meanDir(1)[1];      // abdomen up-ness
                const auto head = meanDir(5);            // head direction
                // head must point UP; penalize both drooping AND tipping back
                // (|Z| forward/back lean of the head axis).
                score -= std::max(0.0f, 0.85f - head[1]) * 60.0f;
                score -= std::abs(head[2]) * 25.0f;
                score -= std::max(0.0f, 0.85f - spineY) * 60.0f;
                if (locomotion) {
                    // Each arm's upper-arm must HANG (signed Y strongly
                    // negative). An arm reaching out/forward sits near Y≈0
                    // and is heavily penalized; the worst arm dominates so a
                    // single flung arm can't hide behind the other.
                    const float ry = meanDir(7)[1];
                    const float ly = meanDir(11)[1];
                    score -= std::max(0.0f, ry + 0.35f) * 40.0f;
                    score -= std::max(0.0f, ly + 0.35f) * 40.0f;
                }
            }
            if (score > bestScore) { bestScore = score; best = std::move(w); }
        }
        if (best.empty()) {
            r.error = QStringLiteral("t2m inference produced no usable candidate");
            return r;
        }

        // ---- pack into a MotionLibrary::Clip ----
        MotionLibrary::Clip clip;
        clip.action = matched;
        clip.source = QStringLiteral("t2m-model");
        clip.fps = fps;
        clip.quats.resize(static_cast<size_t>(T));
        for (int f = 0; f < T; ++f) {
            clip.quats[f].resize(static_cast<size_t>(J));
            for (int j = 0; j < J; ++j)
                clip.quats[f][j] = best[f][j];
        }
        clip.frames = T;

        // optional retime (mirror the template path's stride/pad resampling)
        if (durationSec > 0.05) {
            const int want = std::max(2, int(durationSec * clip.fps));
            std::vector<std::vector<std::array<float, 4>>> retimed(want);
            for (int f = 0; f < want; ++f) {
                const float src = (clip.frames - 1) *
                                  (float(f) / float(want - 1));
                retimed[f] = clip.quats[std::min(clip.frames - 1,
                                                 int(src + 0.5f))];
            }
            clip.quats.swap(retimed);
            clip.frames = want;
        }

        clip.restWorld = vocabRestWorld;
        clip.restDir = vocabRestDir;
        if (!clip.restDir.empty()) {
            // Ballerina-feet guard: feet are low-variance joints the model
            // under-fits — near-static predicted feet aim the target's foot
            // at the canonical FORWARD axis and point the toes. When a foot
            // role barely articulates, drop its reference direction so the
            // retarget leaves those bones at the rig's own bind pitch.
            for (const int foot : {17, 21}) {
                float maxDev = 0.0f;
                const Q4& q0 = best[0][static_cast<size_t>(foot)];
                for (int f = 1; f < T; ++f) {
                    const Q4 d = qMul(qConj(q0),
                                      best[f][static_cast<size_t>(foot)]);
                    maxDev = std::max(maxDev, qAngle(d));
                }
                if (maxDev < 0.21f)   // < ~12 degrees over the whole clip
                    clip.restDir[static_cast<size_t>(foot)] = {0.f, 0.f, 0.f};
            }
        }
        r.clip = std::move(clip);
        r.worldFrame = worldFrame;
        r.ok = true;
        return r;
    } catch (const Ort::Exception& e) {
        r.error = QStringLiteral("t2m ONNX error: %1").arg(QString::fromUtf8(e.what()));
        return r;
    } catch (const std::exception& e) {
        r.error = QStringLiteral("t2m error: %1").arg(QString::fromUtf8(e.what()));
        return r;
    }
#endif
}
