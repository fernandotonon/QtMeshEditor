#include "TripoSGPredictor.h"

#include "MarchingCubes.h"
#include "MeshRefine.h"
#include "PbrMapSynth.h"   // toNCHW (image → planar [0,1])

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

#ifdef ENABLE_ONNX
#include "ModelDownloader.h"
#include <QEventLoop>
#include <QSettings>
#include <QTimer>
#include <onnxruntime_cxx_api.h>
#include <string>
#include <unordered_map>
#endif

namespace {

constexpr const char* kImageEncoderFile = "triposg_image_encoder.onnx";
constexpr const char* kDitStepFile      = "triposg_dit_step.onnx";
constexpr const char* kDitStepDataFile  = "triposg_dit_step.onnx.data";  // >2GB external weights
constexpr const char* kDitStepInt8File  = "triposg_dit_step_int8.onnx";
constexpr const char* kVaeLatentsFile   = "triposg_vae_latents.onnx";
constexpr const char* kVaeDecoderFile   = "triposg_vae_decoder.onnx";
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/triposg/";
constexpr const char* kBaseUrlSettingsKey = "ai/triposgModelBaseUrl";

// Query-point half-extent of the VAE's SDF field, measured from the reference
// pipeline (docs/TRIPOSG_EXPORT_NOTES.md): bounds (−1.005, 1.005)³.
constexpr float kRadius = 1.005f;

// Field convention: the exported decoder graph already NEGATES the raw SDF so
// the field is INSIDE-POSITIVE — the same convention our MarchingCubes uses
// (upstream runs skimage marching_cubes at level 0 on the same negated grid).
constexpr float kIsoLevel = 0.0f;

// TripoSG's custom RectifiedFlowScheduler: num_train_timesteps=1000, shift=1
// (identity) → σᵢ = 1 − i/N with σ_N = 0, and the DiT's timestep input is
// 1000·σ. See docs/TRIPOSG_EXPORT_NOTES.md.
constexpr float kNumTrainTimesteps = 1000.0f;

QString modelDir()
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("ai_models/triposg/"));
}

} // namespace

TripoSGPredictor::Options::Options() = default;

QString TripoSGPredictor::imageEncoderPath()
{
    return QDir(modelDir()).filePath(QString::fromLatin1(kImageEncoderFile));
}

QString TripoSGPredictor::ditStepPath(bool int8Tier)
{
    return QDir(modelDir()).filePath(
        QString::fromLatin1(int8Tier ? kDitStepInt8File : kDitStepFile));
}

QString TripoSGPredictor::vaeLatentsPath()
{
    return QDir(modelDir()).filePath(QString::fromLatin1(kVaeLatentsFile));
}

QString TripoSGPredictor::vaeDecoderPath()
{
    return QDir(modelDir()).filePath(QString::fromLatin1(kVaeDecoderFile));
}

bool TripoSGPredictor::modelsPresent(bool int8Tier)
{
    if (!QFileInfo::exists(imageEncoderPath())
        || !QFileInfo::exists(ditStepPath(int8Tier))
        || !QFileInfo::exists(vaeLatentsPath())
        || !QFileInfo::exists(vaeDecoderPath()))
        return false;
    // The fp32 DiT ships as .onnx + .onnx.data (external weights >2GB); the
    // graph is unloadable without its sidecar.
    if (!int8Tier
        && !QFileInfo::exists(QDir(modelDir()).filePath(
               QString::fromLatin1(kDitStepDataFile))))
        return false;
    return true;
}

#ifndef ENABLE_ONNX

bool TripoSGPredictor::isAvailable() { return false; }

QString TripoSGPredictor::ensureModelBlocking(bool) { return {}; }

MeshGenPredictor::Result TripoSGPredictor::predict(
    const QImage&, const Options&, const MeshGenPredictor::ProgressFn&)
{
    MeshGenPredictor::Result r;
    r.error = QStringLiteral(
        "TripoSG generation was not built into this binary "
        "(rebuild with -DENABLE_ONNX=ON).");
    return r;
}

#else // ENABLE_ONNX

