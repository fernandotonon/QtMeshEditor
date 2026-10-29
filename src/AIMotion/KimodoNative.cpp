#include "KimodoNative.h"

#include "KimodoCorrection.h"
#include "KimodoMath.h"
#include "KimodoTextEncoder.h"
#include "MotionInterchange.h"
#include "../AppStorage.h"
#include "../AudioToFace/NpzReader.h"
#include "../ModelFetch.h"

#ifdef ENABLE_ONNX
#include "../OnnxRuntimeSettings.h"
#include <onnxruntime_cxx_api.h>
#endif

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <random>

namespace AIMotion {

namespace {

constexpr const char* kTextEncoderFile = "Llama-3-LLM2Vec-8B-Kimodo-TextEncoder-Q8_0.gguf";
// Default hosting. The ONNX set is NVIDIA Open Model License material mirrored in the QtMeshEditor model repo;
// the GGUF is a Meta Llama 3 derivative in its own repo whose name starts with "Llama-3" (licence requirement).
// Pinned to hub COMMITS (not main) so a re-upload cannot invalidate the digests of released builds: bump the
// revision and the digests together (scripts/upload-kimodo-models.sh prints both).
constexpr const char* kDefaultModelBase =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/5004019f7262057d2fc1088c1f7dfc3f2fe56aa8/kimodo/";
constexpr const char* kDefaultTextEncoderBase =
    "https://huggingface.co/fernandotonon/Llama-3-LLM2Vec-8B-Kimodo-TextEncoder-GGUF/resolve/d9931ecb2e20c7f0fd6c86284cd00eb83154bdf9/";

KimodoPredictor::Result failed(const QString& message)
{
    KimodoPredictor::Result r;
    r.error = message;
    return r;
}

QString baseUrl(const char* envVar, const char* settingsKey, const char* fallback, bool* overridden)
{
    QString b = qEnvironmentVariable(envVar);
    if (b.isEmpty()) b = QSettings().value(QLatin1String(settingsKey)).toString();
    *overridden = !b.isEmpty();
    if (b.isEmpty()) b = QString::fromLatin1(fallback);
    if (!b.endsWith(QLatin1Char('/'))) b += QLatin1Char('/');
    return b;
}

// SHA-256 of the hosted files (HF LFS oids), checked on download AND for an existing file (default hosting only).
const std::map<QString, QString>& pinnedDigests()
{
    static const std::map<QString, QString> d = {
        {QStringLiteral("kimodo_denoiser.onnx"), QStringLiteral("a0ed4676d44fb60da5b2d88799d42adcf6bcd3dd331a026834d4400aed4edee6")},
        {QStringLiteral("kimodo_decode.onnx"), QStringLiteral("d5ad3087de0015517d9edecf8d0e1c134fba938d65183716c5f7f22c8eaba176")},
        {QStringLiteral("kimodo_encode.onnx"), QStringLiteral("e5c4b13a6966edc6af9e9cd5120ecda3d69adfb858cdd5af736a620305e6a60b")},
        {QStringLiteral("kimodo_transition.onnx"), QStringLiteral("b3a5e244eee980ecd22037d62fb8ed04b222a65bbedd252fd48c83ddbc08871d")},
        {QStringLiteral("kimodo_meta.json"), QStringLiteral("70f639cc09ded1dc9ead8d3e2678878147e0b179c97e667d829f42b97ca26c15")},
        {QStringLiteral("Llama-3-LLM2Vec-8B-Kimodo-TextEncoder-Q8_0.gguf"),
         QStringLiteral("332f86e0b7f5eb022795bd00507245d2efe12f5b5057cedcce678d8a32ad4ce6")},
    };
    return d;
}

// --- kimodo_meta.json ------------------------------------------------------------------------------------------

struct Meta {
    int D = 0;
    float fps = 30.0f;
    int transitionFrames = 5;
    int textDim = 4096;
    std::vector<float> mean, stdEps, acpBase;
    float wText = 2.0f, wConstraint = 2.0f;
    int smoothRootStart = 0;
    int J30 = 0;
    std::vector<std::string> names77;
    std::vector<int> parents77;
    std::vector<float> rest77, relaxed77;   // [77*3], [77*9]
    std::vector<int> slice77;               // [J30]
    KimodoCorrection::Rig rig;
    float contactThreshold = 0.5f, rootMargin = 0.04f;
};

std::vector<float> floatsOf(const QJsonValue& v)
{
    std::vector<float> out;
    const std::function<void(const QJsonValue&)> walk = [&](const QJsonValue& x) {
        if (x.isArray())
            for (const QJsonValue& e : x.toArray()) walk(e);
        else
            out.push_back(float(x.toDouble()));
    };
    walk(v);
    return out;
}

std::vector<int> intsOf(const QJsonValue& v)
{
    std::vector<int> out;
    for (const QJsonValue& e : v.toArray()) out.push_back(e.toInt());
    return out;
}

bool loadMeta(const QString& path, Meta* m, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot read %1").arg(path);
        return false;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.value(QStringLiteral("format")).toString() != QLatin1String("qtmesh-kimodo-onnx-v1")) {
        *error = QStringLiteral("%1 is not a qtmesh-kimodo-onnx-v1 file").arg(path);
        return false;
    }
    m->D = o.value(QStringLiteral("featureSize")).toInt();
    m->fps = float(o.value(QStringLiteral("fps")).toDouble(30.0));
    m->transitionFrames = o.value(QStringLiteral("transitionFrames")).toInt(5);
    m->textDim = o.value(QStringLiteral("textDim")).toInt(4096);
    m->mean = floatsOf(o.value(QStringLiteral("mean")));
    m->stdEps = floatsOf(o.value(QStringLiteral("stdEps")));
    m->acpBase = floatsOf(o.value(QStringLiteral("diffusion")).toObject().value(QStringLiteral("alphasCumprodBase")));
    const std::vector<float> w = floatsOf(o.value(QStringLiteral("cfg")).toObject().value(QStringLiteral("weights")));
    if (w.size() == 2) { m->wText = w[0]; m->wConstraint = w[1]; }
    m->smoothRootStart = o.value(QStringLiteral("slices")).toObject().value(QStringLiteral("smooth_root_pos")).toArray().at(0).toInt();
    const QJsonObject s30 = o.value(QStringLiteral("skeleton30")).toObject();
    m->J30 = int(s30.value(QStringLiteral("names")).toArray().size());
    m->slice77 = intsOf(s30.value(QStringLiteral("slice77")));
    const QJsonObject s77 = o.value(QStringLiteral("skeleton77")).toObject();
    for (const QJsonValue& n : s77.value(QStringLiteral("names")).toArray()) m->names77.push_back(n.toString().toStdString());
    m->parents77 = intsOf(s77.value(QStringLiteral("parents")));
    m->rest77 = floatsOf(s77.value(QStringLiteral("neutralJoints")));
    m->relaxed77 = floatsOf(s77.value(QStringLiteral("relaxedHandsRestPose")));
    const QJsonObject pp = o.value(QStringLiteral("postprocess")).toObject();
    m->contactThreshold = float(pp.value(QStringLiteral("contactThreshold")).toDouble(0.5));
    m->rootMargin = float(pp.value(QStringLiteral("rootMargin")).toDouble(0.04));
    const QJsonObject rig = pp.value(QStringLiteral("rig")).toObject();
    m->rig.parents = intsOf(rig.value(QStringLiteral("parents")));
    for (const QJsonValue& t : rig.value(QStringLiteral("translations")).toArray()) {
        const QJsonArray a = t.toArray();
        m->rig.translations.push_back({float(a.at(0).toDouble()), float(a.at(1).toDouble()), float(a.at(2).toDouble())});
    }
    for (const QJsonValue& t : rig.value(QStringLiteral("rotationsXYZW")).toArray()) {
        const QJsonArray a = t.toArray();
        m->rig.rotationsXyzw.push_back({float(a.at(0).toDouble()), float(a.at(1).toDouble()), float(a.at(2).toDouble()),
                                        float(a.at(3).toDouble())});
    }
    m->rig.leftHand = rig.value(QStringLiteral("leftHand")).toInt(-1);
    m->rig.rightHand = rig.value(QStringLiteral("rightHand")).toInt(-1);
    m->rig.leftFoot = rig.value(QStringLiteral("leftFoot")).toInt(-1);
    m->rig.rightFoot = rig.value(QStringLiteral("rightFoot")).toInt(-1);
    m->rig.hasDoubleAnkleJoints = rig.value(QStringLiteral("hasDoubleAnkleJoints")).toBool(false);

