#include "A2FPredictor.h"

#include "A2FCoefficients.h"
#include "AudioPreprocess.h"
#include "BlendshapeSolve.h"
#include "NpzReader.h"

#include "../AppStorage.h"
#include "../ModelFetch.h"
#include "../SentryReporter.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

#include <algorithm>
#include <cmath>
#include <thread>

namespace AudioToFace {

namespace {

// NVIDIA's Hugging Face repo. Unlike our other models this is a THIRD-PARTY
// host, so the base URL is overridable the same way and the licence
// obligation travels with the files (see the NOTICE written beside them).
constexpr const char* kDefaultBaseUrl =
    "https://huggingface.co/nvidia/Audio2Face-3D-v2.3-Mark/resolve/main/";
constexpr const char* kBaseUrlSettingsKey = "ai/a2fModelBaseUrl";
constexpr const char* kEnvBaseUrl = "QTMESH_A2F_MODEL_BASE_URL";
constexpr const char* kEnvNoDownload = "QTMESH_A2F_NO_DOWNLOAD";
constexpr const char* kBreadcrumb = "ai.assist.audio2face";

/// Scales the solved `mouthClose` weight before it is emitted. NVIDIA's basis
/// and a real ARKit rig author this shape with OPPOSITE conventions (see the
/// long note in `load`), so the raw weight over-drives the lower lip. 0.5 was
/// chosen by measurement, not taste: over a 58-frame take, 9 frames had
/// `mouthClose` running at more than twice `jawOpen` -- the frames that read as
/// unnatural -- and halving the weight is what brings the pair back into the
/// jaw-dominant balance the shape is meant to be a correction to.
constexpr float kMouthCloseScale = 0.5f;

// The four files the pipeline needs, with their sizes for the timeout budget.
// `sha256` is NVIDIA's published Git-LFS oid, verified against a real
// download. Pinning matters here for the reason #1025 documented: a model
// that arrives corrupted is byte-identical in SIZE and loads without
// complaint, then produces garbage nobody traces back to the file. The two
// JSON files are small and not LFS-tracked, so they carry no published oid
// and stay unpinned rather than pinned to a value we made up ourselves.
struct ModelFile { const char* name; int timeoutMs; const char* sha256; };
constexpr ModelFile kFiles[] = {
    {"network.onnx",         600000,
     "dcd16d3b1affb090d4da1ba88791faa6bcd755f97c853b3b667abad9be15cb4b"},   // ~76 MB
    {"model_data.npz",      1800000,
     "03790f24942ec26796ce101c2e238eb7ff5c1bb94a6c14a840ffcd530d3a301e"},   // ~204 MB — the PCA basis
    {"bs_skin.npz",          900000,
     "95fbe12499271a9c1ca0d718262fad76d6218f026a450d07006c6b70bc001cc5"},   // ~39 MB  — the ARKit deltas
    {"network_info.json",     60000, nullptr},
    {"bs_skin_config.json",   60000, nullptr},
};

// Required by the NVIDIA Open Model License when the weights are
// redistributed or shipped onward. Written beside them so it travels with any
// copy of the directory.
constexpr const char* kNoticeText =
    "Audio2Face-3D model weights\n"
    "Licensed by NVIDIA Corporation under the NVIDIA Open Model License\n"
    "https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-open-model-license/\n"
    "\n"
    "Source: https://huggingface.co/nvidia/Audio2Face-3D-v2.3-Mark\n"
    "\n"
    "The Audio2Emotion models are licensed separately and are NOT distributed\n"
    "here: their terms permit use only together with Audio2Face, which does\n"
    "not meet this project's redistribution bar.\n";

QString resolveBaseUrl()
{
    QSettings s;
    QString base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
    if (base.isEmpty()) {
        const QByteArray env = qgetenv(kEnvBaseUrl);
        base = env.isEmpty() ? QString::fromLatin1(kDefaultBaseUrl)
                             : QString::fromUtf8(env);
    }
    if (!base.isEmpty() && !base.endsWith(QLatin1Char('/')))
        base += QLatin1Char('/');
    return base;
}

}  // namespace

struct A2FPredictor::Impl {
    bool loaded = false;
    CoefficientLayout layout;
    PcaBasis skin;
    PoseBasis poseBasis;
    SolveOptions solve;
    QStringList poseNames;
    QString version;
#ifdef ENABLE_ONNX
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> inputNames;
    std::vector<std::string> outputNames;
    int emotionWidth = 26;
#endif
};

A2FPredictor::A2FPredictor() : m_impl(std::make_unique<Impl>()) {}
A2FPredictor::~A2FPredictor() = default;

bool A2FPredictor::available()
{
#ifdef ENABLE_ONNX
    return true;
#else
    return false;
#endif
}

QString A2FPredictor::modelDirectory()
{
    return QDir(AppStorage::aiModelsRoot()).filePath(QStringLiteral("a2f"));
}

bool A2FPredictor::present()
{
    const QDir dir(modelDirectory());
    for (const auto& f : kFiles)
        if (!QFileInfo::exists(dir.filePath(QString::fromLatin1(f.name))))
            return false;
    return true;
}

QString A2FPredictor::ensureModelBlocking(QString* error)
{
    const QString dir = modelDirectory();
    QDir().mkpath(dir);

    QStringList missing;
    bool anyPinned = false;
    for (const auto& f : kFiles) {
        if (!QFileInfo::exists(QDir(dir).filePath(QString::fromLatin1(f.name))))
            missing << QString::fromLatin1(f.name);
        else if (f.sha256)
            anyPinned = true;
    }
    // Returning early on mere EXISTENCE would skip ModelFetch entirely, and
    // with it the digest check on files already on disk -- which is the case
    // #1025 was actually about: a corrupted model is byte-identical in size,
    // loads without complaint, and produces garbage. So when anything is
    // pinned we still go through ModelFetch, which hashes an existing file
    // (cached per path+size+mtime, so the 204 MB basis is hashed once, not
    // once per run) and re-fetches only on a mismatch.
    if (missing.isEmpty() && !anyPinned) return dir;

    // The offline guard only bites when something is genuinely MISSING. With
    // every file present it must stay out of the way -- we are here only to
    // re-verify digests, and ModelFetch short-circuits a file that matches
    // without touching the network.
    if (!missing.isEmpty() && !qEnvironmentVariableIsEmpty(kEnvNoDownload)) {
        if (error)
            *error = QStringLiteral("Audio2Face models are missing and %1 is set")
                         .arg(QString::fromLatin1(kEnvNoDownload));
        return {};
    }
    const QString base = resolveBaseUrl();
    if (base.isEmpty()) {
        if (error) *error = QStringLiteral("no Audio2Face model base URL configured");
        return {};
    }

    SentryReporter::addBreadcrumb(QString::fromLatin1(kBreadcrumb),
        QStringLiteral("model download start (%1 files)").arg(missing.size()));

    for (const auto& f : kFiles) {
        const QString name = QString::fromLatin1(f.name);
        const QString dest = QDir(dir).filePath(name);
        // Skip an existing file ONLY when there is no digest to check it
        // against. With a pin, ModelFetch must see it: that is what detects a
        // file that downloaded corrupt (identical size, loads fine, produces
        // garbage) and re-fetches it. A matching file costs one cached hash.
        if (QFileInfo::exists(dest) && !f.sha256) continue;

        ModelFetch::Request req;
        req.url = base + name;
        req.destination = dest;
        req.label = QStringLiteral("Audio2Face %1").arg(name);
        req.timeoutMs = f.timeoutMs;
        // Only when fetching from NVIDIA's own repo: a mirror override may
        // legitimately serve a different export, and pinning the stock digest
        // against it would refuse a file the user deliberately pointed us at.
        // Same rule UniRig follows.
        if (f.sha256 && base == QString::fromLatin1(kDefaultBaseUrl))
            req.expectedSha256 = QString::fromLatin1(f.sha256);
        const ModelFetch::Outcome out = ModelFetch::ensureBlocking(req);
        if (!out.ok) {
            // Carry the downloader's own words: "refusing http://" and
            // "SHA-256 mismatch" are actionable, "unavailable (offline?)" is
            // not (#1037).
            if (error)
                *error = QStringLiteral("Audio2Face model '%1' unavailable: %2")
                             .arg(name, out.error);
            SentryReporter::addBreadcrumb(QString::fromLatin1(kBreadcrumb),
                QStringLiteral("model download failed: %1").arg(name));
            return {};
        }
    }

    // The licence requires this to accompany the weights.
    const QString notice = QDir(dir).filePath(QStringLiteral("NOTICE.txt"));
    if (!QFileInfo::exists(notice)) {
        QFile f(notice);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
            f.write(kNoticeText);
    }

    SentryReporter::addBreadcrumb(QString::fromLatin1(kBreadcrumb),
        QStringLiteral("model download ok"));
    return dir;
}

bool A2FPredictor::isLoaded() const { return m_impl && m_impl->loaded; }

QStringList A2FPredictor::poseNames() const
{
    return m_impl ? m_impl->poseNames : QStringList{};
}

bool A2FPredictor::load(QString* error)
{
#ifndef ENABLE_ONNX
    if (error)
        *error = QStringLiteral("Audio2Face needs ONNX Runtime; rebuild with "
                                "-DENABLE_ONNX=ON");
    return false;
#else
    if (m_impl->loaded) return true;

    const QString dir = ensureModelBlocking(error);
    if (dir.isEmpty()) return false;
    auto path = [&](const char* n) { return QDir(dir).filePath(QString::fromLatin1(n)); };

    // --- sizes, read from the shipped metadata rather than hardcoded: the
    // diffusion model and the per-actor regression models disagree on these.
    {
        QFile f(path("network_info.json"));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
            const QJsonObject p = root.value(QStringLiteral("params")).toObject();
            if (p.contains(QStringLiteral("num_shapes_skin")))
                m_impl->layout.skinShapes = p.value(QStringLiteral("num_shapes_skin")).toInt();
            if (p.contains(QStringLiteral("num_shapes_tongue")))
                m_impl->layout.tongueShapes = p.value(QStringLiteral("num_shapes_tongue")).toInt();
            if (p.contains(QStringLiteral("result_jaw_size")))
                m_impl->layout.jawValues = p.value(QStringLiteral("result_jaw_size")).toInt();
            if (p.contains(QStringLiteral("result_eyes_size")))
                m_impl->layout.eyeValues = p.value(QStringLiteral("result_eyes_size")).toInt();
            const QJsonObject id = root.value(QStringLiteral("id")).toObject();
            m_impl->version = QStringLiteral("%1 %2")
                                  .arg(id.value(QStringLiteral("actor")).toString(),
                                       id.value(QStringLiteral("version")).toString())
                                  .trimmed();
        }
    }

