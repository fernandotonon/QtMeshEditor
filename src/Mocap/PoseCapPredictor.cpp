#ifdef ENABLE_MOCAP

#include "PoseCapPredictor.h"

#include "FaceCapGeom.h"
#include "../ModelDownloader.h"
#include "../OnnxRuntimeSettings.h"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <vector>

#include "AppStorage.h"

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace {

constexpr const char* kDetectorFile = "pose_detector.onnx";
constexpr const char* kLandmarksFile = "pose_landmarks.onnx";
// base URL points at the mocap ROOT (shared with FaceCapPredictor); the
// pose/ subdir is appended so ONE override serves both bundles.
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/mocap/";
constexpr const char* kBaseUrlSettingsKey = "ai/mocapModelBaseUrl";

constexpr float kPresenceThreshold = 0.5f;

float stableSigmoid(float x)
{
    x = std::clamp(x, -50.f, 50.f);
    return x >= 0.f ? 1.f / (1.f + std::exp(-x))
                    : std::exp(x) / (1.f + std::exp(x));
}

}  // namespace

QString PoseCapPredictor::modelDir()
{
    return QDir(AppStorage::aiModelsRoot()).filePath(QStringLiteral("mocap/pose"));
}

bool PoseCapPredictor::modelsPresent()
{
    const QDir dir(modelDir());
    return QFileInfo::exists(dir.filePath(QLatin1String(kDetectorFile)))
           && QFileInfo::exists(dir.filePath(QLatin1String(kLandmarksFile)));
}

QString PoseCapPredictor::ensureModelsBlocking()
{
#ifndef ENABLE_ONNX
    return {};
#else
    if (modelsPresent())
        return modelDir();
    if (!qEnvironmentVariableIsEmpty("QTMESH_MOCAP_NO_DOWNLOAD"))
        return {};

    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_MOCAP_MODEL_BASE_URL");
            base = env.isEmpty() ? QString::fromLatin1(kDefaultModelBaseUrl)
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

    const QDir dir(modelDir());
    auto downloadOne = [&](const char* fileName) -> bool {
        const QString dest = dir.filePath(QLatin1String(fileName));
        if (QFileInfo::exists(dest))
            return true;
        QDir().mkpath(QFileInfo(dest).absolutePath());
        const QString label =
            QStringLiteral("Pose capture model (%1)").arg(QLatin1String(fileName));
        QEventLoop loop;
        bool ok = false, timedOut = false;
        auto onDone = QObject::connect(
            dl, &ModelDownloader::downloadCompleted, &loop,
            [&](const QString& name, const QString&) {
                if (name == label) { ok = true; loop.quit(); }
            });
        auto onErr = QObject::connect(
            dl, &ModelDownloader::downloadError, &loop,
            [&](const QString& name, const QString&) {
                if (name == label) { ok = false; loop.quit(); }
            });
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop,
                         [&]() { timedOut = true; loop.quit(); });
        timeout.start(300000);  // 5 min — ~25 MB total
        dl->startDownload(base + QLatin1String("pose/") + QLatin1String(fileName), dest, label);
        loop.exec();
        QObject::disconnect(onDone);
        QObject::disconnect(onErr);
        if (timedOut && dl)
            dl->cancelDownload();
        return ok && !timedOut && QFileInfo::exists(dest);
    };

    if (!downloadOne(kDetectorFile) || !downloadOne(kLandmarksFile))
        return {};
    return modelDir();
#endif
}

#ifndef ENABLE_ONNX

struct PoseCapPredictor::Impl {
    QString error = QStringLiteral(
        "pose capture predictor unavailable (built without ENABLE_ONNX)");
};

PoseCapPredictor::PoseCapPredictor() : d(new Impl) {}
PoseCapPredictor::~PoseCapPredictor() = default;
bool PoseCapPredictor::load(const QString&) { return false; }
bool PoseCapPredictor::isAvailable() const { return false; }
QString PoseCapPredictor::lastError() const { return d->error; }
PoseSample PoseCapPredictor::predict(const QImage&, double timeSec)
{
    PoseSample s;
    s.timeSec = timeSec;
    return s;
}
void PoseCapPredictor::resetTracking() {}
int PoseCapPredictor::detectorRuns() const { return 0; }

#else  // ENABLE_ONNX

struct PoseCapPredictor::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "qtmesh_posecap"};
    std::unique_ptr<Ort::Session> detector;
    std::unique_ptr<Ort::Session> landmarks;
    std::vector<std::array<float, 2>> anchors;
    int detectorSize = 224;
    int landmarksSize = 256;
    QString error;
    bool available = false;

    bool tracking = false;
    FaceCapGeom::RotatedRect trackedRect;
    int detectorRuns = 0;

    std::vector<float> detInput;
    std::vector<float> lmkInput;

    std::unique_ptr<Ort::Session> openSession(const QString& path)
    {
        Ort::SessionOptions so;
        OnnxRuntimeSettings::configureSessionOptions(so);
        so.SetIntraOpNumThreads(2);
#ifdef Q_OS_WIN
        const std::wstring wpath = path.toStdWString();
        return std::make_unique<Ort::Session>(env, wpath.c_str(), so);
#else
        const QByteArray p = path.toUtf8();
        return std::make_unique<Ort::Session>(env, p.constData(), so);
#endif
    }
};

