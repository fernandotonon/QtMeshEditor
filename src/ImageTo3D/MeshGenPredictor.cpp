#include "MeshGenPredictor.h"

#include "MarchingCubes.h"
#include "MeshRefine.h"       // Taubin smoothing + iso-surface reprojection
#include "MeshGenBaker.h"     // xatlas unwrap + diffuse texture bake
#include "PbrMapSynth.h"      // toNCHW (image → planar [0,1])
#include "BackgroundRemover.h"
#include "TripoSGPredictor.h" // Backend::TripoSG dispatch

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <array>

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

constexpr const char* kDecoderFile = "triposr_decoder.onnx";
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/triposr/";
constexpr const char* kBaseUrlSettingsKey = "ai/triposrModelBaseUrl";
constexpr const char* kEncoderLabel = "TripoSR encoder model";
constexpr const char* kDecoderLabel = "TripoSR decoder model";

// TripoSR renderer.cfg.radius — the query-point half-extent (measured at export;
// see docs/IMAGE_TO_3D_SPIKE_764.md). scene_codes is [1,3,40,64,64].
constexpr float kRadius        = 0.87f;
constexpr int   kEncoderImageSize = 512;

QString modelDir()
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("ai_models/triposr/"));
}

} // namespace

MeshGenPredictor::Options::Options() = default;

QString MeshGenPredictor::encoderFileName(Quality q)
{
    switch (q) {
        case Quality::Int8: return QStringLiteral("triposr_encoder_int8.onnx");
        case Quality::Fp32:
        default:            return QStringLiteral("triposr_encoder.onnx");
    }
}

QString MeshGenPredictor::encoderModelPath(Quality q)
{
    return QDir(modelDir()).filePath(encoderFileName(q));
}

QString MeshGenPredictor::decoderModelPath()
{
    return QDir(modelDir()).filePath(QString::fromLatin1(kDecoderFile));
}

QString MeshGenPredictor::modelPath() { return encoderModelPath(Quality::Fp32); }

bool MeshGenPredictor::modelsPresent(Quality q)
{
    return QFileInfo::exists(encoderModelPath(q))
        && QFileInfo::exists(decoderModelPath());
}

// buildGridPoints is pure-data (no ONNX) — always compiled so tests + the MC
// layout agree regardless of the build flag.
std::vector<float> MeshGenPredictor::buildGridPoints(int resolution, float radius)
{
    std::vector<float> pts;
    if (resolution < 2) return pts;
    pts.reserve(static_cast<size_t>(resolution) * resolution * resolution * 3);
    // x fastest, then y, then z — matches MarchingCubes' row-major
    // field[z*n*n + y*n + x]. Corners span the full [-radius, radius] box so the
    // extracted vertices land in world space directly.
    const float step = (2.0f * radius) / float(resolution - 1);
    for (int z = 0; z < resolution; ++z) {
        const float wz = -radius + z * step;
        for (int y = 0; y < resolution; ++y) {
            const float wy = -radius + y * step;
            for (int x = 0; x < resolution; ++x) {
                pts.push_back(-radius + x * step);
                pts.push_back(wy);
                pts.push_back(wz);
            }
        }
    }
    return pts;
}

#ifndef ENABLE_ONNX

bool MeshGenPredictor::isAvailable() { return false; }

QString MeshGenPredictor::ensureModelBlocking(Quality) { return {}; }

MeshGenPredictor::Result MeshGenPredictor::predict(const QImage&, const QString&,
                                                   const QString&, const Options&,
                                                   const ProgressFn&)
{
    Result r;
    r.error = QStringLiteral(
        "Image-to-3D needs an ONNX-enabled build — rebuild with -DENABLE_ONNX.");
    return r;
}

#else // ENABLE_ONNX

bool MeshGenPredictor::isAvailable() { return true; }

