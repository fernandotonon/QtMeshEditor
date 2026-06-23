#include "TextureUpscaler.h"
#include "PbrMapSynth.h"   // reuse toNCHW / nchwToRgb

#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <new>
#include <thread>
#include <vector>

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#include <QFileInfo>
#include <array>
#include <string>
#include <unordered_map>
#endif

namespace TextureUpscaler {

#ifndef ENABLE_ONNX

Result upscale(const QImage&, const QString&, const Options&, const ProgressFn&)
{
    Result r;
    r.error = QStringLiteral(
        "Texture upscaling was not built into this binary "
        "(rebuild with -DENABLE_ONNX=ON).");
    return r;
}

#else // ENABLE_ONNX

namespace {

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Run the model on one RGB tile; returns the upscaled planar-RGB float buffer
// plus its output dimensions. Empty on failure.
struct TileOut {
    std::vector<float> rgb;   // 3 * ow * oh planar, [0,1]
    int ow = 0, oh = 0;
};

bool runTile(Ort::Session& session, Ort::AllocatorWithDefaultOptions& alloc,
             const QImage& rgbTile, TileOut& out, QString& err)
{
    const int w = rgbTile.width(), h = rgbTile.height();
    std::vector<float> input = PbrMapSynth::toNCHW(rgbTile, 3);
    const std::array<int64_t, 4> shape = {1, 3, h, w};
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value inT = Ort::Value::CreateTensor<float>(
        mem, input.data(), input.size(), shape.data(), shape.size());

    Ort::AllocatedStringPtr inName = session.GetInputNameAllocated(0, alloc);
    Ort::AllocatedStringPtr outName = session.GetOutputNameAllocated(0, alloc);
    const char* inNames[]  = {inName.get()};
    const char* outNames[] = {outName.get()};

    std::vector<Ort::Value> outs;
    try {
        outs = session.Run(Ort::RunOptions{nullptr}, inNames, &inT, 1, outNames, 1);
    } catch (const Ort::Exception& e) {
        err = QStringLiteral("ONNX inference failed: %1").arg(e.what());
        return false;
    }
    if (outs.empty()) { err = QStringLiteral("model produced no output"); return false; }

    auto info = outs[0].GetTensorTypeAndShapeInfo();
    auto sh = info.GetShape();
    if (sh.size() != 4 || sh[1] < 3) {
        err = QStringLiteral("unexpected model output shape");
        return false;
    }
    out.oh = static_cast<int>(sh[2]);
    out.ow = static_cast<int>(sh[3]);
    const size_t oplane = static_cast<size_t>(out.ow) * out.oh;
    if (info.GetElementCount() < oplane * 3) {
        err = QStringLiteral("model output smaller than its declared shape");
        return false;
    }
    const float* d = outs[0].GetTensorData<float>();
    out.rgb.assign(d, d + oplane * 3);
    return true;
}

} // namespace

Result upscale(const QImage& srcIn, const QString& modelPath, const Options& opts,
               const ProgressFn& onProgress)
{
    Result r;
    if (srcIn.isNull()) { r.error = QStringLiteral("source image is null"); return r; }
    if (modelPath.isEmpty() || !QFileInfo::exists(modelPath)) {
        r.error = QStringLiteral("upscale model not available at '%1' — connect to "
            "the internet to download it, or set the model path in AI Settings.")
            .arg(modelPath);
        return r;
    }

    // Keep the original (possibly alpha-bearing) image so a cutout/opacity
    // texture doesn't come back fully opaque; the model still gets RGB888.
    const bool hasAlpha = srcIn.hasAlphaChannel();
    const QImage srcAlpha = hasAlpha ? srcIn.convertToFormat(QImage::Format_RGBA8888)
                                     : QImage();
    const QImage src = srcIn.convertToFormat(QImage::Format_RGB888);
    const int W = src.width(), H = src.height();

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_upscale");
        Ort::SessionOptions so;
        // Real-ESRGAN is CPU-heavy; use all cores for intra-op parallelism
        // (was pinned to 1, which made large textures take hours). Leave one
        // core free so the UI/host stays responsive. ONNX Runtime treats 0 as
        // "let ORT decide", but we cap explicitly for predictability.
        const unsigned hw = std::thread::hardware_concurrency();
        const int threads = (hw > 1) ? static_cast<int>(hw - 1) : 1;
        so.SetIntraOpNumThreads(threads);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef __APPLE__
        try {
            std::unordered_map<std::string, std::string> coreml;
            so.AppendExecutionProvider("CoreML", coreml);
        } catch (const Ort::Exception&) {}
#endif
#ifdef _WIN32
        std::wstring wp = modelPath.toStdWString();
        Ort::Session session(env, wp.c_str(), so);
#else
        const std::string p = modelPath.toStdString();
        Ort::Session session(env, p.c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;

        // Detect the scale factor with a tiny probe tile so we can size the
        // output canvas before the (expensive) full run.
        const int probe = std::min({W, H, opts.tileSize > 0 ? opts.tileSize : 64, 64});
        {
            TileOut t; QString err;
            if (!runTile(session, alloc, src.copy(0, 0, probe, probe), t, err)) {
                r.error = err; return r;
            }
            // Require an integer, equal scale on BOTH axes (a model that scales
            // W and H differently, or non-integrally, would mis-place pixels).
            if (probe <= 0 || t.ow % probe != 0 || t.oh % probe != 0
                || (t.ow / probe) != (t.oh / probe)) {
                r.error = QStringLiteral("model output scale is not a uniform integer "
                    "factor (probe %1x%1 -> %2x%3)").arg(probe).arg(t.ow).arg(t.oh);
                return r;
            }
            r.scale = t.ow / probe;
        }
        if (r.scale < 1) { r.error = QStringLiteral("could not determine upscale factor"); return r; }

        // Guard the output-canvas allocation: compute in 64-bit and cap the
        // total so a huge input or bad scale can't overflow int or trigger an
        // uncaught std::bad_alloc outside the Ort handler.
        const long long outWll = static_cast<long long>(W) * r.scale;
        const long long outHll = static_cast<long long>(H) * r.scale;
        const long long pixels = outWll * outHll;
        constexpr long long kMaxOutputPixels = 256LL * 1024 * 1024;  // 256 Mpx (~3 GB at 3ch float)
        if (pixels <= 0 || pixels > kMaxOutputPixels) {
            r.error = QStringLiteral("upscaled size %1x%2 is too large to allocate "
                "(limit %3 Mpx) — use a smaller source or lower scale.")
                .arg(outWll).arg(outHll).arg(kMaxOutputPixels / (1024 * 1024));
            return r;
        }
        const int outW = static_cast<int>(outWll), outH = static_cast<int>(outHll);
        const size_t oplane = static_cast<size_t>(outW) * outH;
        std::vector<float> acc, weight;
        try {
            acc.assign(oplane * 3, 0.0f);
            weight.assign(oplane, 0.0f);
        } catch (const std::bad_alloc&) {
            r.error = QStringLiteral("out of memory allocating the %1x%2 upscaled canvas")
                .arg(outW).arg(outH);
            return r;
        }

        const int tile = (opts.tileSize > 0) ? opts.tileSize : std::max(W, H);
        const int step = std::max(1, tile - std::max(0, opts.overlap));
        const int s = r.scale;

        // Count tiles up front for progress reporting.
        int totalTiles = 0;
        for (int ty = 0; ty < H; ty += step) {
            for (int tx = 0; tx < W; tx += step) {
                if (std::min(tile, W - tx) > 0 && std::min(tile, H - ty) > 0) ++totalTiles;
                if (tx + std::min(tile, W - tx) >= W) break;
            }
            if (ty + tile >= H) break;
        }
        int tilesDone = 0;
        if (onProgress && !onProgress(0, totalTiles)) {
            r.error = QStringLiteral("cancelled"); return r;
        }

        for (int ty = 0; ty < H; ty += step) {
            for (int tx = 0; tx < W; tx += step) {
                const int tw = std::min(tile, W - tx);
                const int th = std::min(tile, H - ty);
                if (tw <= 0 || th <= 0) continue;

                TileOut to; QString err;
                if (!runTile(session, alloc, src.copy(tx, ty, tw, th), to, err)) {
                    r.error = err; return r;
                }
                // The composite assumes the tile upscaled to exactly tw*s × th*s;
                // bail if the model returned something else rather than silently
                // cropping/misplacing pixels.
                if (to.ow != tw * s || to.oh != th * s) {
                    r.error = QStringLiteral("tile output %1x%2 != expected %3x%4")
                        .arg(to.ow).arg(to.oh).arg(tw * s).arg(th * s);
                    return r;
                }
                // Composite in OUTPUT space at (tx*s, ty*s), feathered.
                const size_t tplane = static_cast<size_t>(to.ow) * to.oh;
                for (int y = 0; y < to.oh; ++y) {
                    const int gy = ty * s + y;
                    if (gy >= outH) break;
                    for (int x = 0; x < to.ow; ++x) {
                        const int gx = tx * s + x;
                        if (gx >= outW) break;
                        const int fo = opts.overlap * s;
                        const float wx = (fo > 0)
                            ? std::min({1.0f, (x + 1.0f) / fo, (to.ow - x) / float(fo)}) : 1.0f;
                        const float wy = (fo > 0)
                            ? std::min({1.0f, (y + 1.0f) / fo, (to.oh - y) / float(fo)}) : 1.0f;
                        const float fw = std::max(1e-3f, wx * wy);
                        const size_t gi = static_cast<size_t>(gy) * outW + gx;
                        const size_t ti = static_cast<size_t>(y) * to.ow + x;
                        weight[gi] += fw;
                        for (int c = 0; c < 3; ++c)
                            acc[c * oplane + gi] += to.rgb[c * tplane + ti] * fw;
                    }
                }
                ++tilesDone;
                if (onProgress && !onProgress(tilesDone, totalTiles)) {
                    r.error = QStringLiteral("cancelled"); return r;
                }
                if (tx + tw >= W) break;
            }
            if (ty + tile >= H) break;
        }

        for (size_t i = 0; i < oplane; ++i) {
            const float wsum = weight[i] > 1e-6f ? weight[i] : 1.0f;
            for (int c = 0; c < 3; ++c) acc[c * oplane + i] /= wsum;
        }
        QImage rgb = PbrMapSynth::nchwToRgb(acc, outW, outH);
        if (rgb.isNull()) {
            r.error = QStringLiteral("failed to assemble the upscaled image");
            return r;
        }
        // Reapply the source alpha (upscaled by nearest-neighbour to match) so
        // cutout/opacity textures keep their transparency.
        if (hasAlpha && !srcAlpha.isNull()) {
            const QImage upAlpha = srcAlpha.scaled(outW, outH, Qt::IgnoreAspectRatio,
                                                   Qt::SmoothTransformation);
            QImage rgba = rgb.convertToFormat(QImage::Format_RGBA8888);
            for (int y = 0; y < outH; ++y) {
                const uchar* aline = upAlpha.constScanLine(y);
                uchar* dline = rgba.scanLine(y);
                for (int x = 0; x < outW; ++x)
                    dline[x * 4 + 3] = aline[x * 4 + 3];  // copy alpha channel
            }
            r.image = rgba;
        } else {
            r.image = rgb;
        }
        r.ok = true;
        return r;
    } catch (const Ort::Exception& e) {
        r.error = QStringLiteral("ONNX session failed: %1").arg(e.what());
        return r;
    }
}

#endif // ENABLE_ONNX

} // namespace TextureUpscaler