PoseCapPredictor::PoseCapPredictor() : d(new Impl) {}
PoseCapPredictor::~PoseCapPredictor() = default;

bool PoseCapPredictor::load(const QString& dirIn)
{
    const QDir dir(dirIn.isEmpty() ? modelDir() : dirIn);
    const QString det = dir.filePath(QLatin1String(kDetectorFile));
    const QString lmk = dir.filePath(QLatin1String(kLandmarksFile));
    if (!QFileInfo::exists(det) || !QFileInfo::exists(lmk)) {
        d->error = QStringLiteral(
            "pose capture models not found in %1 — they download on first "
            "use, or set QTMESH_MOCAP_MODEL_BASE_URL").arg(dir.absolutePath());
        return false;
    }
    try {
        d->detector = d->openSession(det);
        d->landmarks = d->openSession(lmk);
        {
            const auto info = d->detector->GetInputTypeInfo(0);
            const auto shape = info.GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() == 4 && shape[1] > 0)
                d->detectorSize = static_cast<int>(shape[1]);
        }
        {
            const auto info = d->landmarks->GetInputTypeInfo(0);
            const auto shape = info.GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() == 4 && shape[1] > 0)
                d->landmarksSize = static_cast<int>(shape[1]);
        }
        d->anchors =
            FaceCapGeom::genSsdAnchors(d->detectorSize, {8, 16, 32, 32, 32});
        {
            const auto info = d->detector->GetOutputTypeInfo(0);
            const auto shape = info.GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() == 3
                && shape[1] != static_cast<int64_t>(d->anchors.size())) {
                d->error = QStringLiteral(
                    "pose detector anchor count mismatch (model %1, ours %2)")
                               .arg(shape[1]).arg(d->anchors.size());
                return false;
            }
        }
        d->detInput.resize(static_cast<size_t>(d->detectorSize)
                           * d->detectorSize * 3);
        d->lmkInput.resize(static_cast<size_t>(d->landmarksSize)
                           * d->landmarksSize * 3);
        d->available = true;
        d->error.clear();
        return true;
    } catch (const Ort::Exception& e) {
        d->error = QStringLiteral("failed to load pose capture models: %1")
                       .arg(QString::fromUtf8(e.what()));
        d->available = false;
        return false;
    }
}

bool PoseCapPredictor::isAvailable() const { return d->available; }
QString PoseCapPredictor::lastError() const { return d->error; }
void PoseCapPredictor::resetTracking() { d->tracking = false; }
int PoseCapPredictor::detectorRuns() const { return d->detectorRuns; }

