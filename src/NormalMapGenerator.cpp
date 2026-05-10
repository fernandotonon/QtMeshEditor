#include "NormalMapGenerator.h"

#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <algorithm>
#include <cmath>

namespace NormalMapGenerator {

namespace {

// Rec.601 luminance, fixed-point — matches TextureChannelPacker so
// behaviour is consistent across the two utilities.
inline uint8_t luminance(QRgb px)
{
    const int r = qRed(px);
    const int g = qGreen(px);
    const int b = qBlue(px);
    return static_cast<uint8_t>((r * 77 + g * 150 + b * 29 + 128) >> 8);
}

// Edge-clamp height sample. The image is in RGBA8888; we sample its
// luminance and normalise to [0..1].
inline float sampleHeight(const QImage& img, int x, int y)
{
    const int w = img.width();
    const int h = img.height();
    const int cx = std::clamp(x, 0, w - 1);
    const int cy = std::clamp(y, 0, h - 1);
    return luminance(img.pixel(cx, cy)) / 255.0f;
}

// Encode a unit-length normal (components in [-1..1]) to RGB8.
inline void encodeNormal(uchar* outRow, int x, float nx, float ny, float nz)
{
    auto encode = [](float c) {
        // (c + 1) * 0.5 → [0..1] → [0..255]
        const float v = (c + 1.0f) * 0.5f * 255.0f;
        return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
    };
    outRow[x*3 + 0] = encode(nx);
    outRow[x*3 + 1] = encode(ny);
    outRow[x*3 + 2] = encode(nz);
}

} // namespace

GenResult generate(const GenSpec& spec)
{
    GenResult res;

    if (spec.sourcePath.isEmpty()) {
        res.error = QStringLiteral("source path is empty");
        return res;
    }

    QImageReader reader(spec.sourcePath);
    QImage src = reader.read();
    if (src.isNull()) {
        res.error = QStringLiteral("failed to read '%1': %2")
                        .arg(spec.sourcePath, reader.errorString());
        return res;
    }
    src = src.convertToFormat(QImage::Format_RGBA8888);

    // Resolve output dimensions: explicit override beats source size.
    int outW = spec.outputWidth  > 0 ? spec.outputWidth  : src.width();
    int outH = spec.outputHeight > 0 ? spec.outputHeight : src.height();
    if (outW <= 0 || outH <= 0) {
        res.error = QStringLiteral("invalid output dimensions");
        return res;
    }

    // Resize the source to the output dimensions up-front so the Sobel
    // sampler can stay a simple per-pixel lookup.
    if (src.size() != QSize(outW, outH)) {
        src = src.scaled(outW, outH, Qt::IgnoreAspectRatio,
                          Qt::SmoothTransformation);
        if (src.format() != QImage::Format_RGBA8888)
            src = src.convertToFormat(QImage::Format_RGBA8888);
    }

    QImage out(outW, outH, QImage::Format_RGB888);
    if (out.isNull()) {
        res.error = QStringLiteral("failed to allocate %1x%2 output image")
                        .arg(outW).arg(outH);
        return res;
    }

    // Strength gates how aggressively we lift the gradient. The user-
    // facing slider is 0..10ish; clamp to a sane band so a runaway
    // value can't produce all-saturated output.
    const float strength = std::clamp(spec.strength, 0.0f, 32.0f);

    // Per-pixel Sobel. dx and dy are in [-4..+4] before strength scaling
    // (a uniform-luminance-difference 1×8 windowed gradient).
    for (int y = 0; y < outH; ++y) {
        uchar* row = out.scanLine(y);
        for (int x = 0; x < outW; ++x) {
            const float h00 = sampleHeight(src, x - 1, y - 1);
            const float h10 = sampleHeight(src, x,     y - 1);
            const float h20 = sampleHeight(src, x + 1, y - 1);
            const float h01 = sampleHeight(src, x - 1, y);
            const float h21 = sampleHeight(src, x + 1, y);
            const float h02 = sampleHeight(src, x - 1, y + 1);
            const float h12 = sampleHeight(src, x,     y + 1);
            const float h22 = sampleHeight(src, x + 1, y + 1);

            const float dx = (h20 + 2.0f * h21 + h22) - (h00 + 2.0f * h01 + h02);
            const float dy = (h02 + 2.0f * h12 + h22) - (h00 + 2.0f * h10 + h20);

            // Tangent-space normal: -gradient gives "bumps point at the
            // viewer". Z=1 keeps the normal pointing up out of the
            // surface; strength scales the in-plane components.
            float nx = -dx * strength;
            float ny = -dy * strength;
            float nz = 1.0f;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-6f) {
                nx /= len;
                ny /= len;
                nz /= len;
            } else {
                nx = 0.0f; ny = 0.0f; nz = 1.0f;
            }

            if (spec.invertR) nx = -nx;
            // OpenGL convention is +Y up. invertG flips for DirectX
            // pipelines that expect +Y down.
            if (spec.invertG) ny = -ny;

            encodeNormal(row, x, nx, ny, nz);
        }
    }

    res.ok = true;
    res.image = std::move(out);
    res.usedWidth = outW;
    res.usedHeight = outH;
    return res;
}

GenResult generateToFile(const GenSpec& spec, const QString& outPath)
{
    GenResult r = generate(spec);
    if (!r.ok) return r;

    if (outPath.isEmpty()) {
        r.ok = false;
        r.error = QStringLiteral("output path is empty");
        return r;
    }
    QImageWriter writer(outPath);
    if (!writer.canWrite()) {
        r.ok = false;
        r.error = QStringLiteral("cannot write '%1': format unsupported")
                      .arg(QFileInfo(outPath).suffix());
        return r;
    }
    if (!writer.write(r.image)) {
        r.ok = false;
        r.error = QStringLiteral("write failed: %1").arg(writer.errorString());
        return r;
    }
    return r;
}

} // namespace NormalMapGenerator
