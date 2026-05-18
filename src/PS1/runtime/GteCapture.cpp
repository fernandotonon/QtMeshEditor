#include "GteCapture.h"

#include <cstring>

namespace GteCapture {

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

} // namespace GteCapture
