#include "QtMeshT2M.h"

#include "../AppStorage.h"
#include "../ModelFetch.h"

#ifdef ENABLE_ONNX
#include "../OnnxRuntimeSettings.h"
#include <onnxruntime_cxx_api.h>
#endif
#ifdef ENABLE_LOCAL_LLM
#include <llama.h>
#endif

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <random>

namespace AIMotion {

namespace {

constexpr const char* kOnnxFile = "t2m-text.onnx";
constexpr const char* kMetaFile = "t2m-text.json";
constexpr const char* kEncoderFile = "Qwen3-Embedding-0.6B-Q8_0.gguf";
constexpr int kRoles = 22;
// The CMU buttock roles: identity in every training source, so never supervised (the export pins them).
constexpr int kDeadRoles[] = {14, 18};

// Pinned to a hub COMMIT (not main) so a re-upload cannot invalidate the digests of released builds: bump the revision
// and the digests together. The same files are in fernandotonon/QtMeshEditor-t2m (qtmesh_t2m/), with the model card.
constexpr const char* kDefaultBase =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/720575fd11f54caa74bc926f00f0a6c38b2af324/qtmesh_t2m/";

QString baseUrl(bool* overridden)
{
    QString b = qEnvironmentVariable("QTMESH_QTMESH_T2M_BASE_URL");
    if (b.isEmpty()) b = QSettings().value(QStringLiteral("ai/qtmeshT2mBaseUrl")).toString();
    *overridden = !b.isEmpty();
    if (b.isEmpty()) b = QString::fromLatin1(kDefaultBase);
    if (!b.endsWith(QLatin1Char('/'))) b += QLatin1Char('/');
    return b;
}

// SHA-256 of the hosted files, checked on download and for an existing file (default hosting only).
const std::map<QString, QString>& pinnedDigests()
{
    static const std::map<QString, QString> d = {
        {QStringLiteral("t2m-text.onnx"), QStringLiteral("679854e2621536c413abafb1ddfdc3c33f8b8d774097c24ceb403799ba610da3")},
        {QStringLiteral("t2m-text.json"), QStringLiteral("c850f82603308ccb4a4ba73556c519dd1e2ab38ffa1f8fb0952ac97b1327afd3")},
        {QStringLiteral("Qwen3-Embedding-0.6B-Q8_0.gguf"), QStringLiteral("06507c7b42688469c4e7298b0a1e16deff06caf291cf0a5b278c308249c3e439")},
    };
    return d;
}

float angleDeg(const std::array<float, 4>& a, const std::array<float, 4>& b)
{
    const float d = std::min(1.0f, std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]));
    return 2.0f * std::acos(d) * 57.29577951f;
}

struct Meta {
    int T = QtMeshT2M::kMaxFrames;
    int Z = 0;
    int C = 220;
    int J = kRoles;
    int fps = QtMeshT2M::kFps;
    int embedDim = 1024;
    QString instruct;
    std::vector<std::array<float, 4>> restWorld;
    std::vector<std::array<float, 3>> restDir;
};

bool loadMeta(const QString& path, Meta* m, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { *error = QStringLiteral("cannot read %1").arg(path); return false; }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.value(QStringLiteral("version")).toString() != QLatin1String("v9-text")) {
        *error = QStringLiteral("%1 is not a QtMesh T2M text model (version %2)").arg(path, o.value(QStringLiteral("version")).toString());
        return false;
    }
    m->T = o.value(QStringLiteral("T")).toInt(m->T);
    m->Z = o.value(QStringLiteral("Z")).toInt();
    m->C = o.value(QStringLiteral("C")).toInt(m->C);
    m->J = o.value(QStringLiteral("J")).toInt(m->J);
    m->fps = o.value(QStringLiteral("fps")).toInt(m->fps);
    m->embedDim = o.value(QStringLiteral("embedDim")).toInt(m->embedDim);
    m->instruct = o.value(QStringLiteral("instruct")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("restWorld")).toArray()) {
        const QJsonArray a = v.toArray();
        m->restWorld.push_back({float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble()), float(a[3].toDouble())});
    }
    for (const QJsonValue& v : o.value(QStringLiteral("restDir")).toArray()) {
        const QJsonArray a = v.toArray();
        m->restDir.push_back({float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble())});
    }
    if (m->J != kRoles || m->C != kRoles * 10 || m->Z <= 0 || m->T <= 0 || m->instruct.isEmpty()
        || int(m->restWorld.size()) != kRoles || int(m->restDir.size()) != kRoles) {
        *error = QStringLiteral("%1 is incomplete or has an unexpected shape").arg(path);
        return false;
    }
    return true;
}

