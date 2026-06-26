#include "MotionInbetween.h"

#include "ModelDownloader.h"

#include <QByteArray>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
// Hosted alongside the other QtMeshEditor ONNX models (#404/#408) — the RMIB
// export lands under the inbetween/ subdir of the same HF models repo.
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/inbetween/";
constexpr const char* kBaseUrlSettingsKey = "ai/inbetweenModelBaseUrl";
} // namespace

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#include <array>
#include <unordered_map>
#endif

// Out-of-line ctor (same idiom as AutoRig::Options / UniRigPredictor::Options):
// keeps the `{}` default arg on predict()/interpolateSpline() from forcing
// aggregate init of the nested struct while MotionInbetween is incomplete.
MotionInbetween::Options::Options() = default;

bool MotionInbetween::isModelBackendAvailable()
{
#ifdef ENABLE_ONNX
    return true;
#else
    return false;
#endif
}

QString MotionInbetween::modelPath()
{
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath(QStringLiteral("ai_models/inbetween/rmib.onnx"));
}

bool MotionInbetween::modelPresent()
{
    return QFileInfo::exists(modelPath());
}

QString MotionInbetween::ensureModelBlocking()
{
#ifndef ENABLE_ONNX
    // A non-ONNX build can't run the model — don't touch disk/network. The
    // spline fallback is always used in this configuration.
    return {};
#else
    const QString dest = modelPath();
    if (QFileInfo::exists(dest)) return dest;

    // Offline guard (tests/CI set this so first-run never hits the network).
    if (!qEnvironmentVariableIsEmpty("QTMESH_INBETWEEN_NO_DOWNLOAD"))
        return {};

    // Base URL: QSettings override > env override > default HF repo.
    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_INBETWEEN_MODEL_BASE_URL");
            base = env.isEmpty() ? QString::fromLatin1(kDefaultModelBaseUrl)
                                 : QString::fromUtf8(env);
        }
    }
    if (base.isEmpty()) return {};
    if (!base.endsWith('/')) base += '/';

    auto* dl = ModelDownloader::instance();
    if (!dl) return {};

    QDir().mkpath(QFileInfo(dest).absolutePath());
    const QString url = base + QStringLiteral("rmib.onnx");
    const QString label = QStringLiteral("RMIB in-betweening model");

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
    timeout.start(300000);  // 5 min — RMIB is ~10 MB but allow slow links

    dl->startDownload(url, dest, label);
    loop.exec();

    QObject::disconnect(onDone);
    QObject::disconnect(onErr);
    if (timedOut && dl) dl->cancelDownload();

    return (ok && !timedOut && QFileInfo::exists(dest)) ? dest : QString();
#endif
}

// ---------------------------------------------------------------------------
// Pure-data helpers
// ---------------------------------------------------------------------------

std::array<float, 4> MotionInbetween::slerpQuat(const std::array<float, 4>& a,
                                                const std::array<float, 4>& b,
                                                float t)
{
    // a, b are [x,y,z,w]. Shortest-arc: flip b if the dot is negative so we
    // interpolate over the short way round.
    double ax = a[0], ay = a[1], az = a[2], aw = a[3];
    double bx = b[0], by = b[1], bz = b[2], bw = b[3];
    double dot = ax*bx + ay*by + az*bz + aw*bw;
    if (dot < 0.0) { bx = -bx; by = -by; bz = -bz; bw = -bw; dot = -dot; }

    double s0, s1;
    if (dot > 0.9995) {
        // Nearly parallel — fall back to normalised lerp to avoid div-by-~0.
        s0 = 1.0 - t;
        s1 = t;
    } else {
        const double theta0 = std::acos(std::clamp(dot, -1.0, 1.0));
        const double sinTheta0 = std::sin(theta0);
        const double theta = theta0 * t;
        s1 = std::sin(theta) / sinTheta0;
        s0 = std::cos(theta) - dot * s1;
    }
    double rx = s0*ax + s1*bx;
    double ry = s0*ay + s1*by;
    double rz = s0*az + s1*bz;
    double rw = s0*aw + s1*bw;
    const double n = std::sqrt(rx*rx + ry*ry + rz*rz + rw*rw);
    if (n > 1e-12) { rx/=n; ry/=n; rz/=n; rw/=n; }
    else { rx=0; ry=0; rz=0; rw=1; }
    return { static_cast<float>(rx), static_cast<float>(ry),
             static_cast<float>(rz), static_cast<float>(rw) };
}

float MotionInbetween::hermite(float p0, float p1, float m0, float m1, float t)
{
    // Cubic Hermite basis.
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2*t3 - 3*t2 + 1;
    const float h10 = t3 - 2*t2 + t;
    const float h01 = -2*t3 + 3*t2;
    const float h11 = t3 - t2;
    return h00*p0 + h10*m0 + h01*p1 + h11*m1;
}

// ---------------------------------------------------------------------------
// Spline fallback (always available)
// ---------------------------------------------------------------------------

