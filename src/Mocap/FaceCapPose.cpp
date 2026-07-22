#include "FaceCapPose.h"

#include "FaceCapCanonicalData.h"

#include <cmath>

namespace FaceCapPose {

namespace {

// Largest-eigenvalue eigenvector of a symmetric 4x4 matrix via cyclic Jacobi.
// Deterministic, ~1e-12 accurate after a handful of sweeps.
std::array<double, 4> maxEigenvector4(double m[4][4])
{
    double v[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    for (int sweep = 0; sweep < 32; ++sweep) {
        double off = 0.0;
        for (int p = 0; p < 4; ++p)
            for (int q = p + 1; q < 4; ++q)
                off += m[p][q] * m[p][q];
        if (off < 1e-24)
            break;
        for (int p = 0; p < 4; ++p) {
            for (int q = p + 1; q < 4; ++q) {
                if (std::abs(m[p][q]) < 1e-30)
                    continue;
                const double theta = (m[q][q] - m[p][p]) / (2.0 * m[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0)
                                 / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (int k = 0; k < 4; ++k) {
                    const double mkp = m[k][p], mkq = m[k][q];
                    m[k][p] = c * mkp - s * mkq;
                    m[k][q] = s * mkp + c * mkq;
                }
                for (int k = 0; k < 4; ++k) {
                    const double mpk = m[p][k], mqk = m[q][k];
                    m[p][k] = c * mpk - s * mqk;
                    m[q][k] = s * mpk + c * mqk;
                }
                for (int k = 0; k < 4; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
    int best = 0;
    for (int i = 1; i < 4; ++i)
        if (m[i][i] > m[best][best])
            best = i;
    return {v[0][best], v[1][best], v[2][best], v[3][best]};
}

void rotate(const std::array<float, 4>& q, const double in[3], double out[3])
{
    // (x,y,z,w) quaternion rotation of a vector
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double tx = 2.0 * (y * in[2] - z * in[1]);
    const double ty = 2.0 * (z * in[0] - x * in[2]);
    const double tz = 2.0 * (x * in[1] - y * in[0]);
    out[0] = in[0] + w * tx + (y * tz - z * ty);
    out[1] = in[1] + w * ty + (z * tx - x * tz);
    out[2] = in[2] + w * tz + (x * ty - y * tx);
}

}  // namespace

Result solve(const float* src, const float* dst, const float* weights, int count)
{
    Result r;
    if (count < 3)
        return r;

    double wSum = 0.0;
    double muS[3] = {0, 0, 0}, muD[3] = {0, 0, 0};
    for (int i = 0; i < count; ++i) {
        const double w = weights ? weights[i] : 1.0;
        if (w <= 0.0)
            continue;
        wSum += w;
        for (int k = 0; k < 3; ++k) {
            muS[k] += w * src[i * 3 + k];
            muD[k] += w * dst[i * 3 + k];
        }
    }
    if (wSum <= 0.0)
        return r;
    for (int k = 0; k < 3; ++k) {
        muS[k] /= wSum;
        muD[k] /= wSum;
    }

    // weighted covariance src x dst (Horn's S = sum s.d^T) + source variance
    // for the scale. Order matters: S[a][b] = s[a]*d[b] yields the rotation
    // taking SRC into DST; the transpose yields its inverse.
    double cov[3][3] = {{0}};
    double varS = 0.0;
    for (int i = 0; i < count; ++i) {
        const double w = weights ? weights[i] : 1.0;
        if (w <= 0.0)
            continue;
        double s[3], d[3];
        for (int k = 0; k < 3; ++k) {
            s[k] = src[i * 3 + k] - muS[k];
            d[k] = dst[i * 3 + k] - muD[k];
        }
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                cov[a][b] += w * s[a] * d[b];
        varS += w * (s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    }
    if (varS <= 0.0)
        return r;

    // Horn's method: max eigenvector of the 4x4 built from the covariance
    const double sxx = cov[0][0], sxy = cov[0][1], sxz = cov[0][2];
    const double syx = cov[1][0], syy = cov[1][1], syz = cov[1][2];
    const double szx = cov[2][0], szy = cov[2][1], szz = cov[2][2];
    double n[4][4] = {
        {sxx + syy + szz, syz - szy, szx - sxz, sxy - syx},
        {syz - szy, sxx - syy - szz, sxy + syx, szx + sxz},
        {szx - sxz, sxy + syx, -sxx + syy - szz, syz + szy},
        {sxy - syx, szx + sxz, syz + szy, -sxx - syy + szz},
    };
    const std::array<double, 4> e = maxEigenvector4(n);  // (w, x, y, z)
    double norm = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2] + e[3] * e[3]);
    if (norm <= 0.0)
        return r;
    // store as (x,y,z,w), w kept positive for a canonical representation
    const double sign = e[0] >= 0.0 ? 1.0 : -1.0;
    r.rotation = {static_cast<float>(sign * e[1] / norm),
                  static_cast<float>(sign * e[2] / norm),
                  static_cast<float>(sign * e[3] / norm),
                  static_cast<float>(sign * e[0] / norm)};

    // scale = sum(w * d . (R s)) / sum(w * |s|^2)
    double dot = 0.0;
    for (int i = 0; i < count; ++i) {
        const double w = weights ? weights[i] : 1.0;
        if (w <= 0.0)
            continue;
        double s[3], rs[3], d[3];
        for (int k = 0; k < 3; ++k) {
            s[k] = src[i * 3 + k] - muS[k];
            d[k] = dst[i * 3 + k] - muD[k];
        }
        rotate(r.rotation, s, rs);
        dot += w * (d[0] * rs[0] + d[1] * rs[1] + d[2] * rs[2]);
    }
    r.scale = static_cast<float>(dot / varS);

    // t = muD - scale * R * muS  (scale participates: the fit is a similarity;
    // the caller treats rotation as the pose and translation as informational)
    double rMu[3];
    rotate(r.rotation, muS, rMu);
    for (int k = 0; k < 3; ++k)
        r.translation[k] = static_cast<float>(muD[k] - r.scale * rMu[k]);
    r.ok = true;
    return r;
}

Result solveHeadPose(const float* landmarksXyz, int landmarkCount)
{
    if (landmarkCount < FaceCap::kCanonicalVertexCount)
        return {};
    // gather the weighted Procrustes basis subset in both frames
    float src[FaceCap::kProcrustesBasisCount * 3];
    float dst[FaceCap::kProcrustesBasisCount * 3];
    float wgt[FaceCap::kProcrustesBasisCount];
    for (int i = 0; i < FaceCap::kProcrustesBasisCount; ++i) {
        const int id = FaceCap::kProcrustesLandmarkIds[i];
        for (int k = 0; k < 3; ++k)
            src[i * 3 + k] = FaceCap::kCanonicalFaceModel[id * 3 + k];
        // image frame -> pose frame: +X right stays, y and z flip
        dst[i * 3 + 0] = landmarksXyz[id * 3 + 0];
        dst[i * 3 + 1] = -landmarksXyz[id * 3 + 1];
        dst[i * 3 + 2] = -landmarksXyz[id * 3 + 2];
        wgt[i] = FaceCap::kProcrustesWeights[i];
    }
    return solve(src, dst, wgt, FaceCap::kProcrustesBasisCount);
}

}  // namespace FaceCapPose
