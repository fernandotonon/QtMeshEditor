#include "SamBodyPredictor.h"
#include "SamBodyHands.h"

#include "AppStorage.h"
#include "ModelFetch.h"
#include "../ImageTo3D/BackgroundRemover.h"
#ifdef ENABLE_ONNX
#include "OnnxRuntimeSettings.h"
#include <onnxruntime_cxx_api.h>
#endif

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <map>
#include <memory>

namespace {

// Pinned to a COMMIT, not `main`: the SHA-256 table below describes exactly these files, and a re-export uploaded to the same
// paths on main would otherwise make every released build fail its digest check. After re-exporting, upload, then update this
// revision AND the digests together.
constexpr const char* kDefaultBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/98f137577e13a2321a84db0a3a665bbe9142b562/sam3dbody/";
constexpr const char* kBaseUrlSettingsKey = "ai/sam3dbodyModelBaseUrl";

struct FileSpec { const char* name; const char* label; const char* sha256; };
// SHA-256 of the files we host (default hosting only; a mirror override may serve a different export).
constexpr FileSpec kFiles[] = {
    {"sam3d_backbone.onnx",      "SAM 3D Body backbone (graph)",   "d92e6a0129c57b02f3330f655fdde88f6331440c92206793400b3b1a086652b7"},
    {"sam3d_backbone.onnx.data", "SAM 3D Body backbone (weights)", "479e7da52d115344a3fd3e7711dae58176f68c723486e80ba2c3efc046f4dbc6"},
    {"sam3d_decoder.onnx",       "SAM 3D Body decoder",            "b47971ea3f1cd7533f4a0d6ef7113db8bfb1e0a4726019d2fe3ebd9801440c47"},
    {"mhr_body.onnx",            "MHR body model",                 "1b5eea8284226dbee296968d89605cceb463f278f5ecdb73f59cba26fed71359"},
    {"mhr_faces.i32",            "MHR body topology",              "f6748e290ef37fbb6877c4cc5bd7287105db9e98252b0ba170ae9ac3c45eacd6"},
    // The SAM License requires a copy of itself to accompany the SAM materials wherever they are distributed.
    {"LICENSE-SAM.txt",          "SAM License",                    "b3a5a0e2d973ab80e6610ccf1cffc40756050d0ace3cd4fec879b3ec290b2e9b"},
    {"LICENSE-MHR.txt",          "MHR (Apache-2.0) licence",       "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"},
};
// Hand refinement (Meta's "full" inference, SamBodyHands.h). OPTIONAL: fetched after the files above, with the same digest
// checks, but a failed download only means body-only fingers, never a failed body capture.
constexpr FileSpec kHandFiles[] = {
    {"sam3d_decoder_v2.onnx",    "SAM 3D Body decoder (hand refinement)", "17877852394fd5a933ac4b017baf609205af1764de4572cccdf21e522b17de3d"},
    {"sam3d_hand_decoder.onnx",  "SAM 3D Body hand decoder",              "51687f413ef69f5026831b8b1cd61b2a308b2982cecbd8bd0b7ef59084923aed"},
    {"sam3d_hand_fuse.onnx",     "SAM 3D Body hand fusion",               "9e8848827b90b13a3854f87ba0380a0ef3fd7cd6bc8c8c18df2bdfc0e37fc743"},
};

// The reference fixes aspect first to 0.75 (person-shaped), then to the 1:1 model input.
void fixAspect(double& w, double& h, double aspect)
{
    if (w > h * aspect) h = w / aspect; else w = h * aspect;
}

} // namespace

