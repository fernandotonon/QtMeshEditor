#include "MeshGenPredictor.h"

#include "MarchingCubes.h"
#include "PbrMapSynth.h"   // toNCHW (image → planar [0,1])
#include "BackgroundRemover.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

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

constexpr const char* kEncoderFile = "triposr_encoder.onnx";
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

QString MeshGenPredictor::encoderModelPath()
{
    return QDir(modelDir()).filePath(QString::fromLatin1(kEncoderFile));
}

QString MeshGenPredictor::decoderModelPath()
{
    return QDir(modelDir()).filePath(QString::fromLatin1(kDecoderFile));
}

QString MeshGenPredictor::modelPath() { return encoderModelPath(); }

bool MeshGenPredictor::modelsPresent()
{
    return QFileInfo::exists(encoderModelPath())
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

QString MeshGenPredictor::ensureModelBlocking() { return {}; }

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

QString MeshGenPredictor::ensureModelBlocking()
{
    const QString enc = encoderModelPath();
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
        !downloadOne(QString::fromLatin1(kEncoderFile), enc,
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
        auto encRes = encoder.Run(Ort::RunOptions{nullptr}, encIn, &imgTensor, 1, encOut, 1);

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
        // Points are GENERATED PER CHUNK into a small reusable buffer rather than
        // materialising the whole res^3 * 3 grid up front (that's ~192 MiB at 256,
        // ~1.5 GiB at 512 — enough to OOM before the ONNX buffers). Order matches
        // MarchingCubes' row-major field[z*n*n + y*n + x] (x fastest).
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

            if (progress && !progress(static_cast<int>(start + n),
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
            MarchingCubes::extract(densityField.data(), res, res, res, 0.0f, gmin, gmax);

        if (mc.vertexCount == 0 || mc.triangleCount == 0)
            return fail(QStringLiteral("MeshGen: empty surface (nothing above the "
                                       "density threshold — try a cleaner input image)."));

        Result out;
        out.positions     = std::move(mc.positions);
        out.indices       = std::move(mc.indices);
        out.vertexCount   = mc.vertexCount;
        out.triangleCount = mc.triangleCount;
        out.usedModel     = true;

        // ---- (4) Optional per-vertex color: one more decoder pass on verts ----
        if (wantColor && out.vertexCount > 0) {
            const size_t nv = static_cast<size_t>(out.vertexCount);
            out.colors.assign(nv * 3, 0.8f);
            for (size_t start = 0; start < nv; start += static_cast<size_t>(chunk)) {
                const size_t n = std::min(static_cast<size_t>(chunk), nv - start);
                const int64_t ptShape[3] = {1, static_cast<int64_t>(n), 3};
                Ort::Value ptTensor = Ort::Value::CreateTensor<float>(
                    mem, out.positions.data() + start * 3, n * 3, ptShape, 3);
                Ort::Value scTensor = Ort::Value::CreateTensor<float>(
                    mem, sceneCodes.data(), sceneCodes.size(), scShape.data(), scShape.size());
                const char* decIn[] = { decScName.get(), decPtName.get() };
                Ort::Value decInVals[] = { std::move(scTensor), std::move(ptTensor) };
                auto decRes = decoder.Run(Ort::RunOptions{nullptr}, decIn, decInVals, 2,
                                          decOutNames.data(), decOutNames.size());
                const float* col = decRes[colorIdx].GetTensorData<float>();
                for (size_t i = 0; i < n * 3; ++i)
                    out.colors[start * 3 + i] = col[i];
            }
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