#ifdef ENABLE_LOCAL_LLM
struct ModelDeleter { void operator()(llama_model* m) const { if (m) llama_model_free(m); } };
struct ContextDeleter { void operator()(llama_context* c) const { if (c) llama_free(c); } };

// Qwen3-Embedding query vectors: instruct prefix + prompt, last-token pooling, L2-normalised (what
// llama-embedding --pooling last --embd-normalize 2 prints, which scripts/embed-t2m-text.py trained on).
bool encodeTexts(const QString& gguf, const QString& instruct, const QStringList& texts,
                 std::vector<std::vector<float>>* out, QString* error)
{
    static std::once_flag backendOnce;
    std::call_once(backendOnce, [] { llama_backend_init(); });
    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 99;
    std::unique_ptr<llama_model, ModelDeleter> model(llama_model_load_from_file(gguf.toUtf8().constData(), mp));
    if (!model) { *error = QStringLiteral("could not load the text encoder %1").arg(gguf); return false; }
    const llama_vocab* vocab = llama_model_get_vocab(model.get());
    const int dim = llama_model_n_embd(model.get());
    constexpr int kCtx = 512;
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = kCtx;
    cp.n_batch = kCtx;
    cp.n_ubatch = kCtx;
    cp.embeddings = true;
    cp.pooling_type = LLAMA_POOLING_TYPE_LAST;
    std::unique_ptr<llama_context, ContextDeleter> ctx(llama_init_from_model(model.get(), cp));
    if (!ctx) { *error = QStringLiteral("could not create a llama.cpp context for the text encoder"); return false; }

    for (const QString& text : texts) {
        const QByteArray utf8 = (instruct + text).toUtf8();
        std::vector<llama_token> ids(size_t(utf8.size()) + 8);
        int n = llama_tokenize(vocab, utf8.constData(), int(utf8.size()), ids.data(), int(ids.size()), true, true);
        if (n < 0) {
            ids.resize(size_t(-n));
            n = llama_tokenize(vocab, utf8.constData(), int(utf8.size()), ids.data(), int(ids.size()), true, true);
        }
        ids.resize(size_t(std::max(n, 0)));
        if (ids.empty()) { *error = QStringLiteral("the prompt tokenized to nothing"); return false; }
        if (int(ids.size()) > kCtx) ids.resize(size_t(kCtx));
        llama_memory_clear(llama_get_memory(ctx.get()), true);
        llama_batch batch = llama_batch_init(int(ids.size()), 0, 1);
        for (size_t i = 0; i < ids.size(); ++i) {
            batch.token[i] = ids[i];
            batch.pos[i] = llama_pos(i);
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i] = i + 1 == ids.size();
        }
        batch.n_tokens = int(ids.size());
        const int rc = llama_decode(ctx.get(), batch);
        llama_batch_free(batch);
        if (rc != 0) { *error = QStringLiteral("llama_decode failed (%1) in the text encoder").arg(rc); return false; }
        const float* e = llama_get_embeddings_seq(ctx.get(), 0);
        if (!e) { *error = QStringLiteral("the text encoder returned no embedding"); return false; }
        double norm = 0.0;
        for (int k = 0; k < dim; ++k) norm += double(e[k]) * double(e[k]);
        const float inv = norm > 0.0 ? float(1.0 / std::sqrt(norm)) : 0.0f;
        std::vector<float> v(static_cast<size_t>(dim));
        for (int k = 0; k < dim; ++k) v[size_t(k)] = e[k] * inv;
        out->push_back(std::move(v));
    }
    return true;
}
#endif

} // namespace