bool TripoSGPredictor::isAvailable() { return true; }

QString TripoSGPredictor::ensureModelBlocking(bool int8Tier)
{
    if (modelsPresent(int8Tier))
        return imageEncoderPath();

    // Offline / test guard — never hit the network when set.
    if (!qEnvironmentVariableIsEmpty("QTMESH_TRIPOSG_NO_DOWNLOAD"))
        return {};

    // Resolve the download base URL (QSettings override → env → default HF repo).
    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_TRIPOSG_MODEL_BASE_URL");
            base = env.isEmpty() ? QString::fromLatin1(kDefaultModelBaseUrl)
                                 : QString::fromUtf8(env);
        }
    }
    if (base.isEmpty()) return {};
    if (!base.endsWith('/')) base += '/';

    auto* dl = ModelDownloader::instance();
    if (!dl) return {};

    // Download one file, blocking via a local event loop — the exact
    // UniRigPredictor::ensureModelBlocking pattern, with a hard timeout so a
    // stalled connection can't hang the synchronous call. The DiT step graph
    // is the big one (~3 GB fp32), so the cap is generous.
    auto downloadOne = [&](const char* fileName, const QString& dest) -> bool {
        QDir().mkpath(QFileInfo(dest).absolutePath());
        const QString label =
            QStringLiteral("TripoSG %1").arg(QString::fromLatin1(fileName));
        const QString url = base + QString::fromLatin1(fileName);
        QEventLoop loop;
        bool ok = false, timedOut = false;
        auto onDone = QObject::connect(dl, &ModelDownloader::downloadCompleted, &loop,
            [&](const QString& name, const QString&) {
                if (name == label) { ok = true; loop.quit(); }
            });
        auto onErr = QObject::connect(dl, &ModelDownloader::downloadError, &loop,
            [&](const QString& name, const QString&) {
                if (name == label) { ok = false; loop.quit(); }
            });
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop,
            [&]() { timedOut = true; loop.quit(); });
        timeout.start(3600000);   // 60 min for the ~3 GB DiT on slow links
        dl->startDownload(url, dest, label);
        loop.exec();
        QObject::disconnect(onDone);
        QObject::disconnect(onErr);
        if (timedOut) dl->cancelDownload();
        return ok && !timedOut && QFileInfo::exists(dest);
    };

    struct FileSpec { const char* file; QString path; };
    std::vector<FileSpec> files{
        { kImageEncoderFile, imageEncoderPath() },
        { kVaeLatentsFile,   vaeLatentsPath() },
        { kVaeDecoderFile,   vaeDecoderPath() },
    };
    if (int8Tier) {
        files.push_back({ kDitStepInt8File, ditStepPath(true) });
    } else {
        files.push_back({ kDitStepFile, ditStepPath(false) });
        // fp32 DiT external-weights sidecar (>2GB graphs split the weights out).
        files.push_back({ kDitStepDataFile,
                          QDir(modelDir()).filePath(
                              QString::fromLatin1(kDitStepDataFile)) });
    }
    for (const auto& f : files) {
        if (!QFileInfo::exists(f.path) && !downloadOne(f.file, f.path))
            return {};
    }
    return modelsPresent(int8Tier) ? imageEncoderPath() : QString();
}

namespace {

MeshGenPredictor::Result fail(const QString& msg)
{
    MeshGenPredictor::Result r;
    r.error = msg;
    return r;
}

// TripoSG's RectifiedFlowScheduler sigma schedule (shift=1 → identity):
// σᵢ = 1 − i/N for i in [0, N), with a trailing σ_N = 0 so the last Euler
// step lands exactly on the data sample.
std::vector<float> sigmaSchedule(int steps)
{
    std::vector<float> sigmas;
    sigmas.reserve(static_cast<size_t>(steps) + 1);
    for (int i = 0; i < steps; ++i)
        sigmas.push_back(1.0f - float(i) / float(steps));
    sigmas.push_back(0.0f);
    return sigmas;
}

struct IoNames {
    std::vector<Ort::AllocatedStringPtr> holders;
    std::vector<const char*> in, out;
};

IoNames ioNames(Ort::Session& s, Ort::AllocatorWithDefaultOptions& alloc)
{
    IoNames n;
    for (size_t i = 0; i < s.GetInputCount(); ++i) {
        n.holders.push_back(s.GetInputNameAllocated(i, alloc));
        n.in.push_back(n.holders.back().get());
    }
    for (size_t i = 0; i < s.GetOutputCount(); ++i) {
        n.holders.push_back(s.GetOutputNameAllocated(i, alloc));
        n.out.push_back(n.holders.back().get());
    }
    return n;
}

} // namespace

