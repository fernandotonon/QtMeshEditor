#include "MeshGenPredictor.h"

#include "MarchingCubes.h"
#include "MeshRefine.h"       // Taubin smoothing + iso-surface reprojection
#include "MeshGenBaker.h"     // xatlas unwrap + diffuse texture bake
#include "PbrMapSynth.h"      // toNCHW (image → planar [0,1])
#include "BackgroundRemover.h"
#include "OnnxRuntimeSettings.h"
#include "TripoSGPredictor.h" // Backend::TripoSG dispatch
#include "Trellis2Predictor.h" // Backend::Trellis2 dispatch (no ONNX needed)
#include "Trellis2Bake.h"      // game-ready simplify + detail-normal bake (all backends)

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

MeshGenPredictor::Backend MeshGenPredictor::defaultBackend()
{
    // TRELLIS.2 becomes the default the moment its runtime is installed on
    // this machine; otherwise the local ONNX TripoSR path stays the default.
    return Trellis2Predictor::runtimeAvailable() ? Backend::Trellis2
                                                 : Backend::TripoSR;
}

namespace {
// Backend::Trellis2 dispatch, shared by the ONNX and non-ONNX builds — the
// sidecar backend has no ONNX dependency (only the optional U²-Net matte
// does, and Trellis2Predictor degrades that gracefully).
MeshGenPredictor::Result predictTrellis2(
    const QImage& image,
    const MeshGenPredictor::Options& opts,
    const MeshGenPredictor::ProgressFn& progress)
{
    Trellis2Predictor::Options t2;
    t2.preset           = opts.trellis2Preset;
    t2.seed             = opts.seed;
    t2.targetTriangles  = opts.targetTriangles;
    t2.bakeTexture      = opts.bakeTexture;
    t2.textureSize      = opts.textureSize;
    t2.bakeNormalMap    = opts.bakeNormalMap;
    t2.removeBackground = opts.removeBackground;
    t2.mock             = opts.trellis2Mock;
    t2.sourceKeepDir    = opts.trellis2SourceKeepDir;
    t2.sourceKeepBaseName = opts.trellis2SourceKeepBaseName;
    MeshGenPredictor::Result r = Trellis2Predictor::predict(image, t2, progress);
    // TRELLIS.2 decodes in a Z-up frame — in the viewer's Y-up world the
    // model arrives face-down. Bake a -90° X rotation into the geometry:
    // (x, y, z) -> (x, z, -y). Rigid (det +1), so winding, UVs and the
    // tangent-space normal map are untouched; object-space vertex normals
    // rotate with the positions. The kept .qtm3d source stays in the native
    // frame (re-bakes come back through this same path).
    if (r.ok) {
        for (size_t v = 0; v + 2 < r.positions.size(); v += 3) {
            const float y = r.positions[v + 1];
            r.positions[v + 1] = r.positions[v + 2];
            r.positions[v + 2] = -y;
        }
        for (size_t v = 0; v + 2 < r.normals.size(); v += 3) {
            const float y = r.normals[v + 1];
            r.normals[v + 1] = r.normals[v + 2];
            r.normals[v + 2] = -y;
        }
    }
    return r;
}

// Game-ready pass for the LOCAL backends (TripoSR/TripoSG): weld, drop
// floating debris, simplify toward Options::targetTriangles. Marching-cubes
// output decimated blind turns into a blob and skins terribly — the fix is
// the standard high→low workflow: simplify hard here, then bake the lost
// detail back as textures (diffuse via the field bake, relief via
// bakeDetailNormal against the dense pre-simplify source kept in srcPos/Idx).
// Returns false only on a hard failure (result untouched, warning appended).
bool applyGameReady(MeshGenPredictor::Result& out,
                    int targetTriangles,
                    std::vector<float>* srcPosOut,
                    std::vector<uint32_t>* srcIdxOut)
{
    if (targetTriangles <= 0 || out.vertexCount <= 0)
        return false;
    Trellis2Bake::GameReadyOptions gr;
    gr.targetTriangles = targetTriangles;
    const Trellis2Bake::GameReadyResult processed =
        Trellis2Bake::makeGameReady(out.positions, out.indices, gr);
    if (!processed.ok) {
        if (!out.warning.isEmpty())
            out.warning += QStringLiteral(" ");
        out.warning += QStringLiteral("game-ready pass failed (%1) — keeping "
                                      "the full-density mesh.")
                           .arg(processed.error);
        return false;
    }
    if (srcPosOut) *srcPosOut = std::move(out.positions);
    if (srcIdxOut) *srcIdxOut = std::move(out.indices);
    out.positions = processed.positions;
    out.indices = processed.indices;
    out.vertexCount = static_cast<int>(out.positions.size() / 3);
    out.triangleCount = static_cast<int>(out.indices.size() / 3);
    // Per-vertex colours (if any) belonged to the old vertices.
    out.colors.clear();
    out.uvs.clear();
    return true;
}
} // namespace

