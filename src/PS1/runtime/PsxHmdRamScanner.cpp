#include "PsxHmdRamScanner.h"

#include "EmuHooks.h"
#include "PsxModelRamScanCommon.h"

#include <QtGlobal>

namespace {

constexpr uint32_t kHmdMagic = 0x00000050u;
constexpr size_t kScanStrideBytes = 4u;
constexpr int kMaxCandidates = 32;

bool plausibleHmdHeader(const uint8_t *ram, size_t byteSize, size_t offset)
{
    // The first u32 is the magic (already checked). The next u32 is the "map length" in
    // dwords; subsequent fields are primitive header counts that should be modest. The
    // checks below filter the vast majority of noise — they aren't strong enough to fully
    // validate an HMD, which is why v1 stops short of mesh extraction.
    if (offset + 16u > byteSize)
        return false;
    const uint32_t mapLen = PsxModelRamScan::readU32le(ram + offset + 4);
    const uint32_t numPrimHeaders = PsxModelRamScan::readU32le(ram + offset + 8);
    if (mapLen == 0u || mapLen > 0x40000u)
        return false;
    if (numPrimHeaders == 0u || numPrimHeaders > 4096u)
        return false;
    return true;
}

} // namespace

int PsxHmdRamScanner::countHmdCandidates(const uint8_t *ram, size_t byteSize)
{
    if (!ram || byteSize < 16u)
        return 0;
    int count = 0;
    for (size_t offset = 0; offset + 4 <= byteSize && count < kMaxCandidates;
         offset += kScanStrideBytes) {
        if (PsxModelRamScan::readU32le(ram + offset) != kHmdMagic)
            continue;
        if (!plausibleHmdHeader(ram, byteSize, offset))
            continue;
        ++count;
    }
    return count;
}

int PsxHmdRamScanner::captureFromSystemRam(const uint8_t *ram, size_t byteSize, EmuHooks *hooks)
{
    if (!ram || !hooks || !hooks->isCaptureEnabled())
        return 0;

    const bool enabled = qEnvironmentVariableIsSet("QTMESH_PS1_HMD_SCANNER")
                         && qEnvironmentVariableIntValue("QTMESH_PS1_HMD_SCANNER") != 0;
    if (!enabled)
        return 0;

    // v1: count candidates for diagnostics, but don't emit meshes — extracting from the
    // hierarchical scene graph requires parsing primitive header types (4-byte tag +
    // type-specific layout) which we haven't validated against a known-good asset yet.
    // The candidate count is logged so testers can confirm the magic-bytes path works on
    // HMD-using titles; the actual walker lands in a follow-up.
    return countHmdCandidates(ram, byteSize);
}
