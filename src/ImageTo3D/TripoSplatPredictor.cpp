#include "TripoSplatPredictor.h"

#include "AppStorage.h"
#include "MeshRefine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QFile>
#include <QPainter>
#include <QSettings>
#include <QThread>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <random>

#ifdef ENABLE_ONNX
#include "AudioToFace/NpzReader.h"
#include "ModelFetch.h"
#include "OnnxRuntimeSettings.h"
#include <QSettings>
#include <onnxruntime_cxx_api.h>
#endif

namespace {

constexpr int kImageSize = 1024;
constexpr int kLatentTokens = 8192;
constexpr int kLatentChannels = 16;
constexpr int kCameraChannels = 5;
constexpr int kVaeNoiseChannels = 32;
constexpr int kVaeNoiseSize = 128;
constexpr int kGaussiansPerPoint = 32;
constexpr int kOctreeLevels = 8;
constexpr int kMinGaussians = 32768;
constexpr int kMaxGaussians = 262144;

const char* const kGraphs[] = {"dinov3", "vae", "flow", "octree", "gs"};

constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/triposplat/";
constexpr const char* kBaseUrlSettingsKey = "ai/triposplatModelBaseUrl";

// SHA-256 of every hosted file, verified on download AND on an existing file
// (default hosting only — a mirror override may serve a different export).
// Produced by scripts/export-triposplat-onnx.py; re-export => re-pin.
struct Digest { const char* file; const char* sha256; };
constexpr Digest kDigests[] = {
    {"triposplat_dinov3.onnx",      "e22701651ae8ce3c9ca5c196edc0d3dfe0e8f3df479a68f7cdfdb135901d0735"},
    {"triposplat_dinov3.onnx.data", "163bdfaee3be63958d540ac710b4901b932909abb61cecba86eeaab1518df6f5"},
    {"triposplat_flow.onnx",        "00f13724662beac07e5a9d4982ea709cb7a5abd78973c9aa2229486bba61130a"},
    {"triposplat_flow.onnx.data",   "f7071def9d9bff7741e71d9c293ef1aac180f334aed80f7b9298294e6bc81dc6"},
    {"triposplat_gs.onnx",          "925c2a4d920fbca59ccf6938247f3e1fc63a35e87bef09564bcec2e44c94decf"},
    {"triposplat_gs.onnx.data",     "616a1b482875d332f67adf0e21fa59b0c6bad816db267be8382681129c03f78e"},
    {"triposplat_octree.onnx",      "baf9216ce42a11ce5a3b649da92e51449270c88a5604b3c1688962ed68b56b48"},
    {"triposplat_octree.onnx.data", "70f2d2b8424f826644b14a728b7cbed98eb95446b75f58fb1678b6ed1c9f7b0c"},
    {"triposplat_vae.onnx",         "f221f55aa13db3469d3469b4a5231a36ae4af87f759a13b2c104c1dac9df8d54"},
    {"triposplat_vae.onnx.data",    "0c0498815644ab3bd24b21ce42588954e206392c9fe1db77844783a258e336e8"},
};

// Licence texts that must travel with the weights: the DINOv3 License
// (sec. 1.b.i) requires a copy with any redistribution of DINOv3 weights or
// derivatives — the dinov3 graph is one.
const char* const kLicenseFiles[] = {"LICENSE-TripoSplat.txt", "LICENSE-DINOv3.txt"};

MeshGenPredictor::Result failResult(const QString& msg)
{
    MeshGenPredictor::Result r;
    r.error = msg;
    return r;
}

bool hasRealAlpha(const QImage& img)
{
    if (!img.hasAlphaChannel())
        return false;
    const QImage a = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < a.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        for (int x = 0; x < a.width(); ++x)
            if (qAlpha(row[x]) < 255)
                return true;
    }
    return false;
}

} // namespace

TripoSplatPredictor::Options::Options() = default;

QString TripoSplatPredictor::modelDir()
{
    return QDir(AppStorage::aiModelsRoot()).filePath(QStringLiteral("triposplat/"));
}

QStringList TripoSplatPredictor::modelFiles()
{
    QStringList files;
    for (const char* g : kGraphs) {
        files << QStringLiteral("triposplat_%1.onnx").arg(QLatin1String(g));
        files << QStringLiteral("triposplat_%1.onnx.data").arg(QLatin1String(g));
    }
    return files;
}

bool TripoSplatPredictor::modelsPresent()
{
    const QDir dir(modelDir());
    for (const QString& f : modelFiles())
        if (!QFileInfo::exists(dir.filePath(f)))
            return false;
    return true;
}