PoseSample PoseCapPredictor::predict(const QImage& image, double timeSec)
{
    PoseSample sample;
    sample.timeSec = timeSec;
    if (!d->available) {
        if (d->error.isEmpty())
            d->error = QStringLiteral("predictor not loaded — call load()");
        return sample;
    }
    QImage rgb = image.format() == QImage::Format_RGB888
                     ? image
                     : image.convertToFormat(QImage::Format_RGB888);
    const int W = rgb.width();
    const int H = rgb.height();
    if (W <= 0 || H <= 0)
        return sample;

    try {
        Ort::MemoryInfo mem =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::AllocatorWithDefaultOptions alloc;

        auto runSession = [&](Ort::Session* session, Ort::Value input)
            -> std::vector<Ort::Value> {
            const Ort::AllocatedStringPtr inName =
                session->GetInputNameAllocated(0, alloc);
            std::vector<Ort::AllocatedStringPtr> outHolders;
            std::vector<const char*> outNames;
            const size_t n = session->GetOutputCount();
            for (size_t i = 0; i < n; ++i) {
                outHolders.push_back(session->GetOutputNameAllocated(i, alloc));
                outNames.push_back(outHolders.back().get());
            }
            const char* in = inName.get();
            return session->Run(Ort::RunOptions{nullptr}, &in, &input, 1,
                                outNames.data(), outNames.size());
        };

        FaceCapGeom::RotatedRect rect;
        if (d->tracking) {
            rect = d->trackedRect;
        } else {
            const int ds = d->detectorSize;
            FaceCapGeom::Letterbox lb = FaceCapGeom::letterboxToTensor(
                rgb, ds, -1.f, 1.f, d->detInput.data());
            const std::array<int64_t, 4> shape{1, ds, ds, 3};
            Ort::Value input = Ort::Value::CreateTensor<float>(
                mem, d->detInput.data(), d->detInput.size(), shape.data(),
                shape.size());
            auto outs = runSession(d->detector.get(), std::move(input));
            ++d->detectorRuns;

            const float* rawBoxes = nullptr;
            const float* rawScores = nullptr;
            for (auto& o : outs) {
                const auto s = o.GetTensorTypeAndShapeInfo().GetShape();
                if (s.size() == 3 && s[2] > 1)
                    rawBoxes = o.GetTensorData<float>();
                else if (s.size() == 3 && s[2] == 1)
                    rawScores = o.GetTensorData<float>();
            }
            if (!rawBoxes || !rawScores) {
                d->error = QStringLiteral("unexpected pose detector outputs");
                return sample;
            }
            auto dets = FaceCapGeom::decodeDetections(
                rawBoxes, rawScores, d->anchors, ds, /*numKeypoints=*/4);
            if (dets.empty())
                return sample;
            FaceCapGeom::Detection& best = dets.front();
            for (auto& kp : best.keypoints)
                FaceCapGeom::unletterbox(lb, kp[0], kp[1]);
            rect = FaceCapGeom::rectFromPoseDetection(best, W, H);
        }

        const int ls = d->landmarksSize;
        FaceCapGeom::cropRotatedRectToTensor(rgb, rect, ls, 0.f, 1.f,
                                             d->lmkInput.data());
        const std::array<int64_t, 4> lshape{1, ls, ls, 3};
        Ort::Value linput = Ort::Value::CreateTensor<float>(
            mem, d->lmkInput.data(), d->lmkInput.size(), lshape.data(),
            lshape.size());
        auto louts = runSession(d->landmarks.get(), std::move(linput));

        // identify outputs by element count: 195 = 39x5 screen landmarks,
        // 117 = 39x3 world landmarks, 1 = presence PROBABILITY (not a logit)
        const float* rawScreen = nullptr;
        const float* rawWorld = nullptr;
        float presence = 0.f;
        for (auto& o : louts) {
            const size_t count = o.GetTensorTypeAndShapeInfo().GetElementCount();
            if (count == 39 * 5)
                rawScreen = o.GetTensorData<float>();
            else if (count == 39 * 3)
                rawWorld = o.GetTensorData<float>();
            else if (count == 1)
                presence = o.GetTensorData<float>()[0];
        }
        if (!rawScreen || !rawWorld) {
            d->error = QStringLiteral("unexpected pose landmark outputs");
            return sample;
        }
        if (presence < kPresenceThreshold) {
            if (d->tracking) {
                d->tracking = false;
                return predict(image, timeSec);  // retry with the detector
            }
            return sample;
        }

        // world landmarks: rotate x,y by the ROI rotation only
        const float ca = std::cos(rect.angle);
        const float sa = std::sin(rect.angle);
        for (int i = 0; i < 33; ++i) {
            const float wx = rawWorld[i * 3 + 0];
            const float wy = rawWorld[i * 3 + 1];
            sample.world[i * 3 + 0] = wx * ca - wy * sa;
            sample.world[i * 3 + 1] = wx * sa + wy * ca;
            sample.world[i * 3 + 2] = rawWorld[i * 3 + 2];
            sample.screenCrop[i * 3 + 0] = rawScreen[i * 5 + 0] / static_cast<float>(ls);
            sample.screenCrop[i * 3 + 1] = rawScreen[i * 5 + 1] / static_cast<float>(ls);
            sample.screenCrop[i * 3 + 2] = rawScreen[i * 5 + 2] / static_cast<float>(ls);
            sample.visibility[i] = stableSigmoid(rawScreen[i * 5 + 3]);
        }
        {
            float pts[33 * 3];
            for (int i = 0; i < 33; ++i) {
                pts[i * 3 + 0] = sample.screenCrop[i * 3 + 0];
                pts[i * 3 + 1] = sample.screenCrop[i * 3 + 1];
                pts[i * 3 + 2] = sample.screenCrop[i * 3 + 2];
            }
            FaceCapGeom::projectLandmarks(pts, 33, 3, rect);
            for (int i = 0; i < 33; ++i) {
                sample.imageXy[i * 2 + 0] = pts[i * 3 + 0];
                sample.imageXy[i * 2 + 1] = pts[i * 3 + 1];
            }
        }
        sample.confidence = presence;

        // next-frame ROI from the auxiliary alignment landmarks (raw 33/34):
        // centre + scale point in crop space -> image px -> alignment rect
        {
            float aux[2 * 3] = {
                rawScreen[33 * 5 + 0] / ls, rawScreen[33 * 5 + 1] / ls, 0.f,
                rawScreen[34 * 5 + 0] / ls, rawScreen[34 * 5 + 1] / ls, 0.f,
            };
            FaceCapGeom::projectLandmarks(aux, 2, 3, rect);
            FaceCapGeom::Detection synth;
            synth.keypoints = {{aux[0] / W, aux[1] / H},
                               {aux[3] / W, aux[4] / H}};
            d->trackedRect = FaceCapGeom::rectFromPoseDetection(synth, W, H);
            d->tracking = d->trackedRect.w > 8.f && d->trackedRect.h > 8.f;
        }
        return sample;
    } catch (const Ort::Exception& e) {
        d->error = QStringLiteral("pose capture inference failed: %1")
                       .arg(QString::fromUtf8(e.what()));
        d->tracking = false;
        return sample;
    }
}

#endif  // ENABLE_ONNX
#endif  // ENABLE_MOCAP
