#include "PsxGteRamScanner.h"

#include "GteCapture.h"
#include "RipperHooks.h"

#include <QSet>

#include <cmath>
#include <cstring>

namespace {

constexpr size_t kMatrixBytes = sizeof(MatrixRecord) - sizeof(uint64_t);
constexpr int kMaxMatricesPerFrame = 48;

bool looksLikeMatrixRecord(const MatrixRecord &matrix)
{
    if (matrix.h < 50 || matrix.h > 8192)
        return false;
    if (matrix.ofx < 0 || matrix.ofy < 0)
        return false;

    bool anyRotation = false;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const int32_t v = matrix.rt.m[r][c];
            if (v == 0)
                continue;
            if (std::abs(v) > (1 << 16))
                return false;
            anyRotation = true;
        }
    }
    if (!anyRotation)
        return false;

    for (int i = 0; i < 3; ++i) {
        if (std::abs(matrix.tr[i]) > (1 << 20))
            return false;
    }
    return true;
}

bool readMatrixAt(const uint8_t *ram, size_t byteSize, size_t byteOffset, MatrixRecord &out)
{
    if (byteOffset + kMatrixBytes > byteSize)
        return false;

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

void PsxGteRamScanner::captureFromSystemRam(const uint8_t *ram, size_t byteSize, RipperHooks *hooks)
{
    if (!ram || byteSize < kMatrixBytes || !hooks || !hooks->isCaptureEnabled())
        return;

    QSet<uint64_t> seenHashes;
    int found = 0;

    for (size_t offset = 0; offset + kMatrixBytes <= byteSize && found < kMaxMatricesPerFrame; offset += 4) {
        MatrixRecord matrix{};
        if (!readMatrixAt(ram, byteSize, offset, matrix))
            continue;

        matrix.hash = GteCapture::hashMatrix(matrix);
        if (seenHashes.contains(matrix.hash))
            continue;
        seenHashes.insert(matrix.hash);

        hooks->onGteMatrix(matrix);
        ++found;
    }
}
