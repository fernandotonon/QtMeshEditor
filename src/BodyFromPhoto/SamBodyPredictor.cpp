#include "SamBodyPredictor.h"

#include "AppStorage.h"
#include "ModelFetch.h"
#include "../ImageTo3D/BackgroundRemover.h"
#ifdef ENABLE_ONNX
#include "OnnxRuntimeSettings.h"
#include <onnxruntime_cxx_api.h>
#endif

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <map>

namespace {

// Pinned to a COMMIT, not `main`: the SHA-256 table below describes exactly these files, and a re-export uploaded to the same
// paths on main would otherwise make every released build fail its digest check. After re-exporting, upload, then update this
// revision AND the digests together.
constexpr const char* kDefaultBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/26daed748226af17fb8c402465cb79281f9a5ac1/sam3dbody/";
constexpr const char* kBaseUrlSettingsKey = "ai/sam3dbodyModelBaseUrl";

struct FileSpec { const char* name; const char* label; const char* sha256; };
// SHA-256 of the files we host (default hosting only; a mirror override may serve a different export).
constexpr FileSpec kFiles[] = {
    {"sam3d_backbone.onnx",      "SAM 3D Body backbone (graph)",   "d92e6a0129c57b02f3330f655fdde88f6331440c92206793400b3b1a086652b7"},
    {"sam3d_backbone.onnx.data", "SAM 3D Body backbone (weights)", "479e7da52d115344a3fd3e7711dae58176f68c723486e80ba2c3efc046f4dbc6"},
    {"sam3d_decoder.onnx",       "SAM 3D Body decoder",            "b47971ea3f1cd7533f4a0d6ef7113db8bfb1e0a4726019d2fe3ebd9801440c47"},
    {"mhr_body.onnx",            "MHR body model",                 "1b5eea8284226dbee296968d89605cceb463f278f5ecdb73f59cba26fed71359"},
    {"mhr_faces.i32",            "MHR body topology",              "f6748e290ef37fbb6877c4cc5bd7287105db9e98252b0ba170ae9ac3c45eacd6"},
    {"mhr_uv.bin",               "MHR UV layout",                  "87dd98b37aa3c54147c6fb84a1bd774261d21b0c25677e4d994dc9e6eed20caf"},
    // The SAM License requires a copy of itself to accompany the SAM materials wherever they are distributed.
    {"LICENSE-SAM.txt",          "SAM License",                    "b3a5a0e2d973ab80e6610ccf1cffc40756050d0ace3cd4fec879b3ec290b2e9b"},
    {"LICENSE-MHR.txt",          "MHR (Apache-2.0) licence",       "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"},
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
QString uvPath()       { return QDir(modelDir()).filePath(QStringLiteral("mhr_uv.bin")); }

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
    for (const FileSpec& f : kFiles) {
        const QString dest = QDir(modelDir()).filePath(QString::fromLatin1(f.name));
        if (QFileInfo::exists(dest) && !defaultHosting) continue;   // a mirror may serve a different export
        if (noDownload && !QFileInfo::exists(dest)) {
            if (error) *error = QStringLiteral("%1 is missing and downloads are disabled by QTMESH_SAM3DBODY_NO_DOWNLOAD")
                                    .arg(QString::fromLatin1(f.name));
            return false;
        }
        ModelFetch::Request req;
        // An empty URL = verify only: with downloads disabled an existing file is still checked against its digest
        // (a truncated or tampered file must not reach ONNX Runtime); a mismatch deletes it and reports why.
        req.url = noDownload ? QString() : base + QString::fromLatin1(f.name);
        req.destination = dest;
        req.label = QString::fromLatin1(f.label);
        req.timeoutMs = 3600000;                       // the backbone weights are ~3.4 GB
        if (defaultHosting) req.expectedSha256 = QString::fromLatin1(f.sha256);
        const ModelFetch::Outcome fo = ModelFetch::ensureBlocking(req);
        if (!fo.ok) {
            if (error) *error = fo.error;
            return false;
        }
    }
    return true;
}

namespace {

struct NamedTensor { std::vector<float> data; std::vector<int64_t> shape; };

Ort::SessionOptions makeOptions()
{
    Ort::SessionOptions so;
    OnnxRuntimeSettings::configureSessionOptions(so);
    return so;
}

std::vector<std::vector<float>> run(Ort::Env& env, const QString& file,
                                    const std::map<std::string, NamedTensor>& inputs,
                                    const std::vector<std::string>& outputNames)
{
    Ort::SessionOptions so = makeOptions();
#ifdef _WIN32
    Ort::Session session(env, file.toStdWString().c_str(), so);
#else
    Ort::Session session(env, file.toStdString().c_str(), so);
#endif
    Ort::AllocatorWithDefaultOptions alloc;
    auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    std::vector<Ort::AllocatedStringPtr> keep;
    std::vector<const char*> inNames;
    std::vector<Ort::Value> inValues;
    for (size_t i = 0; i < session.GetInputCount(); ++i) {
        keep.push_back(session.GetInputNameAllocated(i, alloc));
        const std::string name = keep.back().get();
        const auto it = inputs.find(name);
        if (it == inputs.end())
            throw std::runtime_error("graph input '" + name + "' has no value");
        auto& t = const_cast<NamedTensor&>(it->second);
        inNames.push_back(keep.back().get());
        inValues.push_back(Ort::Value::CreateTensor<float>(mem, t.data.data(), t.data.size(),
                                                           t.shape.data(), t.shape.size()));
    }
    std::vector<const char*> outNames;
    for (const std::string& n : outputNames) outNames.push_back(n.c_str());
    auto outs = session.Run(Ort::RunOptions{nullptr}, inNames.data(), inValues.data(), inValues.size(),
                            outNames.data(), outNames.size());
    std::vector<std::vector<float>> result;
    for (auto& o : outs) {
        const size_t n = o.GetTensorTypeAndShapeInfo().GetElementCount();
        const float* d = o.GetTensorData<float>();
        result.emplace_back(d, d + n);
    }
    return result;
}

bool allFinite(const std::vector<float>& v)
{
    return std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); });
}

} // namespace