QString MeshGenPredictor::ensureModelBlocking(Quality q)
{
    const QString enc = encoderModelPath(q);
    const QString dec = decoderModelPath();
    if (QFileInfo::exists(enc) && QFileInfo::exists(dec))
        return enc;

    if (!qEnvironmentVariableIsEmpty("QTMESH_TRIPOSR_NO_DOWNLOAD"))
        return {};

    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_TRIPOSR_MODEL_BASE_URL");
            base = env.isEmpty() ? QString::fromLatin1(kDefaultModelBaseUrl)
                                 : QString::fromUtf8(env);
        }
    }
    if (base.isEmpty()) return {};
    if (!base.endsWith('/')) base += '/';

    auto* dl = ModelDownloader::instance();
    if (!dl) return {};

    auto downloadOne = [&](const QString& fileName, const QString& dest,
                           const QString& label) -> bool {
        QDir().mkpath(QFileInfo(dest).absolutePath());
        const QString url = base + fileName;
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
        timeout.start(1800000);   // 30 min — the encoder is ~1.7 GB
        dl->startDownload(url, dest, label);
        loop.exec();
        QObject::disconnect(onDone);
        QObject::disconnect(onErr);
        if (timedOut) dl->cancelDownload();
        return ok && !timedOut && QFileInfo::exists(dest);
    };

    if (!QFileInfo::exists(enc) &&
        !downloadOne(encoderFileName(q), enc,
                     QString::fromLatin1(kEncoderLabel)))
        return {};
    if (!QFileInfo::exists(dec) &&
        !downloadOne(QString::fromLatin1(kDecoderFile), dec,
                     QString::fromLatin1(kDecoderLabel)))
        return {};

    return (QFileInfo::exists(enc) && QFileInfo::exists(dec)) ? enc : QString();
}