    const size_t J77 = m->names77.size();
    const bool ok = m->D > 0 && m->mean.size() == size_t(m->D) && m->stdEps.size() == size_t(m->D) && m->acpBase.size() > 1
                    && m->J30 > 0 && m->slice77.size() == size_t(m->J30) && J77 > 0 && m->parents77.size() == J77
                    && m->rest77.size() == J77 * 3 && m->relaxed77.size() == J77 * 9 && m->rig.parents.size() == size_t(m->J30)
                    && m->transitionFrames > 0;
    if (!ok) *error = QStringLiteral("%1 is incomplete").arg(path);
    for (int idx : m->slice77)
        if (idx < 0 || size_t(idx) >= J77) {
            *error = QStringLiteral("%1: bad 30->77 joint slice").arg(path);
            return false;
        }
    return ok;
}

// A motion in feature space: [T][D] flattened.
struct Feats {
    int T = 0;
    std::vector<float> v;
};

#ifdef ENABLE_ONNX

struct Input {
    std::vector<int64_t> shape;
    const float* f = nullptr;
    const int64_t* i64 = nullptr;
    const uint8_t* b = nullptr;
    size_t n = 0;
};

class Graph {
public:
    Graph(Ort::Env& env, const QString& file)
    {
        Ort::SessionOptions so;
        // CPU only: CoreML splits the denoiser into ~77 partitions (slower than CPU: 45 s vs 23 s per 5 s clip) and
        // its MLProgram path rejects the dynamic-length attention reshape.
        OnnxRuntimeSettings::SessionConfig cfg;
        cfg.appendGpu = false;
        OnnxRuntimeSettings::configureSessionOptions(so, cfg);
#ifdef _WIN32
        m_session = std::make_unique<Ort::Session>(env, file.toStdWString().c_str(), so);
#else
        m_session = std::make_unique<Ort::Session>(env, file.toStdString().c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;
        for (size_t i = 0; i < m_session->GetInputCount(); ++i) m_inputs.push_back(m_session->GetInputNameAllocated(i, alloc).get());
        for (size_t i = 0; i < m_session->GetOutputCount(); ++i) m_outputs.push_back(m_session->GetOutputNameAllocated(i, alloc).get());
    }

    /// Inputs the graph does not declare are skipped (the exporter prunes unused ones). Returns outputs by name.
    std::map<std::string, std::vector<float>> run(const std::map<std::string, Input>& in, const std::vector<std::string>& want)
    {
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<const char*> names;
        std::vector<Ort::Value> values;
        for (const std::string& name : m_inputs) {
            const auto it = in.find(name);
            if (it == in.end()) throw std::runtime_error("kimodo graph input '" + name + "' has no value");
            const Input& x = it->second;
            names.push_back(name.c_str());
            if (x.f)
                values.push_back(Ort::Value::CreateTensor<float>(mem, const_cast<float*>(x.f), x.n, x.shape.data(), x.shape.size()));
            else if (x.i64)
                values.push_back(Ort::Value::CreateTensor<int64_t>(mem, const_cast<int64_t*>(x.i64), x.n, x.shape.data(), x.shape.size()));
            else
                values.push_back(Ort::Value::CreateTensor<bool>(mem, reinterpret_cast<bool*>(const_cast<uint8_t*>(x.b)), x.n,
                                                                x.shape.data(), x.shape.size()));
        }
        std::vector<const char*> outNames;
        for (const std::string& w : want) outNames.push_back(w.c_str());
        auto outs = m_session->Run(Ort::RunOptions{nullptr}, names.data(), values.data(), values.size(), outNames.data(), outNames.size());
        std::map<std::string, std::vector<float>> result;
        for (size_t k = 0; k < outs.size(); ++k) {
            auto info = outs[k].GetTensorTypeAndShapeInfo();
            const size_t count = info.GetElementCount();
            std::vector<float> data(count);
            if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                const float* p = outs[k].GetTensorData<float>();
                std::copy(p, p + count, data.begin());
            } else if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL) {
                const bool* p = outs[k].GetTensorData<bool>();
                for (size_t i = 0; i < count; ++i) data[i] = p[i] ? 1.0f : 0.0f;
            } else {
                throw std::runtime_error("kimodo graph output '" + want[k] + "' has an unexpected type");
            }
            result[want[k]] = std::move(data);
        }
        return result;
    }

private:
    std::unique_ptr<Ort::Session> m_session;
    std::vector<std::string> m_inputs, m_outputs;
};

