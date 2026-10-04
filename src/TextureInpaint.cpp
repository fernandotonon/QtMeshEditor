#include "TextureInpaint.h"

#include "AppStorage.h"
#include "ModelDownloader.h"
#include "ModelFetch.h"
#ifdef ENABLE_ONNX
#include "OnnxRuntimeSettings.h"
#include <onnxruntime_cxx_api.h>
#endif

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr const char* kModelFile = "lama.onnx";
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/pbr/";
constexpr const char* kBaseUrlSettingsKey = "ai/inpaintModelBaseUrl";
constexpr const char* kModelLabel = "LaMa texture inpainting model";

QString modelDir()
{
    return QDir(AppStorage::aiModelsRoot()).filePath(QStringLiteral("pbr/"));
}

} // namespace

namespace TextureInpaint {

QString modelPath()
{
    return QDir(modelDir()).filePath(QString::fromLatin1(kModelFile));
}

bool modelPresent() { return QFileInfo::exists(modelPath()); }

// ---------------------------------------------------------------------------
// Pure helpers — always compiled, so they are testable on any build
// ---------------------------------------------------------------------------

std::vector<float> toImageTensor(const QImage& rgbTile, int tile)
{
    const int t = std::max(1, tile);
    std::vector<float> out(static_cast<size_t>(t) * t * 3, 0.0f);
    if (rgbTile.isNull()) return out;

    const QImage src = rgbTile.convertToFormat(QImage::Format_RGB888);
    const size_t plane = static_cast<size_t>(t) * t;
    const int h = std::min(t, src.height());
    const int w = std::min(t, src.width());
    for (int y = 0; y < h; ++y) {
        const uchar* line = src.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                // The graph wants [0,1] here even though it returns 0..255.
                out[c * plane + static_cast<size_t>(y) * t + x] =
                    static_cast<float>(line[x * 3 + c]) / 255.0f;
            }
        }
    }
    return out;
}

std::vector<float> toMaskTensor(const QImage& maskTile, int tile)
{
    const int t = std::max(1, tile);
    std::vector<float> out(static_cast<size_t>(t) * t, 0.0f);
    if (maskTile.isNull()) return out;

    const QImage src = maskTile.convertToFormat(QImage::Format_Grayscale8);
    const int h = std::min(t, src.height());
    const int w = std::min(t, src.width());
    for (int y = 0; y < h; ++y) {
        const uchar* line = src.constScanLine(y);
        for (int x = 0; x < w; ++x)
            // Any non-zero pixel means "inpaint". The model was trained on a
            // hard 0/1 mask, and a soft one measurably changes the fill, so
            // binarise rather than passing the grey through.
            out[static_cast<size_t>(y) * t + x] = line[x] ? 1.0f : 0.0f;
    }
    return out;
}

QImage fromOutputTensor(const std::vector<float>& out, int w, int h,
                        const QImage& source)
{
    if (w <= 0 || h <= 0
        || out.size() < static_cast<size_t>(w) * h * 3)
        return {};

    const QImage src = source.isNull()
                           ? QImage()
                           : source.convertToFormat(QImage::Format_RGB888);
    QImage img(w, h, QImage::Format_RGB888);
    const size_t plane = static_cast<size_t>(w) * h;
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(y);
        const uchar* sline = (!src.isNull() && y < src.height())
                                 ? src.constScanLine(y) : nullptr;
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float v = out[c * plane + static_cast<size_t>(y) * w + x];
                if (std::isfinite(v)) {
                    // Already 0..255 — do NOT rescale.
                    line[x * 3 + c] =
                        static_cast<uchar>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
                } else {
                    // A broken graph degrades to "unchanged", never to noise.
                    line[x * 3 + c] =
                        (sline && x < src.width()) ? sline[x * 3 + c] : 0;
                }
            }
        }
    }
    return img;
}

QImage dilateMask(const QImage& mask, int px)
{
    if (mask.isNull() || px <= 0)
        return mask.isNull() ? mask
                             : mask.convertToFormat(QImage::Format_Grayscale8);

    QImage cur = mask.convertToFormat(QImage::Format_Grayscale8);
    const int w = cur.width(), h = cur.height();
    for (int pass = 0; pass < px; ++pass) {
        QImage next = cur;
        for (int y = 0; y < h; ++y) {
            uchar* nl = next.scanLine(y);
            for (int x = 0; x < w; ++x) {
                if (cur.constScanLine(y)[x]) continue;
                bool hit = false;
                for (int dy = -1; dy <= 1 && !hit; ++dy) {
                    const int ny = y + dy;
                    if (ny < 0 || ny >= h) continue;
                    const uchar* cl = cur.constScanLine(ny);
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx;
                        if (nx < 0 || nx >= w) continue;
                        if (cl[nx]) { hit = true; break; }
                    }
                }
                if (hit) nl[x] = 255;
            }
        }
        cur = next;
    }
    return cur;
}

