#include "FaceRigAlign.h"

#include "Mocap/FaceCapPose.h"

#include <algorithm>
#include <cmath>

namespace FaceRig {

namespace {

std::array<float, 9> quatToMatrix(float x, float y, float z, float w)
{
    const float n = std::sqrt(x*x + y*y + z*z + w*w);
    if (n > 1e-12f) { x /= n; y /= n; z /= n; w /= n; }
    return { 1 - 2*(y*y + z*z), 2*(x*y - z*w),     2*(x*z + y*w),
             2*(x*y + z*w),     1 - 2*(x*x + z*z), 2*(y*z - x*w),
             2*(x*z - y*w),     2*(y*z + x*w),     1 - 2*(x*x + y*y) };
}

float rotationAngleDeg(const std::array<float, 9>& R)
{
    const float tr = R[0] + R[4] + R[8];
    const float c = std::clamp((tr - 1.0f) * 0.5f, -1.0f, 1.0f);
    return std::acos(c) * 180.0f / 3.14159265358979f;
}

}  // namespace

std::array<float, 3> rotateVec(const std::array<float, 9>& R,
                               const std::array<float, 3>& v, bool transpose)
{
    if (transpose)
        return { R[0]*v[0] + R[3]*v[1] + R[6]*v[2],
                 R[1]*v[0] + R[4]*v[1] + R[7]*v[2],
                 R[2]*v[0] + R[5]*v[1] + R[8]*v[2] };
    return { R[0]*v[0] + R[1]*v[1] + R[2]*v[2],
             R[3]*v[0] + R[4]*v[1] + R[5]*v[2],
             R[6]*v[0] + R[7]*v[1] + R[8]*v[2] };
}

void rotateInPlace(std::vector<float>& xyz, const std::array<float, 9>& R,
                   const std::array<float, 3>& pivot, bool transpose)
{
    for (size_t i = 0; i + 2 < xyz.size(); i += 3) {
        const std::array<float, 3> p{ xyz[i] - pivot[0], xyz[i+1] - pivot[1], xyz[i+2] - pivot[2] };
        const std::array<float, 3> q = rotateVec(R, p, transpose);
        xyz[i] = q[0] + pivot[0]; xyz[i+1] = q[1] + pivot[1]; xyz[i+2] = q[2] + pivot[2];
    }
}

SimilarityAlign alignPoints(const std::vector<std::array<float, 3>>& src,
                            const std::vector<std::array<float, 3>>& dst)
{
    SimilarityAlign a;
    const int n = int(std::min(src.size(), dst.size()));
    a.count = n;
    if (n < 3) return a;

    std::vector<float> s(size_t(n) * 3), d(size_t(n) * 3), w(size_t(n), 1.0f);
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) {
            s[size_t(i)*3 + size_t(k)] = src[size_t(i)][size_t(k)];
            d[size_t(i)*3 + size_t(k)] = dst[size_t(i)][size_t(k)];
        }
    const FaceCapPose::Result r = FaceCapPose::solve(s.data(), d.data(), w.data(), n);
    if (!r.ok || !std::isfinite(r.scale) || r.scale <= 0.0f) return a;

    a.R = quatToMatrix(r.rotation[0], r.rotation[1], r.rotation[2], r.rotation[3]);
    a.scale = r.scale;
    a.translation = r.translation;
    a.angleDeg = rotationAngleDeg(a.R);

    // Residual after the fit, normalised by the destination spread so it is
    // comparable across mesh units (the convention the anchor gate and the
    // marker-swap heuristic already used).
    std::array<double, 3> cD{0, 0, 0};
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) cD[size_t(k)] += dst[size_t(i)][size_t(k)] / n;
    double spread = 0.0, resid = 0.0;
    for (int i = 0; i < n; ++i) {
        const std::array<float, 3> fit = rotateVec(a.R, src[size_t(i)], false);
        double e2 = 0.0, s2 = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double f = double(a.scale) * fit[size_t(k)] + a.translation[size_t(k)];
            const double e = dst[size_t(i)][size_t(k)] - f;
            const double sp = dst[size_t(i)][size_t(k)] - cD[size_t(k)];
            e2 += e * e; s2 += sp * sp;
        }
        resid += std::sqrt(e2); spread += std::sqrt(s2);
    }
    if (spread < 1e-12) return a;
    a.residual = float(resid / spread);   // (resid/n) / (spread/n)
    a.ok = std::isfinite(a.residual);
    return a;
}

SimilarityAlign alignTemplateToUser(const std::vector<float>& tmplV,
                                    const std::vector<NricpLandmark>& anchors)
{
    std::vector<std::array<float, 3>> src, dst;
    const int tvc = int(tmplV.size() / 3);
    for (const auto& lm : anchors) {
        if (lm.tmplVertex < 0 || lm.tmplVertex >= tvc) continue;
        src.push_back({ tmplV[size_t(lm.tmplVertex)*3],
                        tmplV[size_t(lm.tmplVertex)*3+1],
                        tmplV[size_t(lm.tmplVertex)*3+2] });
        dst.push_back(lm.target);
    }
    return alignPoints(src, dst);
}

double rotationAwareResidual(const std::vector<std::array<float, 3>>& src,
                             const std::vector<std::array<float, 3>>& dst)
{
    const SimilarityAlign a = alignPoints(src, dst);
    return a.ok ? double(a.residual) : 1e9;
}

std::array<float, 9> yawToPlusZ(const std::array<float, 3>& faceDir,
                                float minAngleDeg, float* outAngleDeg)
{
    std::array<float, 9> I{1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (outAngleDeg) *outAngleDeg = 0.0f;
    const float fx = faceDir[0], fz = faceDir[2];
    const float len = std::sqrt(fx*fx + fz*fz);
    if (!(len > 1e-6f)) return I;
    const float nx = fx / len, nz = fz / len;
    // angle from +Z to (nx, nz) about +Y
    const float ang = std::atan2(nx, nz);
    const float deg = std::fabs(ang) * 180.0f / 3.14159265358979f;
    if (deg < minAngleDeg) return I;
    if (outAngleDeg) *outAngleDeg = deg;
    // Rotation about +Y by -ang maps (nx, nz) onto (0, 1).
    const float c = std::cos(-ang), s = std::sin(-ang);
    return { c, 0, s,  0, 1, 0,  -s, 0, c };
}

}  // namespace FaceRig