Input floatIn(const std::vector<float>& v, std::vector<int64_t> shape) { Input x; x.f = v.data(); x.n = v.size(); x.shape = std::move(shape); return x; }

class Runtime {
public:
    Runtime(const Meta& meta, const QString& dir) : m_meta(meta)
    {
        m_den = std::make_unique<Graph>(m_env, QDir(dir).filePath(QStringLiteral("kimodo_denoiser.onnx")));
        m_dec = std::make_unique<Graph>(m_env, QDir(dir).filePath(QStringLiteral("kimodo_decode.onnx")));
        m_enc = std::make_unique<Graph>(m_env, QDir(dir).filePath(QStringLiteral("kimodo_encode.onnx")));
        m_tra = std::make_unique<Graph>(m_env, QDir(dir).filePath(QStringLiteral("kimodo_transition.onnx")));
    }

    struct Decoded {
        int T = 0;
        std::vector<float> local, root, contacts;   // [T*J*9], [T*3], [T*4]
    };

    Decoded decode(const Feats& f)
    {
        auto o = m_dec->run({{"features", floatIn(f.v, {1, f.T, m_meta.D})}}, {"local_rot_mats", "root_positions", "foot_contacts"});
        return {f.T, std::move(o["local_rot_mats"]), std::move(o["root_positions"]), std::move(o["foot_contacts"])};
    }

    Feats encode(const std::vector<float>& local, const std::vector<float>& root, int T)
    {
        std::vector<std::array<float, 2>> xz(static_cast<size_t>(T));
        for (int t = 0; t < T; ++t) xz[size_t(t)] = {root[size_t(t) * 3], root[size_t(t) * 3 + 2]};
        const auto sm = KimodoMath::smoothSignal(xz, 0.06f);
        std::vector<float> smooth(size_t(T) * 3);
        for (int t = 0; t < T; ++t) {
            smooth[size_t(t) * 3] = sm[size_t(t)][0];
            smooth[size_t(t) * 3 + 1] = root[size_t(t) * 3 + 1];
            smooth[size_t(t) * 3 + 2] = sm[size_t(t)][1];
        }
        auto o = m_enc->run({{"local_rot_mats", floatIn(local, {1, T, m_meta.J30, 3, 3})},
                             {"root_positions", floatIn(root, {1, T, 3})},
                             {"smooth_root_pos", floatIn(smooth, {1, T, 3})}},
                            {"features"});
        return {T, std::move(o["features"])};
    }

