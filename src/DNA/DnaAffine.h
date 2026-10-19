#ifndef DNA_DNAAFFINE_H
#define DNA_DNAAFFINE_H

// Tiny 3x4 affine helpers shared by the face bake and the rig-fitting prototype
// (epic #1095). Header-only, double precision, no dependencies.

#include <array>
#include <cmath>

namespace Dna {
namespace Affine {

/// Affine 3x4: [ A | t ] (row-major A).
struct Aff {
    double a[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double t[3] = {0, 0, 0};
};

inline Aff compose(const std::array<double, 3>& t, const std::array<double, 4>& q, const std::array<double, 3>& s)
{
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    const double n = std::sqrt(w * w + x * x + y * y + z * z);
    const double iw = n > 1e-12 ? w / n : 1, ix = n > 1e-12 ? x / n : 0, iy = n > 1e-12 ? y / n : 0, iz = n > 1e-12 ? z / n : 0;
    const double r[9] = {1 - 2 * (iy * iy + iz * iz), 2 * (ix * iy - iz * iw), 2 * (ix * iz + iy * iw),
                         2 * (ix * iy + iz * iw), 1 - 2 * (ix * ix + iz * iz), 2 * (iy * iz - ix * iw),
                         2 * (ix * iz - iy * iw), 2 * (iy * iz + ix * iw), 1 - 2 * (ix * ix + iy * iy)};
    Aff m;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            m.a[row * 3 + col] = r[row * 3 + col] * s[size_t(col)];   // R * diag(S)
    m.t[0] = t[0]; m.t[1] = t[1]; m.t[2] = t[2];
    return m;
}

inline Aff mul(const Aff& p, const Aff& c)   // p * c
{
    Aff m;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            double v = 0;
            for (int k = 0; k < 3; ++k) v += p.a[row * 3 + k] * c.a[k * 3 + col];
            m.a[row * 3 + col] = v;
        }
        m.t[row] = p.a[row * 3] * c.t[0] + p.a[row * 3 + 1] * c.t[1] + p.a[row * 3 + 2] * c.t[2] + p.t[row];
    }
    return m;
}

inline Aff inverse(const Aff& m)
{
    const double* a = m.a;
    const double c00 = a[4] * a[8] - a[5] * a[7], c01 = a[5] * a[6] - a[3] * a[8], c02 = a[3] * a[7] - a[4] * a[6];
    double det = a[0] * c00 + a[1] * c01 + a[2] * c02;
    Aff r;
    if (std::abs(det) < 1e-18) return r;        // singular: identity (a zero-scale joint cannot skin)
    const double id = 1.0 / det;
    r.a[0] = c00 * id;
    r.a[1] = (a[2] * a[7] - a[1] * a[8]) * id;
    r.a[2] = (a[1] * a[5] - a[2] * a[4]) * id;
    r.a[3] = c01 * id;
    r.a[4] = (a[0] * a[8] - a[2] * a[6]) * id;
    r.a[5] = (a[2] * a[3] - a[0] * a[5]) * id;
    r.a[6] = c02 * id;
    r.a[7] = (a[1] * a[6] - a[0] * a[7]) * id;
    r.a[8] = (a[0] * a[4] - a[1] * a[3]) * id;
    for (int row = 0; row < 3; ++row)
        r.t[row] = -(r.a[row * 3] * m.t[0] + r.a[row * 3 + 1] * m.t[1] + r.a[row * 3 + 2] * m.t[2]);
    return r;
}

}  // namespace Affine
}  // namespace Dna

#endif  // DNA_DNAAFFINE_H