QString TripoSplatPredictor::baseUrl()
{
    QString base = QSettings().value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
    if (base.isEmpty()) {
        const QByteArray env = qgetenv("QTMESH_TRIPOSPLAT_MODEL_BASE_URL");
        base = env.isEmpty() ? QString::fromLatin1(kDefaultModelBaseUrl)
                             : QString::fromUtf8(env);
    }
    if (!base.endsWith(QLatin1Char('/')))
        base += QLatin1Char('/');
    return base;
}

bool TripoSplatPredictor::isDefaultHosting(const QString& base)
{
    return base == QLatin1String(kDefaultModelBaseUrl);
}

QString TripoSplatPredictor::expectedSha256(const QString& fileName)
{
    for (const Digest& d : kDigests)
        if (fileName == QLatin1String(d.file))
            return QString::fromLatin1(d.sha256);
    return {};
}

QStringList TripoSplatPredictor::licenseFiles()
{
    QStringList out;
    for (const char* lic : kLicenseFiles)
        out << QLatin1String(lic);
    return out;
}

// ---- Pure helpers ---------------------------------------------------------------

QImage TripoSplatPredictor::prepareImage(const QImage& input)
{
    if (input.isNull())
        return {};
    QImage img = input.convertToFormat(QImage::Format_ARGB32);
    // Upstream scales the short side to the canvas size first.
    const double s = double(kImageSize) / std::min(img.width(), img.height());
    img = img.scaled(std::max(1, int(std::lround(img.width() * s))),
                     std::max(1, int(std::lround(img.height() * s))),
                     Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const int w = img.width(), h = img.height();

    // Erode the alpha by one pixel (3x3 minimum) so matte-border colour does
    // not bleed into the composite.
    std::vector<uint8_t> alpha(size_t(w) * h);
    for (int y = 0; y < h; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < w; ++x)
            alpha[size_t(y) * w + x] = uint8_t(qAlpha(row[x]));
    }
    std::vector<uint8_t> eroded(alpha.size());
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t m = 255;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    // Pixels past the edge do not count (PIL MinFilter clamps).
                    const int a = std::clamp(x + dx, 0, w - 1);
                    const int b = std::clamp(y + dy, 0, h - 1);
                    m = std::min(m, alpha[size_t(b) * w + a]);
                }
            eroded[size_t(y) * w + x] = m;
            if (m > 0) {
                x0 = std::min(x0, x); y0 = std::min(y0, y);
                x1 = std::max(x1, x); y1 = std::max(y1, y);
            }
        }
    if (x1 < 0) {                 // nothing opaque: use the whole frame
        x0 = 0; y0 = 0; x1 = w - 1; y1 = h - 1;
    }
    for (int y = 0; y < h; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb p = row[x];
            row[x] = qRgba(qRed(p), qGreen(p), qBlue(p), eroded[size_t(y) * w + x]);
        }
    }

    // Square crop 1.2x the bbox around its centre; outside the image is
    // transparent (PIL crop semantics). Upstream measures the bbox as
    // max - min (inclusive pixel indices) and truncates the crop box to int.
    const double cx = (x0 + x1) / 2.0, cy = (y0 + y1) / 2.0;
    const double half = std::max(x1 - x0, y1 - y0) / 2.0 * 1.2;
    const int cl = int(cx - half), ct = int(cy - half);
    const int cr = int(cx + half), cb = int(cy + half);
    const int cw = std::max(1, cr - cl), ch = std::max(1, cb - ct);
    QImage crop(cw, ch, QImage::Format_ARGB32);
    crop.fill(Qt::transparent);
    {
        QPainter p(&crop);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(-cl, -ct, img);
    }
    const QImage scaled = crop.scaled(kImageSize, kImageSize, Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation);
    // Composite over black: rgb * alpha.
    QImage out(kImageSize, kImageSize, QImage::Format_RGB888);
    for (int y = 0; y < kImageSize; ++y) {
        const QRgb* src = reinterpret_cast<const QRgb*>(scaled.constScanLine(y));
        uchar* dst = out.scanLine(y);
        for (int x = 0; x < kImageSize; ++x) {
            const int a = qAlpha(src[x]);
            dst[3 * x + 0] = uchar((qRed(src[x]) * a + 127) / 255);
            dst[3 * x + 1] = uchar((qGreen(src[x]) * a + 127) / 255);
            dst[3 * x + 2] = uchar((qBlue(src[x]) * a + 127) / 255);
        }
    }
    return out;
}