namespace {

Ort::Session openSession(Ort::Env& env, Ort::SessionOptions& so, const QString& path)
{
#ifdef _WIN32
    std::wstring wpath = path.toStdWString();
    return Ort::Session(env, wpath.c_str(), so);
#else
    const std::string p = path.toStdString();
    return Ort::Session(env, p.c_str(), so);
#endif
}

MeshGenPredictor::Result fail(const QString& msg)
{
    MeshGenPredictor::Result r;
    r.error = msg;
    return r;
}

// Colour oracle for the geometry-only TripoSG backend (#764): TripoSR's
// decoder predicts an image-conditioned colour for ANY 3D point — including
// occluded ones, consistently with the input photo — so it bakes the diffuse
// for a TripoSG mesh. The mesh (TripoSG frame, +Y-up) is mapped into
// TripoSR's native reconstruction frame (inverse of MeshGenBuilder's
// -90°X/+90°Y fix-up) and affine-fitted per axis onto TripoSR's own occupied
// bounds (coarse density probe) so corresponding body regions line up
// despite the two models' different normalisations. Best-effort: on any
// failure the result keeps its clay look and gains a warning.
void colorizeWithTripoSR(MeshGenPredictor::Result& out, const QImage& image,
                         const MeshGenPredictor::Options& opts,
                         const MeshGenPredictor::ProgressFn& progress)
{
    using Stage = MeshGenPredictor::Stage;
    auto warn = [&](const QString& w) {
        out.warning = out.warning.isEmpty()
            ? w : out.warning + QStringLiteral("; ") + w;
    };
    // Prefer the requested TripoSR tier, fall back to fp32; never download
    // here (the colour bake is opportunistic — missing models just warn).
    QString encPath = MeshGenPredictor::encoderModelPath(opts.quality);
    if (!QFileInfo::exists(encPath))
        encPath = MeshGenPredictor::encoderModelPath(MeshGenPredictor::Quality::Fp32);
    const QString decPath = MeshGenPredictor::decoderModelPath();
    if (!QFileInfo::exists(encPath) || !QFileInfo::exists(decPath)) {
        warn(QStringLiteral("colour bake skipped — TripoSR models not on disk "
                            "(they provide the colour field for TripoSG geometry)"));
        return;
    }
    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_sg_colorize");
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef __APPLE__
        try {
            std::unordered_map<std::string, std::string> coremlOpts;
            so.AppendExecutionProvider("CoreML", coremlOpts);
        } catch (const Ort::Exception&) {}
#endif
        Ort::Session encoder = openSession(env, so, encPath);
        Ort::Session decoder = openSession(env, so, decPath);
        Ort::AllocatorWithDefaultOptions alloc;
        Ort::MemoryInfo mem =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // TripoSR's own background convention (gray-128 composite) — the
        // TripoSG dispatch composited over WHITE, which TripoSR reads as a
        // reconstructed wall.
        QImage subject = image;
        if (opts.removeBackground) {
            const QString bgModel = BackgroundRemover::ensureModelBlocking();
            subject = BackgroundRemover::removeBackground(image, bgModel, {}).image;
        }
        QImage resized = subject.convertToFormat(QImage::Format_RGB888)
                             .scaled(kEncoderImageSize, kEncoderImageSize,
                                     Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        std::vector<float> imgNCHW = PbrMapSynth::toNCHW(resized, 3);
        const int64_t imgShape[4] = {1, 3, kEncoderImageSize, kEncoderImageSize};
        Ort::Value imgTensor = Ort::Value::CreateTensor<float>(
            mem, imgNCHW.data(), imgNCHW.size(), imgShape, 4);
        auto encInName  = encoder.GetInputNameAllocated(0, alloc);
        auto encOutName = encoder.GetOutputNameAllocated(0, alloc);
        const char* encIn[]  = { encInName.get() };
        const char* encOut[] = { encOutName.get() };
        if (progress && !progress(Stage::Bake, -1, -1))
            return;   // cancelled — caller returns the geometry it has
        auto encRes = encoder.Run(Ort::RunOptions{nullptr}, encIn, &imgTensor,
                                  1, encOut, 1);
        auto scInfo = encRes[0].GetTensorTypeAndShapeInfo();
        std::vector<int64_t> scShape = scInfo.GetShape();
        const float* scData = encRes[0].GetTensorData<float>();
        std::vector<float> sceneCodes(scData, scData + scInfo.GetElementCount());

        auto decScName = decoder.GetInputNameAllocated(0, alloc);
        auto decPtName = decoder.GetInputNameAllocated(1, alloc);
        const size_t decOutCount = decoder.GetOutputCount();
        std::vector<Ort::AllocatedStringPtr> outHolders;
        std::vector<const char*> outNames;
        int densityIdx = -1, colorIdx = -1;
        for (size_t i = 0; i < decOutCount; ++i) {
            outHolders.push_back(decoder.GetOutputNameAllocated(i, alloc));
            outNames.push_back(outHolders.back().get());
            const std::string nm = outHolders.back().get();
            if (nm.find("color") != std::string::npos) colorIdx = int(i);
            else if (nm.find("density") != std::string::npos) densityIdx = int(i);
        }
        if (colorIdx < 0 || densityIdx < 0) {
            warn(QStringLiteral(
                "colour bake skipped — TripoSR decoder exposes no colour output"));
            return;
        }

        const int chunk =
            std::max(1, opts.chunkPoints > 0 ? opts.chunkPoints : 262144);
        auto query = [&](const float* pts, size_t count, float* outDens,
                         float* outRgb) -> bool {
            for (size_t start = 0; start < count; start += size_t(chunk)) {
                if (progress && !progress(Stage::Bake, -1, -1)) return false;
                const size_t n = std::min(size_t(chunk), count - start);
                const int64_t ptShape[3] = {1, int64_t(n), 3};
                Ort::Value ptTensor = Ort::Value::CreateTensor<float>(
                    mem, const_cast<float*>(pts) + start * 3, n * 3, ptShape, 3);
                Ort::Value scTensor = Ort::Value::CreateTensor<float>(
                    mem, sceneCodes.data(), sceneCodes.size(), scShape.data(),
                    scShape.size());
                const char* decIn[] = { decScName.get(), decPtName.get() };
                Ort::Value ins[] = { std::move(scTensor), std::move(ptTensor) };
                auto res = decoder.Run(Ort::RunOptions{nullptr}, decIn, ins, 2,
                                       outNames.data(), outNames.size());
                if (outDens) {
                    const float* d = res[size_t(densityIdx)].GetTensorData<float>();
                    std::copy(d, d + n, outDens + start);
                }
                if (outRgb) {
                    const float* c = res[size_t(colorIdx)].GetTensorData<float>();
                    std::copy(c, c + n * 3, outRgb + start * 3);
                }
            }
            return true;
        };

        // Coarse occupied AABB of TripoSR's own reconstruction of this image.
        constexpr int kProbe = 40;
        std::vector<float> probePts(size_t(kProbe) * kProbe * kProbe * 3);
        {
            const float pstep = (2.0f * kRadius) / float(kProbe - 1);
            size_t idx = 0;
            for (int z = 0; z < kProbe; ++z)
                for (int y = 0; y < kProbe; ++y)
                    for (int x = 0; x < kProbe; ++x) {
                        probePts[idx++] = -kRadius + x * pstep;
                        probePts[idx++] = -kRadius + y * pstep;
                        probePts[idx++] = -kRadius + z * pstep;
                    }
        }
        std::vector<float> probeDens(size_t(kProbe) * kProbe * kProbe);
        if (!query(probePts.data(), probeDens.size(), probeDens.data(), nullptr))
            return;
        float srMin[3] = {1e30f, 1e30f, 1e30f};
        float srMax[3] = {-1e30f, -1e30f, -1e30f};
        bool any = false;
        for (size_t i = 0; i < probeDens.size(); ++i) {
            if (probeDens[i] - opts.threshold <= 0.0f) continue;
            any = true;
            for (int a = 0; a < 3; ++a) {
                srMin[a] = std::min(srMin[a], probePts[i * 3 + a]);
                srMax[a] = std::max(srMax[a], probePts[i * 3 + a]);
            }
        }
        if (!any) {
            warn(QStringLiteral(
                "colour bake skipped — TripoSR found no surface for this image"));
            return;
        }

        // Mesh AABB in TripoSR's native frame. MeshGenBuilder's fix-up is
        // F(n) = (-n_y, n_z, -n_x); TripoSG output is already upright, so
        // native = F⁻¹(u) = (-u_z, -u_x, u_y).
        auto toNative = [](const float* u, float* n) {
            n[0] = -u[2]; n[1] = -u[0]; n[2] = u[1];
        };
        float mMin[3] = {1e30f, 1e30f, 1e30f};
        float mMax[3] = {-1e30f, -1e30f, -1e30f};
        for (int v = 0; v < out.vertexCount; ++v) {
            float n[3];
            toNative(out.positions.data() + size_t(v) * 3, n);
            for (int a = 0; a < 3; ++a) {
                mMin[a] = std::min(mMin[a], n[a]);
                mMax[a] = std::max(mMax[a], n[a]);
            }
        }
        // Per-axis affine fit, native-mesh box → TripoSR box: both models
        // reconstruct the SAME subject, so corresponding extents align.
        float s[3], t[3];
        for (int a = 0; a < 3; ++a) {
            const float me = mMax[a] - mMin[a];
            s[a] = (me > 1e-6f) ? (srMax[a] - srMin[a]) / me : 1.0f;
            t[a] = srMin[a] - s[a] * mMin[a];
        }

        MeshGenBaker::Options bakeOpts;
        bakeOpts.textureSize = opts.textureSize;
        bakeOpts.chunkPoints = chunk;
        if (progress)
            bakeOpts.progress = [&](int done, int total) {
                return progress(Stage::Bake, done, total);
            };
        std::vector<float> mapped;   // scratch: TripoSG frame → TripoSR frame
        const MeshGenBaker::Result baked = MeshGenBaker::bake(
            out.positions, out.indices,
            [&](const float* pts, size_t count, float* rgb) -> bool {
                mapped.resize(count * 3);
                for (size_t i = 0; i < count; ++i) {
                    float n[3];
                    toNative(pts + i * 3, n);
                    mapped[i * 3 + 0] = s[0] * n[0] + t[0];
                    mapped[i * 3 + 1] = s[1] * n[1] + t[1];
                    mapped[i * 3 + 2] = s[2] * n[2] + t[2];
                }
                return query(mapped.data(), count, nullptr, rgb);
            },
            bakeOpts);
        if (baked.ok) {
            out.positions     = baked.positions;
            out.indices       = baked.indices;
            out.uvs           = baked.uvs;
            out.texture       = baked.texture;
            out.vertexCount   = baked.vertexCount;
            out.triangleCount = baked.triangleCount;
        } else if (!baked.cancelled) {
            warn(QStringLiteral("colour bake failed (%1) — clay material kept")
                     .arg(baked.error));
        }
    } catch (const Ort::Exception& e) {
        warn(QStringLiteral("colour bake failed (ONNX: %1) — clay material kept")
                 .arg(QString::fromUtf8(e.what())));
    } catch (const std::exception& e) {
        warn(QStringLiteral("colour bake failed (%1) — clay material kept")
                 .arg(QString::fromUtf8(e.what())));
    }
}

} // namespace