QString QtMeshT2M::modelDir() { return QDir(AppStorage::aiModelsRoot()).filePath(QStringLiteral("qtmesh_t2m/")); }

std::vector<QtMeshT2M::ModelFile> QtMeshT2M::modelFiles()
{
    bool overridden = false;
    const QString base = baseUrl(&overridden);
    std::vector<ModelFile> files;
    const auto add = [&](const QString& name, qint64 bytes, bool licence) {
        QString digest;
        if (!overridden && !licence)
            if (const auto it = pinnedDigests().find(name); it != pinnedDigests().end()) digest = it->second;
        files.push_back({name, base + name, digest, bytes, licence});
    };
    add(QString::fromLatin1(kOnnxFile), 97549608, false);
    add(QString::fromLatin1(kMetaFile), 2000, false);
    add(QString::fromLatin1(kEncoderFile), 639150592, false);
    add(QStringLiteral("LICENSE-Qwen3-Embedding.txt"), 11633, true);
    add(QStringLiteral("ATTRIBUTION.md"), 99381, true);   // CC-BY-4.0 credits of the training motion: travel with the model
    return files;
}

bool QtMeshT2M::modelsInstalled(QStringList* missing)
{
    bool ok = true;
    for (const ModelFile& f : modelFiles())
        if (!QFileInfo(QDir(modelDir()).filePath(f.name)).isFile()) {
            ok = false;
            if (missing) *missing << f.name;
        }
    return ok;
}

qint64 QtMeshT2M::missingBytes()
{
    qint64 total = 0;
    for (const ModelFile& f : modelFiles())
        if (!QFileInfo(QDir(modelDir()).filePath(f.name)).isFile()) total += f.approxBytes;
    return total;
}

bool QtMeshT2M::buildSupported()
{
#if defined(ENABLE_ONNX) && defined(ENABLE_LOCAL_LLM)
    return true;
#else
    return false;
#endif
}

QString QtMeshT2M::description()
{
    if (!buildSupported())
        return QStringLiteral("QtMesh T2M needs an ONNX Runtime + llama.cpp build (-DENABLE_ONNX=ON -DENABLE_LOCAL_LLM=ON)");
    if (modelsInstalled()) return QStringLiteral("QtMesh T2M ready");
    return QStringLiteral("QtMesh T2M model not downloaded (~%1 MB)").arg(double(missingBytes()) / 1e6, 0, 'f', 0);
}

bool QtMeshT2M::downloadModels(QString* error, const ProgressFn& progress)
{
    const auto fail = [error](const QString& msg) {
        if (error) *error = msg;
        return false;
    };
    if (qEnvironmentVariableIsSet("QTMESH_QTMESH_T2M_NO_DOWNLOAD")) return fail(QStringLiteral("QTMESH_QTMESH_T2M_NO_DOWNLOAD is set"));
    QDir().mkpath(modelDir());
    const std::vector<ModelFile> files = modelFiles();
    const int total = int(files.size());
    int done = 0;
    bool cancelled = false;
    const auto probe = [&progress, &done, total, &cancelled] {
        cancelled = cancelled || (progress && !progress(QStringLiteral("download"), done, total));
        return cancelled;
    };
    for (const ModelFile& f : files) {
        if (probe()) return fail(QStringLiteral("cancelled"));
        ModelFetch::Request req;
        req.url = f.url;
        req.destination = QDir(modelDir()).filePath(f.name);
        req.label = QStringLiteral("qtmesh_t2m/%1").arg(f.name);
        req.timeoutMs = 60 * 60 * 1000;
        req.expectedSha256 = f.sha256;
        req.cancelled = probe;
        if (const ModelFetch::Outcome out = ModelFetch::ensureBlocking(req); !out.ok)
            return fail(out.cancelled ? QStringLiteral("cancelled") : QStringLiteral("%1: %2").arg(f.name, out.error));
        ++done;
    }
    if (probe()) return fail(QStringLiteral("cancelled"));
    return true;
}

