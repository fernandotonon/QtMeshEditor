#include "FaceLandmarkDetector.h"

#include "ArkitTemplate.h"        // reuse its model dir + base-url convention
#include "../ModelDownloader.h"
#include "../OnnxRuntimeSettings.h"
#include "../SentryReporter.h"

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

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.face_rig"),
        QStringLiteral("face landmark model download start"));

    QEventLoop loop;
    bool ok = false, timedOut = false, done = false;
    auto onDone = QObject::connect(dl, &ModelDownloader::downloadCompleted, &loop,
        [&](const QString& name, const QString&) {
            if (name == label) { ok = true; done = true; loop.quit(); }
        });
    auto onErr = QObject::connect(dl, &ModelDownloader::downloadError, &loop,
        [&](const QString& name, const QString&) {
            if (name == label) { ok = false; done = true; loop.quit(); }
        });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop,
                     [&]() { timedOut = true; loop.quit(); });
    timeout.start(120000);  // 2 min — the model is small (~3 MB)

    dl->startDownload(url, dest, label);
    // done-guard: a synchronous downloadError would otherwise block the
    // loop for the full timeout (same pattern as ArkitTemplate).
    if (!done)
        loop.exec();

    QObject::disconnect(onDone);
    QObject::disconnect(onErr);
    if (timedOut && dl)
        dl->cancelDownload();

    const bool success = ok && !timedOut && QFileInfo::exists(dest);
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.face_rig"),
        success ? QStringLiteral("face landmark model download ok")
                : QStringLiteral("face landmark model download failed%1")
                      .arg(timedOut ? QStringLiteral(" (timeout)") : QString()));
    return success ? dest : QString();
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
        OnnxRuntimeSettings::configureSessionOptions(so);
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

    const int W = image.width(), H = image.height();
    const int side0 = std::min(W, H);

    // The landmark graph is trained on tight, detector-cropped FACES. Measured
    // on our own renders: presence logit -27 full-frame, -8.6 with the head
    // filling the frame, +19.8 on a chin-to-forehead crop — so candidate crops
    // must reach face-tightness before the model reports a face at all.
    //
    // Our renders put the subject on a black background, so a silhouette scan
    // finds the head deterministically: bbox of non-black pixels, head width
    // measured over the TOP part of the blob (shoulders are wider), then a few
    // face-square candidates around it. A non-black background degrades to the
    // centre-square fallback candidate.
    struct Cand { int ox, oy, side; };
    std::vector<Cand> cands;
    {
        const QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
        int top = H, bot = -1;
        std::vector<int> rowMin(size_t(H), W), rowMax(size_t(H), -1);
        for (int y = 0; y < H; ++y) {
            const uchar* ln = gray.constScanLine(y);
            for (int x = 0; x < W; ++x) {
                if (ln[x] > 12) {
                    top = std::min(top, y); bot = std::max(bot, y);
                    rowMin[size_t(y)] = std::min(rowMin[size_t(y)], x);
                    rowMax[size_t(y)] = std::max(rowMax[size_t(y)], x);
                }
            }
        }
        if (bot > top + 16) {
            // head width = median row span over the top 35% of the blob
            const int headRows = std::max(8, (bot - top) * 35 / 100);
            int wMin = W, wMax = -1;
            long cxSum = 0; int cxN = 0;
            for (int y = top + headRows / 4; y < top + headRows; ++y) {
                if (rowMax[size_t(y)] < 0) continue;
                wMin = std::min(wMin, rowMin[size_t(y)]);
                wMax = std::max(wMax, rowMax[size_t(y)]);
                cxSum += (rowMin[size_t(y)] + rowMax[size_t(y)]) / 2; ++cxN;
            }
            if (wMax > wMin + 16 && cxN > 0) {
                const int headW = wMax - wMin;
                const int cx = int(cxSum / cxN);
                // face square candidates: the face sits in the lower-middle of
                // the head — try a few sizes/centres around it.
                for (float s : {0.9f, 1.15f, 1.45f}) {
                    int side = std::min(int(headW * s), std::min(W, H));
                    if (side < 32) continue;
                    const int cyFace = top + int(side * 0.62f);
                    cands.push_back({
                        std::clamp(cx - side / 2, 0, W - side),
                        std::clamp(cyFace - side / 2, 0, H - side),
                        side });
                }
            }
        }
    }
    cands.push_back({ (W - side0) / 2, (H - side0) / 2, side0 });  // fallback

    LandmarkResult best;
    for (const Cand& c : cands) {
        LandmarkResult pr = runPass(image, c.ox, c.oy, c.side);
        if (pr.ok && !pr.points.empty()
            && pr.presenceLogit > best.presenceLogit) {
            best = std::move(pr);
        }
    }
    if (!best.ok || best.points.empty()) return best;

    // Refine: tight re-crop around the winning landmark bbox (×1.6, the
    // expansion MediaPipe's own face detector applies).
    float mnx = best.points[0][0], mxx = mnx;
    float mny = best.points[0][1], mxy = mny;
    for (const auto& p : best.points) {
        mnx = std::min(mnx, p[0]); mxx = std::max(mxx, p[0]);
        mny = std::min(mny, p[1]); mxy = std::max(mxy, p[1]);
    }
    const float cx = (mnx + mxx) * 0.5f, cy = (mny + mxy) * 0.5f;
    int side1 = int(std::max(mxx - mnx, mxy - mny) * 1.6f);
    if (side1 >= 32) {
        side1 = std::min(side1, std::min(W, H));
        const int ox1 = std::clamp(int(cx - side1 * 0.5f), 0, W - side1);
        const int oy1 = std::clamp(int(cy - side1 * 0.5f), 0, H - side1);
        LandmarkResult pass2 = runPass(image, ox1, oy1, side1);
        if (pass2.ok && !pass2.points.empty()
            && pass2.presenceLogit >= best.presenceLogit)
            return pass2;
    }
    return best;
}

LandmarkResult FaceLandmarkDetector::runPass(const QImage& image,
                                             int ox, int oy, int side)
{
    LandmarkResult r;
    if (!isAvailable() || image.isNull() || side <= 0) return r;
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
        r.presenceLogit = presenceFound ? presenceLogit : 0.f;

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
