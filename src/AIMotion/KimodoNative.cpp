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
#include <stdexcept>

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

// Dedicated exceptions: a failed graph run and a user's Cancel are reported differently.
struct KimodoFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct KimodoCancelled : std::runtime_error {
    KimodoCancelled() : std::runtime_error("cancelled") {}
};

// One graph input. It OWNS its buffer (a copy of at most a few hundred KB, nothing next to a 100+ ms run), so
// tensors are built from mutable memory without casting constness away.
struct Input {
    ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    std::vector<int64_t> shape;
    std::vector<float> f;
    std::vector<int64_t> i64;
    std::vector<uint8_t> b;

    void* data()
    {
        if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) return static_cast<void*>(i64.data());
        if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL) return static_cast<void*>(b.data());
        return static_cast<void*>(f.data());
    }
    size_t bytes() const
    {
        if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) return i64.size() * sizeof(int64_t);
        if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL) return b.size();
        return f.size() * sizeof(float);
    }
};

using Inputs = std::map<std::string, Input, std::less<>>;
using Outputs = std::map<std::string, std::vector<float>, std::less<>>;

Input floatIn(std::vector<float> v, std::vector<int64_t> shape)
{
    Input x;
    x.f = std::move(v);
    x.shape = std::move(shape);
    return x;
}

Input int64In(int64_t v)
{
    Input x;
    x.type = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
    x.i64 = {v};
    x.shape = {1};
    return x;
}

Input trueMaskIn(int64_t T)
{
    Input x;
    x.type = ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL;
    x.b.assign(size_t(T), uint8_t(1));
    x.shape = {1, T};
    return x;
}

std::vector<float> floatsOf(Ort::Value& v, const std::string& name)
{
    const auto info = v.GetTensorTypeAndShapeInfo();
    const size_t count = info.GetElementCount();
    std::vector<float> data(count);
    if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        const float* p = v.GetTensorData<float>();
        std::copy(p, p + count, data.begin());
    } else if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL) {
        const bool* p = v.GetTensorData<bool>();
        std::transform(p, p + count, data.begin(), [](bool x) { return x ? 1.0f : 0.0f; });
    } else {
        throw KimodoFailure("kimodo graph output '" + name + "' has an unexpected type");
    }
    return data;
}

