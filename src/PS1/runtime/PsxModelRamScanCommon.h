#ifndef PSXMODELRAMSCANCOMMON_H
#define PSXMODELRAMSCANCOMMON_H

#include <cstddef>
#include <cstdint>
#include <cstring>

/**
 * Tiny shared helpers for PsxTmdRamScanner / PsxHmdRamScanner / other model-space
 * format scanners (#674). Header-only so each scanner stays self-contained.
 */
namespace PsxModelRamScan {

inline uint32_t readU32le(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
           | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint16_t readU16le(const uint8_t *p)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0])
                                | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8));
}

inline int16_t readI16le(const uint8_t *p)
{
    return static_cast<int16_t>(readU16le(p));
}

/** Bounds-checked u32 fetch. Returns 0 (i.e. usually "fails validation") on OOB. */
inline uint32_t safeReadU32(const uint8_t *ram, size_t byteSize, size_t offset)
{
    if (offset + 4 > byteSize)
        return 0;
    return readU32le(ram + offset);
}

/**
 * FNV-1a 64-bit hash over a byte range. Used as a content-hash for cross-frame TMD/HMD
 * dedupe — same blob at the same offset across many frames should only emit once.
 */
inline uint64_t fnv1a64(const uint8_t *data, size_t size)
{
    constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
    constexpr uint64_t kFnvPrime = 1099511628211ULL;
    uint64_t hash = kFnvOffset;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

} // namespace PsxModelRamScan

#endif // PSXMODELRAMSCANCOMMON_H