namespace SamBody {

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

CropGeometry cropGeometry(int photoW, int photoH, const QRectF& bbox, double padding)
{
    CropGeometry g;
    if (photoW <= 0 || photoH <= 0) return g;
    const double bw = bbox.width(), bh = bbox.height();
    if (!(bw > 0.0) || !(bh > 0.0) || !std::isfinite(bw) || !std::isfinite(bh)
        || !std::isfinite(bbox.x()) || !std::isfinite(bbox.y()))
        return g;

    g.photoW = photoW; g.photoH = photoH;
    g.center = {bbox.x() + bw * 0.5, bbox.y() + bh * 0.5};
    double w = bw * padding, h = bh * padding;
    fixAspect(w, h, 0.75);
    fixAspect(w, h, 1.0);
    g.scale = w;                                   // square: w == h

    const double a = static_cast<double>(kCropSize) / g.scale;
    g.affine = {static_cast<float>(a), 0.0f, static_cast<float>(kCropSize * 0.5 - a * g.center[0]),
                0.0f, static_cast<float>(a), static_cast<float>(kCropSize * 0.5 - a * g.center[1])};

    g.focal = static_cast<float>(std::sqrt(static_cast<double>(photoW) * photoW + static_cast<double>(photoH) * photoH));
    g.principal = {photoW * 0.5f, photoH * 0.5f};
    g.cond = {static_cast<float>((g.center[0] - g.principal[0]) / g.focal),
              static_cast<float>((g.center[1] - g.principal[1]) / g.focal),
              static_cast<float>(g.scale / g.focal)};
    g.valid = true;
    return g;
}

std::vector<float> warpCrop(const QImage& photo, const CropGeometry& geo)
{
    const size_t plane = static_cast<size_t>(kCropSize) * kCropSize;
    std::vector<float> out(plane * 3, 0.0f);
    if (photo.isNull() || !geo.valid) return out;

    const QImage src = photo.convertToFormat(QImage::Format_RGB888);
    const int W = src.width(), H = src.height();
    const double a = geo.affine[0], tx = geo.affine[2], ty = geo.affine[5];

    auto texel = [&](int x, int y, int c) -> float {
        if (x < 0 || y < 0 || x >= W || y >= H) return 0.0f;   // cv2 BORDER_CONSTANT 0
        return static_cast<float>(src.constScanLine(y)[x * 3 + c]);
    };

    for (int v = 0; v < kCropSize; ++v) {
        const double sy = (v - ty) / a;
        const int y0 = static_cast<int>(std::floor(sy));
        const float fy = static_cast<float>(sy - y0);
        for (int u = 0; u < kCropSize; ++u) {
            const double sx = (u - tx) / a;
            const int x0 = static_cast<int>(std::floor(sx));
            const float fx = static_cast<float>(sx - x0);
            for (int c = 0; c < 3; ++c) {
                const float top = texel(x0, y0, c) * (1.0f - fx) + texel(x0 + 1, y0, c) * fx;
                const float bot = texel(x0, y0 + 1, c) * (1.0f - fx) + texel(x0 + 1, y0 + 1, c) * fx;
                out[c * plane + static_cast<size_t>(v) * kCropSize + u] = (top * (1.0f - fy) + bot * fy) / 255.0f;
            }
        }
    }
    return out;
}

std::vector<float> rayCondition(const CropGeometry& geo)
{
    const size_t plane = static_cast<size_t>(kCropSize) * kCropSize;
    std::vector<float> out(plane * 2, 0.0f);
    if (!geo.valid) return out;
    const double a = geo.affine[0], tx = geo.affine[2], ty = geo.affine[5];
    for (int y = 0; y < kCropSize; ++y)
        for (int x = 0; x < kCropSize; ++x) {
            const size_t i = static_cast<size_t>(y) * kCropSize + x;
            out[i]         = static_cast<float>(((x - tx) / a - geo.principal[0]) / geo.focal);
            out[plane + i] = static_cast<float>(((y - ty) / a - geo.principal[1]) / geo.focal);
        }
    return out;
}

QRectF alphaBounds(const QImage& rgba, int threshold)
{
    if (rgba.isNull() || !rgba.hasAlphaChannel()) return {};
    const QImage img = rgba.convertToFormat(QImage::Format_ARGB32);
    int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(line[x]) >= threshold) {
                x0 = std::min(x0, x); x1 = std::max(x1, x);
                y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
    }
    if (x1 < 0) return {};
    return QRectF(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

std::vector<float> neutralParams(const std::vector<float>& modelParams)
{
    std::vector<float> p = modelParams;
    for (int i = 0; i < kPoseParams && i < static_cast<int>(p.size()); ++i) p[i] = 0.0f;
    return p;
}

QByteArray toObj(const std::vector<float>& v, const std::vector<int>& tris, bool facingMinusZ)
{
    QByteArray o;
    o.reserve(static_cast<int>(v.size()) * 12);
    o += "# SAM 3D Body / MHR body (metres)\n";
    const float sx = facingMinusZ ? -1.0f : 1.0f;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        // QByteArray::number: printf("%g") would write decimal commas under pt_BR-style locales.
        o += "v " + QByteArray::number(v[i] * sx, 'g', 9) + ' ' + QByteArray::number(v[i + 1], 'g', 9) + ' '
             + QByteArray::number(v[i + 2] * sx, 'g', 9) + '\n';
    }
    for (size_t i = 0; i + 2 < tris.size(); i += 3)
        o += "f " + QByteArray::number(tris[i] + 1) + ' ' + QByteArray::number(tris[i + 1] + 1) + ' '
             + QByteArray::number(tris[i + 2] + 1) + '\n';
    return o;
}

// ---------------------------------------------------------------------------
// Model files
// ---------------------------------------------------------------------------

QString modelDir() { return QDir(AppStorage::aiModelsRoot()).filePath(QStringLiteral("sam3dbody/")); }
QString backbonePath() { return QDir(modelDir()).filePath(QStringLiteral("sam3d_backbone.onnx")); }
QString decoderPath()  { return QDir(modelDir()).filePath(QStringLiteral("sam3d_decoder.onnx")); }
QString mhrPath()      { return QDir(modelDir()).filePath(QStringLiteral("mhr_body.onnx")); }
QString facesPath()    { return QDir(modelDir()).filePath(QStringLiteral("mhr_faces.i32")); }
QString decoderV2Path()   { return QDir(modelDir()).filePath(QStringLiteral("sam3d_decoder_v2.onnx")); }
QString handDecoderPath() { return QDir(modelDir()).filePath(QStringLiteral("sam3d_hand_decoder.onnx")); }
QString handFusePath()    { return QDir(modelDir()).filePath(QStringLiteral("sam3d_hand_fuse.onnx")); }

bool handModelsPresent()
{
    return QFileInfo::exists(decoderV2Path()) && QFileInfo::exists(handDecoderPath()) && QFileInfo::exists(handFusePath());
}

bool modelsPresent()
{
    for (const FileSpec& f : kFiles)
        if (!QFileInfo::exists(QDir(modelDir()).filePath(QString::fromLatin1(f.name)))) return false;
    return true;
}

static std::vector<int> loadFaces(const QString& path)
{
    std::vector<int> t;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return t;
    const QByteArray b = f.readAll();
    const size_t n = static_cast<size_t>(b.size()) / 4;
    t.resize(n - n % 3);
    for (size_t i = 0; i < t.size(); ++i) {
        const uchar* p = reinterpret_cast<const uchar*>(b.constData()) + i * 4;
        t[i] = static_cast<int>(static_cast<quint32>(p[0]) | (static_cast<quint32>(p[1]) << 8)
                                | (static_cast<quint32>(p[2]) << 16) | (static_cast<quint32>(p[3]) << 24));
    }
    return t;
}

#ifndef ENABLE_ONNX

bool isAvailable() { return false; }
bool ensureModelsBlocking(QString* error)
{
    if (error) *error = QStringLiteral("photo to body needs an ONNX build (rebuild with -DENABLE_ONNX)");
    return false;
}
struct Runner::Impl {};
Runner::Runner() : d(std::make_unique<Impl>()) {}
Runner::~Runner() = default;
bool Runner::restPose(std::vector<float>&, std::vector<float>&, QString* error)
{
    if (error) *error = QStringLiteral("needs an ONNX build");
    return false;
}
Result Runner::predict(const QImage&, const Options&)
{
    Result r;
    r.error = QStringLiteral("photo to body needs an ONNX build (rebuild with -DENABLE_ONNX).");
    return r;
}

Result predict(const QImage&, const Options&)
{
    Result r;
    r.error = QStringLiteral("photo to body needs an ONNX build (rebuild with -DENABLE_ONNX).");
    return r;
}

#else // ENABLE_ONNX

bool isAvailable() { return true; }

bool ensureModelsBlocking(QString* error)
{
    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        const bool settingsOverride = !base.isEmpty();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_SAM3DBODY_MODEL_BASE_URL");
            base = env.isEmpty() ? QString::fromLatin1(kDefaultBaseUrl) : QString::fromUtf8(env);
        }
        Q_UNUSED(settingsOverride);
    }
    const bool defaultHosting = base == QString::fromLatin1(kDefaultBaseUrl);
    if (!base.endsWith('/')) base += '/';
    const bool noDownload = !qEnvironmentVariableIsEmpty("QTMESH_SAM3DBODY_NO_DOWNLOAD");

    QDir().mkpath(modelDir());
    // One file: verified (digest) when present, fetched when missing. Empty return = ok, else why not.
    auto ensure = [&](const FileSpec& f) -> QString {
        const QString dest = QDir(modelDir()).filePath(QString::fromLatin1(f.name));
        if (QFileInfo::exists(dest) && !defaultHosting) return {};   // a mirror may serve a different export
        if (noDownload && !QFileInfo::exists(dest))
            return QStringLiteral("%1 is missing and downloads are disabled by QTMESH_SAM3DBODY_NO_DOWNLOAD")
                .arg(QString::fromLatin1(f.name));
        ModelFetch::Request req;
        // An empty URL = verify only: with downloads disabled an existing file is still checked against its digest
        // (a truncated or tampered file must not reach ONNX Runtime); a mismatch deletes it and reports why.
        req.url = noDownload ? QString() : base + QString::fromLatin1(f.name);
        req.destination = dest;
        req.label = QString::fromLatin1(f.label);
        req.timeoutMs = 3600000;                       // the backbone weights are ~3.4 GB
        if (defaultHosting) req.expectedSha256 = QString::fromLatin1(f.sha256);
        const ModelFetch::Outcome fo = ModelFetch::ensureBlocking(req);
        return fo.ok ? QString() : fo.error;
    };
    for (const FileSpec& f : kFiles) {
        const QString why = ensure(f);
        if (!why.isEmpty()) {
            if (error) *error = why;
            return false;
        }
    }
    // Optional hand-refinement graphs: a failure is logged and capture keeps the body-only fingers.
    for (const FileSpec& f : kHandFiles) {
        const QString why = ensure(f);
        if (!why.isEmpty()) {
            qWarning("SAM 3D Body: hand refinement unavailable (%s): %s", f.name, qPrintable(why));
            break;
        }
    }
    return true;
}

