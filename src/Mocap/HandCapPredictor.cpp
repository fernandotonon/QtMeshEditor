#ifdef ENABLE_MOCAP

#include "HandCapPredictor.h"

#include "FaceCapGeom.h"
#include "../ModelDownloader.h"
#include "../OnnxRuntimeSettings.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#include "AppStorage.h"
#endif

namespace {

constexpr const char* kLandmarksFile = "hand_landmarks.onnx";
constexpr const char* kDetectorFile = "hand_detector.onnx";
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/mocap/";
constexpr const char* kUnityHandBaseUrl =
    "https://huggingface.co/unity/inference-engine-blaze-hand/resolve/main/models/";
constexpr const char* kBaseUrlSettingsKey = "ai/mocapModelBaseUrl";
constexpr float kPresenceThreshold = 0.40f;

float stableSigmoid(float x)
{
    x = std::clamp(x, -50.f, 50.f);
    return x >= 0.f ? 1.f / (1.f + std::exp(-x))
                    : std::exp(x) / (1.f + std::exp(x));
}

float asUnitInterval(float v)
{
    if (v < 0.f || v > 1.2f)
        return stableSigmoid(v);
    return std::clamp(v, 0.f, 1.f);
}

bool downloadFileBlocking(const QStringList& urls, const QString& dest,
                          const QString& label)
{
    auto* dl = ModelDownloader::instance();
    if (!dl)
        return false;
    QDir().mkpath(QFileInfo(dest).absolutePath());
    for (const QString& url : urls) {
        if (url.isEmpty())
            continue;
        QEventLoop loop;
        bool ok = false, timedOut = false;
        auto onDone = QObject::connect(
            dl, &ModelDownloader::downloadCompleted, &loop,
            [&](const QString& name, const QString&) {
                if (name == label) {
                    ok = true;
                    loop.quit();
                }
            });
        auto onErr = QObject::connect(
            dl, &ModelDownloader::downloadError, &loop,
            [&](const QString& name, const QString&) {
                if (name == label) {
                    ok = false;
                    loop.quit();
                }
            });
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
            timedOut = true;
            loop.quit();
        });
        timeout.start(180000);
        dl->startDownload(url, dest, label);
        loop.exec();
        QObject::disconnect(onDone);
        QObject::disconnect(onErr);
        if (timedOut && dl)
            dl->cancelDownload();
        if (ok && !timedOut && QFileInfo::exists(dest)
            && QFileInfo(dest).size() > 1024)
            return true;
        QFile::remove(dest);
    }
    return false;
}

void flipNhwcHorizontal(float* nhwc, int size)
{
    for (int v = 0; v < size; ++v) {
        for (int u = 0; u < size / 2; ++u) {
            const int a = (v * size + u) * 3;
            const int b = (v * size + (size - 1 - u)) * 3;
            for (int c = 0; c < 3; ++c)
                std::swap(nhwc[a + c], nhwc[b + c]);
        }
    }
}

}  // namespace

QString HandCapPredictor::modelDir()
{
    return QDir(AppStorage::aiModelsRoot()).filePath(QStringLiteral("mocap/hands"));
}

bool HandCapPredictor::modelsPresent()
{
    return QFileInfo::exists(
        QDir(modelDir()).filePath(QLatin1String(kLandmarksFile)));
}

