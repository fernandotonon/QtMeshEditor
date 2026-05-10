#include "TextureChannelPacker.h"

#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <algorithm>

namespace TextureChannelPacker {

namespace {

constexpr int kDefaultSize = 256;

struct LoadedSource {
    bool present = false;     // true when an image was loaded
    QImage img;
    float constantValue = 0.0f;
    bool invert = false;
};

LoadedSource loadSource(const ChannelSource& src, QString* errOut)
{
    LoadedSource out;
    out.constantValue = std::clamp(src.constantValue, 0.0f, 1.0f);
    out.invert = src.invert;
    if (src.path.isEmpty()) return out;
    QImageReader reader(src.path);
    QImage img = reader.read();
    if (img.isNull()) {
        if (errOut)
            *errOut = QStringLiteral("failed to read '%1': %2")
                          .arg(src.path, reader.errorString());
        return out;
    }
    // Convert to RGBA8 once so per-pixel sampling is uniform.
    out.img = img.convertToFormat(QImage::Format_RGBA8888);
    out.present = true;
    return out;
}

// Rec.601 luminance in [0..255], converted from RGBA8888 pixel.
inline uint8_t luminance(QRgb px)
{
    const int r = qRed(px);
    const int g = qGreen(px);
    const int b = qBlue(px);
    // 0.299 R + 0.587 G + 0.114 B, fixed-point.
    return static_cast<uint8_t>((r * 77 + g * 150 + b * 29 + 128) >> 8);
}

inline uint8_t sampleChannel(const LoadedSource& src, int x, int y)
{
    if (!src.present) {
        // Constant value path.
        const uint8_t v = static_cast<uint8_t>(std::clamp(
            std::round(src.constantValue * 255.0f), 0.0f, 255.0f));
        return src.invert ? static_cast<uint8_t>(255 - v) : v;
    }
    const QRgb px = src.img.pixel(x, y);
    const uint8_t v = luminance(px);
    return src.invert ? static_cast<uint8_t>(255 - v) : v;
}

// Resolve output dimensions from the largest source. If every source is
// constant-only, fall back to kDefaultSize so we still produce a usable
// flat texture.
QSize resolveOutputSize(const PackingSpec& spec,
                        const std::array<LoadedSource, 4>& srcs)
{
    if (spec.outputWidth > 0 && spec.outputHeight > 0)
        return {spec.outputWidth, spec.outputHeight};

    int w = 0, h = 0;
    for (const auto& s : srcs) {
        if (!s.present) continue;
        w = std::max(w, s.img.width());
        h = std::max(h, s.img.height());
    }
    if (w == 0 || h == 0)
        return {kDefaultSize, kDefaultSize};
    return {w, h};
}

// Bilinearly scale a source image up/down to the target dimensions. We
// rescale once up-front so the per-pixel pack loop is a simple lookup.
void normaliseToSize(LoadedSource& src, QSize target)
{
    if (!src.present) return;
    if (src.img.size() == target) return;
    src.img = src.img.scaled(target, Qt::IgnoreAspectRatio,
                              Qt::SmoothTransformation);
    if (src.img.format() != QImage::Format_RGBA8888)
        src.img = src.img.convertToFormat(QImage::Format_RGBA8888);
}

} // namespace

PackResult pack(const PackingSpec& spec)
{
    PackResult res;

    QString loadErr;
    std::array<LoadedSource, 4> sources{
        loadSource(spec.red,   &loadErr),
        loadSource(spec.green, &loadErr),
        loadSource(spec.blue,  &loadErr),
        loadSource(spec.alpha, &loadErr),
    };
    if (!loadErr.isEmpty()) {
        res.error = loadErr;
        return res;
    }

    const QSize outSize = resolveOutputSize(spec, sources);
    res.usedWidth  = outSize.width();
    res.usedHeight = outSize.height();

    for (auto& s : sources)
        normaliseToSize(s, outSize);

    const QImage::Format fmt = spec.includeAlpha
                                   ? QImage::Format_RGBA8888
                                   : QImage::Format_RGB888;
    QImage out(outSize, fmt);
    if (out.isNull()) {
        res.error = QStringLiteral("failed to allocate %1x%2 output image")
                        .arg(outSize.width())
                        .arg(outSize.height());
        return res;
    }

    // Per-pixel pack. We use scanline pointers for speed on large images.
    const int W = outSize.width();
    const int H = outSize.height();
    if (spec.includeAlpha) {
        for (int y = 0; y < H; ++y) {
            uchar* row = out.scanLine(y);
            for (int x = 0; x < W; ++x) {
                row[x*4 + 0] = sampleChannel(sources[0], x, y); // R
                row[x*4 + 1] = sampleChannel(sources[1], x, y); // G
                row[x*4 + 2] = sampleChannel(sources[2], x, y); // B
                row[x*4 + 3] = sampleChannel(sources[3], x, y); // A
            }
        }
    } else {
        for (int y = 0; y < H; ++y) {
            uchar* row = out.scanLine(y);
            for (int x = 0; x < W; ++x) {
                row[x*3 + 0] = sampleChannel(sources[0], x, y);
                row[x*3 + 1] = sampleChannel(sources[1], x, y);
                row[x*3 + 2] = sampleChannel(sources[2], x, y);
            }
        }
    }

    res.ok = true;
    res.image = std::move(out);
    return res;
}

PackResult packToFile(const PackingSpec& spec, const QString& outPath)
{
    PackResult r = pack(spec);
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

} // namespace TextureChannelPacker