bool QtMeshT2M::validateOptions(const Options& o, QString* error)
{
    const auto fail = [error](const QString& m) {
        if (error) *error = m;
        return false;
    };
    if (o.prompt.trimmed().isEmpty()) return fail(QStringLiteral("enter a prompt (e.g. \"a person walks forward\")"));
    if (o.prompt.trimmed().contains(QLatin1Char('\n')))
        return fail(QStringLiteral("QtMesh T2M generates one motion per prompt: use a single line (multi-segment timelines need Kimodo)"));
    if (!(o.duration > 0.0) || o.duration > 600.0) return fail(QStringLiteral("duration must be in (0, 600] seconds"));
    if (o.variations < 1 || o.variations > 16) return fail(QStringLiteral("variations must be 1..16"));
    return true;
}

void QtMeshT2M::planFrames(int wanted, int* request, int* maxTrim)
{
    wanted = std::clamp(wanted, 2, kMaxFrames);
    *request = std::min(kMaxFrames, wanted + kLeadInFrames);
    *maxTrim = *request - wanted;
}

int QtMeshT2M::leadInFrames(const Frames& q, int maxTrim, float stillDegPerFrame)
{
    const int n = int(q.size());
    int k = 0;
    while (k < maxTrim && k + 1 < n) {
        float sum = 0.0f;
        int count = 0;
        for (int r = 0; r < int(q[size_t(k)].size()); ++r) {
            if (std::find(std::begin(kDeadRoles), std::end(kDeadRoles), r) != std::end(kDeadRoles)) continue;
            sum += angleDeg(q[size_t(k)][size_t(r)], q[size_t(k + 1)][size_t(r)]);
            ++count;
        }
        if (count == 0 || sum / float(count) >= stillDegPerFrame) break;
        ++k;
    }
    return k;
}