MeshGenPredictor::Result TripoSGPredictor::predict(
    const QImage& image, const Options& opts,
    const MeshGenPredictor::ProgressFn& progress)
{
    using Stage = MeshGenPredictor::Stage;

    if (image.isNull())
        return fail(QStringLiteral("TripoSG: input image is empty."));
    if (!modelsPresent(opts.useInt8Dit))
        return fail(QStringLiteral(
            "TripoSG models unavailable — they download on first use; if not "
            "hosted yet, export them with scripts/export-triposg-onnx.py and "
            "point QTMESH_TRIPOSG_MODEL_BASE_URL at them, or drop the files "
            "in the ai_models/triposg/ cache."));

    const int res   = std::max(16, opts.sdfResolution);
    const int steps = std::clamp(opts.flowSteps, 1, 200);

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_triposg");
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef __APPLE__
        try {
            std::unordered_map<std::string, std::string> coreml;
            so.AppendExecutionProvider("CoreML", coreml);
        } catch (const Ort::Exception&) {}
#endif
        auto open = [&](const QString& p) {
            const std::string s = p.toStdString();
            return Ort::Session(env, s.c_str(), so);
        };
        Ort::AllocatorWithDefaultOptions alloc;
        Ort::MemoryInfo mem =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // The four graphs total ~4-6 GB of weights. Sessions are opened ONE AT
        // A TIME and released as soon as their stage completes — holding them
        // all made the 25-step run's working set big enough for macOS to
        // SIGTERM the process under memory pressure. Peak is now the largest
        // single stage (the DiT), not the sum.

        // ---- (1) Image encoder: preprocessed RGB → conditioning tokens -------
        // Conditioning tokens (image_embeds [1,257,1024]). CFG's unconditional
        // embedding is simply ZEROS of the same shape (upstream zeros_like).
        std::vector<float> cond;
        std::vector<int64_t> condShape;
        {
            Ort::Session imgEnc = open(imageEncoderPath());
            // Input size comes from the graph itself (fallback 224 — the
            // BitImageProcessor's crop size; mean/std are baked in).
            int imgSize = 224;
            {
                // Keep the owning TypeInfo alive — GetTensorTypeAndShapeInfo()
                // returns an unowned view into it; chaining off the temporary
                // dangles and GetShape() segfaults inside GetDimensions.
                Ort::TypeInfo ti = imgEnc.GetInputTypeInfo(0);
                auto info = ti.GetTensorTypeAndShapeInfo();
                const auto shape = info.GetShape();
                if (shape.size() == 4 && shape[2] > 0)
                    imgSize = static_cast<int>(shape[2]);
            }
            QImage resized = image.convertToFormat(QImage::Format_RGB888)
                                 .scaled(imgSize, imgSize, Qt::IgnoreAspectRatio,
                                         Qt::SmoothTransformation);
            // Preprocessing baked INTO the exported graph (raw [0,1] RGB in).
            std::vector<float> imgNCHW = PbrMapSynth::toNCHW(resized, 3);
            const int64_t imgShape[4] = {1, 3, imgSize, imgSize};
            Ort::Value imgTensor = Ort::Value::CreateTensor<float>(
                mem, imgNCHW.data(), imgNCHW.size(), imgShape, 4);

            IoNames encIo = ioNames(imgEnc, alloc);
            if (progress && !progress(Stage::Encode, 0, 1))
                return fail(QStringLiteral("cancelled"));
            auto encRes = imgEnc.Run(Ort::RunOptions{nullptr}, encIo.in.data(),
                                     &imgTensor, 1, encIo.out.data(),
                                     encIo.out.size());
            if (progress && !progress(Stage::Encode, 1, 1))
                return fail(QStringLiteral("cancelled"));

            auto condInfo = encRes[0].GetTensorTypeAndShapeInfo();
            condShape = condInfo.GetShape();
            const float* condData = encRes[0].GetTensorData<float>();
            cond.assign(condData, condData + condInfo.GetElementCount());
        }   // encoder session (~1.1 GB) released here
        std::vector<float> uncond(cond.size(), 0.0f);
        std::vector<int64_t> uncondShape = condShape;

        // ---- (2) Rectified-flow Euler loop over the DiT step graph -----------
        // Latent shape from the DiT's `latents` input (e.g. [1, 2048, 64]).
        std::vector<int64_t> latShape;
        std::vector<float> latents;
        {
            Ort::Session ditStep = open(ditStepPath(opts.useInt8Dit));
            IoNames ditIo = ioNames(ditStep, alloc);
            {
                // Same TypeInfo-lifetime rule as above.
                Ort::TypeInfo ti = ditStep.GetInputTypeInfo(0);
                auto info = ti.GetTensorTypeAndShapeInfo();
                latShape = info.GetShape();
                for (auto& d : latShape)
                    if (d < 0) d = 1;   // defensive: dynamic dims default to 1
            }
            size_t latCount = 1;
            for (int64_t d : latShape) latCount *= static_cast<size_t>(d);
            if (latCount <= 1)
                return fail(QStringLiteral("TripoSG: could not determine the "
                                           "latent shape from the DiT graph."));

            // Deterministic gaussian init (same image + seed → same mesh).
            latents.resize(latCount);
            {
                std::mt19937 rng(opts.seed);
                std::normal_distribution<float> gauss(0.0f, 1.0f);
                for (float& v : latents) v = gauss(rng);
            }

            const std::vector<float> sigmas = sigmaSchedule(steps);
            const bool wantCfg = opts.guidanceScale > 0.0f;

            std::vector<float> vPred(latCount), vUncond(latCount);
            for (int i = 0; i < steps; ++i) {
                if (progress && !progress(Stage::Denoise, i, steps))
                    return fail(QStringLiteral("cancelled"));

                // One DiT evaluation for a given conditioning buffer. Contract:
                // (latents[1,2048,64], timestep[1] = 1000·σ, image_embeds
                // [1,257,1024]) → velocity[1,2048,64]. CFG runs as two B=1
                // calls (the export's --verify checks this equals the doubled
                // batch).
                auto evalStep = [&](std::vector<float>& condBuf,
                                    std::vector<int64_t>& condShp,
                                    std::vector<float>& out) {
                    float tval = kNumTrainTimesteps
                               * sigmas[static_cast<size_t>(i)];
                    const int64_t tShape[1] = {1};

                    std::vector<Ort::Value> ins;
                    ins.push_back(Ort::Value::CreateTensor<float>(
                        mem, latents.data(), latents.size(), latShape.data(),
                        latShape.size()));
                    ins.push_back(Ort::Value::CreateTensor<float>(
                        mem, &tval, 1, tShape, 1));
                    ins.push_back(Ort::Value::CreateTensor<float>(
                        mem, condBuf.data(), condBuf.size(), condShp.data(),
                        condShp.size()));

                    auto outVals = ditStep.Run(
                        Ort::RunOptions{nullptr}, ditIo.in.data(), ins.data(),
                        std::min<size_t>(ins.size(), ditIo.in.size()),
                        ditIo.out.data(), 1);
                    const float* d = outVals[0].GetTensorData<float>();
                    out.assign(d, d + latCount);
                };

                evalStep(cond, condShape, vPred);
                if (wantCfg) {
                    // Classifier-free guidance: v = v_u + s·(v_c − v_u), with
                    // the unconditional pass conditioned on zero embeddings.
                    evalStep(uncond, uncondShape, vUncond);
                    for (size_t k = 0; k < latCount; ++k)
                        vPred[k] = vUncond[k]
                                 + opts.guidanceScale * (vPred[k] - vUncond[k]);
                }

                // TripoSG RectifiedFlowScheduler update: x ← x + (σᵢ − σᵢ₊₁)·v.
                // NOTE the sign — opposite of diffusers' stock FlowMatchEuler
                // (TripoSG's model predicts ≈ x₀ − ε).
                const float dSigma = sigmas[static_cast<size_t>(i)]
                                   - sigmas[static_cast<size_t>(i) + 1];
                for (size_t k = 0; k < latCount; ++k)
                    latents[k] += dSigma * vPred[k];
            }
        }   // DiT session (the largest graph, 1.3-5.4 GB) released here
        // Conditioning buffers are only consumed by the DiT.
        std::vector<float>().swap(cond);
        std::vector<float>().swap(uncond);
        if (progress && !progress(Stage::Denoise, steps, steps))
            return fail(QStringLiteral("cancelled"));

        // ---- (3) VAE: precompute the latent kv-cache ONCE, then tile the ------
        // res³ grid through the per-point decoder. The kv-cache split saves
        // re-running the VAE's 16-block latent self-attention stack for every
        // chunk (~hundreds of chunks at high resolution).
        std::vector<float> kvCache;
        std::vector<int64_t> kvShape;
        {
            Ort::Session vaeLat = open(vaeLatentsPath());
            IoNames latIo = ioNames(vaeLat, alloc);
            Ort::Value latTensor = Ort::Value::CreateTensor<float>(
                mem, latents.data(), latents.size(), latShape.data(),
                latShape.size());
            auto kvRes = vaeLat.Run(Ort::RunOptions{nullptr}, latIo.in.data(),
                                    &latTensor, 1, latIo.out.data(), 1);
            auto info = kvRes[0].GetTensorTypeAndShapeInfo();
            kvShape = info.GetShape();
            const float* d = kvRes[0].GetTensorData<float>();
            kvCache.assign(d, d + info.GetElementCount());
        }   // vae_latents session (~769 MB) released here
        std::vector<float>().swap(latents);

        // Only the small per-point decoder (~48 MB) stays alive for the long
        // Decode/Refine tail.
        Ort::Session vaeDec = open(vaeDecoderPath());
        IoNames vaeIo = ioNames(vaeDec, alloc);
        const size_t totalPts = static_cast<size_t>(res) * res * res;
        std::vector<float> field(totalPts, 0.0f);
        const float step = (2.0f * kRadius) / float(res - 1);
        const int chunk = (opts.chunkPoints > 0) ? opts.chunkPoints
                                                 : static_cast<int>(totalPts);

        // Chunked field sampler shared by the grid pass and the refine pass.
        // The exported decoder already emits the INSIDE-POSITIVE field (raw
        // SDF negated in-graph), matching our MarchingCubes convention.
        std::vector<float> chunkPts(static_cast<size_t>(chunk) * 3);
        auto sampleField = [&](const float* pts, size_t count, float* out,
                               Stage stage) -> bool {
            for (size_t start = 0; start < count;
                 start += static_cast<size_t>(chunk)) {
                if (progress && !progress(stage, static_cast<int>(start),
                                          static_cast<int>(count)))
                    return false;
                const size_t n =
                    std::min(static_cast<size_t>(chunk), count - start);
                const int64_t ptShape[3] = {1, static_cast<int64_t>(n), 3};
                Ort::Value ptTensor = Ort::Value::CreateTensor<float>(
                    mem, const_cast<float*>(pts) + start * 3, n * 3, ptShape, 3);
                Ort::Value kvTensor = Ort::Value::CreateTensor<float>(
                    mem, kvCache.data(), kvCache.size(), kvShape.data(),
                    kvShape.size());
                // Export contract: inputs (kv_cache, points).
                Ort::Value ins[] = {std::move(kvTensor), std::move(ptTensor)};
                auto outVals = vaeDec.Run(Ort::RunOptions{nullptr},
                                          vaeIo.in.data(), ins, 2,
                                          vaeIo.out.data(), 1);
                const float* d = outVals[0].GetTensorData<float>();
                std::copy(d, d + n, out + start);
            }
            return true;
        };

        {
            // Generate grid coordinates per chunk (never the full res³ buffer).
            for (size_t start = 0; start < totalPts;
                 start += static_cast<size_t>(chunk)) {
                const size_t n =
                    std::min(static_cast<size_t>(chunk), totalPts - start);
                for (size_t k = 0; k < n; ++k) {
                    const size_t lin = start + k;
                    const int x = static_cast<int>(lin % res);
                    const int y = static_cast<int>((lin / res) % res);
                    const int z = static_cast<int>(
                        lin / (static_cast<size_t>(res) * res));
                    chunkPts[k * 3 + 0] = -kRadius + x * step;
                    chunkPts[k * 3 + 1] = -kRadius + y * step;
                    chunkPts[k * 3 + 2] = -kRadius + z * step;
                }
                if (!sampleField(chunkPts.data(), n, field.data() + start,
                                 Stage::Decode))
                    return fail(QStringLiteral("cancelled"));
            }
        }

        // ---- (4) Surface + polish (same pipeline as TripoSR) ------------------
        const std::array<float, 3> gmin = {-kRadius, -kRadius, -kRadius};
        const std::array<float, 3> gmax = { kRadius,  kRadius,  kRadius};
        MarchingCubes::Mesh mc = MarchingCubes::extract(
            field.data(), res, res, res, kIsoLevel, gmin, gmax, field.size());
        if (mc.vertexCount == 0 || mc.triangleCount == 0)
            return fail(QStringLiteral(
                "TripoSG: empty surface — try more flow steps or a cleaner "
                "input image."));

        MeshGenPredictor::Result out;
        out.positions     = std::move(mc.positions);
        out.indices       = std::move(mc.indices);
        out.vertexCount   = mc.vertexCount;
        out.triangleCount = mc.triangleCount;
        out.usedModel     = true;

        if (opts.smoothMesh && opts.smoothIterations > 0)
            MeshRefine::taubinSmooth(out.positions, out.indices,
                                     opts.smoothIterations);
        if (opts.refineSurface && out.vertexCount > 0) {
            const size_t nv  = static_cast<size_t>(out.vertexCount);
            const float  eps = step * 0.5f;
            std::vector<float> probes(nv * 4 * 3);
            for (size_t v = 0; v < nv; ++v) {
                const float* p = out.positions.data() + v * 3;
                float* q = probes.data() + v * 12;
                for (int k = 0; k < 4; ++k) {
                    q[k * 3 + 0] = p[0];
                    q[k * 3 + 1] = p[1];
                    q[k * 3 + 2] = p[2];
                }
                q[3] += eps; q[7] += eps; q[11] += eps;
            }
            std::vector<float> dens(nv * 4);
            if (!sampleField(probes.data(), nv * 4, dens.data(), Stage::Refine))
                return fail(QStringLiteral("cancelled"));
            std::vector<float> f(nv), grad(nv * 3);
            for (size_t v = 0; v < nv; ++v) {
                const float f0 = dens[v * 4 + 0];
                f[v] = f0;
                grad[v * 3 + 0] = (dens[v * 4 + 1] - f0) / eps;
                grad[v * 3 + 1] = (dens[v * 4 + 2] - f0) / eps;
                grad[v * 3 + 2] = (dens[v * 4 + 3] - f0) / eps;
            }
            MeshRefine::isoProjectStep(out.positions, f, grad, step);
        }

        // TripoSG is geometry-only: no colours, no uvs — the caller decides
        // how to dress it (neutral material / future input-image projection).
        out.ok = true;
        return out;
    } catch (const Ort::Exception& e) {
        return fail(QStringLiteral("TripoSG ONNX error: %1")
                        .arg(QString::fromUtf8(e.what())));
    } catch (const std::exception& e) {
        return fail(QStringLiteral("TripoSG error: %1")
                        .arg(QString::fromUtf8(e.what())));
    }
}

#endif // ENABLE_ONNX