    struct Transition {
        std::vector<float> observed, mask, hipIn, rotIn;   // [NT*D], [NT*D], [NT*3], [NT*J*4]
        float heading = 0.0f;
        float root2d[2] = {0.0f, 0.0f};
    };

    Transition transition(const Feats& latest)
    {
        auto o = m_tra->run({{"latest", floatIn(latest.v, {1, latest.T, m_meta.D})}},
                            {"observed", "mask", "first_heading_angle", "last_root_2d", "hip_input", "rotations_input"});
        Transition t;
        t.observed = std::move(o["observed"]);
        t.mask = std::move(o["mask"]);
        t.hipIn = std::move(o["hip_input"]);
        t.rotIn = std::move(o["rotations_input"]);
        t.heading = o["first_heading_angle"].at(0);
        t.root2d[0] = o["last_root_2d"].at(0);
        t.root2d[1] = o["last_root_2d"].at(1);
        return t;
    }

    std::vector<float> denoise(const std::vector<float>& x, int T, const std::vector<float>& text, int64_t t, float heading,
                               const std::vector<float>& mask, const std::vector<float>& obs)
    {
        std::vector<uint8_t> pad(size_t(T), 1);
        Input padIn;
        padIn.b = pad.data();
        padIn.n = pad.size();
        padIn.shape = {1, T};
        Input tIn;
        tIn.i64 = &t;
        tIn.n = 1;
        tIn.shape = {1};
        const std::vector<float> h = {heading};
        auto o = m_den->run({{"x", floatIn(x, {1, T, m_meta.D})},
                             {"x_pad_mask", padIn},
                             {"text_feat", floatIn(text, {1, 1, int64_t(text.size())})},
                             {"timesteps", tIn},
                             {"first_heading_angle", floatIn(h, {1})},
                             {"motion_mask", floatIn(mask, {1, T, m_meta.D})},
                             {"observed_motion", floatIn(obs, {1, T, m_meta.D})}},
                            {"pred_clean"});
        return std::move(o["pred_clean"]);
    }

    const Meta& meta() const { return m_meta; }

private:
    Meta m_meta;
    Ort::Env m_env{ORT_LOGGING_LEVEL_WARNING, "qtmesh_kimodo"};
    std::unique_ptr<Graph> m_den, m_dec, m_enc, m_tra;
};

// QTMESH_KIMODO_DEBUG=1: per-stage wall-clock on stderr.
struct StageTimer {
    const char* name;
    qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    explicit StageTimer(const char* n) : name(n) {}
    ~StageTimer()
    {
        static const bool on = qEnvironmentVariableIsSet("QTMESH_KIMODO_DEBUG");
        if (on) fprintf(stderr, "[kimodo] %s %lld ms\n", name, QDateTime::currentMSecsSinceEpoch() - t0);
    }
};

// Kimodo post_process_motion: decode, MotionCorrection, re-encode (with the C++ smooth root).
Feats postprocess(Runtime& rt, const Feats& f, const Runtime::Transition* tr)
{
    StageTimer timer("postprocess");
    const Meta& m = rt.meta();
    const Runtime::Decoded d = rt.decode(f);
    const int T = d.T, J = m.J30;
    std::vector<float> hip = d.root;
    std::vector<float> rot(size_t(T) * J * 4);
    for (int k = 0; k < T * J; ++k) {
        const auto q = KimodoMath::quatFromMatrixWxyz(&d.local[size_t(k) * 9]);
        std::copy(q.begin(), q.end(), rot.begin() + size_t(k) * 4);
    }
    std::vector<float> hipIn(size_t(T) * 3, 0.0f), rotIn(size_t(T) * J * 4, 0.0f);
    for (int k = 0; k < T * J; ++k) rotIn[size_t(k) * 4] = 1.0f;
    KimodoCorrection::Masks masks;
    if (tr) {
        const int nt = std::min(m.transitionFrames, T);
        masks.fullBody.assign(size_t(T), 0.0f);
        for (int t = 0; t < nt; ++t) masks.fullBody[size_t(t)] = 1.0f;
        std::copy(tr->hipIn.begin(), tr->hipIn.begin() + size_t(nt) * 3, hipIn.begin());
        std::copy(tr->rotIn.begin(), tr->rotIn.begin() + size_t(nt) * J * 4, rotIn.begin());
    }
    std::string err;
    if (!KimodoCorrection::correctMotion(hip, rot, hipIn, rotIn, masks, d.contacts, m.rig, m.contactThreshold, m.rootMargin, &err))
        throw std::runtime_error(err);
    std::vector<float> local(size_t(T) * J * 9);
    for (int k = 0; k < T * J; ++k)
        KimodoMath::matrixFromQuatWxyz({rot[size_t(k) * 4], rot[size_t(k) * 4 + 1], rot[size_t(k) * 4 + 2], rot[size_t(k) * 4 + 3]},
                                       &local[size_t(k) * 9]);
    return rt.encode(local, hip, T);
}

