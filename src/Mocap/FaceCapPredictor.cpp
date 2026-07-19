#ifdef ENABLE_MOCAP

#include "FaceCapPredictor.h"

#include "FaceCapCanonicalData.h"
#include "FaceCapGeom.h"
#include "FaceCapPose.h"
#include "../ModelDownloader.h"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <vector>

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace {

constexpr const char* kDetectorFile = "face_detector.onnx";
constexpr const char* kLandmarksFile = "face_landmarks.onnx";
constexpr const char* kBlendshapesFile = "face_blendshapes.onnx";
// base URL points at the mocap ROOT; the face/pose subdir is appended so ONE
// override serves both bundles.
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/mocap/";
constexpr const char* kBaseUrlSettingsKey = "ai/mocapModelBaseUrl";

constexpr float kPresenceThreshold = 0.5f;

}  // namespace

// ---------------------------------------------------------------------------
// static model management
// ---------------------------------------------------------------------------

QString FaceCapPredictor::modelDir()
{
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath(QStringLiteral("ai_models/mocap/face"));
}

bool FaceCapPredictor::modelsPresent()
{
    const QDir dir(modelDir());
    return QFileInfo::exists(dir.filePath(QLatin1String(kDetectorFile)))
           && QFileInfo::exists(dir.filePath(QLatin1String(kLandmarksFile)))
           && QFileInfo::exists(dir.filePath(QLatin1String(kBlendshapesFile)));
}

QString FaceCapPredictor::ensureModelsBlocking()
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
            QStringLiteral("Face capture model (%1)").arg(QLatin1String(fileName));
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
        timeout.start(300000);  // 5 min — the three graphs total ~6.4 MB
        dl->startDownload(base + QLatin1String("face/") + QLatin1String(fileName), dest, label);
        loop.exec();
        QObject::disconnect(onDone);
        QObject::disconnect(onErr);
        if (timedOut && dl)
            dl->cancelDownload();
        return ok && !timedOut && QFileInfo::exists(dest);
    };

    if (!downloadOne(kDetectorFile) || !downloadOne(kLandmarksFile)
        || !downloadOne(kBlendshapesFile))
        return {};
    return modelDir();
#endif
}

// ---------------------------------------------------------------------------
// non-ONNX stub (ENABLE_MOCAP requires ENABLE_ONNX in CMake, but keep the
// TU compilable standalone)
// ---------------------------------------------------------------------------

#ifndef ENABLE_ONNX

struct FaceCapPredictor::Impl {
    QString error = QStringLiteral(
        "face capture predictor unavailable (built without ENABLE_ONNX)");
};

FaceCapPredictor::FaceCapPredictor() : d(new Impl) {}
FaceCapPredictor::~FaceCapPredictor() = default;
bool FaceCapPredictor::load(const QString&) { return false; }
bool FaceCapPredictor::isAvailable() const { return false; }
QString FaceCapPredictor::lastError() const { return d->error; }
FaceSample FaceCapPredictor::predict(const QImage&, double timeSec)
{
    FaceSample s;
    s.timeSec = timeSec;
    return s;
}
void FaceCapPredictor::resetTracking() {}
int FaceCapPredictor::detectorRuns() const { return 0; }

#else  // ENABLE_ONNX

struct FaceCapPredictor::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "qtmesh_facecap"};
    std::unique_ptr<Ort::Session> detector;
    std::unique_ptr<Ort::Session> landmarks;
    std::unique_ptr<Ort::Session> blendshapes;
    std::vector<std::array<float, 2>> anchors;
    int detectorSize = 128;
    int landmarksSize = 256;
    QString error;
    bool available = false;

    // detector-skip tracking state
    bool tracking = false;
    FaceCapGeom::RotatedRect trackedRect;
    int detectorRuns = 0;

    // scratch buffers (avoid per-frame allocation in live mode)
    std::vector<float> detInput;
    std::vector<float> lmkInput;

    std::unique_ptr<Ort::Session> openSession(const QString& path)
    {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(2);
#ifdef Q_OS_MACOS
        // CoreML EP where it helps; CPU fallback is always registered.
        try {
            std::unordered_map<std::string, std::string> opts;
            so.AppendExecutionProvider("CoreML", opts);
        } catch (const Ort::Exception&) {}
#endif
#ifdef Q_OS_WIN
        const std::wstring wpath = path.toStdWString();
        return std::make_unique<Ort::Session>(env, wpath.c_str(), so);
#else
        const QByteArray p = path.toUtf8();
        return std::make_unique<Ort::Session>(env, p.constData(), so);
#endif
    }
};

