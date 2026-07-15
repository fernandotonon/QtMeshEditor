#include "FaceLandmarkDetector.h"

#include "ArkitTemplate.h"        // reuse its model dir + base-url convention
#include "../ModelDownloader.h"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace FaceRig {

namespace {
constexpr const char* kModelFile = "face_landmarks.onnx";
constexpr int kInputSize = 256;                  // MediaPipe FaceMesh V2 crop
constexpr int kMinLandmarkFloats = 468 * 3;      // 468/478 landmarks × xyz
}  // namespace

struct FaceLandmarkDetector::Impl {
#ifdef ENABLE_ONNX
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> inputNames;
    std::vector<std::string> outputNames;
    std::vector<const char*> inputNamesC;
    std::vector<const char*> outputNamesC;
#endif
    bool loaded = false;
};

FaceLandmarkDetector::FaceLandmarkDetector() : d(std::make_unique<Impl>()) {}
FaceLandmarkDetector::~FaceLandmarkDetector() = default;

QString FaceLandmarkDetector::modelPath()
{
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath(QStringLiteral("ai_models/facerig/")
                                   + QString::fromLatin1(kModelFile));
}

bool FaceLandmarkDetector::present() { return QFileInfo::exists(modelPath()); }

bool FaceLandmarkDetector::backendAvailable()
{
#ifdef ENABLE_ONNX
    return true;
#else
    return false;
#endif
}

QString FaceLandmarkDetector::ensureModelBlocking()
{
    const QString dest = modelPath();
    if (QFileInfo::exists(dest))
        return dest;
    if (!qEnvironmentVariableIsEmpty("QTMESH_FACERIG_NO_DOWNLOAD"))
        return {};

    // Same base URL as the ARKit template (they live together in facerig/).
    QString base;
    {
        QSettings s;
        base = s.value(QStringLiteral("ai/facerigModelBaseUrl")).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_FACERIG_MODEL_BASE_URL");
            base = env.isEmpty()
                ? QStringLiteral("https://huggingface.co/fernandotonon/"
                                 "QtMeshEditor-models/resolve/main/facerig/")
                : QString::fromUtf8(env);
        }
    }
    if (base.isEmpty())
        return {};
    if (!base.endsWith('/'))
        base += '/';

    auto* dl = ModelDownloader::instance();
    if (!dl)
        return {};

    QDir().mkpath(QFileInfo(dest).absolutePath());
    const QString url = base + QString::fromLatin1(kModelFile);
    const QString label = QStringLiteral("face landmark model");

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
    timeout.start(120000);  // 2 min — the model is small (~3 MB)

    dl->startDownload(url, dest, label);
    loop.exec();

    QObject::disconnect(onDone);
    QObject::disconnect(onErr);
    if (timedOut && dl)
        dl->cancelDownload();

    return (ok && !timedOut && QFileInfo::exists(dest)) ? dest : QString();
}

bool FaceLandmarkDetector::isAvailable() const { return d && d->loaded; }

#ifdef ENABLE_ONNX

bool FaceLandmarkDetector::load(const QString& path)
{
    m_error.clear();
    d->loaded = false;
    const QString p = path.isEmpty() ? modelPath() : path;
    if (!QFileInfo::exists(p)) {
        m_error = QStringLiteral("face landmark model not found at %1").arg(p);
        return false;
    }
    try {
        d->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                            "qtmesh_facelmk");
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#if defined(__APPLE__)
        try {
            std::unordered_map<std::string, std::string> coreml;
            so.AppendExecutionProvider("CoreML", coreml);
        } catch (...) { /* CPU fallback */ }
#endif
#ifdef _WIN32
        const std::wstring wp = p.toStdWString();
        d->session = std::make_unique<Ort::Session>(*d->env, wp.c_str(), so);
#else
        const std::string sp = p.toStdString();
        d->session = std::make_unique<Ort::Session>(*d->env, sp.c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;
        const size_t ni = d->session->GetInputCount();
        for (size_t i = 0; i < ni; ++i) {
            auto n = d->session->GetInputNameAllocated(i, alloc);
            d->inputNames.emplace_back(n.get());
        }
        const size_t no = d->session->GetOutputCount();
        for (size_t i = 0; i < no; ++i) {
            auto n = d->session->GetOutputNameAllocated(i, alloc);
            d->outputNames.emplace_back(n.get());
        }
        for (auto& s : d->inputNames) d->inputNamesC.push_back(s.c_str());
        for (auto& s : d->outputNames) d->outputNamesC.push_back(s.c_str());
        d->loaded = !d->inputNames.empty() && !d->outputNames.empty();
        if (!d->loaded)
            m_error = QStringLiteral("model has no inputs/outputs");
        return d->loaded;
    } catch (const std::exception& e) {
        m_error = QStringLiteral("failed to load face landmark model: %1")
                      .arg(QString::fromUtf8(e.what()));
        d->loaded = false;
        return false;
    }
}