QString HandCapPredictor::ensureModelsBlocking()
{
#ifndef ENABLE_ONNX
    return {};
#else
    const QDir dir(modelDir());
    const QString lmkDest = dir.filePath(QLatin1String(kLandmarksFile));
    const QString detDest = dir.filePath(QLatin1String(kDetectorFile));
    const bool haveLmk = QFileInfo::exists(lmkDest);
    const bool haveDet = QFileInfo::exists(detDest);
    if (haveLmk && haveDet)
        return modelDir();
    if (!qEnvironmentVariableIsEmpty("QTMESH_MOCAP_NO_DOWNLOAD"))
        return haveLmk ? modelDir() : QString{};

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
    if (!base.isEmpty() && !base.endsWith('/'))
        base += '/';

    QStringList lmkUrls;
    if (!base.isEmpty())
        lmkUrls << (base + QLatin1String("hands/") + QLatin1String(kLandmarksFile));
    lmkUrls << (QString::fromLatin1(kUnityHandBaseUrl)
                + QLatin1String("hand_landmarks_detector.onnx"));

    QStringList detUrls;
    if (!base.isEmpty())
        detUrls << (base + QLatin1String("hands/") + QLatin1String(kDetectorFile));
    detUrls << (QString::fromLatin1(kUnityHandBaseUrl)
                + QLatin1String("hand_detector.onnx"));

    if (!QFileInfo::exists(lmkDest)
        && !downloadFileBlocking(
            lmkUrls, lmkDest,
            QStringLiteral("Hand capture model (hand_landmarks.onnx)")))
        return {};
    if (!QFileInfo::exists(detDest)) {
        downloadFileBlocking(
            detUrls, detDest,
            QStringLiteral("Hand palm detector (hand_detector.onnx)"));
    }
    return QFileInfo::exists(lmkDest) ? modelDir() : QString{};
#endif
}

#ifndef ENABLE_ONNX

struct HandCapPredictor::Impl {
    QString error = QStringLiteral(
        "hand capture predictor unavailable (built without ENABLE_ONNX)");
};

HandCapPredictor::HandCapPredictor() : d(new Impl) {}
HandCapPredictor::~HandCapPredictor() = default;
bool HandCapPredictor::load(const QString&) { return false; }
bool HandCapPredictor::isAvailable() const { return false; }
QString HandCapPredictor::lastError() const { return d->error; }
HandsLiveFrame HandCapPredictor::predict(const QImage&, const float*,
                                         const float*, double)
{
    return {};
}

#else  // ENABLE_ONNX

struct HandCapPredictor::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "qtmesh_handcap"};
    std::unique_ptr<Ort::Session> detector;
    std::unique_ptr<Ort::Session> landmarks;
    int detectorSize = 192;
    int landmarksSize = 224;
    QString error;
    bool available = false;
    std::vector<float> detInput;
    std::vector<float> lmkInput;
    std::vector<std::array<float, 2>> anchors;
    HandLandmarks prevLeft;
    HandLandmarks prevRight;

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

HandCapPredictor::HandCapPredictor() : d(new Impl) {}
HandCapPredictor::~HandCapPredictor() = default;

bool HandCapPredictor::load(const QString& dirIn)
{
    const QDir dir(dirIn.isEmpty() ? modelDir() : dirIn);
    const QString lmk = dir.filePath(QLatin1String(kLandmarksFile));
    if (!QFileInfo::exists(lmk)) {
        d->available = false;
        d->error = QStringLiteral(
            "hand capture model not found in %1 — they download on first "
            "use, or set QTMESH_MOCAP_MODEL_BASE_URL")
                       .arg(dir.absolutePath());
        return false;
    }
    try {
        d->landmarks = d->openSession(lmk);
        {
            const auto info = d->landmarks->GetInputTypeInfo(0);
            const auto shape = info.GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() == 4 && shape[1] > 3)
                d->landmarksSize = static_cast<int>(shape[1]);
            else if (shape.size() == 4 && shape[2] > 3)
                d->landmarksSize = static_cast<int>(shape[2]);
        }
        d->lmkInput.resize(static_cast<size_t>(d->landmarksSize)
                           * d->landmarksSize * 3);
        d->available = true;
        d->error.clear();
    } catch (const Ort::Exception& e) {
        d->error = QStringLiteral("failed to load hand capture model: %1")
                       .arg(QString::fromUtf8(e.what()));
        d->available = false;
        return false;
    }

    const QString det = dir.filePath(QLatin1String(kDetectorFile));
    if (QFileInfo::exists(det)) {
        try {
            d->detector = d->openSession(det);
            const auto info = d->detector->GetInputTypeInfo(0);
            const auto shape = info.GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() == 4 && shape[1] > 3)
                d->detectorSize = static_cast<int>(shape[1]);
            else if (shape.size() == 4 && shape[2] > 3)
                d->detectorSize = static_cast<int>(shape[2]);
            d->anchors =
                FaceCapGeom::genSsdAnchors(d->detectorSize, {8, 16, 16, 16});
            d->detInput.resize(static_cast<size_t>(d->detectorSize)
                               * d->detectorSize * 3);
        } catch (const Ort::Exception& e) {
            d->detector.reset();
            d->anchors.clear();
            d->error = QStringLiteral(
                "hand palm detector unavailable (%1) — using pose-seeded "
                "hand crops")
                           .arg(QString::fromUtf8(e.what()));
        }
    }
    return d->available;
}