MeshGenPredictor::Result MeshGenPredictor::predict(const QImage& image,
                                                   const QString& encoderModelPath,
                                                   const QString& decoderModelPath,
                                                   const Options& opts,
                                                   const ProgressFn& progress)
{
    if (image.isNull())
        return fail(QStringLiteral("MeshGen: input image is empty."));

    // ---- Backend dispatch: TripoSG (rectified-flow, geometry-only) ----------
    if (opts.backend == Backend::TripoSG) {
        QImage subject = image;
        if (opts.removeBackground) {
            // TripoSG's reference pipeline composites the cut-out over WHITE
            // (unlike TripoSR's gray-128) — see docs/TRIPOSG_EXPORT_NOTES.md.
            const QString bgModel = BackgroundRemover::ensureModelBlocking();
            BackgroundRemover::Options bg;
            bg.bgR = 255; bg.bgG = 255; bg.bgB = 255;
            const BackgroundRemover::Result br =
                BackgroundRemover::removeBackground(image, bgModel, bg);
            subject = br.image;
        }
        TripoSGPredictor::Options sg;
        sg.sdfResolution    = opts.sdfResolution;
        sg.flowSteps        = opts.flowSteps;
        sg.guidanceScale    = opts.guidanceScale;
        // int8 tier DROPPED for TripoSG: the quantized 1.5B DiT degrades to
        // blobs over the flow loop (verified live even with per-channel
        // quant), and dynamic-int8 MatMuls are no faster than fp32 on ARM.
        // The tier picker stays meaningful for TripoSR only.
        sg.useInt8Dit       = false;
        sg.smoothMesh       = opts.smoothMesh;
        sg.smoothIterations = opts.smoothIterations;
        sg.refineSurface    = opts.refineSurface;
        sg.chunkPoints      = opts.chunkPoints;
        Result r = TripoSGPredictor::predict(subject, sg, progress);
        // TripoSG's field is already +Y-up — skip the TripoSR frame bake.
        r.bakeTripoSROrientation = false;
        // TripoSG is geometry-only; bake colour from TripoSR's image-
        // conditioned field on the same input (best-effort — clay on failure).
        if (r.ok && opts.bakeTexture && r.vertexCount > 0)
            colorizeWithTripoSR(r, image, opts, progress);
        return r;
    }
    if (!QFileInfo::exists(encoderModelPath) || !QFileInfo::exists(decoderModelPath))
        return fail(QStringLiteral("MeshGen: TripoSR model not found (not hosted yet? "
                                   "see docs/IMAGE_TO_3D_SPIKE_764.md)."));

    const int res = std::max(16, opts.sdfResolution);

    // Optional: isolate the subject first (TripoSR needs a clean background).
    // Falls back to the original image if the model/ONNX is unavailable.
    QImage subject = image;
    if (opts.removeBackground) {
        const QString bgModel = BackgroundRemover::ensureModelBlocking();
        const BackgroundRemover::Result br =
            BackgroundRemover::removeBackground(image, bgModel, {});
        subject = br.image;   // cleaned on success, original on fallback
    }

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_triposr");
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef __APPLE__
        try {
            std::unordered_map<std::string, std::string> coremlOpts;
            so.AppendExecutionProvider("CoreML", coremlOpts);
        } catch (const Ort::Exception&) {}
#endif
        Ort::Session encoder = openSession(env, so, encoderModelPath);
        Ort::Session decoder = openSession(env, so, decoderModelPath);
        Ort::AllocatorWithDefaultOptions alloc;
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // ---- (1) Encoder: image [1,3,512,512] (RGB, [0,1]) -> scene_codes -----
        QImage resized = subject.convertToFormat(QImage::Format_RGB888)
                              .scaled(kEncoderImageSize, kEncoderImageSize,
                                      Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        std::vector<float> imgNCHW = PbrMapSynth::toNCHW(resized, 3);   // /255, planar
        const int64_t imgShape[4] = {1, 3, kEncoderImageSize, kEncoderImageSize};
        Ort::Value imgTensor = Ort::Value::CreateTensor<float>(
            mem, imgNCHW.data(), imgNCHW.size(), imgShape, 4);

        auto encInName  = encoder.GetInputNameAllocated(0, alloc);
        auto encOutName = encoder.GetOutputNameAllocated(0, alloc);
        const char* encIn[]  = { encInName.get() };
        const char* encOut[] = { encOutName.get() };
        // The encoder is one blocking Run (not cancellable mid-flight) — report
        // the stage boundary so the GUI's step list can mark it active/done.
        if (progress && !progress(Stage::Encode, 0, 1))
            return fail(QStringLiteral("cancelled"));
        auto encRes = encoder.Run(Ort::RunOptions{nullptr}, encIn, &imgTensor, 1, encOut, 1);
        if (progress && !progress(Stage::Encode, 1, 1))
            return fail(QStringLiteral("cancelled"));

        // scene_codes tensor: keep its data + shape to re-feed the decoder.
        auto scInfo = encRes[0].GetTensorTypeAndShapeInfo();
        std::vector<int64_t> scShape = scInfo.GetShape();
        const float* scData = encRes[0].GetTensorData<float>();
        const size_t scCount = static_cast<size_t>(scInfo.GetElementCount());
        std::vector<float> sceneCodes(scData, scData + scCount);

        // Decoder input/output names (2 in: scene_codes, points; >=1 out: density[,color]).
        auto decScName  = decoder.GetInputNameAllocated(0, alloc);
        auto decPtName   = decoder.GetInputNameAllocated(1, alloc);
        const size_t decOutCount = decoder.GetOutputCount();
        std::vector<Ort::AllocatedStringPtr> decOutHolders;
        std::vector<const char*> decOutNames;
        int densityIdx = -1, colorIdx = -1;
        for (size_t i = 0; i < decOutCount; ++i) {
            decOutHolders.push_back(decoder.GetOutputNameAllocated(i, alloc));
            decOutNames.push_back(decOutHolders.back().get());
            const std::string nm = decOutHolders.back().get();
            if (nm.find("color") != std::string::npos) colorIdx = static_cast<int>(i);
            else if (nm.find("density") != std::string::npos) densityIdx = static_cast<int>(i);
        }
        // Require the named 'density' output — defaulting to index 0 would silently
        // read the wrong tensor (garbage mesh) if the graph ever reorders outputs.
        if (densityIdx < 0)
            return fail(QStringLiteral("MeshGen: decoder has no 'density' output "
                                       "(unexpected model contract)."));
        const bool wantColor = opts.vertexColor && colorIdx >= 0;

        // ---- (2) Tile the grid through the decoder -> density grid ------------
        // The density FIELD itself is unavoidably res^3 floats (it's the MC input):
        // ~64 MiB at 256, ~512 MiB at 512, ~4.3 GiB at 1024 — the caller is warned
        // above 512 (see CLIPipeline/MCPServer). Chunking does NOT cap that; it caps
        // the *query-point* buffer, which is generated per chunk into a small
        // reusable vector rather than materialising the whole res^3 * 3 point grid
        // up front (another ~192 MiB at 256 / ~1.5 GiB at 512 on top of the field).
        // Order matches MarchingCubes' row-major field[z*n*n + y*n + x] (x fastest).
        const size_t totalPts = static_cast<size_t>(res) * res * res;
        std::vector<float> densityField(totalPts, 0.0f);
        const float step = (2.0f * kRadius) / float(res - 1);

        const int chunk = (opts.chunkPoints > 0) ? opts.chunkPoints : static_cast<int>(totalPts);
        std::vector<float> chunkPts(static_cast<size_t>(chunk) * 3);
        for (size_t start = 0; start < totalPts; start += static_cast<size_t>(chunk)) {
            const size_t n = std::min(static_cast<size_t>(chunk), totalPts - start);
            for (size_t i = 0; i < n; ++i) {
                const size_t lin = start + i;
                const int x = static_cast<int>(lin % res);
                const int y = static_cast<int>((lin / res) % res);
                const int z = static_cast<int>(lin / (static_cast<size_t>(res) * res));
                chunkPts[i * 3 + 0] = -kRadius + x * step;
                chunkPts[i * 3 + 1] = -kRadius + y * step;
                chunkPts[i * 3 + 2] = -kRadius + z * step;
            }
            const int64_t ptShape[3] = {1, static_cast<int64_t>(n), 3};
            Ort::Value ptTensor = Ort::Value::CreateTensor<float>(
                mem, chunkPts.data(), n * 3, ptShape, 3);
            Ort::Value scTensor = Ort::Value::CreateTensor<float>(
                mem, sceneCodes.data(), sceneCodes.size(), scShape.data(), scShape.size());
            const char* decIn[] = { decScName.get(), decPtName.get() };
            Ort::Value decInVals[] = { std::move(scTensor), std::move(ptTensor) };
            auto decRes = decoder.Run(Ort::RunOptions{nullptr}, decIn, decInVals, 2,
                                      decOutNames.data(), decOutNames.size());
            const float* dens = decRes[densityIdx].GetTensorData<float>();
            for (size_t i = 0; i < n; ++i)
                densityField[start + i] = dens[i];   // density[1,n,1] contiguous

            if (progress && !progress(Stage::Decode, static_cast<int>(start + n),
                                      static_cast<int>(totalPts)))
                return fail(QStringLiteral("cancelled"));
        }

        // ---- (3) Marching cubes on (density - threshold) at iso 0 ------------
        // Our MarchingCubes is inside-POSITIVE (>= iso), so the surface is where
        // (density - threshold) crosses 0. (TripoSR's own MC is inside-negative
        // and runs on -(density - threshold); the sign flips but it's the same
        // surface — see docs/IMAGE_TO_3D_SPIKE_764.md.)
        for (float& v : densityField) v -= opts.threshold;
        const std::array<float, 3> gmin = {-kRadius, -kRadius, -kRadius};
        const std::array<float, 3> gmax = { kRadius,  kRadius,  kRadius};
        MarchingCubes::Mesh mc =
            MarchingCubes::extract(densityField.data(), res, res, res, 0.0f, gmin, gmax,
                                   densityField.size());

        if (mc.vertexCount == 0 || mc.triangleCount == 0)
            return fail(QStringLiteral("MeshGen: empty surface (nothing above the "
                                       "density threshold — try a cleaner input image)."));

        Result out;
        out.positions     = std::move(mc.positions);
        out.indices       = std::move(mc.indices);
        out.vertexCount   = mc.vertexCount;
        out.triangleCount = mc.triangleCount;
        out.usedModel     = true;

        // Chunked decoder query over a materialized point buffer. Fills
        // outDensity (raw density, one float/point) and/or outRgb (three
        // floats/point) when non-null. The pass-specific loops below (refine,
        // vertex colour, texture bake) all share this. `stage` labels the
        // per-chunk progress reports; pass report=false when someone ELSE owns
        // the stage's progress accounting (the texture baker reports its own
        // texel totals) — cancellation is then still checked via total = -1.
        auto sampleBuffer = [&](const float* pts, size_t count,
                                float* outDensity, float* outRgb,
                                Stage stage, bool report = true) -> bool {
            for (size_t start = 0; start < count; start += static_cast<size_t>(chunk)) {
                if (progress) {
                    const bool keep = report
                        ? progress(stage, static_cast<int>(start),
                                   static_cast<int>(count))
                        : progress(stage, -1, -1);   // cancel check only
                    if (!keep) return false;
                }
                const size_t n = std::min(static_cast<size_t>(chunk), count - start);
                const int64_t ptShape[3] = {1, static_cast<int64_t>(n), 3};
                // The decoder input is non-const in the C API; the buffer is
                // only read, so the const_cast is safe.
                Ort::Value ptTensor = Ort::Value::CreateTensor<float>(
                    mem, const_cast<float*>(pts) + start * 3, n * 3, ptShape, 3);
                Ort::Value scTensor = Ort::Value::CreateTensor<float>(
                    mem, sceneCodes.data(), sceneCodes.size(), scShape.data(), scShape.size());
                const char* decIn[] = { decScName.get(), decPtName.get() };
                Ort::Value decInVals[] = { std::move(scTensor), std::move(ptTensor) };
                auto decRes = decoder.Run(Ort::RunOptions{nullptr}, decIn, decInVals, 2,
                                          decOutNames.data(), decOutNames.size());
                if (outDensity) {
                    const float* dens = decRes[densityIdx].GetTensorData<float>();
                    std::copy(dens, dens + n, outDensity + start);
                }
                if (outRgb && colorIdx >= 0) {
                    const float* col = decRes[colorIdx].GetTensorData<float>();
                    std::copy(col, col + n * 3, outRgb + start * 3);
                }
            }
            return true;
        };

        // ---- (4) Polish: Taubin smoothing + iso-surface reprojection ----------
        // Smoothing removes the res^3-grid stair-stepping; the projection step
        // then snaps the smoothed vertices back onto the network's true
        // iso-surface (field + forward-difference gradient sampled from the
        // decoder), recovering detail the grid quantized away.
        if (opts.smoothMesh && opts.smoothIterations > 0)
            MeshRefine::taubinSmooth(out.positions, out.indices, opts.smoothIterations);

        if (opts.refineSurface && out.vertexCount > 0) {
            const size_t nv  = static_cast<size_t>(out.vertexCount);
            const float  eps = step * 0.5f;   // probe offset: half a grid cell
            // Probe layout per vertex: [v, v+εx̂, v+εŷ, v+εẑ] — forward differences.
            std::vector<float> probes(nv * 4 * 3);
            for (size_t v = 0; v < nv; ++v) {
                const float* p = out.positions.data() + v * 3;
                float* q = probes.data() + v * 12;
                for (int k = 0; k < 4; ++k) {
                    q[k * 3 + 0] = p[0]; q[k * 3 + 1] = p[1]; q[k * 3 + 2] = p[2];
                }
                q[3] += eps; q[7] += eps; q[11] += eps;
            }
            std::vector<float> dens(nv * 4);
            if (!sampleBuffer(probes.data(), nv * 4, dens.data(), nullptr,
                              Stage::Refine))
                return fail(QStringLiteral("cancelled"));
            std::vector<float> f(nv), grad(nv * 3);
            for (size_t v = 0; v < nv; ++v) {
                const float f0 = dens[v * 4 + 0] - opts.threshold;
                f[v] = f0;
                grad[v * 3 + 0] = (dens[v * 4 + 1] - opts.threshold - f0) / eps;
                grad[v * 3 + 1] = (dens[v * 4 + 2] - opts.threshold - f0) / eps;
                grad[v * 3 + 2] = (dens[v * 4 + 3] - opts.threshold - f0) / eps;
            }
            // Clamp each move to one grid cell — the vertex is already close.
            MeshRefine::isoProjectStep(out.positions, f, grad, step);
        }

        // ---- (5) Colour: baked texture (preferred) or per-vertex ---------------
        if (wantColor && out.vertexCount > 0 && opts.bakeTexture) {
            MeshGenBaker::Options bakeOpts;
            bakeOpts.textureSize = opts.textureSize;
            bakeOpts.chunkPoints = chunk;
            // The baker owns the Bake stage's progress (it knows the true
            // texel total); the sampler only cancellation-checks.
            if (progress)
                bakeOpts.progress = [&](int done, int total) {
                    return progress(Stage::Bake, done, total);
                };
            const MeshGenBaker::Result baked = MeshGenBaker::bake(
                out.positions, out.indices,
                [&](const float* pts, size_t count, float* rgb) {
                    return sampleBuffer(pts, count, nullptr, rgb,
                                        Stage::Bake, /*report=*/false);
                },
                bakeOpts);
            if (baked.ok) {
                out.positions     = baked.positions;   // re-indexed along UV seams
                out.indices       = baked.indices;
                out.uvs           = baked.uvs;
                out.texture       = baked.texture;
                out.vertexCount   = baked.vertexCount;
                out.triangleCount = baked.triangleCount;
            } else if (baked.cancelled) {
                return fail(QStringLiteral("cancelled"));
            } else {
                out.warning = QStringLiteral("texture bake failed (%1) — using "
                                             "vertex colours").arg(baked.error);
            }
        }

        // ---- (6) Per-vertex colour (bake disabled or fell back) ---------------
        if (wantColor && out.vertexCount > 0 && out.uvs.empty()) {
            const size_t nv = static_cast<size_t>(out.vertexCount);
            out.colors.assign(nv * 3, 0.8f);
            if (!sampleBuffer(out.positions.data(), nv, nullptr, out.colors.data(),
                              Stage::Color))
                return fail(QStringLiteral("cancelled"));
        }

        out.ok = true;
        return out;
    } catch (const Ort::Exception& e) {
        return fail(QStringLiteral("MeshGen ONNX error: %1")
                        .arg(QString::fromUtf8(e.what())));
    } catch (const std::exception& e) {
        return fail(QStringLiteral("MeshGen error: %1")
                        .arg(QString::fromUtf8(e.what())));
    }
}

#endif // ENABLE_ONNX