    // --- PCA basis
    {
        NpzReader r;
        QString e;
        if (!r.open(path("model_data.npz"), &e)) {
            if (error) *error = QStringLiteral("model_data.npz: %1").arg(e);
            return false;
        }
        const auto mean = r.read(QStringLiteral("shapes_mean_skin"), &e);
        const auto mat  = r.read(QStringLiteral("shapes_matrix_skin"), &e);
        if (!mean.valid() || !mat.valid()) {
            if (error) *error = QStringLiteral("model_data.npz is missing the skin basis: %1").arg(e);
            return false;
        }
        m_impl->skin.valueCount = mean.data.size();
        m_impl->skin.shapeCount = int(mat.data.size() / std::max<size_t>(1, mean.data.size()));
        m_impl->skin.mean = mean.data;
        m_impl->skin.matrix = mat.data;
        if (!m_impl->skin.valid()) {
            if (error) *error = QStringLiteral("the skin PCA basis is inconsistent");
            return false;
        }
    }

    // --- ARKit blendshape deltas.
    //
    // bs_skin.npz stores each pose ALREADY AS A DELTA from the neutral, not as
    // an absolute position. Measured: `neutral` has rms 89.34 while `jawOpen`
    // has rms 0.47 and begins at exactly (0,0,0) — a displacement field, not a
    // face. An earlier version of this subtracted the neutral anyway, which
    // turned each pose into the negated neutral (rms 89.48) and made the basis
    // describe "move the whole head to the origin" instead of "open the jaw".
    // The solve then explained none of the predicted motion: every weight came
    // back 0 with residual 0.93, i.e. a face that never moves.
    {
        NpzReader r;
        QString e;
        if (!r.open(path("bs_skin.npz"), &e)) {
            if (error) *error = QStringLiteral("bs_skin.npz: %1").arg(e);
            return false;
        }
        m_impl->poseNames = r.readStrings(QStringLiteral("poseNames"), &e);
        const auto neutral = r.read(QStringLiteral("neutral"), &e);
        if (!neutral.valid()) {
            if (error) *error = QStringLiteral("bs_skin.npz has no neutral pose: %1").arg(e);
            return false;
        }
        std::vector<PoseDelta> deltas;
        QStringList kept;
        for (const QString& n : m_impl->poseNames) {
            if (n == QStringLiteral("neutral")) continue;   // the reference, not a pose
            const auto p = r.read(n, &e);
            if (!p.valid() || p.data.size() != neutral.data.size()) continue;
            deltas.push_back(p.data);     // already a delta — see above
            kept << n;
        }
        if (deltas.empty()) {
            if (error) *error = QStringLiteral("bs_skin.npz yielded no usable poses");
            return false;
        }
        m_impl->poseNames = kept;
        m_impl->poseBasis.build(deltas);
        if (!m_impl->poseBasis.valid()) {
            if (error) *error = QStringLiteral("the blendshape basis is inconsistent");
            return false;
        }
    }