QtMeshT2M::Result QtMeshT2M::generate(const Options& o, const ProgressFn& progress)
{
    Result r;
    if (QString e; !validateOptions(o, &e)) { r.error = e; return r; }
#if !defined(ENABLE_ONNX) || !defined(ENABLE_LOCAL_LLM)
    Q_UNUSED(progress);
    r.error = description();
    return r;
#else
    if (QStringList missing; !modelsInstalled(&missing)) {
        r.error = QStringLiteral("QtMesh T2M model not downloaded (missing %1)").arg(missing.join(QStringLiteral(", ")));
        return r;
    }
    const QDir dir(modelDir());
    Meta meta;
    if (!loadMeta(dir.filePath(QString::fromLatin1(kMetaFile)), &meta, &r.error)) return r;
    r.fps = meta.fps;
    r.restWorld = meta.restWorld;
    r.restDir = meta.restDir;

    const int wantedRaw = int(std::lround(o.duration * meta.fps));
    if (wantedRaw > meta.T)
        r.note = QStringLiteral("QtMesh T2M generates at most %1 s; the clip is %1 s long").arg(double(meta.T) / meta.fps, 0, 'f', 0);
    int request = 0, maxTrim = 0;
    planFrames(std::min(wantedRaw, meta.T), &request, &maxTrim);
    const int wanted = request - maxTrim;

    const auto step = [&progress](const char* stage, int done, int total) {
        return !progress || progress(QString::fromLatin1(stage), done, total);
    };
    if (!step("encode text", 0, 1)) { r.error = QStringLiteral("cancelled"); return r; }
    QElapsedTimer timer;
    timer.start();
    std::vector<std::vector<float>> emb;
    if (!encodeTexts(dir.filePath(QString::fromLatin1(kEncoderFile)), meta.instruct, {o.prompt.trimmed()}, &emb, &r.error)) return r;
    if (int(emb.front().size()) != meta.embedDim) {
        r.error = QStringLiteral("the text encoder gives %1 floats, the model expects %2").arg(emb.front().size()).arg(meta.embedDim);
        return r;
    }
    r.encodeSeconds = double(timer.restart()) / 1000.0;
    // Debug: the embedding the model sees, for a parity check against scripts/embed-t2m-text.py.
    if (const QString dump = qEnvironmentVariable("QTMESH_T2M_DUMP_EMBEDDING"); !dump.isEmpty())
        if (QFile f(dump); f.open(QIODevice::WriteOnly))
            f.write(reinterpret_cast<const char*>(emb.front().data()), qint64(emb.front().size() * sizeof(float)));

    try {
        static Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "qtmesh_t2m"};
        Ort::SessionOptions so;
        OnnxRuntimeSettings::SessionConfig cfg;
        cfg.appendGpu = false;   // a 24-step sampler over a small transformer: CPU is fast and portable
        OnnxRuntimeSettings::configureSessionOptions(so, cfg);
        const QString onnx = dir.filePath(QString::fromLatin1(kOnnxFile));
#ifdef _WIN32
        Ort::Session session(env, onnx.toStdWString().c_str(), so);
#else
        Ort::Session session(env, onnx.toStdString().c_str(), so);
#endif
        const auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::mt19937 rng(o.hasSeed ? uint32_t(o.seed) : QRandomGenerator::global()->generate());   // NOSONAR: motion variety, not crypto
        std::normal_distribution<float> gauss(0.0f, 0.5f);
        const char* inNames[] = {"text", "seed", "length"};
        const char* outNames[] = {"motion"};
        const std::array<int64_t, 2> textShape{1, meta.embedDim};
        const std::array<int64_t, 2> seedShape{1, meta.Z};
        const std::array<int64_t, 1> lenShape{1};
        std::vector<float> seed(size_t(meta.Z));
        float length = float(request);
        for (int v = 0; v < o.variations; ++v) {
            if (!step("generate", v, o.variations)) { r.error = QStringLiteral("cancelled"); return r; }
            for (float& s : seed) s = gauss(rng);
            std::array<Ort::Value, 3> inputs{
                Ort::Value::CreateTensor<float>(mem, emb.front().data(), emb.front().size(), textShape.data(), textShape.size()),
                Ort::Value::CreateTensor<float>(mem, seed.data(), seed.size(), seedShape.data(), seedShape.size()),
                Ort::Value::CreateTensor<float>(mem, &length, 1, lenShape.data(), lenShape.size())};
            auto out = session.Run(Ort::RunOptions{nullptr}, inNames, inputs.data(), inputs.size(), outNames, 1);
            const float* m = out.front().GetTensorData<float>();
            const auto info = out.front().GetTensorTypeAndShapeInfo();
            if (info.GetElementCount() != size_t(meta.T) * size_t(meta.C)) {
                r.error = QStringLiteral("the motion graph returned %1 values, expected %2").arg(info.GetElementCount()).arg(meta.T * meta.C);
                return r;
            }
            Frames frames(static_cast<size_t>(request), Pose(static_cast<size_t>(kRoles)));
            for (int t = 0; t < request; ++t)
                for (int j = 0; j < kRoles; ++j) {
                    const float* q = m + (size_t(t) * kRoles + size_t(j)) * 10 + 3;
                    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                    const float inv = n > 1e-12f ? 1.0f / n : 0.0f;
                    frames[size_t(t)][size_t(j)] = n > 1e-12f ? std::array<float, 4>{q[0] * inv, q[1] * inv, q[2] * inv, q[3] * inv}
                                                              : std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f};
                }
            Clip c;
            c.trimmedFrames = leadInFrames(frames, maxTrim);
            c.quats.assign(frames.begin() + c.trimmedFrames, frames.begin() + c.trimmedFrames + wanted);
            r.clips.push_back(std::move(c));
        }
    } catch (const Ort::Exception& e) {
        r.error = QStringLiteral("ONNX Runtime: %1").arg(QString::fromUtf8(e.what()));
        return r;
    }
    r.sampleSeconds = double(timer.elapsed()) / 1000.0;
    step("generate", o.variations, o.variations);
    r.ok = true;
    return r;
#endif
}

} // namespace AIMotion
