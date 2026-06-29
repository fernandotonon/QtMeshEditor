#include "HDR/HdrEquirectLoader.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>

#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_HDR
#define STBI_NO_BMP
#define STBI_NO_PNG
#define STBI_NO_JPEG
#define STBI_NO_PSD
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"

#ifdef ENABLE_OPENEXR
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"
#endif

namespace HdrEquirect {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = kPi * 2.f;

float clamp01(float v)
{
    return std::max(0.f, std::min(1.f, v));
}

void sampleBilinearRgb(const FloatImage& img, float u, float v, float outRgb[3])
{
    u = u - std::floor(u); // wrap horizontally
    v = clamp01(v);

    const float fx = u * static_cast<float>(img.width) - 0.5f;
    const float fy = v * static_cast<float>(img.height) - 0.5f;

    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    auto fetch = [&](int x, int y, float dst[3]) {
        x = ((x % img.width) + img.width) % img.width;
        y = std::max(0, std::min(img.height - 1, y));
        const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(img.width)
                            + static_cast<size_t>(x)) * 3u;
        dst[0] = img.rgb[idx + 0];
        dst[1] = img.rgb[idx + 1];
        dst[2] = img.rgb[idx + 2];
    };

    float c00[3], c10[3], c01[3], c11[3];
    fetch(x0, y0, c00);
    fetch(x1, y0, c10);
    fetch(x0, y1, c01);
    fetch(x1, y1, c11);

    for (int c = 0; c < 3; ++c) {
        const float top = c00[c] * (1.f - tx) + c10[c] * tx;
        const float bot = c01[c] * (1.f - tx) + c11[c] * tx;
        outRgb[c] = top * (1.f - ty) + bot * ty;
    }
}

/// Ogre cube-face order: +X, -X, +Y, -Y, +Z, -Z.
void faceUvToDirection(int face, float u, float v, float outDir[3])
{
    const float uc = u * 2.f - 1.f;
    const float vc = v * 2.f - 1.f;
    switch (face) {
    case 0: outDir[0] =  1.f; outDir[1] = -vc; outDir[2] = -uc; break;
    case 1: outDir[0] = -1.f; outDir[1] = -vc; outDir[2] =  uc; break;
    case 2: outDir[0] =  uc; outDir[1] =  1.f; outDir[2] =  vc; break;
    case 3: outDir[0] =  uc; outDir[1] = -1.f; outDir[2] = -vc; break;
    case 4: outDir[0] =  uc; outDir[1] = -vc; outDir[2] =  1.f; break;
    case 5: outDir[0] = -uc; outDir[1] = -vc; outDir[2] = -1.f; break;
    default: outDir[0] = 0.f; outDir[1] = 1.f; outDir[2] = 0.f; break;
    }
    const float len = std::sqrt(outDir[0] * outDir[0]
                                + outDir[1] * outDir[1]
                                + outDir[2] * outDir[2]);
    if (len > 1e-8f) {
        outDir[0] /= len;
        outDir[1] /= len;
        outDir[2] /= len;
    }
}

void directionToEquirectUv(const float dir[3], float& u, float& v)
{
    const float phi = std::atan2(dir[2], dir[0]);
    const float theta = std::asin(std::max(-1.f, std::min(1.f, dir[1])));
    u = phi / kTwoPi + 0.5f;
    // Match UvProject / Radiance convention: image row 0 (+Y pole) maps to v=0.
    v = 0.5f - theta / kPi;
}

bool loadHdrStb(const QString& path, FloatImage& out, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("cannot open file: %1").arg(path);
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty()) {
        error = QStringLiteral("empty file: %1").arg(path);
        return false;
    }

    int w = 0;
    int h = 0;
    int comp = 0;
    float* pixels = stbi_loadf_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.constData()),
        static_cast<int>(bytes.size()),
        &w, &h, &comp, 3);
    if (!pixels) {
        error = QString::fromUtf8(stbi_failure_reason());
        return false;
    }

    out.width = w;
    out.height = h;
    out.rgb.assign(pixels, pixels + static_cast<size_t>(w) * static_cast<size_t>(h) * 3u);
    stbi_image_free(pixels);
    return true;
}