class Graph {
public:
    Graph(const Ort::Env& env, const QString& file)
    {
        Ort::SessionOptions so;
        // CPU only: CoreML splits the denoiser into ~77 partitions (slower than CPU) and its MLProgram path rejects
        // the dynamic-length attention reshape.
        OnnxRuntimeSettings::SessionConfig cfg;
        cfg.appendGpu = false;
        OnnxRuntimeSettings::configureSessionOptions(so, cfg);
#ifdef _WIN32
        m_session = std::make_unique<Ort::Session>(env, file.toStdWString().c_str(), so);
#else
        m_session = std::make_unique<Ort::Session>(env, file.toStdString().c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;
        for (size_t i = 0; i < m_session->GetInputCount(); ++i) m_inputs.emplace_back(m_session->GetInputNameAllocated(i, alloc).get());
    }

    /// Feeds the inputs the graph declares (the exporter prunes unused ones); returns the requested outputs.
    Outputs run(Inputs& in, const std::vector<std::string>& want)
    {
        const auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<const char*> names;
        std::vector<Ort::Value> values;
        for (const std::string& name : m_inputs) {
            const auto it = in.find(name);
            if (it == in.end()) throw KimodoFailure("kimodo graph input '" + name + "' has no value");
            Input& x = it->second;
            names.push_back(name.c_str());
            values.push_back(Ort::Value::CreateTensor(mem, x.data(), x.bytes(), x.shape.data(), x.shape.size(), x.type));
        }
        std::vector<const char*> outNames;
        for (const std::string& w : want) outNames.push_back(w.c_str());
        auto outs = m_session->Run(Ort::RunOptions{nullptr}, names.data(), values.data(), values.size(), outNames.data(), outNames.size());
        Outputs result;
        for (size_t k = 0; k < outs.size(); ++k) result[want[k]] = floatsOf(outs[k], want[k]);
        return result;
    }

private:
    std::unique_ptr<Ort::Session> m_session;
    std::vector<std::string> m_inputs;
};

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
        std::vector<float> local;      // [T*J*9]
        std::vector<float> root;       // [T*3]
        std::vector<float> contacts;   // [T*4]
    };

    Decoded decode(const Feats& f)
    {
        Inputs in{{"features", floatIn(f.v, {1, f.T, m_meta.D})}};
        auto o = m_dec->run(in, {"local_rot_mats", "root_positions", "foot_contacts"});
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
        Inputs in{{"local_rot_mats", floatIn(local, {1, T, m_meta.J30, 3, 3})},
                  {"root_positions", floatIn(root, {1, T, 3})},
                  {"smooth_root_pos", floatIn(std::move(smooth), {1, T, 3})}};
        auto o = m_enc->run(in, {"features"});
        return {T, std::move(o["features"])};
    }

    struct Transition {
        std::vector<float> observed;   // [NT*D]
        std::vector<float> mask;       // [NT*D]
        std::vector<float> hipIn;      // [NT*3]
        std::vector<float> rotIn;      // [NT*J*4]
        float heading = 0.0f;
        std::array<float, 2> root2d = {0.0f, 0.0f};
    };

    Transition transition(const Feats& latest)
    {
        Inputs in{{"latest", floatIn(latest.v, {1, latest.T, m_meta.D})}};
        auto o = m_tra->run(in, {"observed", "mask", "first_heading_angle", "last_root_2d", "hip_input", "rotations_input"});
        Transition t;
        t.observed = std::move(o["observed"]);
        t.mask = std::move(o["mask"]);
        t.hipIn = std::move(o["hip_input"]);
        t.rotIn = std::move(o["rotations_input"]);
        t.heading = o["first_heading_angle"].at(0);
        t.root2d = {o["last_root_2d"].at(0), o["last_root_2d"].at(1)};
        return t;
    }

    struct DenoiseArgs {
        const std::vector<float>* text = nullptr;
        int64_t timestep = 0;
        float heading = 0.0f;
        const std::vector<float>* mask = nullptr;
        const std::vector<float>* observed = nullptr;
    };

    std::vector<float> denoise(const std::vector<float>& x, int T, const DenoiseArgs& a)
    {
        Inputs in{{"x", floatIn(x, {1, T, m_meta.D})},
                  {"x_pad_mask", trueMaskIn(T)},
                  {"text_feat", floatIn(*a.text, {1, 1, int64_t(a.text->size())})},
                  {"timesteps", int64In(a.timestep)},
                  {"first_heading_angle", floatIn({a.heading}, {1})},
                  {"motion_mask", floatIn(*a.mask, {1, T, m_meta.D})},
                  {"observed_motion", floatIn(*a.observed, {1, T, m_meta.D})}};
        auto o = m_den->run(in, {"pred_clean"});
        return std::move(o["pred_clean"]);
    }

    const Meta& meta() const { return m_meta; }

private:
    Meta m_meta;
    Ort::Env m_env{ORT_LOGGING_LEVEL_WARNING, "qtmesh_kimodo"};
    std::unique_ptr<Graph> m_den;
    std::unique_ptr<Graph> m_dec;
    std::unique_ptr<Graph> m_enc;
    std::unique_ptr<Graph> m_tra;
};