#ifndef ENABLE_ONNX

bool MeshGenPredictor::isAvailable() { return false; }

QString MeshGenPredictor::ensureModelBlocking(Quality) { return {}; }

MeshGenPredictor::Result MeshGenPredictor::predict(const QImage& image,
                                                   const QString&,
                                                   const QString&,
                                                   const Options& opts,
                                                   const ProgressFn& progress)
{
    if (opts.backend == Backend::Trellis2)
        return predictTrellis2(image, opts, progress);
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

} // namespace

MeshGenPredictor::Result MeshGenPredictor::predict(const QImage& image,
                                                   const QString& encoderModelPath,
                                                   const QString& decoderModelPath,
                                                   const Options& opts,
                                                   const ProgressFn& progress)
{
    if (image.isNull())
        return fail(QStringLiteral("MeshGen: input image is empty."));

    // ---- Backend dispatch: TRELLIS.2 (out-of-process sidecar) ----------------
    if (opts.backend == Backend::Trellis2)
        return predictTrellis2(image, opts, progress);

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
        // Game-ready simplification (geometry-only backend — nothing to bake;
        // the later GUI AI-texture pass unwraps/bakes the SIMPLIFIED mesh,
        // which is exactly what you want for skinning-friendly assets).
        if (r.ok)
            applyGameReady(r, opts.targetTriangles, nullptr, nullptr);
        // TripoSG is geometry-only. Colour comes SOLELY from the AI image
        // generation pass (multi-view depth-ControlNet, run later in the GUI
        // layer) — no TripoSR field colouring. With no AI texture the mesh
        // ships uncoloured (neutral clay material in MeshGenBuilder).
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
        OnnxRuntimeSettings::configureSessionOptions(so);
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

        // ---- (4b) Game-ready simplification (Options::targetTriangles) --------
        // The dense pre-simplify mesh is kept as the bake SOURCE: the diffuse
        // below re-bakes on the simplified mesh straight from the decoder
        // field (density-independent), and (5b) bakes the lost geometric
        // detail into a tangent-space normal map — the standard high→low
        // workflow that keeps a 10–50k mesh from reading as a blob.
        std::vector<float>    gameReadySrcPos;
        std::vector<uint32_t> gameReadySrcIdx;
        const bool gameReady =
            applyGameReady(out, opts.targetTriangles,
                           &gameReadySrcPos, &gameReadySrcIdx);

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

        // ---- (5b) Detail normal map (game-ready path only) ---------------------
        // Bake the dense source's smooth normals into the SAME atlas the
        // diffuse bake produced, expressed in the simplified target's tangent
        // frame. This is what preserves the perceived detail after a hard
        // simplification. Skipped when there's no baked diffuse to share an
        // unwrap with (vertex-colour mode carries no UVs).
        if (gameReady && opts.bakeNormalMap && !out.uvs.empty()
            && !out.texture.isNull()) {
            Trellis2Bake::BakeOptions nbo;
            if (progress)
                nbo.progress = [&](int done, int total) {
                    return progress(Stage::Bake, done, total);
                };
            const Trellis2Bake::NormalBakeResult nb =
                Trellis2Bake::bakeDetailNormal(
                    out.positions, out.indices, out.uvs,
                    out.texture.width(), out.texture.height(),
                    gameReadySrcPos, gameReadySrcIdx, nbo);
            if (nb.cancelled)
                return fail(QStringLiteral("cancelled"));
            if (nb.ok) {
                out.normalMap = nb.normalMap;
            } else {
                if (!out.warning.isEmpty())
                    out.warning += QStringLiteral(" ");
                out.warning += QStringLiteral(
                    "detail-normal bake failed (%1).").arg(nb.error);
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
