#include "SamMaskProvider.h"

#include "AppStorage.h"
#include "ModelFetch.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSettings>

#include <algorithm>
#include <cmath>

#ifdef ENABLE_ONNX
#include "OnnxRuntimeSettings.h"
#include <onnxruntime_cxx_api.h>
#endif

namespace GenericSeg {

QString SamMaskProvider::encoderFileName() { return QStringLiteral("sam2.1_hiera_small_encoder.onnx"); }
QString SamMaskProvider::decoderFileName() { return QStringLiteral("sam2.1_hiera_small_decoder.onnx"); }

namespace {

// Pinned to a COMMIT, not `main`: the digests below describe exactly these
// files. After re-exporting (scripts/export-sam2-onnx.py) and uploading
// (scripts/upload-sam2-models.sh), update the revision AND the digests together.
constexpr const char* kDefaultBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/9650df88f6341bf66d489512dc88bdcbb42c29ac/segment/";
constexpr const char* kBaseUrlSettingsKey = "ai/segmentModelBaseUrl";   // shared with MeshSegmenter

struct FileSpec { const char* name; const char* label; const char* sha256; };
constexpr FileSpec kFiles[] = {
    {"LICENSE-SAM2.txt", "SAM 2.1 licence (Apache-2.0)",
     "c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4"},
    {"sam2.1_hiera_small_decoder.onnx", "SAM 2.1 mask decoder",
     "ad1e4f9c547b27eeec396d2bd8e3ca94a60eb6692666b20a088ebe74c7a62295"},
    {"sam2.1_hiera_small_encoder.onnx", "SAM 2.1 image encoder",
     "78145ad60deed0a7587fff296d8e29bbec458e2a0b5dc65a9a7b31e3f5af3c9b"},
};

} // namespace

QString SamMaskProvider::modelDir()
{
    return QDir(AppStorage::aiModelsRoot()).filePath(QStringLiteral("segment/"));
}

bool SamMaskProvider::modelsPresent()
{
    for (const FileSpec& f : kFiles)
        if (!QFileInfo::exists(QDir(modelDir()).filePath(QString::fromLatin1(f.name)))) return false;
    return true;
}

bool SamMaskProvider::ensureModelsBlocking(QString* error, const std::function<bool()>& cancelled)
{
#ifndef ENABLE_ONNX
    Q_UNUSED(cancelled);
    if (error) *error = QStringLiteral("Generic segmentation needs ONNX Runtime (rebuild with -DENABLE_ONNX)");
    return false;
#else
    QString base = QSettings().value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
    if (base.isEmpty()) {
        const QByteArray env = qgetenv("QTMESH_SEGMENT_MODEL_BASE_URL");
        base = env.isEmpty() ? QString::fromLatin1(kDefaultBaseUrl) : QString::fromUtf8(env);
    }
    // A mirror may serve a different export: only the default hosting is digest-checked.
    const bool defaultHosting = base == QString::fromLatin1(kDefaultBaseUrl);
    if (!base.endsWith('/')) base += '/';
    const bool noDownload = !qEnvironmentVariableIsEmpty("QTMESH_SEGMENT_NO_DOWNLOAD");
    QDir().mkpath(modelDir());
    for (const FileSpec& f : kFiles) {
        const QString dest = QDir(modelDir()).filePath(QString::fromLatin1(f.name));
        if (QFileInfo::exists(dest) && !defaultHosting) continue;
        if (noDownload && !QFileInfo::exists(dest)) {
            if (error)
                *error = QStringLiteral("%1 is missing and downloads are disabled by QTMESH_SEGMENT_NO_DOWNLOAD")
                             .arg(QString::fromLatin1(f.name));
            return false;
        }
        ModelFetch::Request req;
        // Empty URL = verify only (downloads disabled, file present).
        req.url = noDownload ? QString() : base + QString::fromLatin1(f.name);
        req.destination = dest;
        req.label = QString::fromLatin1(f.label);
        req.timeoutMs = 1800000;
        req.cancelled = cancelled;
        if (defaultHosting) req.expectedSha256 = QString::fromLatin1(f.sha256);
        const ModelFetch::Outcome fo = ModelFetch::ensureBlocking(req);
        if (!fo.ok) {
            if (error) *error = fo.error.isEmpty() ? QStringLiteral("could not fetch %1").arg(QString::fromLatin1(f.name)) : fo.error;
            return false;
        }
    }
    return true;
#endif
}

#ifndef ENABLE_ONNX

struct SamMaskProvider::Impl {};
SamMaskProvider::SamMaskProvider(const QString&, const QString&, const Options&) {}
SamMaskProvider::SamMaskProvider(const QString& e, const QString& dd) : SamMaskProvider(e, dd, Options{}) {}
SamMaskProvider::~SamMaskProvider() = default;
bool SamMaskProvider::ok() const { return false; }
QString SamMaskProvider::error() const
{
    return QStringLiteral("Generic segmentation needs ONNX Runtime (rebuild with -DENABLE_ONNX)");
}
std::vector<Mask> SamMaskProvider::masksFor(const ViewImage&) { return {}; }
SamMaskProvider::Timing SamMaskProvider::timing() const { return {}; }

#else

namespace {

constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kStd[3] = {0.229f, 0.224f, 0.225f};

std::unique_ptr<Ort::Session> openSession(Ort::Env& env, const QString& path, bool gpu)
{
    Ort::SessionOptions so;
    OnnxRuntimeSettings::SessionConfig cfg;
    cfg.appendGpu = gpu;
    OnnxRuntimeSettings::configureSessionOptions(so, cfg);
#ifdef _WIN32
    return std::make_unique<Ort::Session>(env, path.toStdWString().c_str(), so);
#else
    return std::make_unique<Ort::Session>(env, path.toStdString().c_str(), so);
#endif
}

} // namespace

struct SamMaskProvider::Impl {
    Options opts;
    QString error;
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "qtmesh_sam2"};
    std::unique_ptr<Ort::Session> encoder, decoder;
    Timing timing;
};