// QTMESH_KIMODO_DEBUG=1: per-stage wall-clock on stderr.
struct StageTimer {
    const char* name;
    qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    explicit StageTimer(const char* n) : name(n) {}
    StageTimer(const StageTimer&) = delete;
    StageTimer& operator=(const StageTimer&) = delete;
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
    const int T = d.T;
    const int J = m.J30;
    std::vector<float> hip = d.root;
    std::vector<float> rot(size_t(T) * J * 4);
    for (int k = 0; k < T * J; ++k) {
        const auto q = KimodoMath::quatFromMatrixWxyz(&d.local[size_t(k) * 9]);
        std::copy(q.begin(), q.end(), rot.begin() + size_t(k) * 4);
    }
    std::vector<float> hipIn(size_t(T) * 3, 0.0f);
    std::vector<float> rotIn(size_t(T) * J * 4, 0.0f);
    for (int k = 0; k < T * J; ++k) rotIn[size_t(k) * 4] = 1.0f;
    KimodoCorrection::Masks masks;
    if (tr) {
        const int nt = std::min(m.transitionFrames, T);
        masks.fullBody.assign(size_t(T), 0.0f);
        std::fill_n(masks.fullBody.begin(), nt, 1.0f);
        std::copy_n(tr->hipIn.begin(), size_t(nt) * 3, hipIn.begin());
        std::copy_n(tr->rotIn.begin(), size_t(nt) * J * 4, rotIn.begin());
    }
    KimodoCorrection::MotionArrays motion{&hip, &rot, &hipIn, &rotIn, &d.contacts};
    if (std::string err; !KimodoCorrection::correctMotion(motion, masks, m.rig, {m.contactThreshold, m.rootMargin}, &err))
        throw KimodoFailure(err);
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

// The conditioning of one segment: the first starts free; later ones start from the previous segment's tail.
struct SegmentSetup {
    int length = 0;
    float heading = 0.0f;
    std::vector<float> observedNorm;
    std::vector<float> mask;
    bool constrained = false;
    bool first = true;
    Runtime::Transition tr;
    Feats latest;
};

SegmentSetup prepareSegment(Runtime& rt, std::vector<Feats>& generated, int n)
{
    const Meta& m = rt.meta();
    const int D = m.D;
    const int NT = m.transitionFrames;
    SegmentSetup s;
    s.first = generated.empty();
    s.length = n;
    std::vector<float> obs;
    if (s.first) {
        obs.assign(size_t(n) * D, 0.0f);
        s.mask.assign(size_t(n) * D, 0.0f);
    } else {
        Feats lm = std::move(generated.back());
        generated.pop_back();
        generated.push_back({lm.T - NT, std::vector<float>(lm.v.begin(), lm.v.end() - size_t(NT) * D)});
        s.latest = {NT, std::vector<float>(lm.v.end() - size_t(NT) * D, lm.v.end())};
        s.tr = rt.transition(s.latest);
        s.length = n + NT;
        s.heading = s.tr.heading;
        obs = s.tr.observed;
        obs.resize(size_t(s.length) * D, 0.0f);
        s.mask = s.tr.mask;
        s.mask.resize(size_t(s.length) * D, 0.0f);
    }
    s.observedNorm.resize(obs.size());
    for (size_t k = 0; k < obs.size(); ++k) s.observedNorm[k] = (obs[k] - m.mean[k % D]) / m.stdEps[k % D];
    s.constrained = std::any_of(s.mask.begin(), s.mask.end(), [](float v) { return v != 0.0f; });
    return s;
}

// DDIM with separated classifier-free guidance; returns the UN-normalized features.
Feats sampleSegment(Runtime& rt, const KimodoMath::DdimSchedule& sched, const std::vector<float>& text,
                    std::vector<float> x, const SegmentSetup& s, const std::function<bool()>& onStep)
{
    StageTimer timer("sample segment");
    const Meta& m = rt.meta();
    if (x.size() != size_t(s.length) * m.D) throw KimodoFailure("noise has the wrong size");
    const std::vector<float> zeroMask(s.mask.size(), 0.0f);
    const std::vector<float> zeroText(text.size(), 0.0f);
    std::vector<float> pred(x.size());
    const int steps = int(sched.timesteps.size());
    for (int i = steps - 1; i >= 0; --i) {
        const int64_t t = sched.timesteps[size_t(i)];
        const auto pText = rt.denoise(x, s.length, {&text, t, s.heading, &zeroMask, &s.observedNorm});
        const auto pUnc = rt.denoise(x, s.length, {&zeroText, t, s.heading, &zeroMask, &s.observedNorm});
        // Unconstrained, the constraint pass equals the unconditional one: two passes instead of three.
        const auto pCons = s.constrained ? rt.denoise(x, s.length, {&zeroText, t, s.heading, &s.mask, &s.observedNorm}) : pUnc;
        for (size_t k = 0; k < x.size(); ++k)
            pred[k] = pUnc[k] + m.wText * (pText[k] - pUnc[k]) + m.wConstraint * (pCons[k] - pUnc[k]);
        KimodoMath::ddimStep(sched, i, pred.data(), x.data(), x.size());
        if (onStep && !onStep()) throw KimodoCancelled();
    }
    Feats motion{s.length, std::vector<float>(x.size())};
    for (size_t k = 0; k < x.size(); ++k) motion.v[k] = x[k] * m.stdEps[k % m.D] + m.mean[k % m.D];
    return motion;
}

// The no-post-processing junction: linspace(1, 0, NT) blend of the previous tail and the new transition frames.
Feats blendTransition(const Feats& latest, const Feats& motion, int NT, int D)
{
    Feats blended{NT, std::vector<float>(size_t(NT) * D)};
    for (int f = 0; f < NT; ++f) {
        const float alpha = NT > 1 ? 1.0f - float(f) / float(NT - 1) : 1.0f;
        for (int k = 0; k < D; ++k) {
            const size_t i = size_t(f) * D + size_t(k);
            blended.v[i] = latest.v[i] * alpha + (1.0f - alpha) * motion.v[i];
        }
    }
    return blended;
}

// Places a sampled segment in world space and appends it (post-processed, or blended into the previous tail).
void stitchSegment(Runtime& rt, std::vector<Feats>& generated, Feats motion, const SegmentSetup& s, int n, bool post)
{
    const Meta& m = rt.meta();
    if (s.first) {
        generated.push_back(post ? postprocess(rt, motion, nullptr) : std::move(motion));
        return;
    }
    for (int f = 0; f < s.length; ++f) {
        motion.v[size_t(f) * m.D + size_t(m.smoothRootStart)] += s.tr.root2d[0];
        motion.v[size_t(f) * m.D + size_t(m.smoothRootStart) + 2] += s.tr.root2d[1];
    }
    if (post) {
        generated.push_back(postprocess(rt, motion, &s.tr));
        return;
    }
    const int NT = m.transitionFrames;
    generated.push_back(blendTransition(s.latest, motion, NT, m.D));
    generated.push_back({n, std::vector<float>(motion.v.begin() + size_t(NT) * m.D, motion.v.end())});
}

// kimodo_model._multiprompt, transcribed from scripts/verify_kimodo_native.py (Native.generate).
Runtime::Decoded generateOne(Runtime& rt, const std::vector<int>& frames, const SampleInputs& in, int steps, bool post,
                             const std::function<bool()>& onStep)
{
    const KimodoMath::DdimSchedule sched = KimodoMath::ddimSchedule(rt.meta().acpBase, steps);
    std::vector<Feats> generated;
    for (size_t idx = 0; idx < frames.size(); ++idx) {
        const SegmentSetup s = prepareSegment(rt, generated, frames[idx]);
        Feats motion = sampleSegment(rt, sched, in.text[idx], in.noise[idx], s, onStep);
        stitchSegment(rt, generated, std::move(motion), s, frames[idx], post);
    }
    Feats full;
    for (const Feats& g : generated) {
        full.T += g.T;
        full.v.insert(full.v.end(), g.v.begin(), g.v.end());
    }
    return rt.decode(full);
}

// SOMA-30 -> SOMA-77 interchange arrays (relaxed-hand rest rotations for the joints the model does not drive).
InterchangeArrays toInterchange(const Meta& meta, const Runtime::Decoded& d, bool rootMotion)
{
    const size_t J77 = meta.names77.size();
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
    if (!rootMotion)
        for (int t = 0; t < d.T; ++t) {
            a.rootPositions[size_t(t) * 3] = d.root[0];
            a.rootPositions[size_t(t) * 3 + 2] = d.root[2];
        }
    a.contactChannels = 6;   // [L heel, L toe, L toe-end, R heel, R toe, R toe-end]
    for (int t = 0; t < d.T; ++t) {
        const float* c = &d.contacts[size_t(t) * 4];
        a.footContacts.insert(a.footContacts.end(), {c[0], c[1], c[1], c[2], c[3], c[3]});
    }
    return a;
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
    const auto fail = [error](const QString& msg) {
        if (error) *error = msg;
        return false;
    };
    if (qEnvironmentVariableIsSet("QTMESH_KIMODO_NO_DOWNLOAD")) return fail(QStringLiteral("QTMESH_KIMODO_NO_DOWNLOAD is set"));
    QDir().mkpath(modelDir());
    const std::vector<ModelFile> files = modelFiles();
    const int total = int(files.size());
    int done = 0;
    // The progress callback doubles as the cancel probe, polled DURING each transfer too (the GGUF alone is 8 GB).
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
        req.label = QStringLiteral("kimodo/%1").arg(f.name);
        req.timeoutMs = f.approxBytes > 1000000000LL ? 4 * 3600 * 1000 : 30 * 60 * 1000;
        req.expectedSha256 = f.sha256;
        req.cancelled = probe;
        if (const ModelFetch::Outcome out = ModelFetch::ensureBlocking(req); !out.ok)
            return fail(out.cancelled ? QStringLiteral("cancelled") : QStringLiteral("%1: %2").arg(f.name, out.error));
        ++done;
    }
    // A Stop pressed during the last file still counts.
    if (probe()) return fail(QStringLiteral("cancelled"));
    return true;
}

