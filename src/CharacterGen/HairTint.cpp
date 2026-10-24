#include "HairTint.h"

#include <QBuffer>
#include <QColor>

#include <algorithm>
#include <array>
#include <cmath>

namespace CharacterGen {
namespace {
float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

float luminance(const QRgb p) { return 0.299f * float(qRed(p)) + 0.587f * float(qGreen(p)) + 0.114f * float(qBlue(p)); }

Rgb shadow(const Rgb& c) { return {c[0] * 0.55f, c[1] * 0.55f, c[2] * 0.55f}; }
Rgb highlight(const Rgb& c) { return {clamp01(c[0] * 1.45f + 0.06f), clamp01(c[1] * 1.45f + 0.06f), clamp01(c[2] * 1.45f + 0.06f)}; }

// The three-stop gradient: s = 0 shadow, 0.5 the colour, 1 highlight.
std::array<float, 3> gradient(float s, const Rgb& lo, const Rgb& mid, const Rgb& hi)
{
    std::array<float, 3> out{};
    for (int i = 0; i < 3; ++i)
        out[size_t(i)] = s < 0.5f ? lo[size_t(i)] + (mid[size_t(i)] - lo[size_t(i)]) * (s / 0.5f)
                                  : mid[size_t(i)] + (hi[size_t(i)] - mid[size_t(i)]) * ((s - 0.5f) / 0.5f);
    return out;
}

QString normalised(const QString& text)
{
    QString s = text.trimmed().toLower();
    s.replace(QLatin1Char(' '), QLatin1Char('_')).replace(QLatin1Char('-'), QLatin1Char('_'));
    return s;
}
}  // namespace

const std::vector<HairColourPreset>& hairColourPresets()
{
    static const std::vector<HairColourPreset> presets = [] {
        auto rgb = [](int r, int g, int b) { return Rgb{float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f}; };
        return std::vector<HairColourPreset>{
            {QStringLiteral("black"), QStringLiteral("Black"), rgb(0x26, 0x26, 0x26)},
            {QStringLiteral("dark_brown"), QStringLiteral("Dark Brown"), rgb(0x32, 0x22, 0x1a)},
            {QStringLiteral("brown"), QStringLiteral("Brown"), rgb(0x3d, 0x25, 0x17)},
            {QStringLiteral("light_brown"), QStringLiteral("Light Brown"), rgb(0x67, 0x47, 0x29)},
            {QStringLiteral("blonde"), QStringLiteral("Blonde"), rgb(0x9a, 0x7e, 0x53)},
            {QStringLiteral("platinum_blonde"), QStringLiteral("Platinum Blonde"), rgb(0xa9, 0x9e, 0x89)},
            {QStringLiteral("red"), QStringLiteral("Red"), rgb(0x69, 0x23, 0x0e)},
            {QStringLiteral("gray"), QStringLiteral("Gray"), rgb(0x55, 0x54, 0x55)},
            {QStringLiteral("white"), QStringLiteral("White"), rgb(0xb3, 0xb2, 0xb3)},
        };
    }();
    return presets;
}

bool parseHairColour(const QString& text, Rgb* out)
{
    const QString s = normalised(text);
    for (const HairColourPreset& p : hairColourPresets())
        if (p.name == s) { if (out) *out = p.colour; return true; }
    const QString t = text.trimmed();
    if (t.size() == 7 && t.startsWith(QLatin1Char('#'))) {
        const QColor c(t);
        if (c.isValid()) { if (out) *out = Rgb{float(c.redF()), float(c.greenF()), float(c.blueF())}; return true; }
    }
    return false;
}

QString hairColourHex(const Rgb& c)
{
    auto b = [](float v) { return int(std::lround(clamp01(v) * 255.0f)); };
    return QColor(b(c[0]), b(c[1]), b(c[2])).name();
}

QImage tintHairShading(const QImage& shading, const Rgb& colour)
{
    if (shading.isNull()) return QImage();
    const QImage src = shading.convertToFormat(QImage::Format_ARGB32);
    // the median luminance of the opaque pixels is the map's "mid" level
    std::array<int, 256> hist{};
    int opaque = 0;
    for (int y = 0; y < src.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        for (int x = 0; x < src.width(); ++x)
            if (qAlpha(row[x]) > 127) { ++hist[size_t(std::clamp(int(luminance(row[x])), 0, 255))]; ++opaque; }
    }
    float median = 128.0f;
    if (opaque > 0) {
        int seen = 0;
        for (int i = 0; i < 256; ++i) { seen += hist[size_t(i)]; if (seen * 2 >= opaque) { median = float(i); break; } }
    }
    median = std::max(median, 8.0f);
    const Rgb lo = shadow(colour), hi = highlight(colour);
    QImage out(src.size(), QImage::Format_ARGB32);
    for (int y = 0; y < src.height(); ++y) {
        const QRgb* in = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb* o = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            const float s = clamp01(luminance(in[x]) / (2.0f * median));
            const auto c = gradient(s, lo, colour, hi);
            o[x] = qRgba(int(std::lround(clamp01(c[0]) * 255.0f)), int(std::lround(clamp01(c[1]) * 255.0f)),
                         int(std::lround(clamp01(c[2]) * 255.0f)), qAlpha(in[x]));
        }
    }
    return out;
}

QByteArray tintHairTexturePng(const QByteArray& shadingPng, const Rgb& colour)
{
    QImage img;
    if (!img.loadFromData(shadingPng, "PNG")) return QByteArray();
    const QImage tinted = tintHairShading(img, colour);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    if (!tinted.save(&buf, "PNG")) return QByteArray();
    return bytes;
}

}  // namespace CharacterGen
