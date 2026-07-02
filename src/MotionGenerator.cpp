#include "MotionGenerator.h"

#include "ModelDownloader.h"

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
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
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
    {"waving", "wave"}, {"marching", "march"}, {"climbing", "climb"},
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
        so.SetIntraOpNumThreads(1);
#ifdef __APPLE__
        try {
            std::unordered_map<std::string, std::string> opt;
            so.AppendExecutionProvider("CoreML", opt);
        } catch (const Ort::Exception&) { /* CPU fallback */ }
#endif
#ifdef _WIN32
        const std::wstring wpath = modelPathArg.toStdWString();
        Ort::Session session(env, wpath.c_str(), so);
#else
        const std::string p = modelPathArg.toStdString();
        Ort::Session session(env, p.c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;

        // inputs: tokens[1,V] (one-hot), seed[1,Z] (zeros = mean clip)
        std::vector<float> tokens(static_cast<size_t>(V), 0.0f);
        tokens[static_cast<size_t>(act)] = 1.0f;
        std::vector<float> seed(static_cast<size_t>(Z), 0.0f);

        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<int64_t, 2> tShape{1, V};
        const std::array<int64_t, 2> sShape{1, Z};
        std::vector<Ort::Value> inputs;
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memInfo, tokens.data(), tokens.size(), tShape.data(), tShape.size()));
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memInfo, seed.data(), seed.size(), sShape.data(), sShape.size()));

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

        auto out = session.Run(Ort::RunOptions{nullptr}, inNames.data(),
                               inputs.data(), inputs.size(),
                               outNames.data(), 1);
        if (out.empty() || !out[0].IsTensor()) {
            r.error = QStringLiteral("t2m inference produced no tensor"); return r;
        }
        const float* m = out[0].GetTensorData<float>();
        const auto info = out[0].GetTensorTypeAndShapeInfo();
        const auto shape = info.GetShape();   // [1,T,C]
        const int64_t total = info.GetElementCount();
        if (total < static_cast<int64_t>(T) * C) {
            r.error = QStringLiteral("t2m output smaller than expected"); return r;
        }

        // ---- pack into a MotionLibrary::Clip ----
        MotionLibrary::Clip clip;
        clip.action = matched;
        clip.source = QStringLiteral("t2m-model");
        clip.fps = fps;
        clip.quats.resize(static_cast<size_t>(T));
        for (int f = 0; f < T; ++f) {
            clip.quats[f].resize(static_cast<size_t>(J));
            for (int j = 0; j < J; ++j) {
                const float* q = m + (static_cast<size_t>(f) * C + j * 10) + 3;
                clip.quats[f][j] = { q[0], q[1], q[2], q[3] };  // xyzw
            }
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