MotionInbetween::Result MotionInbetween::interpolateSpline(
    const Pose& start, const Pose& end, const std::vector<Channel>& layout,
    const Options& opts, const Pose* preStart, const Pose* postEnd)
{
    Result r;
    const size_t C = layout.size();
    if (start.size() != C || end.size() != C) {
        r.error = QStringLiteral(
            "MotionInbetween: pose/layout channel-count mismatch (start=%1 end=%2 layout=%3)")
            .arg(start.size()).arg(end.size()).arg(C);
        return r;
    }
    const int gap = std::max(0, opts.gapFrames);
    r.frames.reserve(static_cast<size_t>(gap));

    // For scalar channels we use Catmull-Rom tangents: m = (p_{next} - p_{prev})/2.
    // Endpoints' outer neighbours come from preStart/postEnd when provided, else
    // we use the secant (start→end) so the curve is at least C1 at the segment
    // ends and never overshoots more than a Catmull-Rom would.
    auto neighbourOk = [&](const Pose* p) {
        return p != nullptr && p->size() == C;
    };
    const bool havePre  = neighbourOk(preStart);
    const bool havePost = neighbourOk(postEnd);

    for (int f = 0; f < gap; ++f) {
        // Parametric position in (0,1), excluding the endpoints themselves.
        const float t = static_cast<float>(f + 1) / static_cast<float>(gap + 1);
        Pose pose(C, 0.0f);

        for (size_t c = 0; c < C; ++c) {
            if (layout[c] == Channel::QuatCont) {
                continue;  // written by its QuatStart below
            }
            if (layout[c] == Channel::QuatStart) {
                // Pull the 4-wide block [c..c+3] and slerp it as a unit.
                std::array<float,4> qa{ start[c], (c+1<C?start[c+1]:0.f),
                                        (c+2<C?start[c+2]:0.f), (c+3<C?start[c+3]:1.f) };
                std::array<float,4> qb{ end[c], (c+1<C?end[c+1]:0.f),
                                        (c+2<C?end[c+2]:0.f), (c+3<C?end[c+3]:1.f) };
                const std::array<float,4> q = slerpQuat(qa, qb, t);
                pose[c] = q[0];
                if (c+1 < C) pose[c+1] = q[1];
                if (c+2 < C) pose[c+2] = q[2];
                if (c+3 < C) pose[c+3] = q[3];
                continue;
            }
            // Scalar: cubic-Hermite with Catmull-Rom tangents.
            const float p0 = start[c];
            const float p1 = end[c];
            const float prev = havePre  ? (*preStart)[c] : p0;  // secant if absent
            const float next = havePost ? (*postEnd)[c]  : p1;
            const float m0 = 0.5f * (p1 - prev);   // (next_of_p0 - prev_of_p0)/2
            const float m1 = 0.5f * (next - p0);   // (next_of_p1 - prev_of_p1)/2
            pose[c] = hermite(p0, p1, m0, m1, t);
        }
        r.frames.push_back(std::move(pose));
    }

    r.ok = true;
    r.usedModel = false;
    return r;
}

// ---------------------------------------------------------------------------
// ONNX RMIB path
// ---------------------------------------------------------------------------

#ifndef ENABLE_ONNX

MotionInbetween::Result MotionInbetween::predict(
    const Pose& start, const Pose& end, const std::vector<Channel>& layout,
    const QString& /*modelPath*/, const Options& opts,
    const Pose* preStart, const Pose* postEnd)
{
    // No ONNX in this build — always the deterministic spline.
    Result r = interpolateSpline(start, end, layout, opts, preStart, postEnd);
    if (r.ok) {
        r.fallbackReason = QStringLiteral(
            "AI in-betweening needs an ONNX-enabled build (rebuild with "
            "-DENABLE_ONNX) — used the spline fallback.");
    }
    return r;
}

#else // ENABLE_ONNX

