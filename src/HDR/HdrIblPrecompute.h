#pragma once

#include "HDR/HdrEquirectLoader.h"

#include <QString>
#include <array>
#include <vector>

#include <QMetaType>

/// Pure-data IBL precompute (irradiance, prefiltered specular mips, BRDF LUT).
/// No Ogre dependency — safe to unit-test and run on a worker thread.
namespace HdrIbl {

constexpr int kIrradianceFaceSize = 32;
constexpr int kPrefilterBaseFaceSize = 256;
constexpr int kPrefilterMipCount = 6; // 256 → 128 → 64 → 32 → 16 → 8
constexpr int kBrdfLutSize = 512;
constexpr int kDefaultSampleCount = 128;

struct BrdfLut {
    int size = kBrdfLutSize;
    /// Row-major interleaved RG float32 (scale in R, bias in G).
    std::vector<float> rg;
};

struct PrefilterMip {
    int faceSize = 0;
    HdrEquirect::CubemapFaces faces;
};

struct PrefilterChain {
    std::vector<PrefilterMip> mips;
};

struct IblBakeResult {
    HdrEquirect::CubemapFaces irradiance;
    PrefilterChain prefilter;
    BrdfLut brdfLut;
};

/// Lambert cosine-weighted irradiance convolution (Monte Carlo).
bool bakeIrradiance(const HdrEquirect::CubemapFaces& environment,
                    HdrEquirect::CubemapFaces& out,
                    QString& error,
                    int faceSize = kIrradianceFaceSize,
                    int sampleCount = kDefaultSampleCount);

/// GGX-weighted prefiltered specular mip chain (UE4 split-sum style).
bool bakePrefilter(const HdrEquirect::CubemapFaces& environment,
                   PrefilterChain& out,
                   QString& error,
                   int baseFaceSize = kPrefilterBaseFaceSize,
                   int mipCount = kPrefilterMipCount,
                   int sampleCount = kDefaultSampleCount);

/// Environment-independent BRDF integration LUT (512×512 RG).
bool bakeBrdfLut(BrdfLut& out,
                 QString& error,
                 int size = kBrdfLutSize,
                 int sampleCount = kDefaultSampleCount);

/// Full IBL bake bundle.
bool bakeAll(const HdrEquirect::CubemapFaces& environment,
             IblBakeResult& out,
             QString& error,
             int sampleCount = kDefaultSampleCount);

/// Sample irradiance cube at a normalized direction (for tests / validation).
bool sampleIrradianceRgb(const HdrEquirect::CubemapFaces& irradiance,
                         const std::array<float, 3>& dir,
                         std::array<float, 3>& outRgb);

/// CPU reference irradiance for a direction (slow; test helper only).
bool referenceIrradianceRgb(const HdrEquirect::CubemapFaces& environment,
                            const std::array<float, 3>& dir,
                            std::array<float, 3>& outRgb,
                            int sampleCount = kDefaultSampleCount);

} // namespace HdrIbl

Q_DECLARE_METATYPE(HdrIbl::IblBakeResult)
