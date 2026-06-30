#pragma once

#include <QString>
#include <array>
#include <cstdint>
#include <vector>

#include <QMetaType>

/// Pure-data HDR environment loading and equirectangular → cubemap baking.
/// No Ogre dependency — safe to unit-test without a GL context.
namespace HdrEquirect {

/// Row-major interleaved RGB float32 pixels (3 channels).
struct FloatImage {
    int width = 0;
    int height = 0;
    std::vector<float> rgb; // size = width * height * 3
};

/// Six cube faces, each `faceSize × faceSize` interleaved RGB float32.
struct CubemapFaces {
    int faceSize = 0;
    std::array<std::vector<float>, 6> faces;
};

/// SHA-1 hex digest of the raw bytes in `path`. Empty on I/O failure.
QString sha1HexOfFile(const QString& path);

/// Load a Radiance `.hdr` or (when compiled with ENABLE_OPENEXR) `.exr`
/// equirectangular environment map into linear float RGB.
bool loadFromFile(const QString& path, FloatImage& out, QString& error);

/// Default cubemap face resolution for a given equirect width (¼ width, clamped).
int defaultFaceSizeForEquirect(int equirectWidth);

/// CPU equirect → cubemap bake. `faceSize` must be > 0.
bool bakeEquirectToCubemap(const FloatImage& equirect,
                           int faceSize,
                           CubemapFaces& out,
                           QString& error);

/// Per-face mean RGB (for tests / diagnostics).
struct RgbMean {
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
};
RgbMean faceMeanRgb(const std::vector<float>& faceRgb, int faceSize);

/// Sample an environment cubemap along a normalized direction (bilinear).
bool sampleCubemapRgb(const CubemapFaces& cubemap, const std::array<float, 3>& dir,
                      std::array<float, 3>& outRgb);

} // namespace HdrEquirect

Q_DECLARE_METATYPE(HdrEquirect::CubemapFaces)