int countMasked(const QImage& mask)
{
    if (mask.isNull()) return 0;
    const QImage m = mask.convertToFormat(QImage::Format_Grayscale8);
    int n = 0;
    for (int y = 0; y < m.height(); ++y) {
        const uchar* line = m.constScanLine(y);
        for (int x = 0; x < m.width(); ++x)
            if (line[x]) ++n;
    }
    return n;
}

QImage maskFromCoverage(const std::vector<uint8_t>& covered, int w, int h)
{
    if (w <= 0 || h <= 0
        || covered.size() < static_cast<size_t>(w) * h)
        return {};
    QImage m(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        uchar* line = m.scanLine(y);
        for (int x = 0; x < w; ++x)
            // Uncovered texels are the ones to fill.
            line[x] = covered[static_cast<size_t>(y) * w + x] ? 0 : 255;
    }
    return m;
}

#ifndef ENABLE_ONNX

bool isAvailable() { return false; }
QString ensureModelBlocking(QString* error)
{
    if (error) *error = QStringLiteral("texture inpainting needs an ONNX build (rebuild with -DENABLE_ONNX)");
    return {};
}

Result inpaint(const QImage&, const QImage&, const QString&, const Options&)
{
    Result r;
    r.error = QStringLiteral("Texture inpainting needs an ONNX build "
                             "(rebuild with -DENABLE_ONNX).");
    return r;
}

#else // ENABLE_ONNX

bool isAvailable() { return true; }

QString ensureModelBlocking(QString* error)
{
    const QString dst = modelPath();
    if (QFileInfo::exists(dst)) return dst;
    if (!qEnvironmentVariableIsEmpty("QTMESH_INPAINT_NO_DOWNLOAD")) {
        if (error) *error = QStringLiteral("downloads disabled by QTMESH_INPAINT_NO_DOWNLOAD");
        return {};
    }

    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_INPAINT_MODEL_BASE_URL");
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
    ModelFetch::Request req;
    req.url = url;
    req.destination = dst;
    req.label = QString::fromLatin1(kModelLabel);
    req.timeoutMs = 900000;
    // #1037: one shared blocking wait — keeps the downloader's own error text
    // and the synchronous-rejection guard every consumer used to lack.
    const ModelFetch::Outcome fo = ModelFetch::ensureBlocking(req);
    if (!fo.ok) {
        if (error) *error = fo.error;
        return {};
    }
    return fo.path;
}

namespace {

// One model run on a single tile-sized RGB image + mask.
bool runTile(Ort::Session& session, Ort::AllocatorWithDefaultOptions& alloc,
             const QImage& rgbTile, const QImage& maskTile, int tile,
             std::vector<float>& out, QString& err)
{
    std::vector<float> img  = toImageTensor(rgbTile, tile);
    std::vector<float> mask = toMaskTensor(maskTile, tile);

    const int64_t imgShape[4]  = {1, 3, tile, tile};
    const int64_t maskShape[4] = {1, 1, tile, tile};
    auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    std::vector<Ort::Value> ins;
    ins.reserve(2);
    ins.emplace_back(Ort::Value::CreateTensor<float>(
        mem, img.data(), img.size(), imgShape, 4));
    ins.emplace_back(Ort::Value::CreateTensor<float>(
        mem, mask.data(), mask.size(), maskShape, 4));

    auto in0 = session.GetInputNameAllocated(0, alloc);
    auto in1 = session.GetInputNameAllocated(1, alloc);
    auto out0 = session.GetOutputNameAllocated(0, alloc);
    // Bind by NAME, not by index. The graph we host lists image-then-mask, but
    // nothing in ONNX guarantees that order, and a mask-first graph would get
    // the 3-channel tensor in its 1-channel slot. That fails deterministically
    // rather than silently, but it fails at INFERENCE time on the user's
    // machine — and the export verifier cannot catch it, since it runs the
    // graph with a name-keyed dict and never exercises index order.
    {
        const QByteArray n0(in0.get()), n1(in1.get());
        if (n0 == "mask" && n1 == "image") {
            std::swap(ins[0], ins[1]);
        } else if (n0 != "image" || n1 != "mask") {
            err = QStringLiteral("inpaint model has unexpected input names "
                                 "('%1', '%2'); expected 'image' and 'mask'.")
                      .arg(QString::fromUtf8(n0), QString::fromUtf8(n1));
            return false;
        }
    }
    const char* inNames[]  = {in0.get(), in1.get()};
    const char* outNames[] = {out0.get()};

    auto outs = session.Run(Ort::RunOptions{nullptr},
                            inNames, ins.data(), 2, outNames, 1);
    if (outs.empty() || !outs[0].IsTensor()) {
        err = QStringLiteral("inpaint model produced no tensor output.");
        return false;
    }
    auto info = outs[0].GetTensorTypeAndShapeInfo();
    const size_t n = info.GetElementCount();
    const size_t want = static_cast<size_t>(tile) * tile * 3;
    if (n < want) {
        err = QStringLiteral("inpaint output too small (%1 of %2 values).")
                  .arg(n).arg(want);
        return false;
    }
    const float* data = outs[0].GetTensorData<float>();
    out.assign(data, data + want);
    return true;
}

} // namespace