bool HandCapPredictor::isAvailable() const { return d->available; }
QString HandCapPredictor::lastError() const { return d->error; }

HandsLiveFrame HandCapPredictor::predict(const QImage& image,
                                         const float* poseImageXy,
                                         const float* poseVis, double timeSec)
{
    HandsLiveFrame out;
    out.timeSec = timeSec;
    if (!d->available)
        return out;
    QImage rgb = image.format() == QImage::Format_RGB888
                     ? image
                     : image.convertToFormat(QImage::Format_RGB888);
    const int W = rgb.width();
    const int H = rgb.height();
    if (W <= 0 || H <= 0)
        return out;

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
                outHolders.push_back(
                    session->GetOutputNameAllocated(i, alloc));
                outNames.push_back(outHolders.back().get());
            }
            const char* in = inName.get();
            return session->Run(Ort::RunOptions{nullptr}, &in, &input, 1,
                                outNames.data(), outNames.size());
        };

        auto runLandmarks = [&](const FaceCapGeom::RotatedRect& rect,
                                bool flipLeft,
                                HandLandmarks& hand) -> bool {
            if (rect.w < 16.f || rect.h < 16.f)
                return false;
            const int ls = d->landmarksSize;
            FaceCapGeom::cropRotatedRectToTensor(rgb, rect, ls, 0.f, 1.f,
                                                 d->lmkInput.data());
            if (flipLeft)
                flipNhwcHorizontal(d->lmkInput.data(), ls);
            const std::array<int64_t, 4> lshape{1, ls, ls, 3};
            Ort::Value linput = Ort::Value::CreateTensor<float>(
                mem, d->lmkInput.data(), d->lmkInput.size(), lshape.data(),
                lshape.size());
            auto louts = runSession(d->landmarks.get(), std::move(linput));

            const float* rawLmk = nullptr;
            const float* rawWorld = nullptr;
            std::vector<float> scalars;
            for (auto& o : louts) {
                const auto info = o.GetTensorTypeAndShapeInfo();
                const size_t count = info.GetElementCount();
                if (count == 21 * 3) {
                    if (!rawLmk)
                        rawLmk = o.GetTensorData<float>();
                    else if (!rawWorld)
                        rawWorld = o.GetTensorData<float>();
                } else if (count == 1)
                    scalars.push_back(o.GetTensorData<float>()[0]);
            }
            if (!rawLmk)
                return false;
            const float presence =
                scalars.empty() ? 1.f : asUnitInterval(scalars.front());
            if (presence < kPresenceThreshold)
                return false;

            hand = {};
            hand.valid = true;
            hand.presence = presence;
            float cropPts[21 * 3];
            for (int i = 0; i < 21; ++i) {
                float x = rawLmk[i * 3 + 0] / static_cast<float>(ls);
                float y = rawLmk[i * 3 + 1] / static_cast<float>(ls);
                float z = rawLmk[i * 3 + 2] / static_cast<float>(ls);
                if (flipLeft)
                    x = 1.f - x;
                cropPts[i * 3 + 0] = x;
                cropPts[i * 3 + 1] = y;
                cropPts[i * 3 + 2] = z;
                hand.cropXyz[static_cast<size_t>(i * 3 + 0)] = x;
                hand.cropXyz[static_cast<size_t>(i * 3 + 1)] = y;
                hand.cropXyz[static_cast<size_t>(i * 3 + 2)] = z;
                if (rawWorld) {
                    float wx = rawWorld[i * 3 + 0];
                    const float wy = rawWorld[i * 3 + 1];
                    const float wz = rawWorld[i * 3 + 2];
                    if (flipLeft)
                        wx = -wx;
                    hand.worldXyz[static_cast<size_t>(i * 3 + 0)] = wx;
                    hand.worldXyz[static_cast<size_t>(i * 3 + 1)] = wy;
                    hand.worldXyz[static_cast<size_t>(i * 3 + 2)] = wz;
                }
            }
            FaceCapGeom::projectLandmarks(cropPts, 21, 3, rect);
            for (int i = 0; i < 21; ++i) {
                hand.imageXy[static_cast<size_t>(i * 2 + 0)] = cropPts[i * 3 + 0];
                hand.imageXy[static_cast<size_t>(i * 2 + 1)] = cropPts[i * 3 + 1];
            }
            return true;
        };

        auto tryRect = [&](const FaceCapGeom::RotatedRect& rect, bool preferLeft,
                           HandLandmarks& hand) -> bool {
            if (runLandmarks(rect, preferLeft, hand))
                return true;
            return runLandmarks(rect, !preferLeft, hand);
        };

        struct Candidate {
            FaceCapGeom::RotatedRect rect;
            bool preferLeft = false;
        };
        std::vector<Candidate> crops;

        auto addIfNew = [&](const FaceCapGeom::RotatedRect& rect, bool left) {
            if (rect.w < 16.f)
                return;
            for (const auto& c : crops) {
                const float dx = c.rect.cx - rect.cx;
                const float dy = c.rect.cy - rect.cy;
                if (dx * dx + dy * dy < 20.f * 20.f)
                    return;
            }
            crops.push_back({rect, left});
        };

        if (d->prevRight.valid)
            addIfNew(FaceCapGeom::rectFromHandLandmarks(
                         d->prevRight.imageXy.data(), W, H),
                     false);
        if (d->prevLeft.valid)
            addIfNew(FaceCapGeom::rectFromHandLandmarks(
                         d->prevLeft.imageXy.data(), W, H),
                     true);

        if (d->detector) {
            const int ds = d->detectorSize;
            FaceCapGeom::Letterbox lb = FaceCapGeom::letterboxToTensor(
                rgb, ds, 0.f, 1.f, d->detInput.data());
            const std::array<int64_t, 4> shape{1, ds, ds, 3};
            Ort::Value input = Ort::Value::CreateTensor<float>(
                mem, d->detInput.data(), d->detInput.size(), shape.data(),
                shape.size());
            auto outs = runSession(d->detector.get(), std::move(input));
            const float* rawBoxes = nullptr;
            const float* rawScores = nullptr;
            size_t boxElems = 0, scoreElems = 0;
            for (auto& o : outs) {
                const auto info = o.GetTensorTypeAndShapeInfo();
                const auto s = info.GetShape();
                const size_t n = info.GetElementCount();
                if (s.size() == 3 && s[2] > 1) {
                    rawBoxes = o.GetTensorData<float>();
                    boxElems = n;
                } else if (s.size() == 3 && s[2] == 1) {
                    rawScores = o.GetTensorData<float>();
                    scoreElems = n;
                }
            }
            if (rawBoxes && rawScores && !d->anchors.empty()
                && boxElems >= d->anchors.size() * 18u
                && scoreElems >= d->anchors.size()) {
                auto decodePalm = [&](bool reverse) {
                    return FaceCapGeom::decodeDetections(
                        rawBoxes, rawScores, d->anchors, ds, 7, 0.5f, 0.3f,
                        reverse);
                };
                auto dets = decodePalm(true);
                if (dets.empty())
                    dets = decodePalm(false);
                auto unletter = [&](FaceCapGeom::Detection& det) {
                    FaceCapGeom::unletterbox(lb, det.box[0], det.box[1]);
                    if (lb.fracX > 1e-6f)
                        det.box[2] /= lb.fracX;
                    if (lb.fracY > 1e-6f)
                        det.box[3] /= lb.fracY;
                    for (auto& kp : det.keypoints)
                        FaceCapGeom::unletterbox(lb, kp[0], kp[1]);
                };
                for (auto& det : dets) {
                    unletter(det);
                    const FaceCapGeom::RotatedRect rect =
                        FaceCapGeom::rectFromPalmDetection(det, W, H);
                    bool left = false;
                    if (poseImageXy) {
                        const float pcx = (det.box[0] + det.box[2] * 0.5f) * W;
                        const float pcy = (det.box[1] + det.box[3] * 0.5f) * H;
                        const float lx = poseImageXy[15 * 2 + 0];
                        const float ly = poseImageXy[15 * 2 + 1];
                        const float rx = poseImageXy[16 * 2 + 0];
                        const float ry = poseImageXy[16 * 2 + 1];
                        const float dl = (pcx - lx) * (pcx - lx)
                                         + (pcy - ly) * (pcy - ly);
                        const float dr = (pcx - rx) * (pcx - rx)
                                         + (pcy - ry) * (pcy - ry);
                        left = dl < dr;
                    }
                    addIfNew(rect, left);
                }
            }
        }

        if (poseImageXy) {
            struct Spec {
                HandLandmarks* dst;
                int wrist, index, pinky;
                bool left;
            };
            const Spec specs[] = {
                {&out.right, 16, 20, 18, false},
                {&out.left, 15, 19, 17, true},
            };
            for (const Spec& spec : specs) {
                if (poseVis && (poseVis[spec.wrist] < 0.25f
                                || poseVis[spec.index] < 0.15f
                                || poseVis[spec.pinky] < 0.15f))
                    continue;
                const float wx = poseImageXy[spec.wrist * 2 + 0];
                const float wy = poseImageXy[spec.wrist * 2 + 1];
                const float ix = poseImageXy[spec.index * 2 + 0];
                const float iy = poseImageXy[spec.index * 2 + 1];
                const float px = poseImageXy[spec.pinky * 2 + 0];
                const float py = poseImageXy[spec.pinky * 2 + 1];
                addIfNew(FaceCapGeom::rectFromPoseHand(wx, wy, ix, iy, px, py),
                         spec.left);
            }
        }

        for (const Candidate& c : crops) {
            HandLandmarks hand;
            if (!tryRect(c.rect, c.preferLeft, hand))
                continue;
            HandLandmarks& dst = c.preferLeft ? out.left : out.right;
            if (!dst.valid || hand.presence > dst.presence)
                dst = hand;
        }

        // Pose-seeded leftover side if detector only found one hand.
        if (poseImageXy) {
            struct Spec {
                HandLandmarks* dst;
                int wrist, index, pinky;
                bool left;
            };
            const Spec specs[] = {
                {&out.right, 16, 20, 18, false},
                {&out.left, 15, 19, 17, true},
            };
            for (const Spec& spec : specs) {
                if (spec.dst->valid)
                    continue;
                if (poseVis && poseVis[spec.wrist] < 0.25f)
                    continue;
                const FaceCapGeom::RotatedRect rect = FaceCapGeom::rectFromPoseHand(
                    poseImageXy[spec.wrist * 2 + 0],
                    poseImageXy[spec.wrist * 2 + 1],
                    poseImageXy[spec.index * 2 + 0],
                    poseImageXy[spec.index * 2 + 1],
                    poseImageXy[spec.pinky * 2 + 0],
                    poseImageXy[spec.pinky * 2 + 1]);
                tryRect(rect, spec.left, *spec.dst);
            }
        }

        d->prevLeft = out.left;
        d->prevRight = out.right;
        return out;
    } catch (const Ort::Exception& e) {
        d->error = QStringLiteral("hand capture inference failed: %1")
                       .arg(QString::fromUtf8(e.what()));
        d->prevLeft = {};
        d->prevRight = {};
        return {};
    }
}

#endif  // ENABLE_ONNX
#endif  // ENABLE_MOCAP
