#include "PillowResample.h"

#include <algorithm>
#include <cmath>

// Mirrors Pillow 10-13 src/libImaging/Resample.c (ImagingResampleInner with
// the 8 bpc paths) and Convert.c (rgba2rgbA / rgbA2rgba). Verified bit-exact
// against Pillow 13.0 (PillowResample_test.cpp holds a pinned case).

namespace PillowResample {

namespace {

constexpr int kPrecisionBits = 32 - 8 - 2;

double sinc(double x)
{
    if (x == 0.0)
        return 1.0;
    x *= 3.14159265358979323846;
    return std::sin(x) / x;
}

double lanczos(double x)
{
    if (-3.0 <= x && x < 3.0)
        return sinc(x) * sinc(x / 3.0);
    return 0.0;
}

struct Coeffs {
    int ksize = 0;
    std::vector<int> bounds;   // [xmin, count] per output sample
    std::vector<int32_t> kk;   // fixed-point weights, ksize per output sample
};

Coeffs precompute(int inSize, int outSize)
{
    Coeffs c;
    const double scale = double(inSize) / outSize;
    const double filterscale = std::max(scale, 1.0);
    const double support = 3.0 * filterscale;
    c.ksize = int(std::ceil(support)) * 2 + 1;
    c.bounds.resize(size_t(outSize) * 2);
    c.kk.assign(size_t(outSize) * c.ksize, 0);
    std::vector<double> k(size_t(c.ksize));
    for (int xx = 0; xx < outSize; ++xx) {
        const double center = (xx + 0.5) * scale;
        const double ss = 1.0 / filterscale;
        int xmin = int(center - support + 0.5);
        if (xmin < 0)
            xmin = 0;
        int xmax = int(center + support + 0.5);
        if (xmax > inSize)
            xmax = inSize;
        xmax -= xmin;
        double ww = 0.0;
        for (int x = 0; x < xmax; ++x) {
            k[size_t(x)] = lanczos((x + xmin - center + 0.5) * ss);
            ww += k[size_t(x)];
        }
        for (int x = 0; x < xmax; ++x) {
            const double w = ww != 0.0 ? k[size_t(x)] / ww : k[size_t(x)];
            c.kk[size_t(xx) * c.ksize + x] = w < 0
                ? int(-0.5 + w * (1 << kPrecisionBits))
                : int(0.5 + w * (1 << kPrecisionBits));
        }
        c.bounds[size_t(xx) * 2] = xmin;
        c.bounds[size_t(xx) * 2 + 1] = xmax;
    }
    return c;
}

inline uint8_t clip8(int32_t v)
{
    if (v >= (1 << kPrecisionBits << 8))
        return 255;
    if (v <= 0)
        return 0;
    return uint8_t(v >> kPrecisionBits);
}

} // namespace

QSize fitLongSide(QSize size, int maxSide)
{
    const int longSide = std::max(size.width(), size.height());
    if (longSide <= 0)
        return size;
    const double scale = std::min(1.0, double(maxSide) / longSide);
    if (scale >= 1.0)
        return size;
    return {int(size.width() * scale), int(size.height() * scale)};
}

namespace {

std::vector<uint8_t> horizontalPass(const std::vector<uint8_t>& in, int w, int h, int outW)
{
    const Coeffs c = precompute(w, outW);
    std::vector<uint8_t> out(size_t(outW) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = &in[size_t(y) * w * 4];
        for (int xx = 0; xx < outW; ++xx) {
            const int xmin = c.bounds[size_t(xx) * 2];
            const int count = c.bounds[size_t(xx) * 2 + 1];
            const int32_t* k = &c.kk[size_t(xx) * c.ksize];
            for (int ch = 0; ch < 4; ++ch) {
                int32_t ss = 1 << (kPrecisionBits - 1);
                for (int x = 0; x < count; ++x)
                    ss += row[size_t(x + xmin) * 4 + ch] * k[x];
                out[(size_t(y) * outW + xx) * 4 + ch] = clip8(ss);
            }
        }
    }
    return out;
}

std::vector<uint8_t> verticalPass(const std::vector<uint8_t>& in, int w, int h, int outH)
{
    const Coeffs c = precompute(h, outH);
    std::vector<uint8_t> out(size_t(w) * outH * 4);
    for (int yy = 0; yy < outH; ++yy) {
        const int ymin = c.bounds[size_t(yy) * 2];
        const int count = c.bounds[size_t(yy) * 2 + 1];
        const int32_t* k = &c.kk[size_t(yy) * c.ksize];
        for (int xx = 0; xx < w; ++xx) {
            for (int ch = 0; ch < 4; ++ch) {
                int32_t ss = 1 << (kPrecisionBits - 1);
                for (int y = 0; y < count; ++y)
                    ss += in[(size_t(y + ymin) * w + xx) * 4 + ch] * k[y];
                out[(size_t(yy) * w + xx) * 4 + ch] = clip8(ss);
            }
        }
    }
    return out;
}

} // namespace

std::vector<uint8_t> lanczosRgba8(const std::vector<uint8_t>& rgba, int w, int h,
                                  int outW, int outH)
{
    if (w <= 0 || h <= 0 || outW <= 0 || outH <= 0
        || rgba.size() != size_t(w) * h * 4)
        return {};

    // Each pass runs only when its dimension changes. Pillow runs the
    // horizontal pass first unless the height shrinks by more than twice what
    // the width does; the order changes the rounding of the intermediate
    // image, so it is part of being bit-exact. (Pillow restricts the first
    // horizontal pass to the rows the vertical pass reads; that skips work
    // but changes no value, so every row is processed here.)
    const bool horizontalFirst = !((h - outH) > 0 && (h - outH) > (w - outW) * 2);
    std::vector<uint8_t> cur = rgba;
    if (horizontalFirst) {
        if (outW != w)
            cur = horizontalPass(cur, w, h, outW);
        if (outH != h)
            cur = verticalPass(cur, outW, h, outH);
    } else {
        if (outH != h)
            cur = verticalPass(cur, w, h, outH);
        if (outW != w)
            cur = horizontalPass(cur, w, outH, outW);
    }
    return cur;
}

void premultiply(std::vector<uint8_t>& rgba)
{
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const unsigned a = rgba[i + 3];
        for (int c = 0; c < 3; ++c) {
            const unsigned t = rgba[i + c] * a + 128;   // MULDIV255
            rgba[i + c] = uint8_t(((t >> 8) + t) >> 8);
        }
    }
}

void unpremultiply(std::vector<uint8_t>& rgba)
{
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const unsigned a = rgba[i + 3];
        if (a == 255 || a == 0)
            continue;
        for (int c = 0; c < 3; ++c)
            rgba[i + c] = uint8_t(std::min(255u, (255u * rgba[i + c]) / a));
    }
}

QImage resizeLikePillow(const QImage& image, QSize size)
{
    const QImage src = image.convertToFormat(QImage::Format_RGBA8888);
    const int w = src.width(), h = src.height();
    if (w <= 0 || h <= 0 || size.isEmpty())
        return {};
    std::vector<uint8_t> buf(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        std::copy_n(src.constScanLine(y), size_t(w) * 4, &buf[size_t(y) * w * 4]);
    premultiply(buf);
    std::vector<uint8_t> out = lanczosRgba8(buf, w, h, size.width(), size.height());
    if (out.empty())
        return {};
    unpremultiply(out);
    QImage result(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y)
        std::copy_n(&out[size_t(y) * size.width() * 4], size_t(size.width()) * 4,
                    result.scanLine(y));
    return result;
}

} // namespace PillowResample
