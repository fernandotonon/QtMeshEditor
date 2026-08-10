#include "PbrMapSynth.h"
#include "OnnxRuntimeSettings.h"

#include <QtGlobal>
#include <QColor>
#include <algorithm>
#include <cmath>
#include <vector>

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#include <QFileInfo>
#include <array>
#include <string>
#include <unordered_map>
#endif

namespace PbrMapSynth {

namespace {

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Rec.601 luminance, matching TextureChannelPacker's channel sampling.
inline float luma(const QColor& c)
{
    return 0.299f * c.redF() + 0.587f * c.greenF() + 0.114f * c.blueF();
}

// Separable 3x3 box blur on a single-channel float plane (in place via copy).
// Used to keep the heuristic roughness low-frequency.
std::vector<float> boxBlur3(const std::vector<float>& src, int w, int h, int passes)
{
    std::vector<float> a = src;
    std::vector<float> b(src.size());
    for (int p = 0; p < passes; ++p) {
        // horizontal
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int x0 = std::max(0, x - 1), x1 = std::min(w - 1, x + 1);
                b[y * w + x] = (a[y * w + x0] + a[y * w + x] + a[y * w + x1]) / 3.0f;
            }
        }
        // vertical
        for (int y = 0; y < h; ++y) {
            const int y0 = std::max(0, y - 1), y1 = std::min(h - 1, y + 1);
            for (int x = 0; x < w; ++x) {
                a[y * w + x] = (b[y0 * w + x] + b[y * w + x] + b[y1 * w + x]) / 3.0f;
            }
        }
    }
    return a;
}

} // namespace

std::vector<float> toNCHW(const QImage& in, int channels)
{
    // Only 1 (luminance) and 3 (RGB) are meaningful here; clamp anything else to
    // 3 so the allocation always matches the number of planes written below
    // (a stray channels==2 would otherwise allocate 2 planes but write 3 → OOB).
    const int ch = (channels == 1) ? 1 : 3;
    const QImage img = in.convertToFormat(QImage::Format_RGB888);
    const int w = img.width(), h = img.height();
    std::vector<float> out(static_cast<size_t>(ch) * w * h, 0.0f);
    const size_t plane = static_cast<size_t>(w) * h;
    for (int y = 0; y < h; ++y) {
        const uchar* line = img.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const uchar* px = line + x * 3;
            const float r = px[0] / 255.0f, g = px[1] / 255.0f, bl = px[2] / 255.0f;
            if (ch == 1) {
                out[static_cast<size_t>(y) * w + x] =
                    0.299f * r + 0.587f * g + 0.114f * bl;
            } else {
                out[0 * plane + static_cast<size_t>(y) * w + x] = r;
                out[1 * plane + static_cast<size_t>(y) * w + x] = g;
                out[2 * plane + static_cast<size_t>(y) * w + x] = bl;
            }
        }
    }
    return out;
}

QImage nchwToRgb(const std::vector<float>& data, int width, int height)
{
    QImage img(width, height, QImage::Format_RGB888);
    const size_t plane = static_cast<size_t>(width) * height;
    for (int y = 0; y < height; ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y) * width + x;
            uchar* px = line + x * 3;
            px[0] = static_cast<uchar>(clamp01(data[0 * plane + i]) * 255.0f + 0.5f);
            px[1] = static_cast<uchar>(clamp01(data[1 * plane + i]) * 255.0f + 0.5f);
            px[2] = static_cast<uchar>(clamp01(data[2 * plane + i]) * 255.0f + 0.5f);
        }
    }
    return img;
}

QImage decodeNormal(const std::vector<float>& data, int width, int height,
                    float strength, bool invertG)
{
    QImage img(width, height, QImage::Format_RGB888);
    const size_t plane = static_cast<size_t>(width) * height;
    const bool planar = data.size() >= plane * 3;
    for (int y = 0; y < height; ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y) * width + x;
            // Model encodes normal xyz in [0,1]; decode to [-1,1].
            float nx = data[0 * plane + i] * 2.0f - 1.0f;
            float ny = (planar ? data[1 * plane + i] : 0.5f) * 2.0f - 1.0f;
            float nz = (planar ? data[2 * plane + i] : 1.0f) * 2.0f - 1.0f;
            nx *= strength;
            ny *= strength;
            if (invertG) ny = -ny;
            // Z is positive in tangent space; recompute for consistency.
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len < 1e-6f) { nx = 0; ny = 0; nz = 1; len = 1; }
            nx /= len; ny /= len; nz = std::abs(nz / len);
            uchar* px = line + x * 3;
            px[0] = static_cast<uchar>((nx * 0.5f + 0.5f) * 255.0f + 0.5f);
            px[1] = static_cast<uchar>((ny * 0.5f + 0.5f) * 255.0f + 0.5f);
            px[2] = static_cast<uchar>((nz * 0.5f + 0.5f) * 255.0f + 0.5f);
        }
    }
    return img;
}