struct SampleInputs {
    std::vector<std::vector<float>> text;    // per segment [textDim]
    std::vector<std::vector<float>> noise;   // per segment [(frames (+NT)) * D]
};

// kimodo_model._multiprompt, transcribed from scripts/verify_kimodo_native.py (Native.generate).
Runtime::Decoded generateOne(Runtime& rt, const std::vector<int>& frames, const SampleInputs& in, int steps, bool post,
                             const std::function<bool()>& onStep)
{
    const Meta& m = rt.meta();
    const int D = m.D, NT = m.transitionFrames;
    const KimodoMath::DdimSchedule sched = KimodoMath::ddimSchedule(m.acpBase, steps);
    std::vector<Feats> generated;
    for (size_t idx = 0; idx < frames.size(); ++idx) {
        const int n = frames[idx];
        const bool first = generated.empty();
        int length = n;
        float heading = 0.0f;
        std::vector<float> obs, mask;
        Runtime::Transition tr;
        Feats latest;
        if (first) {
            obs.assign(size_t(length) * D, 0.0f);
            mask.assign(size_t(length) * D, 0.0f);
        } else {
            Feats lm = std::move(generated.back());
            generated.pop_back();
            Feats head{lm.T - NT, std::vector<float>(lm.v.begin(), lm.v.end() - size_t(NT) * D)};
            latest = {NT, std::vector<float>(lm.v.end() - size_t(NT) * D, lm.v.end())};
            generated.push_back(std::move(head));
            tr = rt.transition(latest);
            length = n + NT;
            heading = tr.heading;
            obs = tr.observed;
            obs.resize(size_t(length) * D, 0.0f);
            mask = tr.mask;
            mask.resize(size_t(length) * D, 0.0f);
        }
        std::vector<float> obsNorm(obs.size());
        for (size_t k = 0; k < obs.size(); ++k) obsNorm[k] = (obs[k] - m.mean[k % D]) / m.stdEps[k % D];
        const bool constrained = std::any_of(mask.begin(), mask.end(), [](float v) { return v != 0.0f; });
        const std::vector<float> zeroMask(mask.size(), 0.0f);
        const std::vector<float> zeroText(in.text[idx].size(), 0.0f);

        StageTimer timer("sample segment");
        std::vector<float> x = in.noise[idx];
        if (x.size() != size_t(length) * D) throw std::runtime_error("noise has the wrong size");
        std::vector<float> pred(x.size());
        for (int i = steps - 1; i >= 0; --i) {
            const int64_t t = sched.timesteps[size_t(i)];
            const auto pText = rt.denoise(x, length, in.text[idx], t, heading, zeroMask, obsNorm);
            const auto pUnc = rt.denoise(x, length, zeroText, t, heading, zeroMask, obsNorm);
            const std::vector<float> pCons = constrained ? rt.denoise(x, length, zeroText, t, heading, mask, obsNorm) : pUnc;
            for (size_t k = 0; k < x.size(); ++k)
                pred[k] = pUnc[k] + m.wText * (pText[k] - pUnc[k]) + m.wConstraint * (pCons[k] - pUnc[k]);
            KimodoMath::ddimStep(sched, i, pred.data(), x.data(), x.size());
            if (onStep && !onStep()) throw std::runtime_error("cancelled");
        }
        Feats motion{length, std::vector<float>(x.size())};
        for (size_t k = 0; k < x.size(); ++k) motion.v[k] = x[k] * m.stdEps[k % D] + m.mean[k % D];

        if (!first) {
            for (int f = 0; f < length; ++f) {
                motion.v[size_t(f) * D + size_t(m.smoothRootStart)] += tr.root2d[0];
                motion.v[size_t(f) * D + size_t(m.smoothRootStart) + 2] += tr.root2d[1];
            }
            if (post) {
                motion = postprocess(rt, motion, &tr);
            } else {
                Feats blended{NT, std::vector<float>(size_t(NT) * D)};
                for (int f = 0; f < NT; ++f) {
                    const float alpha = NT > 1 ? 1.0f - float(f) / float(NT - 1) : 1.0f;   // linspace(1, 0, NT)
                    for (int k = 0; k < D; ++k)
                        blended.v[size_t(f) * D + k] = latest.v[size_t(f) * D + k] * alpha + (1.0f - alpha) * motion.v[size_t(f) * D + k];
                }
                generated.push_back(std::move(blended));
                motion = {n, std::vector<float>(motion.v.begin() + size_t(NT) * D, motion.v.end())};
            }
        } else if (post) {
            motion = postprocess(rt, motion, nullptr);
        }
        generated.push_back(std::move(motion));
    }
    Feats full;
    for (const Feats& g : generated) {
        full.T += g.T;
        full.v.insert(full.v.end(), g.v.begin(), g.v.end());
    }
    return rt.decode(full);
}

