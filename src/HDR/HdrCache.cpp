#include "HDR/HdrCache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <cstring>
#include <functional>
#include <algorithm>

namespace HdrCache {
namespace {

struct FileHeader {
    uint32_t magic = kMagic;
    uint32_t version = kFormatVersion;
    uint32_t payloadBytes = 0;
};

bool readExact(QFile& file, void* dst, qint64 bytes)
{
    return file.read(reinterpret_cast<char*>(dst), bytes) == bytes;
}

bool writeExact(QFile& file, const void* src, qint64 bytes)
{
    return file.write(reinterpret_cast<const char*>(src), bytes) == bytes;
}

bool readHeader(QFile& file, FileHeader& header, uint32_t expectedPayloadBytes, QString& error)
{
    if (!readExact(file, &header, sizeof(FileHeader))) {
        error = QStringLiteral("truncated cache header");
        return false;
    }
    if (header.magic != kMagic || header.version != kFormatVersion) {
        error = QStringLiteral("cache header mismatch");
        return false;
    }
    if (header.payloadBytes != expectedPayloadBytes) {
        error = QStringLiteral("cache payload size mismatch");
        return false;
    }
    return true;
}

bool writeHeader(QFile& file, uint32_t payloadBytes)
{
    const FileHeader header{kMagic, kFormatVersion, payloadBytes};
    return writeExact(file, &header, sizeof(FileHeader));
}

bool readCubemapFaces(QFile& file, HdrEquirect::CubemapFaces& faces, QString& error)
{
    int32_t faceSize = 0;
    if (!readExact(file, &faceSize, sizeof(int32_t)) || faceSize <= 0) {
        error = QStringLiteral("invalid cubemap face size in cache");
        return false;
    }
    faces.faceSize = faceSize;
    const size_t facePixels = static_cast<size_t>(faceSize) * static_cast<size_t>(faceSize) * 3u;
    for (auto& face : faces.faces) {
        face.resize(facePixels);
        if (!readExact(file, face.data(), static_cast<qint64>(facePixels * sizeof(float)))) {
            error = QStringLiteral("truncated cubemap face payload");
            return false;
        }
    }
    return true;
}

bool writeCubemapFaces(QFile& file, const HdrEquirect::CubemapFaces& faces)
{
    const int32_t faceSize = faces.faceSize;
    if (!writeExact(file, &faceSize, sizeof(int32_t)))
        return false;
    const size_t facePixels = static_cast<size_t>(faceSize) * static_cast<size_t>(faceSize) * 3u;
    for (const auto& face : faces.faces) {
        if (face.size() < facePixels)
            return false;
        if (!writeExact(file, face.data(), static_cast<qint64>(facePixels * sizeof(float))))
            return false;
    }
    return true;
}

uint32_t cubemapPayloadBytes(const HdrEquirect::CubemapFaces& faces)
{
    const size_t facePixels = static_cast<size_t>(faces.faceSize)
                              * static_cast<size_t>(faces.faceSize) * 3u;
    return static_cast<uint32_t>(sizeof(int32_t)
                                 + 6u * facePixels * sizeof(float));
}

uint32_t prefilterPayloadBytes(const HdrIbl::PrefilterChain& chain)
{
    uint32_t total = sizeof(int32_t);
    for (const auto& mip : chain.mips)
        total += cubemapPayloadBytes(mip.faces);
    return total;
}

uint32_t brdfPayloadBytes(const HdrIbl::BrdfLut& lut)
{
    return static_cast<uint32_t>(sizeof(int32_t)
                                 + lut.rg.size() * sizeof(float));
}

bool readBrdfLut(QFile& file, HdrIbl::BrdfLut& lut, QString& error)
{
    int32_t size = 0;
    if (!readExact(file, &size, sizeof(int32_t)) || size <= 0) {
        error = QStringLiteral("invalid BRDF LUT size in cache");
        return false;
    }
    const size_t expected = static_cast<size_t>(size) * static_cast<size_t>(size) * 2u;
    lut.size = size;
    lut.rg.resize(expected);
    if (!readExact(file, lut.rg.data(), static_cast<qint64>(expected * sizeof(float)))) {
        error = QStringLiteral("truncated BRDF LUT payload");
        return false;
    }
    return true;
}

bool writeBrdfLut(QFile& file, const HdrIbl::BrdfLut& lut)
{
    const int32_t size = lut.size;
    if (!writeExact(file, &size, sizeof(int32_t)))
        return false;
    return writeExact(file, lut.rg.data(), static_cast<qint64>(lut.rg.size() * sizeof(float)));
}

bool readPrefilterChain(QFile& file, HdrIbl::PrefilterChain& chain, QString& error)
{
    int32_t mipCount = 0;
    if (!readExact(file, &mipCount, sizeof(int32_t)) || mipCount <= 0) {
        error = QStringLiteral("invalid prefilter mip count in cache");
        return false;
    }
    chain.mips.clear();
    chain.mips.resize(static_cast<size_t>(mipCount));
    for (auto& mip : chain.mips) {
        if (!readCubemapFaces(file, mip.faces, error))
            return false;
        mip.faceSize = mip.faces.faceSize;
    }
    return true;
}

bool writePrefilterChain(QFile& file, const HdrIbl::PrefilterChain& chain)
{
    const int32_t mipCount = static_cast<int32_t>(chain.mips.size());
    if (!writeExact(file, &mipCount, sizeof(int32_t)))
        return false;
    for (const auto& mip : chain.mips) {
        if (!writeCubemapFaces(file, mip.faces))
            return false;
    }
    return true;
}

bool loadPayloadFile(const QString& path, uint32_t expectedPayloadBytes,
                     const std::function<bool(QFile&, QString&)>& reader,
                     QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("cannot open cache file: %1").arg(path);
        return false;
    }
    FileHeader header;
    if (!readHeader(file, header, expectedPayloadBytes, error))
        return false;
    return reader(file, error);
}

bool savePayloadFile(const QString& path, uint32_t payloadBytes,
                     const std::function<bool(QFile&)>& writer)
{
    QFileInfo info(path);
    QDir().mkpath(info.absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    if (!writeHeader(file, payloadBytes))
        return false;
    return writer(file);
}

QString manifestPath(const QString& cacheKey)
{
    return entryDirectory(cacheKey) + QStringLiteral("/manifest.bin");
}

QString irradiancePath(const QString& cacheKey)
{
    return entryDirectory(cacheKey) + QStringLiteral("/irradiance.bin");
}

QString prefilterPath(const QString& cacheKey)
{
    return entryDirectory(cacheKey) + QStringLiteral("/prefilter.bin");
}

QString brdfPath(const QString& cacheKey)
{
    return entryDirectory(cacheKey) + QStringLiteral("/brdf_lut.bin");
}

} // namespace

QString cacheRootDirectory()
{
    const QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath(QStringLiteral("hdr_cache"));
}

QString entryDirectory(const QString& cacheKey)
{
    return QDir(cacheRootDirectory()).filePath(cacheKey);
}

bool isValid(const QString& cacheKey)
{
    if (cacheKey.isEmpty())
        return false;

    HdrIbl::IblBakeResult tmp;
    QString error;
    return load(cacheKey, tmp, error);
}

bool load(const QString& cacheKey, HdrIbl::IblBakeResult& out, QString& error)
{
    out = {};
    if (cacheKey.isEmpty()) {
        error = QStringLiteral("empty cache key");
        return false;
    }

    QFile manifest(manifestPath(cacheKey));
    if (!manifest.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("missing cache manifest");
        return false;
    }

    FileHeader header;
    if (!readExact(manifest, &header, sizeof(FileHeader))) {
        error = QStringLiteral("truncated cache manifest");
        return false;
    }
    if (header.magic != kMagic || header.version != kFormatVersion) {
        error = QStringLiteral("cache manifest version mismatch");
        return false;
    }

    int32_t irradianceSize = 0;
    int32_t prefilterBaseSize = 0;
    int32_t prefilterMipCount = 0;
    int32_t brdfSize = 0;
    if (!readExact(manifest, &irradianceSize, sizeof(int32_t))
        || !readExact(manifest, &prefilterBaseSize, sizeof(int32_t))
        || !readExact(manifest, &prefilterMipCount, sizeof(int32_t))
        || !readExact(manifest, &brdfSize, sizeof(int32_t))) {
        error = QStringLiteral("truncated cache manifest payload");
        return false;
    }

    if (irradianceSize != HdrIbl::kIrradianceFaceSize
        || prefilterBaseSize != HdrIbl::kPrefilterBaseFaceSize
        || prefilterMipCount != HdrIbl::kPrefilterMipCount
        || brdfSize != HdrIbl::kBrdfLutSize) {
        error = QStringLiteral("cache manifest dimensions mismatch");
        return false;
    }

    const uint32_t irradiancePayload = cubemapPayloadBytes(
        HdrEquirect::CubemapFaces{.faceSize = irradianceSize});
    if (!loadPayloadFile(
            irradiancePath(cacheKey),
            irradiancePayload,
            [&](QFile& file, QString& loadError) {
                return readCubemapFaces(file, out.irradiance, loadError);
            },
            error)) {
        return false;
    }

    HdrIbl::PrefilterChain expectedPrefilter;
    expectedPrefilter.mips.resize(static_cast<size_t>(prefilterMipCount));
    for (int mip = 0; mip < prefilterMipCount; ++mip) {
        expectedPrefilter.mips[static_cast<size_t>(mip)].faceSize =
            std::max(1, prefilterBaseSize >> mip);
        expectedPrefilter.mips[static_cast<size_t>(mip)].faces.faceSize =
            expectedPrefilter.mips[static_cast<size_t>(mip)].faceSize;
    }
    const uint32_t prefilterPayload = prefilterPayloadBytes(expectedPrefilter);
    if (!loadPayloadFile(
            prefilterPath(cacheKey),
            prefilterPayload,
            [&](QFile& file, QString& loadError) {
                return readPrefilterChain(file, out.prefilter, loadError);
            },
            error)) {
        return false;
    }

    HdrIbl::BrdfLut expectedBrdf;
    expectedBrdf.size = brdfSize;
    expectedBrdf.rg.resize(static_cast<size_t>(brdfSize) * static_cast<size_t>(brdfSize) * 2u);
    const uint32_t brdfPayload = brdfPayloadBytes(expectedBrdf);
    if (!loadPayloadFile(
            brdfPath(cacheKey),
            brdfPayload,
            [&](QFile& file, QString& loadError) { return readBrdfLut(file, out.brdfLut, loadError); },
            error)) {
        return false;
    }

    return true;
}

bool save(const QString& cacheKey, const HdrIbl::IblBakeResult& in, QString& error)
{
    error.clear();
    if (cacheKey.isEmpty()) {
        error = QStringLiteral("empty cache key");
        return false;
    }

    const QString dir = entryDirectory(cacheKey);
    QDir().mkpath(dir);

    const uint32_t irradiancePayload = cubemapPayloadBytes(in.irradiance);
    if (!savePayloadFile(
            irradiancePath(cacheKey),
            irradiancePayload,
            [&](QFile& file) { return writeCubemapFaces(file, in.irradiance); })) {
        error = QStringLiteral("failed to write irradiance cache");
        invalidate(cacheKey);
        return false;
    }

    const uint32_t prefilterPayload = prefilterPayloadBytes(in.prefilter);
    if (!savePayloadFile(
            prefilterPath(cacheKey),
            prefilterPayload,
            [&](QFile& file) { return writePrefilterChain(file, in.prefilter); })) {
        error = QStringLiteral("failed to write prefilter cache");
        invalidate(cacheKey);
        return false;
    }

    const uint32_t brdfPayload = brdfPayloadBytes(in.brdfLut);
    if (!savePayloadFile(
            brdfPath(cacheKey),
            brdfPayload,
            [&](QFile& file) { return writeBrdfLut(file, in.brdfLut); })) {
        error = QStringLiteral("failed to write BRDF LUT cache");
        invalidate(cacheKey);
        return false;
    }

    QFile manifest(manifestPath(cacheKey));
    if (!manifest.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = QStringLiteral("failed to write cache manifest");
        invalidate(cacheKey);
        return false;
    }
    const FileHeader header{kMagic, kFormatVersion, sizeof(int32_t) * 4u};
    if (!writeExact(manifest, &header, sizeof(FileHeader))) {
        error = QStringLiteral("failed to write cache manifest header");
        invalidate(cacheKey);
        return false;
    }
    const int32_t dims[4] = {
        in.irradiance.faceSize,
        HdrIbl::kPrefilterBaseFaceSize,
        static_cast<int32_t>(in.prefilter.mips.size()),
        in.brdfLut.size,
    };
    if (!writeExact(manifest, dims, sizeof(dims))) {
        error = QStringLiteral("failed to write cache manifest payload");
        invalidate(cacheKey);
        return false;
    }

    return true;
}

void invalidate(const QString& cacheKey)
{
    if (cacheKey.isEmpty())
        return;
    QDir dir(entryDirectory(cacheKey));
    if (dir.exists())
        dir.removeRecursively();
}

} // namespace HdrCache
