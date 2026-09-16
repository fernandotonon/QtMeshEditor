#include "PhotoDepth.h"

#include "AppStorage.h"
#include "ModelDownloader.h"
#include "ModelFetch.h"
#ifdef ENABLE_ONNX
#include "OnnxRuntimeSettings.h"
#include <onnxruntime_cxx_api.h>
#endif

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QPainter>
#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr const char* kModelFile = "da2_small.onnx";
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/depth/";
constexpr const char* kBaseUrlSettingsKey = "ai/depthModelBaseUrl";
constexpr const char* kModelLabel = "Depth-Anything-V2-Small depth model";

// ImageNet normalisation — the values the reference preprocessor uses
// (preprocessor_config.json: image_mean / image_std).
constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kStd[3]  = {0.229f, 0.224f, 0.225f};

QString modelDir()
{
    return QDir(AppStorage::aiModelsRoot()).filePath(QStringLiteral("depth/"));
}

} // namespace

namespace PhotoDepth {

QString modelPath()
{
    return QDir(modelDir()).filePath(QString::fromLatin1(kModelFile));
}

bool modelPresent() { return QFileInfo::exists(modelPath()); }

// ---------------------------------------------------------------------------
// Pure helpers — always compiled, so they are testable on any build
// ---------------------------------------------------------------------------

std::vector<float> toModelInput(const QImage& photo, bool letterbox)
{
    std::vector<float> out(static_cast<size_t>(kInputSize) * kInputSize * 3, 0.0f);
    if (photo.isNull()) return out;

    QImage square(kInputSize, kInputSize, QImage::Format_RGB888);
    square.fill(Qt::black);
    if (letterbox) {
        // Preserve aspect: scale the long edge to kInputSize and centre it.
        const QImage fit = photo.scaled(kInputSize, kInputSize, Qt::KeepAspectRatio,
                                        Qt::SmoothTransformation)
                               .convertToFormat(QImage::Format_RGB888);
        QPainter p(&square);
        p.drawImage((kInputSize - fit.width()) / 2,
                    (kInputSize - fit.height()) / 2, fit);
    } else {
        square = photo.scaled(kInputSize, kInputSize, Qt::IgnoreAspectRatio,
                              Qt::SmoothTransformation)
                      .convertToFormat(QImage::Format_RGB888);
    }

    // CHW, ImageNet-normalised.
    const size_t plane = static_cast<size_t>(kInputSize) * kInputSize;
    for (int y = 0; y < kInputSize; ++y) {
        const uchar* line = square.constScanLine(y);
        for (int x = 0; x < kInputSize; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float v = static_cast<float>(line[x * 3 + c]) / 255.0f;
                out[c * plane + static_cast<size_t>(y) * kInputSize + x] =
                    (v - kMean[c]) / kStd[c];
            }
        }
    }
    return out;
}

QImage toDepthImage(const std::vector<float>& raw, int w, int h,
                    float* outMin, float* outMax)
{
    if (w <= 0 || h <= 0
        || raw.size() < static_cast<size_t>(w) * static_cast<size_t>(h))
        return {};

    float mn = std::numeric_limits<float>::max();
    float mx = -std::numeric_limits<float>::max();
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
        const float v = raw[i];
        // Skip EVERY non-finite value, not just NaN. std::min/max do already
        // shrug off NaN (they return their second argument when the comparison
        // is false), but an isolated +inf/-inf compares normally and poisons
        // the range: +inf collapses every finite pixel toward black, and -inf
        // makes the normalisation inf/inf = NaN on the way to the 0..255 cast.
        // The inference probe cannot catch this — it only rejects output where
        // EVERY sampled value is non-finite.
        if (!std::isfinite(v)) continue;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    if (mn > mx) { mn = 0.0f; mx = 0.0f; }   // all non-finite
    if (outMin) *outMin = mn;
    if (outMax) *outMax = mx;

    QImage img(w, h, QImage::Format_Grayscale8);
    const float span = mx - mn;
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const float v = raw[static_cast<size_t>(y) * w + x];
            // The model emits INVERSE depth (larger = nearer), which is already
            // the near=bright convention MeshDepthRenderer uses, so no flip.
            // A constant field would divide by zero — emit mid-grey instead of
            // a NaN-filled image.
            const float t = (span > 1e-9f && std::isfinite(v))
                                ? (v - mn) / span : 0.5f;
            line[x] = static_cast<uchar>(
                std::clamp(t, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    return img;
}

QImage cropLetterbox(const QImage& square, int srcW, int srcH)
{
    if (square.isNull() || srcW <= 0 || srcH <= 0) return square;
    if (srcW == srcH) return square;
    const int side = square.width();
    // Mirror toModelInput's fit: the long edge fills `side`.
    const double s = static_cast<double>(side) / std::max(srcW, srcH);
    const int w = std::max(1, static_cast<int>(std::lround(srcW * s)));
    const int h = std::max(1, static_cast<int>(std::lround(srcH * s)));
    return square.copy((side - w) / 2, (side - h) / 2, w, h);
}

#ifndef ENABLE_ONNX

bool isAvailable() { return false; }
QString ensureModelBlocking(QString* error)
{
    if (error) *error = QStringLiteral("photo depth needs an ONNX build (rebuild with -DENABLE_ONNX)");
    return {};
}

Result estimate(const QImage&, const QString&, const Options&)
{
    Result r;
    r.error = QStringLiteral("Photo depth needs an ONNX build "
                             "(rebuild with -DENABLE_ONNX).");
    return r;
}

#else // ENABLE_ONNX

bool isAvailable() { return true; }

QString ensureModelBlocking(QString* error)
{
    const QString dst = modelPath();
    if (QFileInfo::exists(dst)) return dst;
    if (!qEnvironmentVariableIsEmpty("QTMESH_DEPTH_NO_DOWNLOAD")) {
        if (error) *error = QStringLiteral("downloads disabled by QTMESH_DEPTH_NO_DOWNLOAD");
        return {};
    }

    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_DEPTH_MODEL_BASE_URL");
            base = env.isEmpty() ? QString::fromLatin1(kDefaultModelBaseUrl)
                                 : QString::fromUtf8(env);
        }
    }
    if (base.isEmpty()) return {};
    if (!base.endsWith('/')) base += '/';

    auto* dl = ModelDownloader::instance();
    if (!dl) return {};

    QDir().mkpath(QFileInfo(dst).absolutePath());
    const QString url = base + QString::fromLatin1(kModelFile);
    ModelFetch::Request req;
    req.url = url;
    req.destination = dst;
    req.label = QString::fromLatin1(kModelLabel);
    req.timeoutMs = 600000;
    // #1037: one shared blocking wait — keeps the downloader's own error text
    // and the synchronous-rejection guard every consumer used to lack.
    const ModelFetch::Outcome fo = ModelFetch::ensureBlocking(req);
    if (!fo.ok) {
        if (error) *error = fo.error;
        return {};
    }
    return fo.path;
}