namespace {

struct NamedTensor { std::vector<float> data; std::vector<int64_t> shape; };

bool allFinite(const std::vector<float>& v)
{
    return std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); });
}

} // namespace

/// The three ONNX Runtime sessions, created on first use and kept: a sequence pays the 3.4 GB backbone load once.
struct Runner::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "qtmesh_sam3dbody"};
    std::unique_ptr<Ort::Session> backbone, decoder, mhr;
    std::unique_ptr<Ort::Session> decoderV2, handDecoder, handFuse;   // hand refinement (optional graphs)
    QElapsedTimer clock;                                              // QTMESH_SAM_TIMING=1: per-stage timings on stderr
    bool timing = qEnvironmentVariableIsSet("QTMESH_SAM_TIMING");
    void mark(const char* what)
    {
        if (!timing) return;
        if (!clock.isValid()) clock.start();
        fprintf(stderr, "[sam-timing] %7.2f s  %s\n", clock.elapsed() / 1000.0, what);
        clock.restart();
    }

    static Ort::Session* open(Ort::Env& env, std::unique_ptr<Ort::Session>& slot, const QString& file)
    {
        if (!slot) {
            Ort::SessionOptions so;
            OnnxRuntimeSettings::SessionConfig cfg;
#ifdef __APPLE__
            // CPU only on macOS: since ONNX Runtime 1.31 the CoreML EP fails to COMPILE these graphs at session creation
            // ("Espresso exception: generic_general_slice: Invalid values 1 in begin_ids"), which aborted every SAM capture
            // from the GUI. The failure comes from Ort::Session's constructor, after the EP was appended, so the append's own
            // fallback never sees it. CPU runs a frame in ~6 s on Apple Silicon.
            cfg.appendGpu = false;
#endif
            OnnxRuntimeSettings::configureSessionOptions(so, cfg);
#ifdef _WIN32
            slot = std::make_unique<Ort::Session>(env, file.toStdWString().c_str(), so);
#else
            slot = std::make_unique<Ort::Session>(env, file.toStdString().c_str(), so);
#endif
        }
        return slot.get();
    }

    static std::vector<std::vector<float>> run(Ort::Session& session, const std::map<std::string, NamedTensor>& inputs,
                                               const std::vector<std::string>& outputNames)
    {
        Ort::AllocatorWithDefaultOptions alloc;
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::AllocatedStringPtr> keep;
        std::vector<const char*> inNames;
        std::vector<Ort::Value> inValues;
        for (size_t i = 0; i < session.GetInputCount(); ++i) {
            keep.push_back(session.GetInputNameAllocated(i, alloc));
            const std::string name = keep.back().get();
            const auto it = inputs.find(name);
            if (it == inputs.end()) throw std::runtime_error("graph input '" + name + "' has no value");
            auto& t = const_cast<NamedTensor&>(it->second);
            inNames.push_back(keep.back().get());
            inValues.push_back(Ort::Value::CreateTensor<float>(mem, t.data.data(), t.data.size(), t.shape.data(), t.shape.size()));
        }
        std::vector<const char*> outNames;
        for (const std::string& n : outputNames) outNames.push_back(n.c_str());
        auto outs = session.Run(Ort::RunOptions{nullptr}, inNames.data(), inValues.data(), inValues.size(), outNames.data(), outNames.size());
        std::vector<std::vector<float>> result;
        for (auto& o : outs) {
            const size_t n = o.GetTensorTypeAndShapeInfo().GetElementCount();
            const float* d = o.GetTensorData<float>();
            result.emplace_back(d, d + n);
        }
        return result;
    }

    /// Meta's run_inference "full" after the backbone (see SamBodyHands.h for the steps). Fills r.body.shape/modelParams/
    /// camT and r.handsRefined/handNote. false + `error` only when a graph returns garbage; a hand that fails a test is not
    /// an error (the body estimate keeps that hand).
    template <typename MakeInputs>
    bool refineWithHands(const QImage& photo, const CropGeometry& geo, std::vector<float> bodyFeatures,
                         const MakeInputs& decoderInputs, Result& r, QString& error)
    {
        namespace H = Hands;
        constexpr int kMaxPrompts = 4;                      // MAX_PROMPTS in scripts/export-sam3dbody-onnx.py
        const int W = photo.width(), Hh = photo.height();
        auto expect = [&](const std::vector<std::vector<float>>& v, std::initializer_list<size_t> sizes, const char* what) {
            bool ok = v.size() == sizes.size();
            size_t i = 0;
            for (size_t n : sizes) {
                if (!ok) break;
                ok = v[i].size() == n && allFinite(v[i]);
                ++i;
            }
            if (!ok) error = QStringLiteral("the %1 returned unusable values").arg(QLatin1String(what));
            return ok;
        };
        mark("refine: start");
        Ort::Session& dv2 = *open(env, decoderV2, decoderV2Path());
        mark("refine: decoder v2 session");
        const std::vector<std::string> bodyOuts = {"shape", "scale", "body_pose", "global_rot", "hand", "cam_t", "kp2d",
                                                   "model_params", "pose_raw", "hand_box"};
        const std::initializer_list<size_t> bodySizes = {kShapeComps, 28, 133, 3, 108, 3, size_t(H::kKeypoints) * 2,
                                                         kModelParams, 522, 8};
        // 1. first body pass: init estimate, one dummy prompt (the features are kept for the keypoint-prompt re-run)
        auto in = decoderInputs(geo, bodyFeatures);
        in["prev_estimate"] = {std::vector<float>(522, 0.0f), {1, 522}};
        // the graph takes a fixed kMaxPrompts slots + a validity mask (masked slots do not take part in the attention)
        auto setPrompts = [](std::map<std::string, NamedTensor>& m, const std::vector<H::Prompt>& prompts) {
            std::vector<float> pv, valid;
            for (int k = 0; k < kMaxPrompts; ++k) {
                const bool used = k < static_cast<int>(prompts.size());
                const H::Prompt p = used ? prompts[size_t(k)] : H::Prompt{};
                pv.insert(pv.end(), {p.x, p.y, p.label});
                valid.push_back(used ? 1.0f : 0.0f);
            }
            m["prompt"] = {pv, {1, kMaxPrompts, 3}};
            m["prompt_valid"] = {valid, {1, kMaxPrompts}};
        };
        setPrompts(in, {H::Prompt{}});                      // first pass: one dummy prompt, like Meta
        in["use_prev"] = {{0.0f}, {1}};
        auto b0 = run(dv2, in, bodyOuts);
        mark("refine: body pass 1");
        if (!expect(b0, bodySizes, "decoder")) return false;
        // body-only result unless a hand gets through
        r.body.shape = b0[0];
        r.body.modelParams = b0[7];
        r.body.camT = {b0[5][0], b0[5][1], b0[5][2]};

        // 2. each hand: crop from the predicted box (the left one on the mirrored photo), backbone, hand decoder
        struct HandRun {
            bool ran = false;
            CropGeometry geo;
            std::vector<std::vector<float>> out;   // hand, scale, shape, wrist_rot, kp2d, kp2d_cropped
            QString note;
        };
        const QImage mirrored =
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
            photo.flipped(Qt::Horizontal);
#else
            photo.mirrored(true, false);
#endif
        HandRun hands[2];
        const char* sideName[2] = {"left", "right"};
        for (int side = 0; side < 2; ++side) {
            HandRun& h = hands[side];
            const std::array<float, 4> box = {b0[9][side * 4], b0[9][side * 4 + 1], b0[9][side * 4 + 2], b0[9][side * 4 + 3]};
            QRectF photoBox = H::handBoxInPhoto(geo, box);
            if (side == 0) photoBox = H::mirrorBox(photoBox, W);
            h.geo = cropGeometry(W, Hh, photoBox, H::kHandPadding);
            if (!h.geo.valid) { h.note = QStringLiteral("%1: no hand box").arg(QLatin1String(sideName[side])); continue; }
            // criterion 2 decided before paying for a backbone run: such a hand is never used
            if (!(h.geo.scale > H::kMinHandBox)) {
                h.note = QStringLiteral("%1: hand %2 px < %3").arg(QLatin1String(sideName[side])).arg(qRound(h.geo.scale))
                             .arg(qRound(H::kMinHandBox));
                continue;
            }
            NamedTensor img{warpCrop(side == 0 ? mirrored : photo, h.geo), {1, 3, kCropSize, kCropSize}};
            auto f = run(*open(env, backbone, backbonePath()), {{"image", std::move(img)}}, {"features"});
            if (!expect(f, {size_t(1280) * kFeatureSize * kFeatureSize}, "backbone")) return false;
            mark(side == 0 ? "refine: left backbone" : "refine: right backbone");
            h.out = run(*open(env, handDecoder, handDecoderPath()), decoderInputs(h.geo, std::move(f[0])),
                        {"hand", "scale", "shape", "wrist_rot", "kp2d", "kp2d_cropped"});
            if (!expect(h.out, {108, 28, kShapeComps, 9, size_t(H::kKeypoints) * 2, size_t(H::kKeypoints) * 2}, "hand decoder"))
                return false;
            h.ran = true;
            mark(side == 0 ? "refine: left hand decoder" : "refine: right hand decoder");
        }
        if (!hands[0].ran && !hands[1].ran) {
            r.handNote = hands[0].note + QStringLiteral("; ") + hands[1].note;
            return true;
        }

        // 3. tests 3 + 4 here, test 1 (wrist angle) inside the fusion graph
        const QPointF bodyWrist[2] = {H::keypoint(b0[6], H::kLeftWristKp), H::keypoint(b0[6], H::kRightWristKp)};
        QPointF handWrist[2];
        std::array<bool, 2> pass{false, false};
        for (int side = 0; side < 2; ++side) {
            HandRun& h = hands[side];
            if (!h.ran) continue;
            QPointF w = H::keypoint(h.out[4], H::kRightWristKp);          // the hand decoder only knows right hands
            if (side == 0) w = H::mirrorPoint(w, W);
            handWrist[side] = w;
            const bool inside = H::keypointsInside(h.out[5]);
            // Meta divides each hand's wrist offset by the OTHER hand's crop edge; kept for parity
            const bool close = H::wristClose(w, bodyWrist[side], hands[1 - side].geo.scale);
            pass[side] = inside && close;
            if (!inside) h.note = QStringLiteral("%1: hand keypoints leave the crop").arg(QLatin1String(sideName[side]));
            else if (!close) h.note = QStringLiteral("%1: hand wrist far from the body wrist").arg(QLatin1String(sideName[side]));
        }
        Ort::Session& fuse = *open(env, handFuse, handFusePath());
        static const std::vector<float> kIdentity = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        auto handIn = [&](int side, int k, size_t n) {
            return hands[side].ran ? hands[side].out[size_t(k)] : (k == 3 ? kIdentity : std::vector<float>(n, 0.0f));
        };
        auto fuseInputs = [&](const std::vector<std::vector<float>>& body, const std::array<bool, 2>& valid) {
            std::map<std::string, NamedTensor> m;
            m["ori_body_pose"] = {b0[2], {1, 133}};
            m["body_pose"] = {body[2], {1, 133}};
            m["global_rot"] = {body[3], {1, 3}};
            m["scale"] = {body[1], {1, 28}};
            m["shape"] = {body[0], {1, kShapeComps}};
            m["hand"] = {body[4], {1, 108}};
            const char* pre[2] = {"l_", "r_"};
            for (int side = 0; side < 2; ++side) {
                const std::string p = pre[side];
                m[p + "hand"] = {handIn(side, 0, 108), {1, 108}};
                m[p + "scale"] = {handIn(side, 1, 28), {1, 28}};
                m[p + "shape"] = {handIn(side, 2, kShapeComps), {1, kShapeComps}};
                m[p + "wrist"] = {handIn(side, 3, 9), {1, 3, 3}};
            }
            m["valid"] = {{valid[0] ? 1.0f : 0.0f, valid[1] ? 1.0f : 0.0f}, {1, 2}};
            return m;
        };
        auto probe = run(fuse, fuseInputs(b0, {true, true}), {"angle_ok"});
        mark("refine: fuse probe");
        if (!expect(probe, {2}, "hand fusion")) return false;
        std::array<bool, 2> valid{};
        for (int side = 0; side < 2; ++side) {
            const bool angleOk = probe[0][size_t(side)] > 0.5f;
            valid[size_t(side)] = hands[side].ran && pass[size_t(side)] && angleOk;
            if (hands[side].ran && pass[size_t(side)] && !angleOk)
                hands[side].note = QStringLiteral("%1: hand wrist disagrees with the arm").arg(QLatin1String(sideName[side]));
        }
        if (!valid[0] && !valid[1]) {
            r.handNote = hands[0].note + QStringLiteral("; ") + hands[1].note;
            return true;
        }

        // 4. keypoint-prompt re-run of the body decoder with the accepted wrists + the elbows
        std::vector<std::vector<float>> body = b0;
        const auto prompts = H::wristElbowPrompts(geo, handWrist[1], handWrist[0], H::keypoint(b0[6], H::kRightElbowKp),
                                                  H::keypoint(b0[6], H::kLeftElbowKp), valid);
        if (!prompts.empty()) {
            in = decoderInputs(geo, std::move(bodyFeatures));
            in["prev_estimate"] = {b0[8], {1, 522}};
            setPrompts(in, prompts);
            in["use_prev"] = {{1.0f}, {1}};
            body = run(dv2, in, bodyOuts);
            mark("refine: body pass 2 (prompts)");
            if (!expect(body, bodySizes, "decoder (keypoint prompts)")) return false;
        }

        // 5. fuse
        auto fused = run(fuse, fuseInputs(body, valid), {"fused_shape", "model_params", "angle_ok"});
        mark("refine: fuse");
        if (!expect(fused, {kShapeComps, kModelParams, 2}, "hand fusion")) return false;
        r.body.shape = fused[0];
        r.body.modelParams = fused[1];
        r.body.camT = {body[5][0], body[5][1], body[5][2]};
        // The graph re-tests the wrist angle against the PROMPTED body's forearm: a hand that passed the probe can still be
        // rejected there, and the graph then keeps the body's hand. Report what was actually applied.
        for (int side = 0; side < 2; ++side)
            if (valid[size_t(side)] && !(fused[2][size_t(side)] > 0.5f)) {
                valid[size_t(side)] = false;
                hands[side].note = QStringLiteral("%1: hand wrist disagrees with the re-prompted arm").arg(QLatin1String(sideName[side]));
            }
        r.handsRefined = valid;
        QStringList notes;
        for (int side = 0; side < 2; ++side)
            if (!valid[size_t(side)] && !hands[side].note.isEmpty()) notes << hands[side].note;
        r.handNote = notes.join(QStringLiteral("; "));
        return true;
    }
};

