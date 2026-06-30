#include "HDR/HdrIblPrecompute.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace HdrIbl {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = kPi * 2.f;
constexpr float kEps = 1e-4f;

std::array<float, 3> normalize3(const std::array<float, 3>& v)
{
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len <= 1e-8f)
        return {0.f, 1.f, 0.f};
    return {v[0] / len, v[1] / len, v[2] / len};
}

float dot3(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<float, 3> cross3(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    return {
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    };
}

std::array<float, 3> reflect3(const std::array<float, 3>& i, const std::array<float, 3>& n)
{
    const float d = 2.f * dot3(i, n);
    return {i[0] - d * n[0], i[1] - d * n[1], i[2] - d * n[2]};
}

void buildTangentFrame(const std::array<float, 3>& n,
                       std::array<float, 3>& tangent,
                       std::array<float, 3>& bitangent)
{
    const std::array<float, 3> up = std::fabs(n[1]) < 0.999f
                                        ? std::array<float, 3>{0.f, 1.f, 0.f}
                                        : std::array<float, 3>{1.f, 0.f, 0.f};
    tangent = normalize3(cross3(up, n));
    bitangent = normalize3(cross3(n, tangent));
}

std::array<float, 3> localToWorld(const std::array<float, 3>& local,
                                    const std::array<float, 3>& n,
                                    const std::array<float, 3>& t,
                                    const std::array<float, 3>& b)
{
    return {
        local[0] * t[0] + local[1] * b[0] + local[2] * n[0],
        local[0] * t[1] + local[1] * b[1] + local[2] * n[1],
        local[0] * t[2] + local[1] * b[2] + local[2] * n[2],
    };
}

float radicalInverseVdC(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

std::array<float, 2> hammersley2d(uint32_t i, uint32_t n)
{
    return {static_cast<float>(i) / static_cast<float>(n), radicalInverseVdC(i)};
}

std::array<float, 3> sampleCosineHemisphere(const std::array<float, 2>& xi,
                                              const std::array<float, 3>& n,
                                              const std::array<float, 3>& t,
                                              const std::array<float, 3>& b)
{
    const float r = std::sqrt(xi[0]);
    const float phi = kTwoPi * xi[1];
    const std::array<float, 3> local{
        r * std::cos(phi),
        r * std::sin(phi),
        std::sqrt(std::max(0.f, 1.f - xi[0])),
    };
    return localToWorld(local, n, t, b);
}

float distributionGGX(float nDotH, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float denom = nDotH * nDotH * (a2 - 1.f) + 1.f;
    return a2 / std::max(kPi * denom * denom, 1e-8f);
}

float geometrySchlickGGX(float nDotV, float roughness)
{
    const float r = roughness + 1.f;
    const float k = (r * r) / 8.f;
    return nDotV / std::max(nDotV * (1.f - k) + k, 1e-8f);
}

float geometrySmith(float nDotV, float nDotL, float roughness)
{
    return geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness);
}

std::array<float, 3> importanceSampleGGX(const std::array<float, 2>& xi, float roughness)
{
    const float a = roughness * roughness;
    const float phi = kTwoPi * xi[0];
    const float cosTheta = std::sqrt((1.f - xi[1]) / std::max(1.f + (a * a - 1.f) * xi[1], 1e-8f));
    const float sinTheta = std::sqrt(std::max(0.f, 1.f - cosTheta * cosTheta));
    return {std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
}

void faceUvToDirection(int face, float u, float v, std::array<float, 3>& outDir)
{
    const float uc = u * 2.f - 1.f;
    const float vc = v * 2.f - 1.f;
    switch (face) {
    case 0: outDir = {1.f, -vc, -uc}; break;
    case 1: outDir = {-1.f, -vc, uc}; break;
    case 2: outDir = {uc, 1.f, vc}; break;
    case 3: outDir = {uc, -1.f, -vc}; break;
    case 4: outDir = {uc, -vc, 1.f}; break;
    case 5: outDir = {-uc, -vc, -1.f}; break;
    default: outDir = {0.f, 1.f, 0.f}; break;
    }
    outDir = normalize3(outDir);
}

bool bakeCubeFaces(int faceSize,
                   const std::function<void(int face, float u, float v, std::array<float, 3>&)>& eval,
                   HdrEquirect::CubemapFaces& out,
                   QString& error)
{
    if (faceSize <= 0) {
        error = QStringLiteral("invalid cubemap face size");
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
                std::array<float, 3> rgb{};
                eval(face, u, v, rgb);
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

} // namespace

bool sampleIrradianceRgb(const HdrEquirect::CubemapFaces& irradiance,
                         const std::array<float, 3>& dir,
                         std::array<float, 3>& outRgb)
{
    return HdrEquirect::sampleCubemapRgb(irradiance, dir, outRgb);
}

bool referenceIrradianceRgb(const HdrEquirect::CubemapFaces& environment,
                            const std::array<float, 3>& dir,
                            std::array<float, 3>& outRgb,
                            int sampleCount)
{
    if (sampleCount <= 0 || environment.faceSize <= 0)
        return false;

    const std::array<float, 3> n = normalize3(dir);
    std::array<float, 3> tangent{};
    std::array<float, 3> bitangent{};
    buildTangentFrame(n, tangent, bitangent);

    std::array<float, 3> accum{0.f, 0.f, 0.f};
    for (int i = 0; i < sampleCount; ++i) {
        const auto xi = hammersley2d(static_cast<uint32_t>(i),
                                     static_cast<uint32_t>(sampleCount));
        const std::array<float, 3> l = sampleCosineHemisphere(xi, n, tangent, bitangent);
        std::array<float, 3> rgb{};
        if (!HdrEquirect::sampleCubemapRgb(environment, l, rgb))
            return false;
        accum[0] += rgb[0];
        accum[1] += rgb[1];
        accum[2] += rgb[2];
    }

    const float inv = 1.f / static_cast<float>(sampleCount);
    outRgb = {accum[0] * inv, accum[1] * inv, accum[2] * inv};
    return true;
}

bool bakeIrradiance(const HdrEquirect::CubemapFaces& environment,
                    HdrEquirect::CubemapFaces& out,
                    QString& error,
                    int faceSize,
                    int sampleCount)
{
    if (environment.faceSize <= 0) {
        error = QStringLiteral("invalid environment cubemap");
        return false;
    }
    if (sampleCount <= 0) {
        error = QStringLiteral("sampleCount must be > 0");
        return false;
    }

    return bakeCubeFaces(
        faceSize,
        [&](int face, float u, float v, std::array<float, 3>& rgb) {
            std::array<float, 3> n{};
            faceUvToDirection(face, u, v, n);
            std::array<float, 3> tangent{};
            std::array<float, 3> bitangent{};
            buildTangentFrame(n, tangent, bitangent);

            std::array<float, 3> accum{0.f, 0.f, 0.f};
            for (int i = 0; i < sampleCount; ++i) {
                const auto xi = hammersley2d(static_cast<uint32_t>(i),
                                             static_cast<uint32_t>(sampleCount));
                const std::array<float, 3> l = sampleCosineHemisphere(xi, n, tangent, bitangent);
                std::array<float, 3> envRgb{};
                HdrEquirect::sampleCubemapRgb(environment, l, envRgb);
                accum[0] += envRgb[0];
                accum[1] += envRgb[1];
                accum[2] += envRgb[2];
            }
            const float inv = 1.f / static_cast<float>(sampleCount);
            rgb = {accum[0] * inv, accum[1] * inv, accum[2] * inv};
        },
        out,
        error);
}

bool bakePrefilter(const HdrEquirect::CubemapFaces& environment,
                   PrefilterChain& out,
                   QString& error,
                   int baseFaceSize,
                   int mipCount,
                   int sampleCount)
{
    if (environment.faceSize <= 0) {
        error = QStringLiteral("invalid environment cubemap");
        return false;
    }
    if (baseFaceSize <= 0 || mipCount <= 0 || sampleCount <= 0) {
        error = QStringLiteral("invalid prefilter parameters");
        return false;
    }

    out.mips.clear();
    out.mips.resize(static_cast<size_t>(mipCount));

    for (int mip = 0; mip < mipCount; ++mip) {
        const int faceSize = std::max(1, baseFaceSize >> mip);
        const float roughness = static_cast<float>(mip)
                                / static_cast<float>(std::max(1, mipCount - 1));

        PrefilterMip level;
        level.faceSize = faceSize;
        if (!bakeCubeFaces(
                faceSize,
                [&](int face, float u, float v, std::array<float, 3>& rgb) {
                    std::array<float, 3> r{};
                    faceUvToDirection(face, u, v, r);
                    const std::array<float, 3> n = r;
                    std::array<float, 3> vDir = r;
                    std::array<float, 3> tangent{};
                    std::array<float, 3> bitangent{};
                    buildTangentFrame(n, tangent, bitangent);

                    std::array<float, 3> accum{0.f, 0.f, 0.f};
                    float weightSum = 0.f;
                    for (int i = 0; i < sampleCount; ++i) {
                        const auto xi = hammersley2d(static_cast<uint32_t>(i),
                                                     static_cast<uint32_t>(sampleCount));
                        std::array<float, 3> hLocal = importanceSampleGGX(xi, roughness);
                        const std::array<float, 3> h = localToWorld(hLocal, n, tangent, bitangent);
                        const std::array<float, 3> l = normalize3(reflect3({-vDir[0], -vDir[1], -vDir[2]}, h));
                        const float nDotL = std::max(0.f, dot3(n, l));
                        if (nDotL <= 0.f)
                            continue;

                        const float nDotH = std::max(0.f, dot3(n, h));
                        const float d = distributionGGX(nDotH, roughness);
                        const float nDotV = std::max(0.f, dot3(n, vDir));
                        const float hDotV = std::max(0.f, dot3(h, vDir));
                        const float pdf = std::max(d * nDotH / std::max(4.f * hDotV, 1e-8f), 1e-8f);
                        const float saSample = 1.f / (static_cast<float>(sampleCount) * pdf + 1e-4f);
                        const float mipLevel = environment.faceSize > 0
                                                   ? 0.5f * std::log2(saSample) + 1.f
                                                   : 0.f;
                        (void)mipLevel; // reserved for future env-mip sampling

                        std::array<float, 3> envRgb{};
                        HdrEquirect::sampleCubemapRgb(environment, l, envRgb);
                        accum[0] += envRgb[0] * nDotL;
                        accum[1] += envRgb[1] * nDotL;
                        accum[2] += envRgb[2] * nDotL;
                        weightSum += nDotL;
                    }

                    if (weightSum > 1e-8f) {
                        rgb = {accum[0] / weightSum, accum[1] / weightSum, accum[2] / weightSum};
                    } else {
                        HdrEquirect::sampleCubemapRgb(environment, r, rgb);
                    }
                },
                level.faces,
                error)) {
            return false;
        }
        out.mips[static_cast<size_t>(mip)] = std::move(level);
    }
    return true;
}

bool bakeBrdfLut(BrdfLut& out, QString& error, int size, int sampleCount)
{
    if (size <= 0 || sampleCount <= 0) {
        error = QStringLiteral("invalid BRDF LUT parameters");
        return false;
    }

    out.size = size;
    out.rg.assign(static_cast<size_t>(size) * static_cast<size_t>(size) * 2u, 0.f);

    for (int y = 0; y < size; ++y) {
        const float nDotV = std::max((static_cast<float>(y) + 0.5f) / static_cast<float>(size), kEps);
        for (int x = 0; x < size; ++x) {
            const float roughness = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const std::array<float, 3> n{0.f, 0.f, 1.f};
            const std::array<float, 3> v{
                std::sqrt(std::max(0.f, 1.f - nDotV * nDotV)),
                0.f,
                nDotV,
            };

            float a = 0.f;
            float b = 0.f;
            for (int i = 0; i < sampleCount; ++i) {
                const auto xi = hammersley2d(static_cast<uint32_t>(i),
                                             static_cast<uint32_t>(sampleCount));
                const std::array<float, 3> hLocal = importanceSampleGGX(xi, roughness);
                const std::array<float, 3> h = normalize3(hLocal);
                const std::array<float, 3> l = normalize3(reflect3({-v[0], -v[1], -v[2]}, h));

                const float nDotL = std::max(0.f, l[2]);
                const float nDotH = std::max(0.f, h[2]);
                const float vDotH = std::max(0.f, dot3(v, h));
                if (nDotL <= 0.f)
                    continue;

                const float g = geometrySmith(nDotV, nDotL, roughness);
                const float gVis = (g * vDotH) / std::max(nDotH * nDotV, 1e-8f);
                const float fc = std::pow(1.f - vDotH, 5.f);
                a += (1.f - fc) * gVis;
                b += fc * gVis;
            }

            const float inv = 1.f / static_cast<float>(sampleCount);
            const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(size)
                                + static_cast<size_t>(x)) * 2u;
            out.rg[idx + 0] = a * inv;
            out.rg[idx + 1] = b * inv;
        }
    }
    return true;
}

bool bakeAll(const HdrEquirect::CubemapFaces& environment,
             IblBakeResult& out,
             QString& error,
             int sampleCount)
{
    out = {};
    if (!bakeIrradiance(environment, out.irradiance, error, kIrradianceFaceSize, sampleCount))
        return false;
    if (!bakePrefilter(environment, out.prefilter, error, kPrefilterBaseFaceSize, kPrefilterMipCount, sampleCount))
        return false;
    if (!bakeBrdfLut(out.brdfLut, error, kBrdfLutSize, sampleCount))
        return false;
    return true;
}

} // namespace HdrIbl