SamMaskProvider::SamMaskProvider(const QString& encoderPath, const QString& decoderPath, const Options& opts)
    : d(std::make_unique<Impl>())
{
    d->opts = opts;
    try {
        d->encoder = openSession(d->env, encoderPath, opts.preferGpu);
        d->decoder = openSession(d->env, decoderPath, opts.preferGpu);
    } catch (const Ort::Exception& e) {
        d->error = QStringLiteral("Could not load the SAM 2.1 graphs: %1").arg(QString::fromUtf8(e.what()));
        d->encoder.reset();
        d->decoder.reset();
    }
}

SamMaskProvider::SamMaskProvider(const QString& e, const QString& dd) : SamMaskProvider(e, dd, Options{}) {}
SamMaskProvider::~SamMaskProvider() = default;

bool SamMaskProvider::ok() const { return d->encoder && d->decoder; }
QString SamMaskProvider::error() const { return d->error; }
SamMaskProvider::Timing SamMaskProvider::timing() const { return d->timing; }

std::vector<Mask> SamMaskProvider::masksFor(const ViewImage& view)
{
    std::vector<Mask> out;
    if (!ok() || !view.raster || view.image.isNull()) return out;
    const Raster& r = *view.raster;
    const auto points = Sam::gridPoints(r, d->opts.pointsPerSide);
    if (points.empty()) return out;
    QElapsedTimer t;
    t.start();

    // Encoder: RGB -> 1024^2, ImageNet normalised NCHW.
    const int S = Sam::kInputSize;
    const QImage img = view.image.convertToFormat(QImage::Format_RGB888)
                           .scaled(S, S, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    std::vector<float> input(size_t(3) * S * S);
    for (int y = 0; y < S; ++y) {
        const uchar* row = img.constScanLine(y);
        for (int x = 0; x < S; ++x)
            for (int c = 0; c < 3; ++c)
                input[size_t(c) * S * S + size_t(y) * S + x] = (row[3 * x + c] / 255.0f - kMean[c]) / kStd[c];
    }
    const Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const int64_t inShape[4] = {1, 3, S, S};
    Ort::Value inTensor = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), inShape, 4);
    const char* encIn[] = {"image"};
    const char* encOut[] = {"image_embed", "high_res_feat0", "high_res_feat1"};
    std::vector<Ort::Value> feats;
    try {
        feats = d->encoder->Run(Ort::RunOptions{nullptr}, encIn, &inTensor, 1, encOut, 3);
    } catch (const Ort::Exception& e) {
        d->error = QString::fromUtf8(e.what());
        return out;
    }
    d->timing.encodeMs += double(t.nsecsElapsed()) / 1e6;
    t.restart();

    // Decoder over the point grid, one foreground point per prompt.
    const char* decIn[] = {"image_embed", "high_res_feat0", "high_res_feat1", "point_coords", "point_labels"};
    const char* decOut[] = {"mask_logits", "iou_scores"};
    std::vector<std::vector<float>> logitStore;   // keeps candidate logits alive
    std::vector<Sam::Candidate> cands;
    const int lowN = Sam::kLowRes * Sam::kLowRes;
    const int batch = std::max(1, d->opts.batch);
    const auto& f = d->opts.filter;
    for (size_t first = 0; first < points.size(); first += batch) {
        if (d->opts.cancelled && d->opts.cancelled()) return {};
        const int b = int(std::min<size_t>(batch, points.size() - first));
        std::vector<float> coords(size_t(b) * 2), labels(size_t(b), 1.0f);
        for (int i = 0; i < b; ++i) {
            coords[2 * i] = points[first + i][0];
            coords[2 * i + 1] = points[first + i][1];
        }
        const int64_t cShape[3] = {b, 1, 2}, lShape[2] = {b, 1};
        Ort::Value inputs[5] = {
            std::move(feats[0]), std::move(feats[1]), std::move(feats[2]),
            Ort::Value::CreateTensor<float>(mem, coords.data(), coords.size(), cShape, 3),
            Ort::Value::CreateTensor<float>(mem, labels.data(), labels.size(), lShape, 2)};
        std::vector<Ort::Value> res;
        try {
            res = d->decoder->Run(Ort::RunOptions{nullptr}, decIn, inputs, 5, decOut, 2);
        } catch (const Ort::Exception& e) {
            d->error = QString::fromUtf8(e.what());
            return {};
        }
        for (int k = 0; k < 3; ++k) feats[k] = std::move(inputs[k]);
        const float* logits = res[0].GetTensorData<float>();
        const float* ious = res[1].GetTensorData<float>();
        for (int i = 0; i < b * 3; ++i) {
            const float* lg = logits + size_t(i) * lowN;
            if (!std::isfinite(ious[i]) || ious[i] < f.minPredictedIou) continue;
            const float stab = Sam::stabilityScore(lg, lowN, f.stabilityOffset);
            if (stab < f.minStability) continue;
            logitStore.emplace_back(lg, lg + lowN);
            Sam::Candidate c;
            c.score = ious[i];
            c.stability = stab;
            c.logits = logitStore.back().data();
            cands.push_back(std::move(c));
        }
        d->timing.prompts += b;
    }
    // logitStore may have reallocated while growing: rebind before use.
    for (size_t i = 0; i < cands.size(); ++i) cands[i].logits = logitStore[i].data();
    d->timing.decodeMs += double(t.nsecsElapsed()) / 1e6;
    t.restart();

    std::vector<Sam::Candidate> valid;
    for (auto& c : cands)
        if (Sam::binarize(c)) valid.push_back(std::move(c));
    d->timing.candidates += int(valid.size());
    int foreground = 0;
    for (int fid : r.face) foreground += fid >= 0;
    const int minArea = std::max(1, int(f.minAreaFraction * float(foreground)));
    for (int i : Sam::nms(valid, f.nmsIou)) {
        Mask m;
        m.pixels = Sam::upsample(valid[i].logits, r.size);
        int area = 0;
        for (size_t p = 0; p < m.pixels.size(); ++p) {
            if (r.face[p] < 0) m.pixels[p] = 0;
            area += m.pixels[p];
        }
        if (area < minArea) continue;
        m.score = std::clamp(valid[i].score, 0.01f, 1.0f);
        out.push_back(std::move(m));
    }
    d->timing.kept += int(out.size());
    d->timing.postMs += double(t.nsecsElapsed()) / 1e6;
    ++d->timing.images;
    return out;
}

#endif // ENABLE_ONNX

} // namespace GenericSeg