QImage decodeHeight(const std::vector<float>& data, int width, int height)
{
    QImage img(width, height, QImage::Format_Grayscale8);
    for (int y = 0; y < height; ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y) * width + x;
            line[x] = static_cast<uchar>(clamp01(data[i]) * 255.0f + 0.5f);
        }
    }
    return img;
}

QImage decodeGrayscaleFromRgb(const std::vector<float>& data, int width, int height)
{
    QImage img(width, height, QImage::Format_Grayscale8);
    const size_t plane = static_cast<size_t>(width) * height;
    const bool planar = data.size() >= plane * 3;
    for (int y = 0; y < height; ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y) * width + x;
            // PBRify roughness/height models emit RGB; take Rec.601 luminance
            // (the three channels are near-identical for a grayscale map).
            const float v = planar
                ? (0.299f * data[0 * plane + i] + 0.587f * data[1 * plane + i]
                   + 0.114f * data[2 * plane + i])
                : data[i];
            line[x] = static_cast<uchar>(clamp01(v) * 255.0f + 0.5f);
        }
    }
    return img;
}

QImage roughnessFromAlbedo(const QImage& albedo, float base, float contrast)
{
    const int w = albedo.width(), h = albedo.height();
    if (w <= 0 || h <= 0)
        return {};
    std::vector<float> rough(static_cast<size_t>(w) * h, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float l = luma(albedo.pixelColor(x, y));
            // Darker albedo → rougher; brighter → smoother.
            rough[static_cast<size_t>(y) * w + x] =
                clamp01(base + contrast * (1.0f - l));
        }
    }
    // Roughness is a low-frequency property — blur out albedo's fine detail.
    rough = boxBlur3(rough, w, h, 2);

    QImage img(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < w; ++x)
            line[x] = static_cast<uchar>(rough[static_cast<size_t>(y) * w + x] * 255.0f + 0.5f);
    }
    return img;
}

#ifndef ENABLE_ONNX

Result synthesize(const QImage&, const QString&, const Options&)
{
    Result r;
    r.error = QStringLiteral(
        "PBR map synthesis was not built into this binary "
        "(rebuild with -DENABLE_ONNX=ON).");
    return r;
}

#else // ENABLE_ONNX

namespace {

// One model run on a single (already tile-sized or full) RGB image. Fills
// `normalOut` (3ch planar) and/or `heightOut` (1ch) depending on what the model
// exposes. Returns false + error on failure. `wantNormal`/`wantHeight` request
// the maps; the model may provide normal, height, or both.
struct InferTile {
    std::vector<float> normal;   // 3*plane, empty if absent
    std::vector<float> height;   // plane, empty if absent
    int w = 0, h = 0;
};

bool runModelOnce(Ort::Session& session, Ort::AllocatorWithDefaultOptions& alloc,
                  const QImage& rgbTile, InferTile& out, QString& err)
{
    const int w = rgbTile.width(), h = rgbTile.height();
    out.w = w; out.h = h;

    // Discover input channel count from the model.
    const size_t inCount = session.GetInputCount();
    if (inCount < 1) { err = QStringLiteral("model has no inputs"); return false; }
    Ort::TypeInfo inInfo = session.GetInputTypeInfo(0);
    auto inShape = inInfo.GetTensorTypeAndShapeInfo().GetShape();
    int channels = 3;
    if (inShape.size() == 4 && inShape[1] > 0 && inShape[1] <= 4)
        channels = static_cast<int>(inShape[1]);

    std::vector<float> input = toNCHW(rgbTile, channels);
    const std::array<int64_t, 4> shape = {1, channels, h, w};

    Ort::MemoryInfo memInfo =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
        memInfo, input.data(), input.size(), shape.data(), shape.size());

    Ort::AllocatedStringPtr inName = session.GetInputNameAllocated(0, alloc);
    const char* inNames[] = {inName.get()};

    const size_t outCount = session.GetOutputCount();
    std::vector<Ort::AllocatedStringPtr> outNameHolders;
    std::vector<const char*> outNames;
    for (size_t i = 0; i < outCount; ++i) {
        outNameHolders.push_back(session.GetOutputNameAllocated(i, alloc));
        outNames.push_back(outNameHolders.back().get());
    }

    std::vector<Ort::Value> outputs;
    try {
        outputs = session.Run(Ort::RunOptions{nullptr}, inNames, &inputTensor, 1,
                              outNames.data(), outNames.size());
    } catch (const Ort::Exception& e) {
        err = QStringLiteral("ONNX inference failed: %1").arg(e.what());
        return false;
    }

    const size_t plane = static_cast<size_t>(w) * h;
    for (auto& ov : outputs) {
        auto info = ov.GetTensorTypeAndShapeInfo();
        auto sh = info.GetShape();
        const int oc = (sh.size() == 4) ? static_cast<int>(sh[1]) : 1;
        // Guard against a model whose output spatial dims differ from the input
        // (e.g. a >1x scale) — copying plane*3/plane would otherwise read past
        // the tensor. We only consume same-resolution (1x) outputs.
        const size_t elems = info.GetElementCount();
        const float* d = ov.GetTensorData<float>();
        if (oc >= 3 && out.normal.empty() && elems >= plane * 3) {
            out.normal.assign(d, d + plane * 3);
        } else if (oc == 1 && out.height.empty() && elems >= plane) {
            out.height.assign(d, d + plane);
        }
    }
    return true;
}

} // namespace