#ifdef ENABLE_ONNX
namespace {

// Everything predict() needs before the first graph runs.
struct NativeJob {
    Meta meta;
    QString dir;
    int steps = 100;
    std::vector<int> frames;
    QStringList texts;
    std::vector<std::vector<float>> textFeat;
    AudioToFace::NpzReader fixture;
    bool useFixture = false;
    bool fixtureText = false;
};

QString notReadyReason(const KimodoPredictor::Options& o, const QString& fixturePath)
{
    if (!KimodoNative::buildSupported()) return KimodoNative::description();
    if (!o.constraintsPath.isEmpty())
        return QStringLiteral("Kimodo constraints files are supported by the Python runtime only (--runtime python)");
    // A fixture (tests / parity checks: text features + noise from an NPZ) replaces the text encoder, so only the
    // GGUF and its licence texts may be absent then.
    QStringList missing;
    KimodoNative::modelsInstalled(&missing);
    if (!fixturePath.isEmpty())
        missing.removeIf([](const QString& n) { return n.contains(QLatin1String("Llama-3")) || n.contains(QLatin1String("LLM2Vec")); });
    if (!missing.isEmpty())
        return QStringLiteral("the native Kimodo models are not downloaded (~%1 GB): request the download first "
                              "(CLI --download, MCP download:true, or the Download button)")
            .arg(double(KimodoNative::missingBytes()) / 1e9, 0, 'f', 1);
    return {};
}

bool prepareJob(const KimodoPredictor::Options& o, NativeJob* job, QString* err)
{
    job->dir = KimodoNative::modelDir();
    if (!loadMeta(QDir(job->dir).filePath(QStringLiteral("kimodo_meta.json")), &job->meta, err)) return false;
    job->steps = o.diffusionSteps > 0 ? o.diffusionSteps : 100;
    if (job->steps > int(job->meta.acpBase.size())) {
        *err = QStringLiteral("diffusion steps must be at most %1").arg(job->meta.acpBase.size());
        return false;
    }
    for (const MotionSegment& s : o.segments) {
        const int f = std::max(2, int(s.duration * double(job->meta.fps)));
        if (f < 8) {
            *err = QStringLiteral("every segment must be at least %1 s long").arg(8.0 / double(job->meta.fps), 0, 'f', 2);
            return false;
        }
        job->frames.push_back(f);
        job->texts << KimodoMath::sanitizeText(s.text);
    }
    const QString fixturePath = qEnvironmentVariable("QTMESH_KIMODO_NATIVE_FIXTURE");
    if (!fixturePath.isEmpty() && !job->fixture.open(fixturePath, err)) {
        *err = QStringLiteral("fixture: %1").arg(*err);
        return false;
    }
    job->useFixture = !fixturePath.isEmpty();
    job->fixtureText = job->useFixture && job->fixture.contains(QStringLiteral("text_0"));
    return true;
}

// Text features: from the fixture when it carries them (text_<s>), else the llama.cpp encoder.
bool encodeTexts(NativeJob* job, const KimodoPredictor::ProgressFn& progress, QString* err)
{
    if (job->fixtureText) {
        for (int s = 0; s < int(job->texts.size()); ++s) {
            const auto a = job->fixture.read(QStringLiteral("text_%1").arg(s), err);
            if (!a.valid() || a.data.size() != size_t(job->meta.textDim)) {
                *err = QStringLiteral("fixture text_%1: %2").arg(s).arg(*err);
                return false;
            }
            job->textFeat.push_back(a.data);
        }
        return true;
    }
    if (progress && !progress(QStringLiteral("load_text_encoder"), 0, 1)) {
        *err = QStringLiteral("cancelled");
        return false;
    }
    StageTimer timer("text encoder");
    const auto enc = KimodoTextEncoder::encode(QDir(job->dir).filePath(QString::fromLatin1(kTextEncoderFile)), job->texts, 99,
                                               [&progress] { return progress && !progress(QStringLiteral("encode_text"), 0, 1); });
    if (!enc.ok) {
        *err = enc.error;
        return false;
    }
    job->textFeat = enc.embeddings;
    return true;
}

// Gaussian diffusion noise for variation v, segment s (or the fixture's). Not a security context: std::mt19937_64
// gives the reproducible, seedable stream a --seed promises.
std::vector<float> noiseFor(NativeJob& job, std::mt19937_64& rng, int v, size_t s)
{
    const int len = job.frames[s] + (s == 0 ? 0 : job.meta.transitionFrames);
    std::vector<float> noise(size_t(len) * job.meta.D);
    if (job.useFixture) {
        QString err;
        const auto a = job.fixture.read(QStringLiteral("noise_%1_%2").arg(v).arg(s), &err);
        if (!a.valid() || a.data.size() != noise.size())
            throw KimodoFailure(QStringLiteral("fixture noise_%1_%2: %3").arg(v).arg(s).arg(err).toStdString());
        return a.data;
    }
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    for (float& x : noise) x = gauss(rng);
    return noise;
}

QJsonObject manifestOf(const KimodoPredictor::Options& o, const NativeJob& job, const QStringList& written, double seconds)
{
    QJsonArray segs;
    for (const MotionSegment& s : o.segments)
        segs.append(QJsonObject{{QStringLiteral("text"), s.text}, {QStringLiteral("duration"), s.duration}});
    return QJsonObject{
        {QStringLiteral("format"), QStringLiteral("qtmesh-motion-interchange")},
        {QStringLiteral("format_version"), kInterchangeVersion},
        {QStringLiteral("source"), QStringLiteral("kimodo")},
        {QStringLiteral("runtime"), QStringLiteral("native")},
        {QStringLiteral("segments"), segs},
        {QStringLiteral("text_conditioning"), job.fixtureText ? QStringLiteral("fixture") : QStringLiteral("prompt")},
        {QStringLiteral("seed"), o.hasSeed ? QJsonValue(o.seed) : QJsonValue()},
        {QStringLiteral("text_encoder"), QStringLiteral("llama.cpp Q8_0 (Llama-3-LLM2Vec-8B-Kimodo-TextEncoder)")},
        {QStringLiteral("num_samples"), o.numSamples},
        {QStringLiteral("diffusion_steps"), job.steps},
        {QStringLiteral("root_motion"), o.rootMotion},
        {QStringLiteral("postprocess"), o.postprocess},
        {QStringLiteral("files"), QJsonArray::fromStringList(written)},
        {QStringLiteral("fps"), double(job.meta.fps)},
        {QStringLiteral("joints"), int(job.meta.names77.size())},
        {QStringLiteral("model"), QStringLiteral("kimodo-soma-rp-v1.2")},
        {QStringLiteral("device"), QStringLiteral("onnxruntime")},
        {QStringLiteral("seconds"), seconds},
    };
}

// Every variation through the motion model; writes motion_NN.npz + result.json. Throws KimodoFailure/KimodoCancelled.
KimodoPredictor::Result runVariations(const KimodoPredictor::Options& o, NativeJob& job, const QString& outDir,
                                      const KimodoPredictor::ProgressFn& progress)
{
    std::unique_ptr<Runtime> rt;
    {
        StageTimer timer("load onnx");
        rt = std::make_unique<Runtime>(job.meta, job.dir);
    }
    // Reproducible diffusion noise from --seed, NOT a security context (no secret derives from it).
    std::mt19937_64 rng(o.hasSeed ? uint64_t(uint32_t(o.seed)) : uint64_t(std::random_device{}()));   // NOSONAR
    const int totalSteps = job.steps * int(job.frames.size()) * o.numSamples;
    int doneSteps = 0;
    const auto onStep = [&progress, &doneSteps, totalSteps] {
        ++doneSteps;
        return !progress || progress(QStringLiteral("generate"), doneSteps, totalSteps);
    };
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    KimodoPredictor::Result result;
    result.outputDir = outDir;
    for (int v = 0; v < o.numSamples; ++v) {
        SampleInputs in;
        in.text = job.textFeat;
        for (size_t s = 0; s < job.frames.size(); ++s) in.noise.push_back(noiseFor(job, rng, v, s));
        const Runtime::Decoded d = generateOne(*rt, job.frames, in, job.steps, o.postprocess, onStep);
        const QString path = QDir(outDir).filePath(QStringLiteral("motion_%1.npz").arg(v, 2, 10, QLatin1Char('0')));
        if (QString err; !saveInterchange(path, toInterchange(job.meta, d, o.rootMotion), &err)) throw KimodoFailure(err.toStdString());
        result.files << path;
        if (progress) progress(QStringLiteral("write"), v + 1, o.numSamples);
    }
    result.manifest = manifestOf(o, job, result.files, double(QDateTime::currentMSecsSinceEpoch() - t0) / 1000.0);
    QFile mf(QDir(outDir).filePath(QStringLiteral("result.json")));
    if (mf.open(QIODevice::WriteOnly | QIODevice::Truncate)) mf.write(QJsonDocument(result.manifest).toJson());
    result.ok = true;
    return result;
}

} // namespace
#endif