Runner::Runner() : d(std::make_unique<Impl>()) {}
Runner::~Runner() = default;

Result Runner::predict(const QImage& photo, const Options& opts)
{
    Result r;
    if (photo.isNull()) { r.error = QStringLiteral("empty photo"); return r; }
    if (!modelsPresent()) {
        r.error = QStringLiteral("SAM 3D Body models are not downloaded (looked in %1).").arg(modelDir());
        return r;
    }

    // 1. Where is the person?
    QRectF box = opts.bbox;
    if (!(box.width() > 0 && box.height() > 0) && opts.autoDetect) {
        QString rembg = opts.matteModel.isEmpty() ? BackgroundRemover::ensureModelBlocking(BackgroundRemover::Quality::Fast) : opts.matteModel;
        if (!rembg.isEmpty()) {
            BackgroundRemover::Options ro;
            const auto cut = BackgroundRemover::removeBackground(photo, rembg, ro);
            // The remover returns a cropped, re-padded square: its own coordinates are not the photo's.
            // foregroundBox is the subject's box in the SOURCE image.
            if (cut.ok && !cut.foregroundBox.isEmpty()) {
                box = QRectF(cut.foregroundBox);
                r.usedMatte = true;
            }
        }
        if (!(box.width() > 0))
            r.personFound = false;
        if (!(box.width() > 0))
            r.warning = QStringLiteral("could not find the person automatically; using the whole photo "
                                       "(pass an explicit box for a better fit)");
    }
    if (!(box.width() > 0 && box.height() > 0)) box = QRectF(0, 0, photo.width(), photo.height());
    r.bboxUsed = box;

    const CropGeometry geo = cropGeometry(photo.width(), photo.height(), box);
    if (!geo.valid) { r.error = QStringLiteral("invalid person box"); return r; }
    r.geometry = geo;

    try {
        // 2. Backbone.
        d->mark("predict: start");
        NamedTensor img{warpCrop(photo, geo), {1, 3, kCropSize, kCropSize}};
        auto feats = Impl::run(*Impl::open(d->env, d->backbone, backbonePath()), {{"image", std::move(img)}}, {"features"});
        d->mark("predict: body backbone (incl. first load)");
        if (feats.empty() || feats[0].size() != static_cast<size_t>(1280) * kFeatureSize * kFeatureSize
            || !allFinite(feats[0])) {
            r.error = QStringLiteral("the backbone returned unusable features");
            return r;
        }

        // 3. Decoder (+ Meta's hand refinement when its graphs are installed).
        auto decoderInputs = [](const CropGeometry& g, std::vector<float> features) {
            std::map<std::string, NamedTensor> in;
            in["features"]    = {std::move(features), {1, 1280, kFeatureSize, kFeatureSize}};
            in["ray_cond"]    = {rayCondition(g), {1, 2, kCropSize, kCropSize}};
            in["cond_info"]   = {{g.cond[0], g.cond[1], g.cond[2]}, {1, 3}};
            in["bbox_center"] = {{static_cast<float>(g.center[0]), static_cast<float>(g.center[1])}, {1, 2}};
            in["bbox_scale"]  = {{static_cast<float>(g.scale)}, {1}};
            in["ori_img_size"] = {{static_cast<float>(g.photoW), static_cast<float>(g.photoH)}, {1, 2}};
            in["cam_int"]     = {{g.focal, 0, g.principal[0], 0, g.focal, g.principal[1], 0, 0, 1}, {1, 3, 3}};
            in["affine"]      = {{g.affine[0], g.affine[1], g.affine[2], g.affine[3], g.affine[4], g.affine[5]}, {1, 2, 3}};
            in["crop_size"]   = {{static_cast<float>(kCropSize), static_cast<float>(kCropSize)}, {1, 2}};
            return in;
        };
        const bool refine = opts.refineHands && handModelsPresent();
        if (!refine) {
            if (opts.refineHands)
                r.handNote = QStringLiteral("hand refinement graphs not installed (body-only fingers)");
            auto in = decoderInputs(geo, std::move(feats[0]));
            auto dec = Impl::run(*Impl::open(d->env, d->decoder, decoderPath()), in, {"shape", "model_params", "cam_t"});
            if (dec.size() != 3 || dec[0].size() != kShapeComps || dec[1].size() != kModelParams
                || !allFinite(dec[0]) || !allFinite(dec[1])) {
                r.error = QStringLiteral("the decoder returned unusable parameters");
                return r;
            }
            r.body.shape = dec[0];
            r.body.modelParams = dec[1];
            if (dec[2].size() >= 3) r.body.camT = {dec[2][0], dec[2][1], dec[2][2]};
        } else {
            QString why;
            if (!d->refineWithHands(photo, geo, std::move(feats[0]), decoderInputs, r, why)) {
                r.error = why;
                return r;
            }
        }
        r.body.focalPx = geo.focal;

        // 4. MHR forward: posed (+ neutral for a mesh).
        Ort::Session& mhrSession = *Impl::open(d->env, d->mhr, mhrPath());
        auto mhr = [&](const std::vector<float>& params) {
            std::map<std::string, NamedTensor> m;
            m["shape"] = {r.body.shape, {1, kShapeComps}};
            m["model_params"] = {params, {1, kModelParams}};
            return Impl::run(mhrSession, m, {"vertices", "joint_t", "joint_q"});
        };
        auto posed = mhr(r.body.modelParams);
        const size_t want = static_cast<size_t>(kVertexCount) * 3;
        if (posed.size() != 3 || posed[0].size() != want || posed[1].size() != static_cast<size_t>(kJointCount) * 3
            || posed[2].size() != static_cast<size_t>(kJointCount) * 4 || !allFinite(posed[0]) || !allFinite(posed[1])
            || !allFinite(posed[2])) {
            r.error = QStringLiteral("the MHR body model returned an unusable result");
            return r;
        }
        for (float& x : posed[0]) x *= 0.01f;                // cm -> m
        for (float& x : posed[1]) x *= 0.01f;
        r.body.posedVertices = std::move(posed[0]);
        r.body.jointPos = std::move(posed[1]);
        r.body.jointQuat = std::move(posed[2]);
        if (opts.wantMesh) {
            auto neutral = mhr(neutralParams(r.body.modelParams));
            if (neutral.empty() || neutral[0].size() != want || !allFinite(neutral[0])) {
                r.error = QStringLiteral("the MHR body model returned an unusable neutral mesh");
                return r;
            }
            for (float& x : neutral[0]) x *= 0.01f;
            r.body.neutralVertices = std::move(neutral[0]);
            r.body.triangles = loadFaces(facesPath());
            if (r.body.triangles.empty()) { r.error = QStringLiteral("mhr_faces.i32 is unreadable"); return r; }
        }
        r.ok = true;
        return r;
    } catch (const std::exception& e) {
        r.error = QStringLiteral("SAM 3D Body inference failed: %1").arg(QString::fromUtf8(e.what()));
        return r;
    }
}

bool Runner::restPose(std::vector<float>& jointPos, std::vector<float>& jointQuat, QString* error)
{
    try {
        std::map<std::string, NamedTensor> m;
        m["shape"] = {std::vector<float>(kShapeComps, 0.0f), {1, kShapeComps}};
        m["model_params"] = {std::vector<float>(kModelParams, 0.0f), {1, kModelParams}};
        auto out = Impl::run(*Impl::open(d->env, d->mhr, mhrPath()), m, {"joint_t", "joint_q"});
        if (out.size() != 2 || out[0].size() != size_t(kJointCount) * 3 || out[1].size() != size_t(kJointCount) * 4
            || !allFinite(out[0]) || !allFinite(out[1])) {
            if (error) *error = QStringLiteral("the MHR body model returned an unusable rest pose");
            return false;
        }
        for (float& x : out[0]) x *= 0.01f;                  // cm -> m
        jointPos = std::move(out[0]);
        jointQuat = std::move(out[1]);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = QString::fromUtf8(e.what());
        return false;
    }
}

Result predict(const QImage& photo, const Options& opts)
{
    Runner runner;
    return runner.predict(photo, opts);
}

#endif // ENABLE_ONNX

} // namespace SamBody