Result predict(const QImage& photo, const Options& opts)
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
        QString rembg = BackgroundRemover::ensureModelBlocking(BackgroundRemover::Quality::Fast);
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
            r.warning = QStringLiteral("could not find the person automatically; using the whole photo "
                                       "(pass an explicit box for a better fit)");
    }
    if (!(box.width() > 0 && box.height() > 0)) box = QRectF(0, 0, photo.width(), photo.height());
    r.bboxUsed = box;

    const CropGeometry geo = cropGeometry(photo.width(), photo.height(), box);
    if (!geo.valid) { r.error = QStringLiteral("invalid person box"); return r; }
    r.geometry = geo;

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_sam3dbody");

        // 2. Backbone (3.4 GB of weights: the session is released before the decoder loads).
        NamedTensor img{warpCrop(photo, geo), {1, 3, kCropSize, kCropSize}};
        auto feats = run(env, backbonePath(), {{"image", std::move(img)}}, {"features"});
        if (feats.empty() || feats[0].size() != static_cast<size_t>(1280) * kFeatureSize * kFeatureSize
            || !allFinite(feats[0])) {
            r.error = QStringLiteral("the backbone returned unusable features");
            return r;
        }

        // 3. Decoder.
        std::map<std::string, NamedTensor> in;
        in["features"]    = {std::move(feats[0]), {1, 1280, kFeatureSize, kFeatureSize}};
        in["ray_cond"]    = {rayCondition(geo), {1, 2, kCropSize, kCropSize}};
        in["cond_info"]   = {{geo.cond[0], geo.cond[1], geo.cond[2]}, {1, 3}};
        in["bbox_center"] = {{static_cast<float>(geo.center[0]), static_cast<float>(geo.center[1])}, {1, 2}};
        in["bbox_scale"]  = {{static_cast<float>(geo.scale)}, {1}};
        in["ori_img_size"] = {{static_cast<float>(geo.photoW), static_cast<float>(geo.photoH)}, {1, 2}};
        in["cam_int"]     = {{geo.focal, 0, geo.principal[0], 0, geo.focal, geo.principal[1], 0, 0, 1}, {1, 3, 3}};
        in["affine"]      = {{geo.affine[0], geo.affine[1], geo.affine[2], geo.affine[3], geo.affine[4], geo.affine[5]}, {1, 2, 3}};
        in["crop_size"]   = {{static_cast<float>(kCropSize), static_cast<float>(kCropSize)}, {1, 2}};
        auto dec = run(env, decoderPath(), in, {"shape", "model_params", "cam_t"});
        if (dec.size() != 3 || dec[0].size() != kShapeComps || dec[1].size() != kModelParams
            || !allFinite(dec[0]) || !allFinite(dec[1])) {
            r.error = QStringLiteral("the decoder returned unusable parameters");
            return r;
        }
        r.body.shape = dec[0];
        r.body.modelParams = dec[1];
        if (dec[2].size() >= 3) r.body.camT = {dec[2][0], dec[2][1], dec[2][2]};
        r.body.focalPx = geo.focal;

        // 4. MHR forward: posed and neutral.
        auto mhr = [&](const std::vector<float>& params) {
            std::map<std::string, NamedTensor> m;
            m["shape"] = {r.body.shape, {1, kShapeComps}};
            m["model_params"] = {params, {1, kModelParams}};
            return run(env, mhrPath(), m, {"vertices"});
        };
        auto posed = mhr(r.body.modelParams);
        auto neutral = mhr(neutralParams(r.body.modelParams));
        const size_t want = static_cast<size_t>(kVertexCount) * 3;
        if (posed.empty() || neutral.empty() || posed[0].size() != want || neutral[0].size() != want
            || !allFinite(posed[0]) || !allFinite(neutral[0])) {
            r.error = QStringLiteral("the MHR body model returned an unusable mesh");
            return r;
        }
        for (auto* v : {&posed[0], &neutral[0]})
            for (float& x : *v) x *= 0.01f;                  // cm -> m
        r.body.posedVertices = std::move(posed[0]);
        r.body.neutralVertices = std::move(neutral[0]);
        r.body.triangles = loadFaces(facesPath());
        if (r.body.triangles.empty()) { r.error = QStringLiteral("mhr_faces.i32 is unreadable"); return r; }
        r.ok = true;
        return r;
    } catch (const std::exception& e) {
        r.error = QStringLiteral("SAM 3D Body inference failed: %1").arg(QString::fromUtf8(e.what()));
        return r;
    }
}

#endif // ENABLE_ONNX

} // namespace SamBody