MotionInbetween::Result MotionInbetween::predict(
    const Pose& start, const Pose& end, const std::vector<Channel>& layout,
    const QString& modelPath, const Options& opts,
    const Pose* preStart, const Pose* postEnd)
{
    auto fallback = [&](const QString& why) -> Result {
        Result r = interpolateSpline(start, end, layout, opts, preStart, postEnd);
        if (r.ok) r.fallbackReason = why;
        return r;
    };

    if (opts.forceFallback)
        return fallback(QStringLiteral("Spline fallback forced by request."));
    if (modelPath.isEmpty() || !QFileInfo::exists(modelPath))
        return fallback(QStringLiteral(
            "RMIB model not found at %1 — used the spline fallback.").arg(modelPath));

    const int gap = std::max(0, opts.gapFrames);
    const size_t C = layout.size();
    if (start.size() != C || end.size() != C)
        return fallback(QStringLiteral(
            "Pose/layout channel mismatch — used the spline fallback."));

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_inbetween");
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(1);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef __APPLE__
        try {
            std::unordered_map<std::string, std::string> coremlOpts;
            so.AppendExecutionProvider("CoreML", coremlOpts);
        } catch (const Ort::Exception&) { /* CPU EP fallback */ }
#endif
#ifdef _WIN32
        const std::wstring wpath = modelPath.toStdWString();
        Ort::Session session(env, wpath.c_str(), so);
#else
        const std::string p = modelPath.toStdString();
        Ort::Session session(env, p.c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;

        // RMIB I/O contract (kept deliberately simple + discovered at runtime so
        // a re-export with different names still works): the model takes a
        // [1, 2, C] tensor (start pose, end pose) plus a scalar gap count, and
        // emits a [1, gap, C] tensor of intermediate poses. If the discovered
        // input rank / channel count doesn't match our layout, fall back —
        // RMIB is skeleton-specific and a mismatch means an incompatible rig.
        const size_t inCount = session.GetInputCount();
        if (inCount < 1)
            return fallback(QStringLiteral(
                "RMIB model exposes no inputs — used the spline fallback."));

        Ort::TypeInfo inInfo = session.GetInputTypeInfo(0);
        const auto inShape = inInfo.GetTensorTypeAndShapeInfo().GetShape();
        // Expect [.., 2, C] or [.., C]; the last dim must equal our channel count
        // for the skeleton to be compatible with this model.
        if (!inShape.empty() && inShape.back() > 0 &&
            static_cast<size_t>(inShape.back()) != C) {
            return fallback(QStringLiteral(
                "Skeleton incompatible with the RMIB model (model expects %1 "
                "channels, rig has %2) — used the spline fallback.")
                .arg(inShape.back()).arg(C));
        }

        // Pack [1, 2, C]: start then end.
        std::vector<float> input;
        input.reserve(2 * C);
        input.insert(input.end(), start.begin(), start.end());
        input.insert(input.end(), end.begin(), end.end());
        const std::array<int64_t, 3> shape = { 1, 2, static_cast<int64_t>(C) };

        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            memInfo, input.data(), input.size(), shape.data(), shape.size());

        // Optional second input: a scalar gap count (some exports take it).
        std::vector<int64_t> gapData{ gap };
        const std::array<int64_t, 1> gapShape = { 1 };

        // Hold the allocated name strings for the lifetime of the Run() call.
        // Ort::AllocatedStringPtr is a move-only unique_ptr, so collect via
        // reserve()+push_back rather than default-constructing slots.
        std::vector<Ort::AllocatedStringPtr> inNameHolders;
        inNameHolders.reserve(2);
        inNameHolders.push_back(session.GetInputNameAllocated(0, alloc));
        std::vector<const char*> inNames{ inNameHolders.back().get() };
        std::vector<Ort::Value> inputs;
        inputs.push_back(std::move(inputTensor));

        if (inCount >= 2) {
            inNameHolders.push_back(session.GetInputNameAllocated(1, alloc));
            inNames.push_back(inNameHolders.back().get());
            inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                memInfo, gapData.data(), gapData.size(),
                gapShape.data(), gapShape.size()));
        }

        const size_t outCount = session.GetOutputCount();
        std::vector<Ort::AllocatedStringPtr> outHolders;
        std::vector<const char*> outNames;
        for (size_t i = 0; i < outCount; ++i) {
            outHolders.push_back(session.GetOutputNameAllocated(i, alloc));
            outNames.push_back(outHolders.back().get());
        }

        std::vector<Ort::Value> outputs = session.Run(
            Ort::RunOptions{nullptr}, inNames.data(), inputs.data(), inputs.size(),
            outNames.data(), outNames.size());

        if (outputs.empty() || !outputs[0].IsTensor())
            return fallback(QStringLiteral(
                "RMIB model produced no usable output — used the spline fallback."));

        auto outTI = outputs[0].GetTensorTypeAndShapeInfo();
        const size_t elems = outTI.GetElementCount();
        // Expect gap*C floats (or a leading batch dim). Validate before copying.
        if (elems < static_cast<size_t>(gap) * C)
            return fallback(QStringLiteral(
                "RMIB output too small (%1 < %2) — used the spline fallback.")
                .arg(elems).arg(static_cast<size_t>(gap) * C));

        const float* d = outputs[0].GetTensorData<float>();
        Result r;
        r.frames.reserve(static_cast<size_t>(gap));
        for (int f = 0; f < gap; ++f) {
            Pose pose(d + static_cast<size_t>(f) * C,
                      d + static_cast<size_t>(f + 1) * C);
            r.frames.push_back(std::move(pose));
        }
        r.ok = true;
        r.usedModel = true;
        return r;
    } catch (const Ort::Exception& e) {
        return fallback(QStringLiteral(
            "RMIB inference failed (%1) — used the spline fallback.")
            .arg(QString::fromUtf8(e.what())));
    } catch (const std::exception& e) {
        return fallback(QStringLiteral(
            "RMIB error (%1) — used the spline fallback.")
            .arg(QString::fromUtf8(e.what())));
    }
}

#endif // ENABLE_ONNX