FaceCapPredictor::FaceCapPredictor() : d(new Impl) {}
FaceCapPredictor::~FaceCapPredictor() = default;

bool FaceCapPredictor::load(const QString& dirIn)
{
    const QDir dir(dirIn.isEmpty() ? modelDir() : dirIn);
    const QString det = dir.filePath(QLatin1String(kDetectorFile));
    const QString lmk = dir.filePath(QLatin1String(kLandmarksFile));
    const QString bs = dir.filePath(QLatin1String(kBlendshapesFile));
    if (!QFileInfo::exists(det) || !QFileInfo::exists(lmk)
        || !QFileInfo::exists(bs)) {
        // Clear availability from any prior successful load() — a later load()
        // pointed at a missing/invalid dir must report unavailable, not leave
        // isAvailable() lying about the previous session's models.
        d->available = false;
        d->error = QStringLiteral(
            "face capture models not found in %1 — they download on first "
            "use, or set QTMESH_MOCAP_MODEL_BASE_URL").arg(dir.absolutePath());
        return false;
    }
    try {
        d->detector = d->openSession(det);
        d->landmarks = d->openSession(lmk);
        d->blendshapes = d->openSession(bs);

        // runtime shape discovery — never hardcode graph tensor names
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
        d->anchors = FaceCapGeom::genSsdAnchors(d->detectorSize, {8, 16, 16, 16});
        {
            const auto info = d->detector->GetOutputTypeInfo(0);
            const auto shape = info.GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() == 3
                && shape[1] != static_cast<int64_t>(d->anchors.size())) {
                d->error = QStringLiteral(
                    "face detector anchor count mismatch (model %1, ours %2)")
                               .arg(shape[1]).arg(d->anchors.size());
                d->available = false;
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
        d->error = QStringLiteral("failed to load face capture models: %1")
                       .arg(QString::fromUtf8(e.what()));
        d->available = false;
        return false;
    }
}

bool FaceCapPredictor::isAvailable() const { return d->available; }
QString FaceCapPredictor::lastError() const { return d->error; }
void FaceCapPredictor::resetTracking() { d->tracking = false; }
int FaceCapPredictor::detectorRuns() const { return d->detectorRuns; }

FaceSample FaceCapPredictor::predict(const QImage& image, double timeSec)
{
    FaceSample sample;
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
            outHolders.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                outHolders.push_back(session->GetOutputNameAllocated(i, alloc));
                outNames.push_back(outHolders.back().get());
            }
            const char* in = inName.get();
            return session->Run(Ort::RunOptions{nullptr}, &in, &input, 1,
                                outNames.data(), outNames.size());
        };

        // --- stage 1: face ROI (tracked, or detector)
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

            // identify regressors vs scores by the last dimension
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
                d->error = QStringLiteral("unexpected face detector outputs");
                return sample;
            }
            auto dets = FaceCapGeom::decodeDetections(
                rawBoxes, rawScores, d->anchors, ds, /*numKeypoints=*/6);
            if (dets.empty())
                return sample;  // confidence 0 — no face this frame
            FaceCapGeom::Detection& best = dets.front();
            FaceCapGeom::unletterbox(lb, best.box[0], best.box[1]);
            // box w/h scale by the content fraction
            best.box[2] /= lb.fracX;
            best.box[3] /= lb.fracY;
            for (auto& kp : best.keypoints)
                FaceCapGeom::unletterbox(lb, kp[0], kp[1]);
            rect = FaceCapGeom::rectFromFaceDetection(best, W, H);
        }

        // --- stage 2: landmarks
        const int ls = d->landmarksSize;
        FaceCapGeom::cropRotatedRectToTensor(rgb, rect, ls, 0.f, 1.f,
                                             d->lmkInput.data());
        const std::array<int64_t, 4> lshape{1, ls, ls, 3};
        Ort::Value linput = Ort::Value::CreateTensor<float>(
            mem, d->lmkInput.data(), d->lmkInput.size(), lshape.data(),
            lshape.size());
        auto louts = runSession(d->landmarks.get(), std::move(linput));

        const float* rawLandmarks = nullptr;
        size_t landmarkFloats = 0;
        float presenceLogit = 0.f;
        bool presenceFound = false;
        for (auto& o : louts) {
            const auto info = o.GetTensorTypeAndShapeInfo();
            const size_t count = info.GetElementCount();
            if (count >= 468 * 3) {
                rawLandmarks = o.GetTensorData<float>();
                landmarkFloats = count;
            } else if (count == 1 && !presenceFound
                       && info.GetShape().size() == 4) {
                presenceLogit = o.GetTensorData<float>()[0];
                presenceFound = true;
            }
        }
        if (!rawLandmarks) {
            d->error = QStringLiteral("unexpected face landmark outputs");
            return sample;
        }
        const float presence =
            1.f / (1.f + std::exp(-std::clamp(presenceLogit, -50.f, 50.f)));
        if (presence < kPresenceThreshold) {
            // lost the face: fall back to the detector next frame
            if (d->tracking) {
                d->tracking = false;
                return predict(image, timeSec);
            }
            return sample;
        }

        const int landmarkCount = static_cast<int>(landmarkFloats / 3);
        std::vector<float> pts(rawLandmarks, rawLandmarks + landmarkFloats);
        for (auto& v : pts)
            v /= ls;  // 256-space px -> normalized crop coords (z: /256 then *w)
        FaceCapGeom::projectLandmarks(pts.data(), landmarkCount, 3, rect);

        // --- stage 3: blendshapes (146-subset pixel coords)
        std::vector<float> bsInput(FaceCap::kBlendshapeInputLandmarks * 2);
        for (int i = 0; i < FaceCap::kBlendshapeInputLandmarks; ++i) {
            const int id = FaceCap::kBlendshapeLandmarkSubset[i];
            bsInput[i * 2 + 0] = pts[id * 3 + 0];
            bsInput[i * 2 + 1] = pts[id * 3 + 1];
        }
        const std::array<int64_t, 3> bshape{1, FaceCap::kBlendshapeInputLandmarks, 2};
        Ort::Value binput = Ort::Value::CreateTensor<float>(
            mem, bsInput.data(), bsInput.size(), bshape.data(), bshape.size());
        auto bouts = runSession(d->blendshapes.get(), std::move(binput));
        const float* scores = bouts.front().GetTensorData<float>();
        const size_t scoreCount =
            bouts.front().GetTensorTypeAndShapeInfo().GetElementCount();
        for (size_t i = 0; i < sample.weights.size() && i < scoreCount; ++i)
            sample.weights[i] = scores[i];

        // --- head pose (weighted Kabsch vs the canonical face model)
        const FaceCapPose::Result pose =
            FaceCapPose::solveHeadPose(pts.data(), landmarkCount);
        if (pose.ok) {
            sample.headRotation = pose.rotation;
            sample.headTranslation = pose.translation;
        }
        sample.confidence = presence;

        // --- next-frame ROI from these landmarks (detector-skip)
        d->trackedRect =
            FaceCapGeom::rectFromFaceLandmarks(pts.data(), landmarkCount, W, H);
        d->tracking = d->trackedRect.w > 4.f && d->trackedRect.h > 4.f;
        return sample;
    } catch (const Ort::Exception& e) {
        d->error = QStringLiteral("face capture inference failed: %1")
                       .arg(QString::fromUtf8(e.what()));
        d->tracking = false;
        return sample;
    }
}

#endif  // ENABLE_ONNX
#endif  // ENABLE_MOCAP
