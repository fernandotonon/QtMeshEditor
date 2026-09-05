#include "BackgroundRemover.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <vector>

#ifdef ENABLE_ONNX
#include "ModelDownloader.h"
#include "OnnxRuntimeSettings.h"
#include <QEventLoop>
#include <QSettings>
#include <QTimer>
#include <onnxruntime_cxx_api.h>
#include <string>
#include <unordered_map>
#endif

namespace {

constexpr const char* kModelFile = "u2net.onnx";
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/rembg/";
constexpr const char* kBaseUrlSettingsKey = "ai/rembgModelBaseUrl";
constexpr const char* kModelLabel = "U2Net background-removal model";

constexpr int kNet = 320;   // u2net input size

QString modelDir()
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("ai_models/rembg/"));
}

} // namespace

BackgroundRemover::Options::Options() = default;

QString BackgroundRemover::modelPath()
{
    return QDir(modelDir()).filePath(QString::fromLatin1(kModelFile));
}

bool BackgroundRemover::modelPresent()
{
    return QFileInfo::exists(modelPath());
}

#ifndef ENABLE_ONNX

bool BackgroundRemover::isAvailable() { return false; }
QString BackgroundRemover::ensureModelBlocking() { return {}; }

BackgroundRemover::Result BackgroundRemover::removeBackground(const QImage& image,
                                                             const QString&,
                                                             const Options&)
{
    Result r;
    r.image = image;   // pass through unchanged
    r.error = QStringLiteral("Background removal needs an ONNX build "
                             "(rebuild with -DENABLE_ONNX). Using the image as-is.");
    return r;
}

#else // ENABLE_ONNX

bool BackgroundRemover::isAvailable() { return true; }

QString BackgroundRemover::ensureModelBlocking()
{
    const QString dst = modelPath();
    if (QFileInfo::exists(dst))
        return dst;
    if (!qEnvironmentVariableIsEmpty("QTMESH_REMBG_NO_DOWNLOAD"))
        return {};

    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_REMBG_MODEL_BASE_URL");
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
    QEventLoop loop;
    bool ok = false, timedOut = false;
    auto onDone = QObject::connect(dl, &ModelDownloader::downloadCompleted, &loop,
        [&](const QString& name, const QString&) {
            if (name == QString::fromLatin1(kModelLabel)) { ok = true; loop.quit(); }
        });
    auto onErr = QObject::connect(dl, &ModelDownloader::downloadError, &loop,
        [&](const QString& name, const QString&) {
            if (name == QString::fromLatin1(kModelLabel)) { ok = false; loop.quit(); }
        });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() { timedOut = true; loop.quit(); });
    timeout.start(600000);   // 10 min — u2net is ~170 MB
    dl->startDownload(url, dst, QString::fromLatin1(kModelLabel));
    loop.exec();
    QObject::disconnect(onDone);
    QObject::disconnect(onErr);
    if (timedOut) dl->cancelDownload();
    return (ok && !timedOut && QFileInfo::exists(dst)) ? dst : QString();
}