    // --- solver strengths + per-pose tables, from the shipped config
    {
        QFile f(path("bs_skin_config.json"));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject p = QJsonDocument::fromJson(f.readAll()).object()
                                      .value(QStringLiteral("blendshape_params")).toObject();
            auto num = [&](const char* k, double dflt) {
                return p.contains(QString::fromLatin1(k))
                           ? p.value(QString::fromLatin1(k)).toDouble() : dflt;
            };
            m_impl->solve.l2       = num("strengthL2regularization", 0.5);
            m_impl->solve.l1       = num("strengthL1regularization", 0.5);
            m_impl->solve.temporal = num("strengthTemporalSmoothing", 0.3);
            m_impl->solve.symmetry = num("strengthSymmetry", 100.0);
            auto ints = [&](const char* k, std::vector<int>& out) {
                for (const auto v : p.value(QString::fromLatin1(k)).toArray())
                    out.push_back(v.toInt());
            };
            auto flts = [&](const char* k, std::vector<float>& out) {
                for (const auto v : p.value(QString::fromLatin1(k)).toArray())
                    out.push_back(float(v.toDouble()));
            };
            ints("bsSolveActivePoses",   m_impl->solve.active);
            ints("bsSolveSymmetryPoses", m_impl->solve.symmetryPartner);
            flts("bsWeightMultipliers",  m_impl->solve.multipliers);
            flts("bsWeightOffsets",      m_impl->solve.offsets);

            // The config's tables are indexed over the FULL 52 including
            // `neutral`, which the basis drops. Realigning here rather than at
            // use keeps the solver free of NVIDIA's indexing quirk.
            auto dropFirst = [](auto& v) { if (!v.empty()) v.erase(v.begin()); };
            if (int(m_impl->solve.active.size()) == int(m_impl->poseNames.size()) + 1) {
                dropFirst(m_impl->solve.active);
                dropFirst(m_impl->solve.symmetryPartner);
                dropFirst(m_impl->solve.multipliers);
                dropFirst(m_impl->solve.offsets);
                for (int& s : m_impl->solve.symmetryPartner)
                    if (s > 0) --s; else if (s == 0) s = -1;
            }
        }
    }

    // --- counter-shape correction: `mouthClose`
    //
    // ARKit defines `mouthClose` as a COUNTER-shape: it is only meaningful as
    // a partial cancellation of `jawOpen`, so on a correctly authored rig it
    // lifts the lower lip hard. NVIDIA's character does NOT follow that
    // convention -- measured on the shipped basis, its `mouthClose` moves
    // 16364 verts DOWN and only 97 up (cosine with `jawOpen` +0.41), whereas a
    // real ARKit rig (the ICT template `qtmesh facerig` generates) moves 12903
    // verts UP by as much as 3.81 (cosine -0.46).
    //
    // So a weight the solver picks to mean "tuck the lips a little" against
    // NVIDIA's basis costs almost no upward motion there, but drives the lower
    // lip to the nose on the rig it is replayed on. Scaling the weight down is
    // the honest correction: the ICT shape follows the ARKit convention and is
    // shared with hand-authored animation, so it must not be altered, and the
    // solver's own fit is measured against NVIDIA's mesh where the weight is
    // right. The multiplier table is exactly the hook for this, and NVIDIA
    // ships a flat 1.0 for every pose (`bsSolveCancelPoses` is all -1 for this
    // character), so nothing tuned is being overwritten.
    {
        const int idx = m_impl->poseNames.indexOf(QStringLiteral("mouthClose"));
        if (idx >= 0) {
            if (int(m_impl->solve.multipliers.size()) <= idx)
                m_impl->solve.multipliers.resize(size_t(idx) + 1, 1.0f);
            m_impl->solve.multipliers[size_t(idx)] *= kMouthCloseScale;
        }
    }

    // --- ONNX session
    try {
        m_impl->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "a2f");
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(std::max(1u, std::thread::hardware_concurrency() / 2));
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // ORT takes ORTCHAR_T, which is wchar_t on Windows -- a narrow path
        // does not even compile there, and a UTF-8 one would mangle a
        // non-ASCII profile directory. Same split as FaceLandmarkDetector.
        const QString modelPath = path("network.onnx");