#endif // ENABLE_ONNX

} // namespace

QString KimodoNative::modelDir() { return QDir(AppStorage::aiModelsRoot()).filePath(QStringLiteral("kimodo/")); }

std::vector<KimodoNative::ModelFile> KimodoNative::modelFiles()
{
    bool modelOverridden = false, encoderOverridden = false;
    const QString mb = baseUrl("QTMESH_KIMODO_MODEL_BASE_URL", "ai/kimodoModelBaseUrl", kDefaultModelBase, &modelOverridden);
    const QString tb = baseUrl("QTMESH_KIMODO_TEXTENC_BASE_URL", "ai/kimodoTextEncoderBaseUrl", kDefaultTextEncoderBase, &encoderOverridden);
    const auto digest = [](const QString& name, bool overridden) {
        if (overridden) return QString();
        const auto it = pinnedDigests().find(name);
        return it == pinnedDigests().end() ? QString() : it->second;
    };
    std::vector<ModelFile> files;
    const auto add = [&](const QString& name, const QString& base, bool overridden, qint64 bytes, bool licence) {
        files.push_back({name, base + name, licence ? QString() : digest(name, overridden), bytes, licence});
    };
    add(QStringLiteral("kimodo_denoiser.onnx"), mb, modelOverridden, 1176500340, false);
    add(QStringLiteral("kimodo_decode.onnx"), mb, modelOverridden, 122716, false);
    add(QStringLiteral("kimodo_encode.onnx"), mb, modelOverridden, 132031, false);
    add(QStringLiteral("kimodo_transition.onnx"), mb, modelOverridden, 203431, false);
    add(QStringLiteral("kimodo_meta.json"), mb, modelOverridden, 60000, false);
    add(QStringLiteral("LICENSE-NVIDIA-OML.txt"), mb, modelOverridden, 20000, true);
    add(QStringLiteral("NOTICE-Kimodo.txt"), mb, modelOverridden, 2000, true);
    add(QString::fromLatin1(kTextEncoderFile), tb, encoderOverridden, 7982600928LL, false);
    add(QStringLiteral("LICENSE-Llama-3.txt"), tb, encoderOverridden, 10000, true);
    add(QStringLiteral("USE_POLICY-Llama-3.md"), tb, encoderOverridden, 5000, true);
    add(QStringLiteral("NOTICE-Llama-3.txt"), tb, encoderOverridden, 1000, true);
    add(QStringLiteral("LICENSE-LLM2Vec-MIT.txt"), tb, encoderOverridden, 1100, true);
    return files;
}

bool KimodoNative::modelsInstalled(QStringList* missing)
{
    bool ok = true;
    for (const ModelFile& f : modelFiles())
        if (!QFileInfo(QDir(modelDir()).filePath(f.name)).isFile()) {
            ok = false;
            if (missing) *missing << f.name;
        }
    return ok;
}

qint64 KimodoNative::missingBytes()
{
    qint64 total = 0;
    for (const ModelFile& f : modelFiles())
        if (!QFileInfo(QDir(modelDir()).filePath(f.name)).isFile()) total += f.approxBytes;
    return total;
}

bool KimodoNative::buildSupported()
{
#ifdef ENABLE_ONNX
    return KimodoTextEncoder::available();
#else
    return false;
#endif
}

QString KimodoNative::description()
{
    if (!buildSupported())
        return QStringLiteral("native Kimodo needs an ONNX Runtime + llama.cpp build (-DENABLE_ONNX=ON -DENABLE_LOCAL_LLM=ON)");
    if (modelsInstalled()) return QStringLiteral("native Kimodo ready (%1)").arg(QDir::toNativeSeparators(modelDir()));
    return QStringLiteral("native Kimodo models not downloaded (~%1 GB)").arg(double(missingBytes()) / 1e9, 0, 'f', 1);
}

bool KimodoNative::downloadModels(QString* error, const KimodoPredictor::ProgressFn& progress)
{
    if (qEnvironmentVariableIsSet("QTMESH_KIMODO_NO_DOWNLOAD")) {
        if (error) *error = QStringLiteral("QTMESH_KIMODO_NO_DOWNLOAD is set");
        return false;
    }
    QDir().mkpath(modelDir());
    const std::vector<ModelFile> files = modelFiles();
    int done = 0;
    for (const ModelFile& f : files) {
        if (progress && !progress(QStringLiteral("download"), done, int(files.size()))) {
            if (error) *error = QStringLiteral("cancelled");
            return false;
        }
        ModelFetch::Request req;
        req.url = f.url;
        req.destination = QDir(modelDir()).filePath(f.name);
        req.label = QStringLiteral("kimodo/%1").arg(f.name);
        req.timeoutMs = f.approxBytes > 1000000000LL ? 4 * 3600 * 1000 : 30 * 60 * 1000;
        req.expectedSha256 = f.sha256;
        const ModelFetch::Outcome out = ModelFetch::ensureBlocking(req);
        if (!out.ok) {
            if (error) *error = QStringLiteral("%1: %2").arg(f.name, out.error);
            return false;
        }
        ++done;
    }
    if (progress) progress(QStringLiteral("download"), done, int(files.size()));
    return true;
}