BackgroundRemover::Result BackgroundRemover::removeBackground(const QImage& image,
                                                             const QString& modelPath,
                                                             const Options& opts)
{
    Result r;
    r.image = image;
    if (image.isNull()) { r.error = QStringLiteral("empty image"); return r; }
    if (!QFileInfo::exists(modelPath)) {
        r.error = QStringLiteral("U2Net model not found — using image as-is.");
        return r;
    }

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_rembg");
        Ort::SessionOptions so;
        OnnxRuntimeSettings::configureSessionOptions(so);
#ifdef _WIN32
        Ort::Session session(env, modelPath.toStdWString().c_str(), so);
#else
        Ort::Session session(env, modelPath.toStdString().c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // --- Preprocess: RGB → 320×320 → NCHW, ImageNet normalize --------------
        const QImage small = image.convertToFormat(QImage::Format_RGB888)
                                  .scaled(kNet, kNet, Qt::IgnoreAspectRatio,
                                          Qt::SmoothTransformation);
        const float mean[3] = {0.485f, 0.456f, 0.406f};
        const float stdv[3] = {0.229f, 0.224f, 0.225f};
        std::vector<float> in(static_cast<size_t>(3) * kNet * kNet);
        const size_t plane = static_cast<size_t>(kNet) * kNet;
        for (int y = 0; y < kNet; ++y) {
            const uchar* line = small.constScanLine(y);
            for (int x = 0; x < kNet; ++x) {
                const uchar* px = line + x * 3;
                for (int c = 0; c < 3; ++c)
                    in[c * plane + static_cast<size_t>(y) * kNet + x] =
                        (px[c] / 255.0f - mean[c]) / stdv[c];
            }
        }
        const int64_t inShape[4] = {1, 3, kNet, kNet};
        Ort::Value inTensor = Ort::Value::CreateTensor<float>(
            mem, in.data(), in.size(), inShape, 4);

        auto inName  = session.GetInputNameAllocated(0, alloc);
        auto outName = session.GetOutputNameAllocated(0, alloc);   // u2net: d0 is first/best
        const char* inN[]  = { inName.get() };
        const char* outN[] = { outName.get() };
        auto out = session.Run(Ort::RunOptions{nullptr}, inN, &inTensor, 1, outN, 1);

        // Validate the output tensor before reading it: a corrupt/wrong model could
        // return a non-float or too-small buffer, and the mask walk below would read
        // out of bounds. Require a float tensor with at least kNet*kNet elements.
        {
            const auto ti = out[0].GetTensorTypeAndShapeInfo();
            if (ti.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
                || ti.GetElementCount() < plane) {
                r.error = QStringLiteral("U2Net output shape/type unexpected — using image as-is.");
                return r;   // r.image is still the original
            }
        }
        const float* mask = out[0].GetTensorData<float>();   // [1,1,320,320], values ~[0,1]

        // u2net saliency is already ~[0,1] but not guaranteed; min-max normalize.
        float lo = 1e9f, hi = -1e9f;
        for (size_t i = 0; i < plane; ++i) { lo = std::min(lo, mask[i]); hi = std::max(hi, mask[i]); }
        const float range = (hi - lo) > 1e-6f ? (hi - lo) : 1.0f;

        // --- Build a full-res alpha by bilinear-upsampling the 320² mask -------
        const int W = image.width(), H = image.height();
        QImage rgb = image.convertToFormat(QImage::Format_RGB888);
        auto sampleMask = [&](float fx, float fy) -> float {
            // fx,fy in [0,1); bilinear on the 320² normalized mask.
            const float gx = std::clamp(fx * (kNet - 1), 0.0f, float(kNet - 1));
            const float gy = std::clamp(fy * (kNet - 1), 0.0f, float(kNet - 1));
            const int x0 = int(gx), y0 = int(gy);
            const int x1 = std::min(x0 + 1, kNet - 1), y1 = std::min(y0 + 1, kNet - 1);
            const float tx = gx - x0, ty = gy - y0;
            auto m = [&](int xx, int yy) {
                return (mask[static_cast<size_t>(yy) * kNet + xx] - lo) / range;
            };
            const float top = m(x0, y0) * (1 - tx) + m(x1, y0) * tx;
            const float bot = m(x0, y1) * (1 - tx) + m(x1, y1) * tx;
            return top * (1 - ty) + bot * ty;
        };

        // Full-res alpha (thresholded + feathered) and the subject bounding box.
        std::vector<float> alpha(static_cast<size_t>(W) * H, 0.0f);
        int bx0 = W, by0 = H, bx1 = -1, by1 = -1;
        int kept = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                float a = sampleMask(float(x) / std::max(1, W - 1),
                                     float(y) / std::max(1, H - 1));
                if (opts.feather > 0) {
                    // Soft band around the threshold, scaled by `feather` so the
                    // option actually widens/narrows the transition (default 2
                    // keeps the original 0.15 band).
                    const float band = 0.075f * static_cast<float>(opts.feather);
                    a = std::clamp((a - (opts.threshold - band)) / (2 * band), 0.0f, 1.0f);
                } else {
                    a = (a >= opts.threshold) ? 1.0f : 0.0f;
                }
                alpha[static_cast<size_t>(y) * W + x] = a;
            }
        }

        // --- Uniform-background color rescue (#978) ------------------------
        // U²-Net's 320² saliency reliably finds the subject's mass but can
        // MISS thin appendages — a T-pose goblin lost a whole arm to the
        // matte, and the 3D generation faithfully reproduced the amputation.
        // Character sheets/renders overwhelmingly sit on a near-uniform
        // background, which gives an independent, resolution-exact signal:
        // when the four corner patches agree on a background color, any
        // pixel whose color clearly differs from it is foreground — OR that
        // mask into the saliency alpha so a saliency miss can never clip
        // real geometry. Busy backgrounds (corners disagree) skip the
        // rescue entirely and behave exactly as before.
        {
            const QImage src32 = image.convertToFormat(QImage::Format_RGB32);
            auto patchStats = [&](int px, int py, double m[3], double& var) {
                double sum[3] = {0, 0, 0}, sq[3] = {0, 0, 0};
                int n = 0;
                px = std::max(0, px);
                py = std::max(0, py);
                for (int y = py; y < py + 24 && y < H; ++y) {
                    const QRgb* line =
                        reinterpret_cast<const QRgb*>(src32.constScanLine(y));
                    for (int x = px; x < px + 24 && x < W; ++x) {
                        const double c[3] = {double(qRed(line[x])),
                                             double(qGreen(line[x])),
                                             double(qBlue(line[x]))};
                        for (int k = 0; k < 3; ++k) {
                            sum[k] += c[k];
                            sq[k] += c[k] * c[k];
                        }
                        ++n;
                    }
                }
                var = 0;
                for (int k = 0; k < 3; ++k) {
                    m[k] = sum[k] / std::max(1, n);
                    var += sq[k] / std::max(1, n) - m[k] * m[k];
                }
            };
            double c0[3], c1[3], c2[3], c3[3], v0, v1, v2, v3;
            // Clamp the patch origins: an image under 24px would otherwise
            // hand negative coordinates to constScanLine/line[x].
            const int px1 = std::max(0, W - 24);
            const int py1 = std::max(0, H - 24);
            patchStats(0, 0, c0, v0);
            patchStats(px1, 0, c1, v1);
            patchStats(0, py1, c2, v2);
            patchStats(px1, py1, c3, v3);
            double bg[3], spread = 0;
            for (int k = 0; k < 3; ++k) {
                bg[k] = (c0[k] + c1[k] + c2[k] + c3[k]) / 4.0;
                const double mx = std::max({c0[k], c1[k], c2[k], c3[k]});
                const double mn = std::min({c0[k], c1[k], c2[k], c3[k]});
                spread = std::max(spread, mx - mn);
            }
            const double maxVar = std::max({v0, v1, v2, v3});
            if (spread < 12.0 && maxVar < 90.0) {
                // Distance threshold above the background's own noise floor.
                const double thr =
                    std::max(28.0, 6.0 * std::sqrt(std::max(0.0, maxVar)));
                for (int y = 0; y < H; ++y) {
                    const QRgb* line =
                        reinterpret_cast<const QRgb*>(src32.constScanLine(y));
                    for (int x = 0; x < W; ++x) {
                        float& a = alpha[static_cast<size_t>(y) * W + x];
                        if (a >= 1.0f)
                            continue;
                        const double d = std::abs(qRed(line[x]) - bg[0])
                                       + std::abs(qGreen(line[x]) - bg[1])
                                       + std::abs(qBlue(line[x]) - bg[2]);
                        if (d > thr)
                            a = 1.0f;
                    }
                }
            }
        }

        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const float a = alpha[static_cast<size_t>(y) * W + x];
                if (a > 0.5f) {
                    ++kept;
                    bx0 = std::min(bx0, x); by0 = std::min(by0, y);
                    bx1 = std::max(bx1, x); by1 = std::max(by1, y);
                }
            }
        }
        // Bad segmentation → don't hand TripoSR a blank image.
        if (kept < (W * H) / 200 || bx1 < bx0 || by1 < by0) {
            r.error = QStringLiteral("segmentation kept too little — using image as-is.");
            r.image = image;
            return r;
        }

        // ---- Compose over GRAY, cropped + re-padded to `foregroundRatio` ------
        // TripoSR's resize_foreground: crop to the subject bbox, pad to a square,
        // then pad again so the subject fills `ratio` of the frame — CENTERED.
        // This is what stops leftover background margin being reconstructed as a
        // slab of geometry, and matches the model's training framing.
        auto srcAt = [&](int x, int y, int c) -> float {
            return rgb.constScanLine(y)[x * 3 + c];
        };
        auto alphaAt = [&](int x, int y) -> float { return alpha[static_cast<size_t>(y) * W + x]; };

        const int fgW = bx1 - bx0 + 1, fgH = by1 - by0 + 1;
        const int square = std::max(fgW, fgH);
        const float ratio = (opts.foregroundRatio > 0.05f && opts.foregroundRatio <= 1.0f)
                                ? opts.foregroundRatio : 0.85f;
        const int outSz = std::max(8, int(square / ratio));
        // Top-left of the subject square within the padded output.
        const int sqOffX = (outSz - fgW) / 2, sqOffY = (outSz - fgH) / 2;

        // keepAlpha (TRELLIS.2): carry the matte in a real alpha channel
        // instead of blending it into the RGB — the consumer premultiplies
        // itself and this is what keeps the upstream (non-commercial) rembg
        // model from ever being needed.
        QImage composed(outSz, outSz,
                        opts.keepAlpha ? QImage::Format_RGBA8888
                                       : QImage::Format_RGB888);
        if (opts.keepAlpha)
            composed.fill(QColor(opts.bgR, opts.bgG, opts.bgB, 0));
        else
            composed.fill(qRgb(opts.bgR, opts.bgG, opts.bgB));
        const int bpp = opts.keepAlpha ? 4 : 3;
        for (int oy = 0; oy < outSz; ++oy) {
            uchar* dp = composed.scanLine(oy);
            const int sy = by0 + (oy - sqOffY);
            for (int ox = 0; ox < outSz; ++ox) {
                const int sx = bx0 + (ox - sqOffX);
                if (sx < 0 || sy < 0 || sx >= W || sy >= H) continue;   // stays bg
                const float a = alphaAt(sx, sy);
                if (a <= 0.0f) continue;
                uchar* px = dp + ox * bpp;
                if (opts.keepAlpha) {
                    px[0] = uchar(srcAt(sx, sy, 0) + 0.5f);
                    px[1] = uchar(srcAt(sx, sy, 1) + 0.5f);
                    px[2] = uchar(srcAt(sx, sy, 2) + 0.5f);
                    px[3] = uchar(a * 255.0f + 0.5f);
                } else {
                    px[0] = uchar(srcAt(sx, sy, 0) * a + opts.bgR * (1 - a) + 0.5f);
                    px[1] = uchar(srcAt(sx, sy, 1) * a + opts.bgG * (1 - a) + 0.5f);
                    px[2] = uchar(srcAt(sx, sy, 2) * a + opts.bgB * (1 - a) + 0.5f);
                }
            }
        }

        r.ok = true;
        r.usedModel = true;
        r.image = composed;
        return r;
    } catch (const Ort::Exception& e) {
        r.error = QStringLiteral("rembg ONNX error: %1 — using image as-is.")
                      .arg(QString::fromUtf8(e.what()));
        r.image = image;
        return r;
    }
}

#endif // ENABLE_ONNX