#ifdef ENABLE_OPENEXR
bool loadExrTiny(const QString& path, FloatImage& out, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("cannot open file: %1").arg(path);
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty()) {
        error = QStringLiteral("empty file: %1").arg(path);
        return false;
    }

    float* rgba = nullptr;
    int w = 0;
    int h = 0;
    const char* err = nullptr;
    const int ret = LoadEXRFromMemory(
        &rgba, &w, &h,
        reinterpret_cast<const unsigned char*>(bytes.constData()),
        static_cast<size_t>(bytes.size()),
        &err);
    if (ret != TINYEXR_SUCCESS) {
        error = err ? QString::fromUtf8(err) : QStringLiteral("LoadEXR failed");
        if (err)
            FreeEXRErrorMessage(err);
        return false;
    }

    out.width = w;
    out.height = h;
    out.rgb.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3u);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t dst = (static_cast<size_t>(y) * static_cast<size_t>(w)
                                + static_cast<size_t>(x)) * 3u;
            const size_t src = (static_cast<size_t>(y) * static_cast<size_t>(w)
                                + static_cast<size_t>(x)) * 4u;
            out.rgb[dst + 0] = rgba[src + 0];
            out.rgb[dst + 1] = rgba[src + 1];
            out.rgb[dst + 2] = rgba[src + 2];
        }
    }
    free(rgba);
    return true;
}
#endif

} // namespace

QString sha1HexOfFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha1);
    if (!hash.addData(&file))
        return {};
    return QString::fromLatin1(hash.result().toHex());
}

bool loadFromFile(const QString& path, FloatImage& out, QString& error)
{
    out = {};
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("hdr"))
        return loadHdrStb(path, out, error);
#ifdef ENABLE_OPENEXR
    if (suffix == QStringLiteral("exr"))
        return loadExrTiny(path, out, error);
#endif
    error = QStringLiteral("unsupported HDR format: %1").arg(suffix);
    return false;
}

int defaultFaceSizeForEquirect(int equirectWidth)
{
    const int raw = std::max(1, equirectWidth / 4);
    return std::max(16, std::min(raw, 2048));
}

bool bakeEquirectToCubemap(const FloatImage& equirect,
                           int faceSize,
                           CubemapFaces& out,
                           QString& error)
{
    out = {};
    if (equirect.width <= 0 || equirect.height <= 0
        || static_cast<int>(equirect.rgb.size())
               != equirect.width * equirect.height * 3) {
        error = QStringLiteral("invalid equirect image buffer");
        return false;
    }
    if (faceSize <= 0) {
        error = QStringLiteral("faceSize must be > 0");
        return false;
    }

    out.faceSize = faceSize;
    const size_t facePixels = static_cast<size_t>(faceSize) * static_cast<size_t>(faceSize) * 3u;
    for (auto& face : out.faces)
        face.resize(facePixels);

    for (int face = 0; face < 6; ++face) {
        auto& dst = out.faces[static_cast<size_t>(face)];
        for (int y = 0; y < faceSize; ++y) {
            for (int x = 0; x < faceSize; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(faceSize);
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(faceSize);
                float dir[3];
                faceUvToDirection(face, u, v, dir);
                float eu = 0.f;
                float ev = 0.f;
                directionToEquirectUv(dir, eu, ev);
                float rgb[3];
                sampleBilinearRgb(equirect, eu, ev, rgb);
                const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(faceSize)
                                    + static_cast<size_t>(x)) * 3u;
                dst[idx + 0] = rgb[0];
                dst[idx + 1] = rgb[1];
                dst[idx + 2] = rgb[2];
            }
        }
    }
    return true;
}

RgbMean faceMeanRgb(const std::vector<float>& faceRgb, int faceSize)
{
    RgbMean mean;
    if (faceSize <= 0)
        return mean;
    const size_t count = static_cast<size_t>(faceSize) * static_cast<size_t>(faceSize);
    if (faceRgb.size() < count * 3u)
        return mean;
    for (size_t i = 0; i < count; ++i) {
        mean.r += faceRgb[i * 3u + 0];
        mean.g += faceRgb[i * 3u + 1];
        mean.b += faceRgb[i * 3u + 2];
    }
    const float inv = 1.f / static_cast<float>(count);
    mean.r *= inv;
    mean.g *= inv;
    mean.b *= inv;
    return mean;
}

} // namespace HdrEquirect