Result inpaint(const QImage& texture, const QImage& maskIn,
               const QString& modelFile, const Options& opts)
{
    Result r;
    if (texture.isNull()) { r.error = QStringLiteral("empty texture"); return r; }
    if (maskIn.isNull())  { r.error = QStringLiteral("empty mask"); return r; }
    if (!QFileInfo::exists(modelFile)) {
        r.error = QStringLiteral("Inpaint model not found at %1.").arg(modelFile);
        return r;
    }

    const int W = texture.width(), H = texture.height();
    QImage mask = maskIn.convertToFormat(QImage::Format_Grayscale8);
    if (mask.size() != texture.size())
        // Nearest, never smooth: a smoothed mask grows soft grey edges that
        // binarise into a ring of spurious inpaint texels.
        mask = mask.scaled(W, H, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    mask = dilateMask(mask, opts.maskDilatePx);

    r.maskedTexels = countMasked(mask);
    if (r.maskedTexels == 0) {
        r.error = QStringLiteral("mask selects no texels — nothing to inpaint.");
        return r;
    }

    const QImage srcRgb = texture.convertToFormat(QImage::Format_RGB888);

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_inpaint");
        Ort::SessionOptions so;
        OnnxRuntimeSettings::configureSessionOptions(so);
#ifdef _WIN32
        Ort::Session session(env, modelFile.toStdWString().c_str(), so);
#else
        Ort::Session session(env, modelFile.toStdString().c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;

        // The graph's spatial dims are pinned, so every tile is exactly
        // kInputSize; a partial edge tile is padded by the source content
        // (taken from a clamped origin) rather than by black, which would make
        // the model inpaint against a false dark border.
        const int tile = kInputSize;
        const int step = std::max(1, tile - std::max(0, opts.overlap));

        std::vector<float> accum(static_cast<size_t>(W) * H * 3, 0.0f);
        std::vector<float> wsum(static_cast<size_t>(W) * H, 0.0f);

        for (int ty = 0; ; ty += step) {
            const int oy = std::min(ty, std::max(0, H - tile));
            for (int tx = 0; ; tx += step) {
                const int ox = std::min(tx, std::max(0, W - tile));

                QImage rgbTile(tile, tile, QImage::Format_RGB888);
                rgbTile.fill(Qt::black);
                QImage maskTile(tile, tile, QImage::Format_Grayscale8);
                maskTile.fill(0);
                {
                    const int cw = std::min(tile, W - ox);
                    const int ch = std::min(tile, H - oy);
                    for (int y = 0; y < ch; ++y) {
                        std::memcpy(rgbTile.scanLine(y),
                                    srcRgb.constScanLine(oy + y) + ox * 3,
                                    static_cast<size_t>(cw) * 3);
                        std::memcpy(maskTile.scanLine(y),
                                    mask.constScanLine(oy + y) + ox,
                                    static_cast<size_t>(cw));
                    }
                }

                // Skip inference on a tile with nothing to fill. With 512px
                // tiles a 4096² texture visits 81 tiles, and a small mask
                // touches only a few — running the model on the rest is pure
                // latency. Feeding the SOURCE pixels into the accumulator
                // leaves the final composite identical, because every
                // destination pixel in such a tile is unmasked and therefore
                // restored from the source anyway.
                std::vector<float> outT;
                QString err;
                if (countMasked(maskTile) == 0) {
                    const size_t tplane0 = static_cast<size_t>(tile) * tile;
                    outT.assign(tplane0 * 3, 0.0f);
                    for (int yy = 0; yy < tile; ++yy) {
                        const uchar* sl = rgbTile.constScanLine(yy);
                        for (int xx = 0; xx < tile; ++xx)
                            for (int c = 0; c < 3; ++c)
                                outT[c * tplane0
                                     + static_cast<size_t>(yy) * tile + xx] =
                                    static_cast<float>(sl[xx * 3 + c]);
                    }
                } else if (!runTile(session, alloc, rgbTile, maskTile, tile,
                                    outT, err)) {
                    r.error = err;
                    return r;
                }

                // Feathered accumulate (PbrMapSynth's seam blend).
                const size_t tplane = static_cast<size_t>(tile) * tile;
                const int cw = std::min(tile, W - ox);
                const int ch = std::min(tile, H - oy);
                for (int y = 0; y < ch; ++y) {
                    for (int x = 0; x < cw; ++x) {
                        const float wx = (opts.overlap > 0)
                            ? std::min({1.0f, (x + 1.0f) / opts.overlap,
                                        (cw - x) / float(opts.overlap)}) : 1.0f;
                        const float wy = (opts.overlap > 0)
                            ? std::min({1.0f, (y + 1.0f) / opts.overlap,
                                        (ch - y) / float(opts.overlap)}) : 1.0f;
                        const float wgt = std::max(1e-4f, wx * wy);
                        const size_t dst =
                            static_cast<size_t>(oy + y) * W + (ox + x);
                        const size_t stile =
                            static_cast<size_t>(y) * tile + x;
                        for (int c = 0; c < 3; ++c)
                            accum[c * static_cast<size_t>(W) * H + dst] +=
                                outT[c * tplane + stile] * wgt;
                        wsum[dst] += wgt;
                    }
                }

                if (ox + tile >= W) break;
            }
            if (oy + tile >= H) break;
        }

        std::vector<float> flat(static_cast<size_t>(W) * H * 3, 0.0f);
        for (size_t i = 0, n = static_cast<size_t>(W) * H; i < n; ++i) {
            const float wv = wsum[i] > 0.0f ? wsum[i] : 1.0f;
            for (int c = 0; c < 3; ++c)
                flat[c * n + i] = accum[c * n + i] / wv;
        }

        QImage result = fromOutputTensor(flat, W, H, srcRgb);
        if (result.isNull()) {
            r.error = QStringLiteral("failed to build the inpainted image.");
            return r;
        }

        if (opts.compositeMaskedOnly) {
            // Keep every unmasked texel bit-exact. The model does preserve them
            // (measured MAE 0.0000 outside the mask), but compositing makes
            // that structural instead of a property we are trusting.
            for (int y = 0; y < H; ++y) {
                const uchar* ml = mask.constScanLine(y);
                const uchar* sl = srcRgb.constScanLine(y);
                uchar* dl = result.scanLine(y);
                for (int x = 0; x < W; ++x) {
                    if (ml[x]) continue;
                    for (int c = 0; c < 3; ++c) dl[x * 3 + c] = sl[x * 3 + c];
                }
            }
        }

        // The model is RGB-only, so an RGBA source would otherwise come back
        // fully opaque — silently flattening a cutout texture and breaking the
        // very bit-exactness promised above. Restore the source alpha, and set
        // the INPAINTED texels opaque: they now carry invented colour, and
        // leaving them transparent would make the fill invisible.
        if (texture.hasAlphaChannel()) {
            const QImage srcRgba =
                texture.convertToFormat(QImage::Format_RGBA8888);
            QImage out(W, H, QImage::Format_RGBA8888);
            for (int y = 0; y < H; ++y) {
                const uchar* ml = mask.constScanLine(y);
                const uchar* rl = result.constScanLine(y);
                const uchar* al = srcRgba.constScanLine(y);
                uchar* ol = out.scanLine(y);
                for (int x = 0; x < W; ++x) {
                    ol[x * 4 + 0] = rl[x * 3 + 0];
                    ol[x * 4 + 1] = rl[x * 3 + 1];
                    ol[x * 4 + 2] = rl[x * 3 + 2];
                    ol[x * 4 + 3] = ml[x] ? 255 : al[x * 4 + 3];
                }
            }
            r.image = out;
        } else {
            r.image = result;
        }
        r.ok = true;
        r.usedModel = true;
        return r;
    } catch (const Ort::Exception& e) {
        r.error = QStringLiteral("inpaint inference failed: %1")
                      .arg(QString::fromUtf8(e.what()));
        return r;
    }
}

#endif // ENABLE_ONNX

} // namespace TextureInpaint