#ifdef _WIN32
        const std::wstring mp = modelPath.toStdWString();
#else
        const std::string mp = modelPath.toStdString();
#endif
        m_impl->session = std::make_unique<Ort::Session>(
            *m_impl->env, mp.c_str(), so);

        Ort::AllocatorWithDefaultOptions al;
        for (size_t i = 0; i < m_impl->session->GetInputCount(); ++i) {
            auto n = m_impl->session->GetInputNameAllocated(i, al);
            m_impl->inputNames.emplace_back(n.get());
            if (std::string(n.get()) == "emotion") {
                const auto sh = m_impl->session->GetInputTypeInfo(i)
                                    .GetTensorTypeAndShapeInfo().GetShape();
                if (!sh.empty() && sh.back() > 0) m_impl->emotionWidth = int(sh.back());
            }
        }
        for (size_t i = 0; i < m_impl->session->GetOutputCount(); ++i) {
            auto n = m_impl->session->GetOutputNameAllocated(i, al);
            m_impl->outputNames.emplace_back(n.get());
        }
    } catch (const Ort::Exception& ex) {
        if (error) *error = QStringLiteral("Audio2Face ONNX session failed: %1")
                                .arg(QString::fromUtf8(ex.what()));
        return false;
    }

    m_impl->loaded = true;
    return true;