Result estimate(const QImage& photo, const QString& modelFile,
                const Options& opts)
{
    Result r;
    if (photo.isNull()) { r.error = QStringLiteral("empty image"); return r; }
    if (!QFileInfo::exists(modelFile)) {
        r.error = QStringLiteral("Depth model not found at %1.").arg(modelFile);
        return r;
    }

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_depth");
        Ort::SessionOptions so;
        OnnxRuntimeSettings::configureSessionOptions(so);
#ifdef _WIN32
        Ort::Session session(env, modelFile.toStdWString().c_str(), so);
#else
        Ort::Session session(env, modelFile.toStdString().c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;

        std::vector<float> input = toModelInput(photo, opts.letterbox);
        const int64_t shape[4] = {1, 3, kInputSize, kInputSize};
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value in = Ort::Value::CreateTensor<float>(
            mem, input.data(), input.size(), shape, 4);

        auto inName = session.GetInputNameAllocated(0, alloc);
        auto outName = session.GetOutputNameAllocated(0, alloc);
        const char* inNames[]  = {inName.get()};
        const char* outNames[] = {outName.get()};

        auto outs = session.Run(Ort::RunOptions{nullptr},
                                inNames, &in, 1, outNames, 1);
        if (outs.empty() || !outs[0].IsTensor()) {
            r.error = QStringLiteral("depth model produced no tensor output.");
            return r;
        }
        auto info = outs[0].GetTensorTypeAndShapeInfo();
        const size_t n = info.GetElementCount();
        if (n < static_cast<size_t>(kInputSize) * kInputSize) {
            r.error = QStringLiteral("depth output too small (%1 values).").arg(n);
            return r;
        }
        const float* data = outs[0].GetTensorData<float>();
        std::vector<float> raw(data, data + n);

        // Guard the #1025 failure mode: a numerically broken export returns
        // finite-shaped, all-NaN output. Say so instead of emitting a
        // mid-grey image that looks like a plausible flat depth map.
        {
            const size_t probe = std::min<size_t>(n, 1024);
            size_t bad = 0;
            for (size_t i = 0; i < probe; ++i)
                if (!std::isfinite(raw[i])) ++bad;
            if (bad == probe) {
                r.error = QStringLiteral(
                    "the depth model returned non-finite values for every "
                    "sampled output — the export is numerically broken.");
                return r;
            }
        }

        QImage square = toDepthImage(raw, kInputSize, kInputSize,
                                     &r.rawMin, &r.rawMax);
        if (square.isNull()) {
            r.error = QStringLiteral("failed to build the depth image.");
            return r;
        }
        if (opts.letterbox)
            square = cropLetterbox(square, photo.width(), photo.height());

        const int outW = opts.outputSize > 0 ? opts.outputSize : photo.width();
        const int outH = opts.outputSize > 0 ? opts.outputSize : photo.height();
        r.depth = square.scaled(outW, outH, Qt::IgnoreAspectRatio,
                                Qt::SmoothTransformation);
        r.ok = !r.depth.isNull();
        if (!r.ok) r.error = QStringLiteral("failed to resize the depth image.");
        return r;
    } catch (const Ort::Exception& e) {
        r.error = QStringLiteral("depth inference failed: %1")
                      .arg(QString::fromUtf8(e.what()));
        return r;
    }
}

#endif // ENABLE_ONNX

} // namespace PhotoDepth