KimodoPredictor::Result KimodoNative::predict(const KimodoPredictor::Options& o, const KimodoPredictor::ProgressFn& progress)
{
#ifndef ENABLE_ONNX
    Q_UNUSED(o); Q_UNUSED(progress);
    return failed(description());
#else
    if (!buildSupported()) return failed(description());
    if (!o.constraintsPath.isEmpty())
        return failed(QStringLiteral("Kimodo constraints files are supported by the Python runtime only (--runtime python)"));
    // A fixture (tests / parity checks: text features + noise from an NPZ) replaces the text encoder, so only the
    // GGUF and its licence texts may be absent then.
    const QString fixturePath = qEnvironmentVariable("QTMESH_KIMODO_NATIVE_FIXTURE");
    QStringList missing;
    modelsInstalled(&missing);
    if (!fixturePath.isEmpty())
        missing.removeIf([](const QString& n) { return n.contains(QLatin1String("Llama-3")) || n.contains(QLatin1String("LLM2Vec")); });
    if (!missing.isEmpty())
        return failed(QStringLiteral("the native Kimodo models are not downloaded (~%1 GB): request the download first "
                                     "(CLI --download, MCP download:true, or the Download button)")
                          .arg(double(missingBytes()) / 1e9, 0, 'f', 1));
    const QString dir = modelDir();
    Meta meta;
    QString err;
    if (!loadMeta(QDir(dir).filePath(QStringLiteral("kimodo_meta.json")), &meta, &err)) return failed(err);

    const int steps = o.diffusionSteps > 0 ? o.diffusionSteps : 100;
    std::vector<int> frames;
    QStringList texts;
    for (const MotionSegment& s : o.segments) {
        frames.push_back(std::max(2, int(s.duration * double(meta.fps))));
        texts << KimodoMath::sanitizeText(s.text);
    }
    for (int f : frames)
        if (f < 8) return failed(QStringLiteral("every segment must be at least %1 s long").arg(8.0 / double(meta.fps), 0, 'f', 2));

    AudioToFace::NpzReader fixture;
    const bool useFixture = !fixturePath.isEmpty() && fixture.open(fixturePath, &err);
    if (!fixturePath.isEmpty() && !useFixture) return failed(QStringLiteral("fixture: %1").arg(err));

    // Text features: from the fixture when it carries them (text_<s>), else the llama.cpp encoder.
    std::vector<std::vector<float>> textFeat;
    if (useFixture && fixture.contains(QStringLiteral("text_0"))) {
        for (int s = 0; s < int(texts.size()); ++s) {
            const auto a = fixture.read(QStringLiteral("text_%1").arg(s), &err);
            if (!a.valid() || a.data.size() != size_t(meta.textDim)) return failed(QStringLiteral("fixture text_%1: %2").arg(s).arg(err));
            textFeat.push_back(a.data);
        }
    } else {
        if (progress && !progress(QStringLiteral("load_text_encoder"), 0, 1)) return failed(QStringLiteral("cancelled"));
        StageTimer timer("text encoder");
        const auto enc = KimodoTextEncoder::encode(QDir(dir).filePath(QString::fromLatin1(kTextEncoderFile)), texts, 99,
                                                   [&] { return progress && !progress(QStringLiteral("encode_text"), 0, 1); });
        if (!enc.ok) return failed(enc.error);
        textFeat = enc.embeddings;
    }

    if (progress && !progress(QStringLiteral("load_model"), 0, 1)) return failed(QStringLiteral("cancelled"));
    QString outDir = o.outputDir;
    if (outDir.isEmpty())
        outDir = QDir(AppStorage::persistentRoot()).filePath(QStringLiteral("generated_motion/kimodo_%1").arg(QDateTime::currentMSecsSinceEpoch()));
    if (!QDir().mkpath(outDir)) return failed(QStringLiteral("cannot create %1").arg(outDir));

    KimodoPredictor::Result result;
    result.outputDir = outDir;
    try {
        std::unique_ptr<Runtime> rtHolder;
        {
            StageTimer timer("load onnx");
            rtHolder = std::make_unique<Runtime>(meta, dir);
        }
        Runtime& rt = *rtHolder;
        std::mt19937_64 rng(o.hasSeed ? uint64_t(uint32_t(o.seed)) : uint64_t(std::random_device{}()));
        std::normal_distribution<float> gauss(0.0f, 1.0f);
        const int totalSteps = steps * int(frames.size()) * o.numSamples;
        int doneSteps = 0;
        bool cancelled = false;
        const auto onStep = [&] {
            ++doneSteps;
            if (progress && !progress(QStringLiteral("generate"), doneSteps, totalSteps)) cancelled = true;
            return !cancelled;
        };
        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        const size_t J77 = meta.names77.size();
        for (int v = 0; v < o.numSamples; ++v) {
            SampleInputs in;
            in.text = textFeat;
            for (size_t s = 0; s < frames.size(); ++s) {
                const int len = frames[s] + (s == 0 ? 0 : meta.transitionFrames);
                std::vector<float> noise(size_t(len) * meta.D);
                if (useFixture) {
                    const auto a = fixture.read(QStringLiteral("noise_%1_%2").arg(v).arg(s), &err);
                    if (!a.valid() || a.data.size() != noise.size()) return failed(QStringLiteral("fixture noise_%1_%2: %3").arg(v).arg(s).arg(err));
                    noise = a.data;
                } else {
                    for (float& x : noise) x = gauss(rng);
                }
                in.noise.push_back(std::move(noise));
            }
            const Runtime::Decoded d = generateOne(rt, frames, in, steps, o.postprocess, onStep);

            // SOMA-30 -> SOMA-77 (relaxed-hand rest rotations for the joints the model does not drive).
            InterchangeArrays a;
            a.names = meta.names77;
            a.parents = meta.parents77;
            a.restJoints = meta.rest77;
            a.fps = meta.fps;
            a.localRotMats.resize(size_t(d.T) * J77 * 9);
            for (int t = 0; t < d.T; ++t) {
                std::copy(meta.relaxed77.begin(), meta.relaxed77.end(), a.localRotMats.begin() + size_t(t) * J77 * 9);
                for (int k = 0; k < meta.J30; ++k)
                    std::copy_n(d.local.begin() + (size_t(t) * meta.J30 + k) * 9, 9,
                                a.localRotMats.begin() + (size_t(t) * J77 + size_t(meta.slice77[size_t(k)])) * 9);
            }
            a.rootPositions = d.root;
            if (!o.rootMotion)
                for (int t = 0; t < d.T; ++t) {
                    a.rootPositions[size_t(t) * 3] = d.root[0];
                    a.rootPositions[size_t(t) * 3 + 2] = d.root[2];
                }
            a.contactChannels = 6;   // [L heel, L toe, L toe-end, R heel, R toe, R toe-end]
            for (int t = 0; t < d.T; ++t) {
                const float* c = &d.contacts[size_t(t) * 4];
                a.footContacts.insert(a.footContacts.end(), {c[0], c[1], c[1], c[2], c[3], c[3]});
            }
            const QString path = QDir(outDir).filePath(QStringLiteral("motion_%1.npz").arg(v, 2, 10, QLatin1Char('0')));
            if (!saveInterchange(path, a, &err)) return failed(err);
            result.files << path;
            if (progress) progress(QStringLiteral("write"), v + 1, o.numSamples);
        }

        QJsonArray segs, files;
        for (size_t s = 0; s < o.segments.size(); ++s)
            segs.append(QJsonObject{{QStringLiteral("text"), o.segments[s].text}, {QStringLiteral("duration"), o.segments[s].duration}});
        for (const QString& f : result.files) files.append(f);
        result.manifest = QJsonObject{
            {QStringLiteral("format"), QStringLiteral("qtmesh-motion-interchange")},
            {QStringLiteral("format_version"), kInterchangeVersion},
            {QStringLiteral("source"), QStringLiteral("kimodo")},
            {QStringLiteral("runtime"), QStringLiteral("native")},
            {QStringLiteral("segments"), segs},
            {QStringLiteral("text_conditioning"), useFixture && fixture.contains(QStringLiteral("text_0")) ? QStringLiteral("fixture")
                                                                                                         : QStringLiteral("prompt")},
            {QStringLiteral("seed"), o.hasSeed ? QJsonValue(o.seed) : QJsonValue()},
            {QStringLiteral("text_encoder"), QStringLiteral("llama.cpp Q8_0 (Llama-3-LLM2Vec-8B-Kimodo-TextEncoder)")},
            {QStringLiteral("num_samples"), o.numSamples},
            {QStringLiteral("diffusion_steps"), steps},
            {QStringLiteral("root_motion"), o.rootMotion},
            {QStringLiteral("postprocess"), o.postprocess},
            {QStringLiteral("files"), files},
            {QStringLiteral("fps"), double(meta.fps)},
            {QStringLiteral("joints"), int(J77)},
            {QStringLiteral("model"), QStringLiteral("kimodo-soma-rp-v1.2")},
            {QStringLiteral("device"), QStringLiteral("onnxruntime")},
            {QStringLiteral("seconds"), double(QDateTime::currentMSecsSinceEpoch() - t0) / 1000.0},
        };
        QFile mf(QDir(outDir).filePath(QStringLiteral("result.json")));
        if (mf.open(QIODevice::WriteOnly | QIODevice::Truncate)) mf.write(QJsonDocument(result.manifest).toJson());
    } catch (const std::exception& e) {
        return failed(QString::fromUtf8(e.what()) == QLatin1String("cancelled") ? QStringLiteral("cancelled")
                                                                                 : QStringLiteral("native Kimodo failed: %1").arg(QString::fromUtf8(e.what())));
    }
    result.ok = true;
    return result;
#endif
}

} // namespace AIMotion