#endif
}

PredictResult A2FPredictor::predict(const std::vector<float>& samples,
                                    int sampleRate,
                                    int channels,
                                    const PredictOptions& options)
{
    PredictResult r;
#ifndef ENABLE_ONNX
    (void)samples; (void)sampleRate; (void)channels; (void)options;
    r.error = QStringLiteral("Audio2Face needs ONNX Runtime; rebuild with -DENABLE_ONNX=ON");
    return r;
#else
    if (!m_impl->loaded && !load(&r.error)) return r;
    if (samples.empty()) {
        r.error = QStringLiteral("no audio samples");
        return r;
    }
    r.modelVersion = m_impl->version;

    std::vector<float> audio = toMono(samples, channels);
    audio = resampleTo16k(audio, sampleRate);
    if (options.normalise) audio = normalisePeak(audio);
    if (audio.empty()) {
        r.error = QStringLiteral("audio became empty after resampling");
        return r;
    }

    const auto centres = frameCentres(audio.size(), options.fps);
    if (centres.empty()) {
        r.error = QStringLiteral("no frames for fps %1").arg(options.fps);
        return r;
    }

    std::vector<float> emotion(size_t(m_impl->emotionWidth), 0.0f);
    for (size_t i = 0; i < options.emotion.size() && i < emotion.size(); ++i)
        emotion[i] = options.emotion[i];

    auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<const char*> inNames, outNames;
    for (const auto& s : m_impl->inputNames) inNames.push_back(s.c_str());
    for (const auto& s : m_impl->outputNames) outNames.push_back(s.c_str());

    std::vector<float> previous;   // warm-starts the solve and feeds the
                                   // temporal term; empty on the first frame
    r.frames.reserve(centres.size());

    for (size_t fi = 0; fi < centres.size(); ++fi) {
        if (options.progress && !options.progress(int(fi), int(centres.size()))) {
            r.cancelled = true;
            break;
        }

        std::vector<float> window = windowAt(audio, centres[fi]);
        std::vector<int64_t> aShape{1, 1, int64_t(kWindowSamples)};
        std::vector<int64_t> eShape{1, 1, int64_t(emotion.size())};

        // Bind BY NAME, not by push order. This model happens to declare
        // `input` then `emotion` (verified against the shipped graph), so a
        // positional bind works today -- but if a future revision swaps them,
        // audio would be fed into the emotion slot and the only symptom is a
        // face that moves wrongly. Order the tensors to match inNames.
        std::vector<Ort::Value> ins;
        ins.reserve(inNames.size());
        for (const char* n : inNames) {
            if (std::string(n) == "emotion")
                ins.push_back(Ort::Value::CreateTensor<float>(
                    mem, emotion.data(), emotion.size(), eShape.data(), eShape.size()));
            else
                ins.push_back(Ort::Value::CreateTensor<float>(
                    mem, window.data(), window.size(), aShape.data(), aShape.size()));
        }
        std::vector<float> coeffs;
        try {
            auto out = m_impl->session->Run(Ort::RunOptions{nullptr}, inNames.data(),
                                            ins.data(), ins.size(),
                                            outNames.data(), outNames.size());
            const float* p = out[0].GetTensorData<float>();
            const size_t n = out[0].GetTensorTypeAndShapeInfo().GetElementCount();
            coeffs.assign(p, p + n);
        } catch (const Ort::Exception& ex) {
            r.error = QStringLiteral("Audio2Face inference failed: %1")
                          .arg(QString::fromUtf8(ex.what()));
            return r;
        }

        const auto deform = reconstructSkin(m_impl->skin, coeffs, m_impl->layout);
        if (deform.empty()) {
            r.error = QStringLiteral("PCA reconstruction failed (expected %1 coefficients, "
                                     "got %2)").arg(m_impl->layout.total()).arg(coeffs.size());
            return r;
        }
        // The solve wants motion relative to rest, not absolute positions.
        std::vector<float> delta(deform.size());
        for (size_t k = 0; k < delta.size(); ++k) delta[k] = deform[k] - m_impl->skin.mean[k];

        const auto solved = m_impl->poseBasis.solve(delta, previous, m_impl->solve);
        FaceFrame f;
        f.timeSec = double(centres[fi]) / double(kSampleRate);
        f.weights = solved.weights;
        f.solveResidual = solved.residual;
        previous = solved.weights;
        r.frames.push_back(std::move(f));
    }

    if (r.frames.empty() && r.error.isEmpty())
        r.error = QStringLiteral("cancelled before any frame was solved");
    return r;
#endif
}

}  // namespace AudioToFace
