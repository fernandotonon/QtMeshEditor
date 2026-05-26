#include "GteInverse.h"

#include <cmath>
#include <limits>

namespace GteInverse {

namespace {

constexpr double kFixedUnit = 4096.0;

// Floating-point RTPS forward (per psx-spx, no SAR rounding):
//   IR[r] = (sum_c RT[r][c] * V[c]) / 4096 + TR[r]
//   SX_pixel = H * IR[0] / IR[2] + OFX/65536
//   SY_pixel = H * IR[1] / IR[2] + OFY/65536
//   SZ      = IR[2]
//
// Pre-#675 this was a diagonal-only divide (`V[c] / RT[r][r]`) which silently
// passed the existing identity-matrix roundtrip test but rejected every real
// rotation matrix — the radius filter in MeshReconstructor::vertexFromPsx then
// fell back to psxScreenToWorld, painting a flat-XY blob.
void rtpsForward(const MatrixRecord &matrix, double mx, double my, double mz, double ir[3])
{
    const double V[3] = {mx, my, mz};
    for (int r = 0; r < 3; ++r) {
        const double rotated = static_cast<double>(matrix.rt.m[r][0]) * V[0]
                               + static_cast<double>(matrix.rt.m[r][1]) * V[1]
                               + static_cast<double>(matrix.rt.m[r][2]) * V[2];
        ir[r] = rotated / kFixedUnit + static_cast<double>(matrix.tr[r]);
    }
}

// RT^T inverse: assumes RT is an orthonormal rotation in 12.4 fixed point.
// For non-orthonormal matrices the caller must gate via
// GteCapture::looksOrthonormalRotation before invoking this path (#675).
//   V[r] = (sum_c RT[c][r] * (IR[c] - TR[c])) / 4096
void rtpsInverse(const MatrixRecord &matrix, const double irMinusTr[3], double v[3])
{
    for (int r = 0; r < 3; ++r) {
        const double rotated = static_cast<double>(matrix.rt.m[0][r]) * irMinusTr[0]
                               + static_cast<double>(matrix.rt.m[1][r]) * irMinusTr[1]
                               + static_cast<double>(matrix.rt.m[2][r]) * irMinusTr[2];
        v[r] = rotated / kFixedUnit;
    }
}

bool inIntRange(double v)
{
    return v >= static_cast<double>(std::numeric_limits<int>::min())
           && v <= static_cast<double>(std::numeric_limits<int>::max());
}

} // namespace

bool modelToScreen(const MatrixRecord &matrix, int mx, int my, int mz, int &sx, int &sy, int &sz)
{
    const double h = matrix.h != 0 ? static_cast<double>(matrix.h) : kFixedUnit;

    double ir[3];
    rtpsForward(matrix, mx, my, mz, ir);

    if (ir[2] == 0.0)
        return false;

    const double ofxPixel = static_cast<double>(matrix.ofx) / 65536.0;
    const double ofyPixel = static_cast<double>(matrix.ofy) / 65536.0;

    const double sxPixel = h * ir[0] / ir[2] + ofxPixel;
    const double syPixel = h * ir[1] / ir[2] + ofyPixel;
    if (!std::isfinite(sxPixel) || !std::isfinite(syPixel) || !std::isfinite(ir[2]))
        return false;
    if (!inIntRange(sxPixel) || !inIntRange(syPixel) || !inIntRange(ir[2]))
        return false;

    sx = static_cast<int>(std::lround(sxPixel));
    sy = static_cast<int>(std::lround(syPixel));
    sz = static_cast<int>(std::lround(ir[2]));
    return true;
}

bool screenToModel(const MatrixRecord &matrix, int sx, int sy, int sz, float &mx, float &my, float &mz)
{
    const double h = matrix.h != 0 ? static_cast<double>(matrix.h) : kFixedUnit;
    if (h == 0.0)
        return false;

    const double ir2 = static_cast<double>(sz);
    const double ofxPixel = static_cast<double>(matrix.ofx) / 65536.0;
    const double ofyPixel = static_cast<double>(matrix.ofy) / 65536.0;

    // Invert the projection: pixel-space -> IR (camera-space pre-divide).
    // We trust sz as IR[2] (GTE writes it directly) so IR[0] / IR[1] follow
    // from the standard pinhole equation.
    const double ir0 = (static_cast<double>(sx) - ofxPixel) * ir2 / h;
    const double ir1 = (static_cast<double>(sy) - ofyPixel) * ir2 / h;

    const double irMinusTr[3] = {
        ir0 - static_cast<double>(matrix.tr[0]),
        ir1 - static_cast<double>(matrix.tr[1]),
        ir2 - static_cast<double>(matrix.tr[2]),
    };

    double v[3];
    rtpsInverse(matrix, irMinusTr, v);

    mx = static_cast<float>(v[0]);
    my = static_cast<float>(v[1]);
    mz = static_cast<float>(v[2]);
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
