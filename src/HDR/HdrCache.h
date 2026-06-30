#pragma once

#include "HDR/HdrIblPrecompute.h"

#include <QString>

/// Versioned on-disk cache for IBL precompute results (#468).
namespace HdrCache {

constexpr uint32_t kMagic = 0x48445249u; // 'HDRI'
constexpr uint32_t kFormatVersion = 1;

QString cacheRootDirectory();
QString entryDirectory(const QString& cacheKey);

bool isValid(const QString& cacheKey);
bool load(const QString& cacheKey, HdrIbl::IblBakeResult& out, QString& error);
bool save(const QString& cacheKey, const HdrIbl::IblBakeResult& in, QString& error);
void invalidate(const QString& cacheKey);

} // namespace HdrCache