std::vector<float> runTiledModel(const QImage& albedoIn, const QString& modelPath,
                                 const Options& opts, int* outW, int* outH,
                                 QString* error)
{
    auto fail = [&](const QString& msg) -> std::vector<float> {
        if (error) *error = msg;
        return {};
    };
    if (albedoIn.isNull()) return fail(QStringLiteral("albedo image is null"));
    if (modelPath.isEmpty() || !QFileInfo::exists(modelPath))
        return fail(QStringLiteral("PBR model not available at '%1' — connect to the "
            "internet to download it, or set the model path in AI Settings.").arg(modelPath));

    const QImage albedo = albedoIn.convertToFormat(QImage::Format_RGB888);
    const int W = albedo.width(), H = albedo.height();
    if (outW) *outW = W;
    if (outH) *outH = H;

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_pbr");
        Ort::SessionOptions so;
        OnnxRuntimeSettings::configureSessionOptions(so);
        so.SetIntraOpNumThreads(1);
#ifdef _WIN32
        std::wstring wpath = modelPath.toStdWString();
        Ort::Session session(env, wpath.c_str(), so);
#else
        const std::string p = modelPath.toStdString();
        Ort::Session session(env, p.c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;

        const size_t plane = static_cast<size_t>(W) * H;
        std::vector<float> acc(plane * 3, 0.0f), weight(plane, 0.0f);
        const int tile = (opts.tileSize > 0) ? opts.tileSize : std::max(W, H);
        const int step = std::max(1, tile - std::max(0, opts.overlap));

        for (int ty = 0; ty < H; ty += step) {
            for (int tx = 0; tx < W; tx += step) {
                const int tw = std::min(tile, W - tx);
                const int th = std::min(tile, H - ty);
                if (tw <= 0 || th <= 0) continue;
                const QImage sub = albedo.copy(tx, ty, tw, th);

                InferTile it;
                QString err;
                if (!runModelOnce(session, alloc, sub, it, err))
                    return fail(err);
                if (it.normal.empty())  // PBRify models always emit 3ch
                    return fail(QStringLiteral("model did not return a 3-channel output"));

                const size_t tplane = static_cast<size_t>(tw) * th;
                for (int y = 0; y < th; ++y) {
                    for (int x = 0; x < tw; ++x) {
                        const float wx = (opts.overlap > 0)
                            ? std::min({1.0f, (x + 1.0f) / opts.overlap,
                                        (tw - x) / float(opts.overlap)}) : 1.0f;
                        const float wy = (opts.overlap > 0)
                            ? std::min({1.0f, (y + 1.0f) / opts.overlap,
                                        (th - y) / float(opts.overlap)}) : 1.0f;
                        const float fw = std::max(1e-3f, wx * wy);
                        const size_t gi = static_cast<size_t>(ty + y) * W + (tx + x);
                        const size_t ti = static_cast<size_t>(y) * tw + x;
                        weight[gi] += fw;
                        for (int c = 0; c < 3; ++c)
                            acc[c * plane + gi] += it.normal[c * tplane + ti] * fw;
                    }
                }
                if (tx + tw >= W) break;
            }
            if (ty + tile >= H) break;
        }

        for (size_t i = 0; i < plane; ++i) {
            const float wsum = weight[i] > 1e-6f ? weight[i] : 1.0f;
            for (int c = 0; c < 3; ++c) acc[c * plane + i] /= wsum;
        }
        return acc;
    } catch (const Ort::Exception& e) {
        return fail(QStringLiteral("ONNX session failed: %1").arg(e.what()));
    }
}

Result synthesize(const QImage& albedoIn, const QString& modelPath,
                  const Options& opts)
{
    Result r;
    if (albedoIn.isNull()) { r.error = QStringLiteral("albedo image is null"); return r; }
    const QImage albedo = albedoIn.convertToFormat(QImage::Format_RGB888);

    if (opts.generateRoughness)
        r.roughness = roughnessFromAlbedo(albedo, opts.roughnessBase, opts.roughnessContrast);

    if (!opts.generateNormal && !opts.generateHeight) {
        r.ok = !r.roughness.isNull();
        if (!r.ok) r.error = QStringLiteral("roughness generation failed");
        return r;
    }

    int W = 0, H = 0;
    QString err;
    const std::vector<float> out = runTiledModel(albedo, modelPath, opts, &W, &H, &err);
    if (out.empty()) { r.error = err; return r; }

    // Legacy single-model behaviour: interpret the 3ch output as a normal map,
    // and the height as its luminance.
    if (opts.generateNormal)
        r.normal = decodeNormal(out, W, H, opts.normalStrength, opts.invertG);
    if (opts.generateHeight)
        r.height = decodeGrayscaleFromRgb(out, W, H);
    r.ok = true;
    return r;
}

#endif // ENABLE_ONNX

} // namespace PbrMapSynth
