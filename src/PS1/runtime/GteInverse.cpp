#include "GteInverse.h"

#include <cmath>

namespace GteInverse {

bool screenToModel(const MatrixRecord &matrix, int sx, int sy, int sz, float &mx, float &my, float &mz)
{
    const double h = matrix.h != 0 ? static_cast<double>(matrix.h) : 4096.0;
    const double ir3 = sz != 0 ? static_cast<double>(sz) : 4096.0;
    const double ir1 = ((static_cast<double>(sx) * 65536.0) - matrix.ofx) * ir3 / h;
    const double ir2 = ((static_cast<double>(sy) * 65536.0) - matrix.ofy) * ir3 / h;

    const double irAdj[3] = {ir1 - matrix.tr[0], ir2 - matrix.tr[1], ir3 - matrix.tr[2]};
    double vx = 0.0;
    double vy = 0.0;
    double vz = 0.0;
    for (int c = 0; c < 3; ++c) {
        vx += static_cast<double>(matrix.rt.m[c][0]) * irAdj[c];
        vy += static_cast<double>(matrix.rt.m[c][1]) * irAdj[c];
        vz += static_cast<double>(matrix.rt.m[c][2]) * irAdj[c];
    }

    constexpr double kFixedScale = 1.0 / 4096.0;
    mx = static_cast<float>(vx * kFixedScale);
    my = static_cast<float>(vy * kFixedScale);
    mz = static_cast<float>(vz * kFixedScale);
    return std::isfinite(mx) && std::isfinite(my) && std::isfinite(mz);
}

void psxScreenToWorld(float sx, float sy, float sz, float &wx, float &wy, float &wz)
{
    constexpr float kCenterX = 160.0f;
    constexpr float kCenterY = 120.0f;
    constexpr float kScale = 0.01f;
    wx = (sx - kCenterX) * kScale;
    wy = -(sy - kCenterY) * kScale;
    wz = sz * kScale;
}

void modelToEditor(float mx, float my, float mz, float &wx, float &wy, float &wz)
{
    constexpr float kScale = 0.01f;
    wx = mx * kScale;
    wy = -my * kScale;
    wz = -mz * kScale;
}

} // namespace GteInverse