std::vector<double> TripoSplatPredictor::timeSchedule(int steps, double shift)
{
    steps = std::max(1, steps);
    std::vector<double> t(size_t(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        const double l = 1.0 - double(i) / steps;
        t[size_t(i)] = shift * l / (1.0 + (shift - 1.0) * l);
    }
    return t;
}

void TripoSplatPredictor::systematicCounts(const float* probs, int count, double u, int out[8])
{
    std::fill(out, out + 8, 0);
    if (count <= 0)
        return;
    double p[8], sum = 0.0;
    for (int i = 0; i < 8; ++i) {
        p[i] = std::max(0.0, double(probs[i]));
        sum += p[i];
    }
    double cdf[8], acc = 0.0;
    for (int i = 0; i < 8; ++i) {
        acc += (sum > 0.0) ? p[i] / std::max(sum, 1.0) : 1.0 / 8.0;
        cdf[i] = std::min(acc, 1.0 - 1e-12);
    }
    for (int k = 0; k < count; ++k) {
        const double us = std::min(u / count + double(k) / count, 1.0 - 1e-12);
        int idx = int(std::lower_bound(cdf, cdf + 8, us) - cdf);
        out[std::min(idx, 7)] += 1;
    }
}

// Stage 6, shared by generation and the import hook: splats -> mesh in the
// editor frame, plus the splat positions/colours the colour bake samples.
TripoSplatPredictor::Output TripoSplatPredictor::meshSplats(
    Output out, const Options& opts, const MeshGenPredictor::ProgressFn& progress,
    const QString& warning)
{
    auto fail = [&](const QString& m) { out.mesh = failResult(m); return out; };
    const SplatToMesh::Splats& sp = out.splatsModelFrame;
    SplatToMesh::Options mo;
    mo.resolution = opts.meshResolution;
    mo.progress = [&](int done, int total) {
        return !progress || progress(MeshGenPredictor::Stage::Refine, done, total);
    };
    SplatToMesh::Result mesh = SplatToMesh::extract(sp, mo);
    if (!mesh.ok)
        return fail(mesh.cancelled ? QStringLiteral("cancelled") : mesh.error);
    SplatToMesh::toEditorFrame(mesh.positions);
    if (opts.smoothMesh && opts.smoothIterations > 0)
        MeshRefine::taubinSmooth(mesh.positions, mesh.indices, opts.smoothIterations);

    out.splatPositionsEditor = sp.xyz;
    SplatToMesh::toEditorFrame(out.splatPositionsEditor);
    out.splatColors.resize(sp.fdc.size());
    for (size_t i = 0; i < sp.fdc.size(); ++i)
        out.splatColors[i] = std::clamp(SplatToMesh::shDcToColor(sp.fdc[i]), 0.0f, 1.0f);
    out.voxelSize = mesh.voxelSize;

    MeshGenPredictor::Result& r = out.mesh;
    r.positions = std::move(mesh.positions);
    r.indices = std::move(mesh.indices);
    r.vertexCount = int(r.positions.size() / 3);
    r.triangleCount = int(r.indices.size() / 3);
    r.usedModel = true;
    r.warning = warning;
    r.ok = true;
    fprintf(stderr, "[triposplat] %d splats used -> %d verts / %d tris\n",
            mesh.splatsUsed, r.vertexCount, r.triangleCount);
    return out;
}

#ifndef ENABLE_ONNX

bool TripoSplatPredictor::isAvailable() { return false; }
bool TripoSplatPredictor::ensureModelsBlocking(QString* error, const std::function<bool()>&)
{
    if (error)
        *error = QStringLiteral("TripoSplat needs an ONNX-enabled build "
                                "(rebuild with -DENABLE_ONNX=ON).");
    return false;
}
TripoSplatPredictor::Output TripoSplatPredictor::predict(
    const QImage&, const Options&, const MeshGenPredictor::ProgressFn&)
{
    Output o;
    o.mesh = failResult(QStringLiteral("TripoSplat needs an ONNX-enabled build "
                                       "(rebuild with -DENABLE_ONNX=ON)."));
    return o;
}

#else // ENABLE_ONNX

bool TripoSplatPredictor::isAvailable() { return true; }

bool TripoSplatPredictor::ensureModelsBlocking(QString* error,
                                               const std::function<bool()>& cancelled)
{
    // The import hook meshes an existing .ply and needs no model.
    if (!qEnvironmentVariableIsEmpty("QTMESH_TRIPOSPLAT_IMPORT"))
        return true;
    const QString base = baseUrl();
    const bool defaultHosting = base == QLatin1String(kDefaultModelBaseUrl);
    const bool noDownload = !qEnvironmentVariableIsEmpty("QTMESH_TRIPOSPLAT_NO_DOWNLOAD");
    const QDir dir(modelDir());
    QDir().mkpath(dir.absolutePath());

    // Licence texts FIRST, before any early return: the DINOv3 License must
    // travel with the weights even when the graphs arrived another way (the
    // Model Catalog, an earlier run whose best-effort licence fetch failed).
    for (const char* lic : kLicenseFiles) {
        const QString dest = dir.filePath(QLatin1String(lic));
        if (QFileInfo::exists(dest) || noDownload)
            continue;
        ModelFetch::Request req;
        req.url = base + QLatin1String(lic);
        req.destination = dest;
        req.label = QStringLiteral("TripoSplat licence");
        req.timeoutMs = 60000;
        req.cancelled = cancelled;
        ModelFetch::ensureBlocking(req);   // best effort, small
    }

    // Fast path: every graph present AND already verified at its current
    // size + mtime (marker written after a full verification pass), so 6 GB
    // is not re-hashed on every call. A file replaced or touched since then
    // fails the marker and goes through the digest check again.
    const QString marker = dir.filePath(QStringLiteral(".verified"));
    auto fingerprint = [&]() {
        QStringList lines;
        for (const QString& f : modelFiles()) {
            const QFileInfo fi(dir.filePath(f));
            if (!fi.exists())
                return QString();
            lines << QStringLiteral("%1 %2 %3 %4")
                         .arg(f).arg(fi.size())
                         .arg(fi.lastModified().toMSecsSinceEpoch())
                         .arg(expectedSha256(f));
        }
        return lines.join(QLatin1Char('\n'));
    };
    if (modelsPresent()) {
        QFile m(marker);
        const QString fp = fingerprint();
        if (!defaultHosting
            || (m.open(QIODevice::ReadOnly) && QString::fromUtf8(m.readAll()) == fp))
            return true;
    }
    if (noDownload && !modelsPresent()) {
        if (error)
            *error = QStringLiteral("TripoSplat models are not downloaded "
                                    "(QTMESH_TRIPOSPLAT_NO_DOWNLOAD is set).");
        return false;
    }

    for (const QString& f : modelFiles()) {
        const QString dest = dir.filePath(f);
        ModelFetch::Request req;
        req.url = base + f;
        req.destination = dest;
        req.label = QStringLiteral("TripoSplat %1").arg(f);
        req.timeoutMs = 3600000;
        req.cancelled = cancelled;
        if (defaultHosting)
            req.expectedSha256 = expectedSha256(f);
        // Without a digest (a mirror override) presence is all we can check.
        if (QFileInfo::exists(dest) && req.expectedSha256.isEmpty())
            continue;
        const ModelFetch::Outcome fo = ModelFetch::ensureBlocking(req);
        if (!fo.ok) {
            if (error)
                *error = fo.cancelled ? QStringLiteral("cancelled")
                       : fo.error.isEmpty()
                           ? QStringLiteral("TripoSplat model download failed (%1).").arg(f)
                           : fo.error;
            return false;
        }
    }
    if (!modelsPresent())
        return false;
    if (defaultHosting) {
        QFile m(marker);
        if (m.open(QIODevice::WriteOnly | QIODevice::Truncate))
            m.write(fingerprint().toUtf8());
    }
    return true;
}

namespace {

struct IoNames {
    std::vector<Ort::AllocatedStringPtr> holders;
    std::vector<const char*> in, out;
};

IoNames ioNames(Ort::Session& s)
{
    Ort::AllocatorWithDefaultOptions alloc;
    IoNames n;
    for (size_t i = 0; i < s.GetInputCount(); ++i) {
        n.holders.push_back(s.GetInputNameAllocated(i, alloc));
        n.in.push_back(n.holders.back().get());
    }
    for (size_t i = 0; i < s.GetOutputCount(); ++i) {
        n.holders.push_back(s.GetOutputNameAllocated(i, alloc));
        n.out.push_back(n.holders.back().get());
    }
    return n;
}

// Feeds are bound BY NAME, so the C++ never depends on graph input order.
struct Feed {
    const char* name;
    Ort::Value value;
};

std::vector<Ort::Value> runByName(Ort::Session& s, std::vector<Feed>& feeds)
{
    IoNames io = ioNames(s);
    std::vector<Ort::Value> values;
    std::vector<const char*> names;
    for (const char* want : io.in)
        for (Feed& f : feeds)
            if (std::strcmp(f.name, want) == 0) {
                names.push_back(f.name);
                values.push_back(std::move(f.value));
            }
    if (names.size() != io.in.size())
        throw std::runtime_error("TripoSplat: graph input mismatch");
    return s.Run(Ort::RunOptions{nullptr}, names.data(), values.data(), values.size(),
                  io.out.data(), io.out.size());
}

template <typename T>
Ort::Value tensor(std::vector<T>& data, std::initializer_list<int64_t> shape)
{
    static Ort::MemoryInfo mem =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const std::vector<int64_t> s(shape);
    return Ort::Value::CreateTensor<T>(mem, data.data(), data.size(), s.data(), s.size());
}

std::vector<float> copyOut(const Ort::Value& v)
{
    const float* d = v.GetTensorData<float>();
    return std::vector<float>(d, d + v.GetTensorTypeAndShapeInfo().GetElementCount());
}

// Random draws, either from the seeded generator or replayed from a fixture
// written by scripts/verify_triposplat_native.py --fixture.
class Draws {
public:
    explicit Draws(unsigned seed) : m_rng(seed) {}
    bool loadFixture(const QString& path, QString* error)
    {
        AudioToFace::NpzReader npz;
        if (!npz.open(path, error))
            return false;
        auto take = [&](const char* name, std::vector<float>& dst) {
            if (!npz.contains(QLatin1String(name)))
                return;
            dst = npz.read(QLatin1String(name)).data;
        };
        take("vae_noise", m_vae);
        take("latent_noise", m_latent);
        take("camera_noise", m_camera);
        take("octree_uniforms", m_uniforms);
        take("octree_jitter", m_jitter);
        take("feature1", feature1);
        take("feature2", feature2);
        m_fixture = true;
        return true;
    }
    bool fixture() const { return m_fixture; }
    std::vector<float> normal(size_t n, std::vector<float>& replay)
    {
        if (m_fixture && replay.size() == n)
            return replay;
        std::normal_distribution<float> g(0.0f, 1.0f);
        std::vector<float> v(n);
        for (float& x : v) x = g(m_rng);
        return v;
    }
    std::vector<float> vaeNoise(size_t n) { return normal(n, m_vae); }
    std::vector<float> latentNoise(size_t n) { return normal(n, m_latent); }
    std::vector<float> cameraNoise(size_t n) { return normal(n, m_camera); }
    double uniform()
    {
        if (m_fixture && m_u < m_uniforms.size())
            return m_uniforms[m_u++];
        return std::uniform_real_distribution<double>(0.0, 1.0)(m_rng);
    }
    float jitter()
    {
        if (m_fixture && m_j < m_jitter.size())
            return m_jitter[m_j++];
        return std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng);
    }
    std::vector<float> feature1, feature2;   // fixture only: skip the encoders

private:
    std::mt19937 m_rng;  // NOSONAR - deterministic seed by design (not security)
    std::vector<float> m_vae, m_latent, m_camera, m_uniforms, m_jitter;
    size_t m_u = 0, m_j = 0;
    bool m_fixture = false;
};

} // namespace

