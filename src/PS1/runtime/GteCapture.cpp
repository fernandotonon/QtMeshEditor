#include "GteCapture.h"

#include <cstdlib>
#include <cstring>

namespace GteCapture {

namespace {

constexpr int64_t kUnit = 4096;
constexpr int64_t kUnitSq = kUnit * kUnit;        // 4096^2 == 16,777,216
constexpr int64_t kUnitCb = kUnitSq * kUnit;      // 4096^3 == 68,719,476,736

// 5% magnitude slack — covers 12.4 fixed-point rounding of cos/sin tables.
constexpr int64_t kMagTol = kUnitSq / 20;
// 5% slack on inter-row dot products relative to unit^2.
constexpr int64_t kDotTol = kUnitSq / 20;
// Determinant must be within +/- 10% of +4096^3 (proper rotation, not reflection).
constexpr int64_t kDetTol = kUnitCb / 10;

inline int64_t rowDot(const MatrixRecord &m, int r1, int r2)
{
    int64_t d = 0;
    for (int c = 0; c < 3; ++c)
        d += static_cast<int64_t>(m.rt.m[r1][c]) * static_cast<int64_t>(m.rt.m[r2][c]);
    return d;
}

inline int64_t determinant3x3(const MatrixRecord &m)
{
    const int64_t a = m.rt.m[0][0];
    const int64_t b = m.rt.m[0][1];
    const int64_t c = m.rt.m[0][2];
    const int64_t d = m.rt.m[1][0];
    const int64_t e = m.rt.m[1][1];
    const int64_t f = m.rt.m[1][2];
    const int64_t g = m.rt.m[2][0];
    const int64_t h = m.rt.m[2][1];
    const int64_t i = m.rt.m[2][2];
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}

} // namespace

uint64_t hashMatrix(const MatrixRecord &matrix)
{
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };

    mix(static_cast<uint64_t>(matrix.ofx));
    mix(static_cast<uint64_t>(matrix.ofy));
    mix(static_cast<uint64_t>(matrix.h));
    for (int i = 0; i < 3; ++i)
        mix(static_cast<uint64_t>(static_cast<uint32_t>(matrix.tr[i])));
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            mix(static_cast<uint64_t>(static_cast<uint32_t>(matrix.rt.m[r][c])));
    }
    return h;
}

bool matricesEqual(const MatrixRecord &a, const MatrixRecord &b)
{
    if (a.ofx != b.ofx || a.ofy != b.ofy || a.h != b.h)
        return false;
    if (std::memcmp(a.tr, b.tr, sizeof(a.tr)) != 0)
        return false;
    return std::memcmp(a.rt.m, b.rt.m, sizeof(a.rt.m)) == 0;
}

bool looksOrthonormalRotation(const MatrixRecord &matrix)
{
    // Row magnitudes — each row of a 12.4-fixed unit rotation has |row|^2 == 4096^2.
    for (int r = 0; r < 3; ++r) {
        const int64_t mag = rowDot(matrix, r, r);
        if (std::abs(mag - kUnitSq) > kMagTol)
            return false;
    }

    // Inter-row orthogonality — dot products should be ~0 within unit^2 tolerance.
    if (std::abs(rowDot(matrix, 0, 1)) > kDotTol)
        return false;
    if (std::abs(rowDot(matrix, 0, 2)) > kDotTol)
        return false;
    if (std::abs(rowDot(matrix, 1, 2)) > kDotTol)
        return false;

    // Determinant — proper rotation has det == +4096^3 (not -4096^3 / reflection).
    const int64_t det = determinant3x3(matrix);
    if (std::abs(det - kUnitCb) > kDetTol)
        return false;

    return true;
}

} // namespace GteCapture