KimodoPredictor::Result KimodoNative::predict(const KimodoPredictor::Options& o, const KimodoPredictor::ProgressFn& progress)
{
#ifndef ENABLE_ONNX
    Q_UNUSED(o);
    Q_UNUSED(progress);
    return failed(description());
#else
    if (QString err; !KimodoPredictor::validateOptions(o, &err)) return failed(err);
    if (const QString why = notReadyReason(o, qEnvironmentVariable("QTMESH_KIMODO_NATIVE_FIXTURE")); !why.isEmpty())
        return failed(why);
    NativeJob job;
    QString err;
    if (!prepareJob(o, &job, &err) || !encodeTexts(&job, progress, &err)) return failed(err);
    if (progress && !progress(QStringLiteral("load_model"), 0, 1)) return failed(QStringLiteral("cancelled"));
    QString outDir = o.outputDir;
    if (outDir.isEmpty())
        outDir = QDir(AppStorage::persistentRoot()).filePath(QStringLiteral("generated_motion/kimodo_%1").arg(QDateTime::currentMSecsSinceEpoch()));
    if (!QDir().mkpath(outDir)) return failed(QStringLiteral("cannot create %1").arg(outDir));
    try {
        return runVariations(o, job, outDir, progress);
    } catch (const KimodoCancelled&) {
        return failed(QStringLiteral("cancelled"));
    } catch (const KimodoFailure& e) {
        return failed(QStringLiteral("native Kimodo failed: %1").arg(QString::fromUtf8(e.what())));
    } catch (const Ort::Exception& e) {
        return failed(QStringLiteral("native Kimodo failed (ONNX Runtime): %1").arg(QString::fromUtf8(e.what())));
    }
#endif
}

} // namespace AIMotion