TripoSplatPredictor::Output TripoSplatPredictor::predict(
    const QImage& image, const Options& opts, const MeshGenPredictor::ProgressFn& progress)
{
    using Stage = MeshGenPredictor::Stage;
    Output out;
    auto fail = [&](const QString& m) { out.mesh = failResult(m); return out; };
    auto report = [&](Stage s, int done, int total) {
        return !progress || progress(s, done, total);
    };

    if (image.isNull())
        return fail(QStringLiteral("TripoSplat: input image is empty."));

    // Dev/test hook: mesh an existing 3DGS .ply instead of running inference
    // (iterating on the mesh + texture stages costs ~25 min of CPU otherwise).
    const QString importPly = qEnvironmentVariable("QTMESH_TRIPOSPLAT_IMPORT");
    if (!importPly.isEmpty()) {
        QString err;
        out.splatsModelFrame = SplatToMesh::readPly(importPly, &err);
        if (out.splatsModelFrame.count() == 0)
            return fail(QStringLiteral("TripoSplat import: %1").arg(err));
        fprintf(stderr, "[triposplat] meshing imported splats %s\n", qPrintable(importPly));
        return meshSplats(std::move(out), opts, progress, QString());
    }
    if (!modelsPresent())
        return fail(QStringLiteral(
            "TripoSplat models are not downloaded — they download on first use "
            "(~6 GB) into %1.").arg(modelDir()));

    Draws draws(opts.seed);
    const QString fixturePath = qEnvironmentVariable("QTMESH_TRIPOSPLAT_FIXTURE");
    if (!fixturePath.isEmpty()) {
        QString err;
        if (!draws.loadFixture(fixturePath, &err))
            return fail(QStringLiteral("TripoSplat fixture: %1").arg(err));
        fprintf(stderr, "[triposplat] replaying fixture %s\n", qPrintable(fixturePath));
    }

    // ---- 1. matte + prepare ----------------------------------------------------
    // Force the matte when the caller asked for it (GUI checkbox / --remove-bg):
    // a few transparent edge pixels must NOT skip rembg and leave the room in
    // the reconstruction. Without a request, still matte a fully-opaque photo
    // — the model is trained on a cut-out over black.
    QString warning;
    QImage subject = image;
    const bool needMatte = opts.removeBackground || !hasRealAlpha(image);
    if (!draws.fixture() && needMatte) {
        if (!report(Stage::Encode, 0, 2))
            return fail(QStringLiteral("cancelled"));
        const bool onMainThread = QCoreApplication::instance()
            && QThread::currentThread() == QCoreApplication::instance()->thread();
        BackgroundRemover::Quality usedQ = opts.mattingQuality;
        QString model;
        if (onMainThread) {
            model = BackgroundRemover::resolveModelBlocking(opts.mattingQuality, &usedQ);
        } else if (BackgroundRemover::modelPresent(opts.mattingQuality)) {
            model = BackgroundRemover::modelPath(opts.mattingQuality);
        } else if (BackgroundRemover::modelPresent(BackgroundRemover::Quality::Fast)) {
            usedQ = BackgroundRemover::Quality::Fast;
            model = BackgroundRemover::modelPath(usedQ);
        }
        BackgroundRemover::Options bg;
        bg.keepAlpha = true;
        bg.foregroundRatio = 0.0f;   // prepareImage does upstream's own crop
        bg.quality = usedQ;
        BackgroundRemover::Result cut =
            BackgroundRemover::removeBackground(image, model, bg);
        // Best (BiRefNet) used to hard-fail under CoreML and silently leave
        // the room in the mesh — CPU-only is fixed in BackgroundRemover, but
        // still fall back to U²-Net if Best fails for any other reason.
        if (!(cut.ok && cut.usedModel)
            && usedQ == BackgroundRemover::Quality::Best
            && BackgroundRemover::modelPresent(BackgroundRemover::Quality::Fast)) {
            usedQ = BackgroundRemover::Quality::Fast;
            bg.quality = usedQ;
            model = BackgroundRemover::modelPath(usedQ);
            cut = BackgroundRemover::removeBackground(image, model, bg);
        }
        if (cut.ok && cut.usedModel) {
            subject = cut.image;
            fprintf(stderr, "[triposplat] matte: %s\n",
                    usedQ == BackgroundRemover::Quality::Best
                        ? "BiRefNet" : "U²-Net");
        } else {
            warning = QStringLiteral("background removal unavailable (%1); the "
                                     "whole image is treated as the subject.")
                          .arg(cut.error.isEmpty() ? QStringLiteral("no model")
                                                   : cut.error);
            fprintf(stderr, "[triposplat] matte FAILED: %s\n",
                    qPrintable(warning));
        }
    }
    const QImage prepared = draws.fixture()
        ? image.convertToFormat(QImage::Format_RGB888).scaled(
              kImageSize, kImageSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        : prepareImage(subject);

    const int numGaussians =
        std::clamp(opts.numGaussians / kGaussiansPerPoint * kGaussiansPerPoint,
                   kMinGaussians, kMaxGaussians);
    const int steps = std::clamp(opts.flowSteps, 1, 100);

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_triposplat");
        Ort::SessionOptions so;
        OnnxRuntimeSettings::SessionConfig cfg;
#ifdef __APPLE__
        // CoreML cannot compile these graphs (MPS matmul assertion aborts the
        // PROCESS, not just the session) — stay on the CPU EP.
        cfg.appendGpu = false;
#endif
        OnnxRuntimeSettings::configureSessionOptions(so, cfg);
        const QDir dir(modelDir());
        auto open = [&](const char* g) {
            const QString p = dir.filePath(QStringLiteral("triposplat_%1.onnx")
                                               .arg(QLatin1String(g)));
#ifdef _WIN32
            const std::wstring s = p.toStdWString();
#else
            const std::string s = p.toStdString();
#endif
            return Ort::Session(env, s.c_str(), so);
        };

        // ---- 2. encoders (one session at a time: DINOv3 alone is 3.4 GB) ------
        std::vector<float> img(size_t(3) * kImageSize * kImageSize);
        for (int y = 0; y < kImageSize; ++y) {
            const uchar* row = prepared.constScanLine(y);
            for (int x = 0; x < kImageSize; ++x)
                for (int c = 0; c < 3; ++c)
                    img[(size_t(c) * kImageSize + y) * kImageSize + x] = row[3 * x + c] / 255.0f;
        }
        std::vector<float> f1 = draws.feature1, f2 = draws.feature2;
        if (f1.empty()) {
            Ort::Session s = open("dinov3");
            std::vector<Feed> feeds;
            feeds.push_back({"image", tensor(img, {1, 3, kImageSize, kImageSize})});
            f1 = copyOut(runByName(s, feeds)[0]);
        }
        if (!report(Stage::Encode, 1, 2))
            return fail(QStringLiteral("cancelled"));
        std::vector<float> vaeNoise =
            draws.vaeNoise(size_t(kVaeNoiseChannels) * kVaeNoiseSize * kVaeNoiseSize);
        if (f2.empty()) {
            Ort::Session s = open("vae");
            std::vector<Feed> feeds;
            feeds.push_back({"image", tensor(img, {1, 3, kImageSize, kImageSize})});
            feeds.push_back({"noise", tensor(vaeNoise, {1, kVaeNoiseChannels,
                                                        kVaeNoiseSize, kVaeNoiseSize})});
            f2 = copyOut(runByName(s, feeds)[0]);
        }
        std::vector<float>().swap(img);
        if (!report(Stage::Encode, 2, 2))
            return fail(QStringLiteral("cancelled"));
        const int64_t condTokens = int64_t(f1.size() / 1280);

        // ---- 3. flow matching: Euler with classifier-free guidance ------------
        std::vector<float> latent = draws.latentNoise(size_t(kLatentTokens) * kLatentChannels);
        std::vector<float> camera = draws.cameraNoise(kCameraChannels);
        {
            Ort::Session flow = open("flow");
            const std::vector<double> ts = timeSchedule(steps, opts.shift);
            const bool cfg = opts.guidanceScale > 1.0f;
            std::vector<float> zero1, zero2;
            if (cfg) {
                zero1.assign(f1.size(), 0.0f);
                zero2.assign(f2.size(), 0.0f);
            }
            auto velocity = [&](std::vector<float>& c1, std::vector<float>& c2, float t,
                                std::vector<float>& vl, std::vector<float>& vc) {
                std::vector<float> tv{t};
                std::vector<float> lat = latent, cam = camera;
                std::vector<Feed> feeds;
                feeds.push_back({"latent", tensor(lat, {1, kLatentTokens, kLatentChannels})});
                feeds.push_back({"camera", tensor(cam, {1, 1, kCameraChannels})});
                feeds.push_back({"t", tensor(tv, {1})});
                feeds.push_back({"feature1", tensor(c1, {1, condTokens, 1280})});
                feeds.push_back({"feature2", tensor(c2, {1, condTokens, 128})});
                auto res = runByName(flow, feeds);
                vl = copyOut(res[0]);
                vc = copyOut(res[1]);
            };
            std::vector<float> vl, vc, nl, nc;
            for (int i = 0; i < steps; ++i) {
                if (!report(Stage::Denoise, i, steps))
                    return fail(QStringLiteral("cancelled"));
                const float t = float(1000.0 * ts[size_t(i)]);
                velocity(f1, f2, t, vl, vc);
                if (cfg) {
                    velocity(zero1, zero2, t, nl, nc);
                    const float g = opts.guidanceScale;
                    for (size_t k = 0; k < vl.size(); ++k) vl[k] = g * vl[k] - (g - 1) * nl[k];
                    for (size_t k = 0; k < vc.size(); ++k) vc[k] = g * vc[k] - (g - 1) * nc[k];
                }
                const float dt = float(ts[size_t(i)] - ts[size_t(i) + 1]);
                for (size_t k = 0; k < latent.size(); ++k) latent[k] -= vl[k] * dt;
                for (size_t k = 0; k < camera.size(); ++k) camera[k] -= vc[k] * dt;
            }
            if (!report(Stage::Denoise, steps, steps))
                return fail(QStringLiteral("cancelled"));
        }
        std::vector<float>().swap(f1);
        std::vector<float>().swap(f2);

        // ---- 4. octree point placement (coarse to fine) -----------------------
        const int numPoints = numGaussians / kGaussiansPerPoint;
        std::vector<std::array<int, 3>> coords{{0, 0, 0}};
        std::vector<int> counts{numPoints};
        {
            Ort::Session octree = open("octree");
            for (int lv = 1; lv <= kOctreeLevels; ++lv) {
                if (!report(Stage::Decode, lv - 1, kOctreeLevels + 1))
                    return fail(QStringLiteral("cancelled"));
                const float resP = float(1 << (lv - 1));
                std::vector<float> parents(coords.size() * 3);
                for (size_t r = 0; r < coords.size(); ++r)
                    for (int a = 0; a < 3; ++a)
                        parents[3 * r + a] = (float(coords[r][a]) + 0.5f) / resP;
                std::vector<int64_t> level{int64_t(1) << lv};
                std::vector<float> lat = latent;
                std::vector<Feed> feeds;
                feeds.push_back({"points", tensor(parents, {1, int64_t(coords.size()), 3})});
                feeds.push_back({"level", tensor(level, {1})});
                feeds.push_back({"latent", tensor(lat, {1, kLatentTokens, kLatentChannels})});
                const std::vector<float> logits = copyOut(runByName(octree, feeds)[0]);
                std::vector<std::array<int, 3>> nextCoords;
                std::vector<int> nextCounts;
                for (size_t r = 0; r < coords.size(); ++r) {
                    float probs[8];
                    const float* l = &logits[8 * r];
                    const float mx = *std::max_element(l, l + 8);
                    float sum = 0.0f;
                    for (int c = 0; c < 8; ++c) sum += (probs[c] = std::exp(l[c] - mx));
                    for (float& p : probs) p /= sum;
                    int split[8];
                    systematicCounts(probs, counts[r], draws.uniform(), split);
                    for (int c = 0; c < 8; ++c) {
                        if (split[c] <= 0)
                            continue;
                        // Child order matches upstream: i fastest, then j, then k.
                        nextCoords.push_back({coords[r][0] * 2 + (c & 1),
                                              coords[r][1] * 2 + ((c >> 1) & 1),
                                              coords[r][2] * 2 + ((c >> 2) & 1)});
                        nextCounts.push_back(split[c]);
                    }
                }
                coords.swap(nextCoords);
                counts.swap(nextCounts);
            }
        }
        std::vector<float> points;
        points.reserve(size_t(numPoints) * 3);
        const float res = float(1 << kOctreeLevels);
        for (size_t r = 0; r < coords.size(); ++r)
            for (int k = 0; k < counts[r]; ++k)
                for (int a = 0; a < 3; ++a)
                    points.push_back((float(coords[r][a]) + draws.jitter()) / res);

        // ---- 5. Gaussian decoder -----------------------------------------------
        if (!report(Stage::Decode, kOctreeLevels, kOctreeLevels + 1))
            return fail(QStringLiteral("cancelled"));
        SplatToMesh::Splats& sp = out.splatsModelFrame;
        {
            Ort::Session gs = open("gs");
            std::vector<Feed> feeds;
            const int64_t P = int64_t(points.size() / 3);
            feeds.push_back({"points", tensor(points, {1, P, 3})});
            feeds.push_back({"latent", tensor(latent, {1, kLatentTokens, kLatentChannels})});
            auto res5 = runByName(gs, feeds);
            sp.xyz = copyOut(res5[0]);
            sp.fdc = copyOut(res5[1]);
            sp.opacity = copyOut(res5[2]);
            sp.scale = copyOut(res5[3]);
            sp.rotation = copyOut(res5[4]);
        }
        if (!report(Stage::Decode, kOctreeLevels + 1, kOctreeLevels + 1))
            return fail(QStringLiteral("cancelled"));
    } catch (const Ort::Exception& e) {
        return fail(QStringLiteral("TripoSplat ONNX error: %1").arg(QString::fromUtf8(e.what())));
    } catch (const std::exception& e) {
        return fail(QStringLiteral("TripoSplat error: %1").arg(QString::fromUtf8(e.what())));
    }

    return meshSplats(std::move(out), opts, progress, warning);
}

#endif // ENABLE_ONNX
