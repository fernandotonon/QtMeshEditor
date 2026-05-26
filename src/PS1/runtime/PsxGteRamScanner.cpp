#include "PsxGteRamScanner.h"

#include "EmuHooks.h"
#include "GteCapture.h"
#include "PsxGp0Opcode.h"

#include <QSet>

#include <cmath>
#include <cstring>

namespace {

constexpr size_t kMatrixBytes = sizeof(MatrixRecord) - sizeof(uint64_t);
constexpr int kMaxMatricesPerFrame = 48;

// #675: tightened from the pre-#675 looksLikeMatrixRecord (`h ∈ [50, 8192]`,
// per-entry magnitude only) which accepted ~192 false positives per single-box
// retail capture. Real PS1 GTE matrices satisfy three structural invariants —
// (a) `H` lives in the cluster of common projection denominators (~64..2048,
// titles author 128/200/240/256/300/320/400/512 almost exclusively per
// psx-spx), (b) `OFX`/`OFY` decoded to pixels fits the 1024×512 framebuffer,
// (c) the RT block is an orthonormal rotation in 12.4 fixed point. The
// orthonormal check (rows have unit magnitude, are mutually orthogonal, det == +1)
// is delegated to GteCapture::looksOrthonormalRotation so the same gate is
// reused by GteInverse::screenToModel's RT^T fast path.
bool looksLikeMatrixRecord(const MatrixRecord &matrix)
{
    if (matrix.h < 64 || matrix.h > 2048)
        return false;
    if (matrix.ofx < 0 || matrix.ofy < 0)
        return false;

    const int ofxScreen = matrix.ofx >> 16;
    const int ofyScreen = matrix.ofy >> 16;
    if (ofxScreen > 1024 || ofyScreen > 1024)
        return false;

    // Reject anything where any RT entry overflows a sane 12.4 fixed-point
    // rotation before the more expensive orthonormality check.
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (std::abs(matrix.rt.m[r][c]) > (1 << 13))
                return false;
        }
    }

    for (int i = 0; i < 3; ++i) {
        if (std::abs(matrix.tr[i]) > (1 << 20))
            return false;
    }

    return GteCapture::looksOrthonormalRotation(matrix);
}

bool readMatrixAt(const uint8_t *ram, size_t byteSize, size_t byteOffset, MatrixRecord &out)
{
    if (byteOffset + kMatrixBytes > byteSize)
        return false;

    for (int word = 0; word < 3; ++word) {
        uint32_t probe = 0;
        std::memcpy(&probe, ram + byteOffset + static_cast<size_t>(word) * 4, sizeof(probe));
        if (psxLooksLikeGp0Opcode(probe))
            return false;
    }

    std::memset(&out, 0, sizeof(out));
    std::memcpy(&out.rt, ram + byteOffset, sizeof(out.rt));
    std::memcpy(out.tr, ram + byteOffset + sizeof(out.rt), sizeof(out.tr));
    std::memcpy(&out.ofx, ram + byteOffset + sizeof(out.rt) + sizeof(out.tr), sizeof(out.ofx));
    std::memcpy(&out.ofy, ram + byteOffset + sizeof(out.rt) + sizeof(out.tr) + sizeof(out.ofx),
                sizeof(out.ofy));
    std::memcpy(&out.h, ram + byteOffset + sizeof(out.rt) + sizeof(out.tr) + sizeof(out.ofx) + sizeof(out.ofy),
                sizeof(out.h));

    return looksLikeMatrixRecord(out);
}

} // namespace

void PsxGteRamScanner::captureFromSystemRam(const uint8_t *ram, size_t byteSize, EmuHooks *hooks)
{
    if (!ram || byteSize < kMatrixBytes || !hooks || !hooks->isCaptureEnabled())
        return;

    QSet<uint64_t> seenHashes;
    int found = 0;

    for (size_t offset = 0; offset + kMatrixBytes <= byteSize && found < kMaxMatricesPerFrame;) {
        uint32_t header = 0;
        std::memcpy(&header, ram + offset, sizeof(header));
        if (psxLooksLikeGp0Opcode(header)) {
            offset += 4;
            continue;
        }

        MatrixRecord matrix{};
        if (!readMatrixAt(ram, byteSize, offset, matrix)) {
            offset += 4;
            continue;
        }

        matrix.hash = GteCapture::hashMatrix(matrix);
        if (seenHashes.contains(matrix.hash)) {
            offset += kMatrixBytes;
            continue;
        }
        seenHashes.insert(matrix.hash);

        hooks->onGteMatrix(matrix);
        ++found;
        offset += kMatrixBytes;
    }
}