LandmarkResult FaceLandmarkDetector::detect(const QImage& image)
{
    LandmarkResult r;
    if (!isAvailable() || image.isNull()) return r;

    // Centre-square crop of the render, then resize to 256 — the head fills the
    // frame (we rendered it), so the upstream face detector is unnecessary. We
    // track the crop rect to map landmark px back into the ORIGINAL image space.
    const int W = image.width(), H = image.height();
    const int side = std::min(W, H);
    const int ox = (W - side) / 2, oy = (H - side) / 2;
    QImage crop = image.copy(ox, oy, side, side)
                      .convertToFormat(QImage::Format_RGB888)
                      .scaled(kInputSize, kInputSize, Qt::IgnoreAspectRatio,
                              Qt::SmoothTransformation);

    // NHWC float [0,1]
    std::vector<float> input(size_t(kInputSize) * kInputSize * 3);
    for (int y = 0; y < kInputSize; ++y) {
        const uchar* line = crop.constScanLine(y);
        for (int x = 0; x < kInputSize; ++x) {
            const uchar* px = line + x * 3;
            const size_t o = (size_t(y) * kInputSize + x) * 3;
            input[o + 0] = px[0] / 255.0f;
            input[o + 1] = px[1] / 255.0f;
            input[o + 2] = px[2] / 255.0f;
        }
    }

    try {
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<int64_t, 4> shape{1, kInputSize, kInputSize, 3};
        Ort::Value in = Ort::Value::CreateTensor<float>(
            mem, input.data(), input.size(), shape.data(), shape.size());
        auto outs = d->session->Run(
            Ort::RunOptions{nullptr}, d->inputNamesC.data(), &in, 1,
            d->outputNamesC.data(), d->outputNamesC.size());

        const float* rawLandmarks = nullptr;
        size_t landmarkFloats = 0;
        float presenceLogit = 0.f;
        bool presenceFound = false;
        for (auto& o : outs) {
            const auto info = o.GetTensorTypeAndShapeInfo();
            const size_t count = info.GetElementCount();
            if (count >= size_t(kMinLandmarkFloats)) {
                rawLandmarks = o.GetTensorData<float>();
                landmarkFloats = count;
            } else if (count == 1 && !presenceFound) {
                presenceLogit = o.GetTensorData<float>()[0];
                presenceFound = true;
            }
        }
        if (!rawLandmarks) {
            m_error = QStringLiteral("unexpected face landmark outputs");
            return r;
        }
        r.confidence = presenceFound
            ? 1.f / (1.f + std::exp(-std::clamp(presenceLogit, -50.f, 50.f)))
            : 1.f;

        const int n = int(landmarkFloats / 3);
        r.points.reserve(size_t(n));
        const float scale = float(side) / float(kInputSize);
        for (int i = 0; i < n; ++i) {
            // model outputs 256-space px (x,y) + relative z; map back to the
            // ORIGINAL image pixel space via the crop offset + scale.
            const float lx = rawLandmarks[i * 3 + 0] * scale + ox;
            const float ly = rawLandmarks[i * 3 + 1] * scale + oy;
            const float lz = rawLandmarks[i * 3 + 2] * scale;
            r.points.push_back({lx, ly, lz});
        }
        r.ok = true;
        return r;
    } catch (const std::exception& e) {
        m_error = QStringLiteral("face landmark inference failed: %1")
                      .arg(QString::fromUtf8(e.what()));
        return r;
    }
}

#else  // !ENABLE_ONNX

bool FaceLandmarkDetector::load(const QString&)
{
    m_error = QStringLiteral("built without ONNX (ENABLE_ONNX off)");
    return false;
}
LandmarkResult FaceLandmarkDetector::detect(const QImage&) { return {}; }

#endif  // ENABLE_ONNX

}  // namespace FaceRig
