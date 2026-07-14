#include "DeformationTransfer.h"

#include <array>
#include <cmath>

namespace FaceRig {

namespace {

using Vec3 = std::array<double, 3>;
using Mat3 = std::array<double, 9>;   // row-major

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
double norm(const Vec3& a) { return std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]); }

Vec3 vert(const std::vector<float>& v, int i)
{
    return {v[size_t(i)*3], v[size_t(i)*3+1], v[size_t(i)*3+2]};
}

// 4th "normal" vertex: v4 = v1 + n/√|n|, n = (v2-v1)×(v3-v1)
Vec3 normalV4(const Vec3& a, const Vec3& b, const Vec3& c)
{
    const Vec3 n = cross(sub(b, a), sub(c, a));
    const double ln = norm(n);
    const double s = ln > 1e-12 ? 1.0 / std::sqrt(ln) : 0.0;
    return {a[0]+n[0]*s, a[1]+n[1]*s, a[2]+n[2]*s};
}

// frame V = [v2-v1, v3-v1, v4-v1] as columns (row-major 3x3)
Mat3 frame(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d)
{
    const Vec3 e1 = sub(b,a), e2 = sub(c,a), e3 = sub(d,a);
    return {e1[0], e2[0], e3[0],
            e1[1], e2[1], e3[1],
            e1[2], e2[2], e3[2]};
}

bool invert3(const Mat3& m, Mat3& out)
{
    const double det =
        m[0]*(m[4]*m[8]-m[5]*m[7]) - m[1]*(m[3]*m[8]-m[5]*m[6])
      + m[2]*(m[3]*m[7]-m[4]*m[6]);
    if (std::abs(det) < 1e-18)
        return false;
    const double id = 1.0/det;
    out[0] = (m[4]*m[8]-m[5]*m[7])*id;
    out[1] = (m[2]*m[7]-m[1]*m[8])*id;
    out[2] = (m[1]*m[5]-m[2]*m[4])*id;
    out[3] = (m[5]*m[6]-m[3]*m[8])*id;
    out[4] = (m[0]*m[8]-m[2]*m[6])*id;
    out[5] = (m[2]*m[3]-m[0]*m[5])*id;
    out[6] = (m[3]*m[7]-m[4]*m[6])*id;
    out[7] = (m[1]*m[6]-m[0]*m[7])*id;
    out[8] = (m[0]*m[4]-m[1]*m[3])*id;
    return true;
}

Mat3 matmul(const Mat3& a, const Mat3& b)
{
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                r[size_t(i*3+j)] += a[size_t(i*3+k)] * b[size_t(k*3+j)];
    return r;
}

}  // namespace

bool DeformationTransfer::init(const std::vector<float>& tmplNeutral,
                               const std::vector<int>& faces,
                               const std::vector<float>& fitted)
{
    m_valid = false;
    const int N = int(tmplNeutral.size() / 3);
    const int F = int(faces.size() / 3);
    if (N < 3 || F < 1 || int(fitted.size() / 3) != N)
        return false;

    m_n = N;
    m_tmplNeutral = tmplNeutral;
    m_faces = faces;
    m_fitted = fitted;
    m_srcRestInv.assign(size_t(F), {});
    m_tgtRestInv.assign(size_t(F), {});
    m_tgtNormalV4.assign(size_t(F), {});

    // Unknowns (per axis, solved independently): the N deformed FITTED vertices
    // PLUS one free "4th vertex" per triangle (columns N..N+F-1). The 4th vertex
    // is Sumner's normal-direction trick: it only exists to make each triangle
    // frame V = [x1-x0, x2-x0, x4-x0] a full-rank 3x3, and in the DEFORMED solve
    // it is left UNCONSTRAINED (no normal equation pins it) — it simply absorbs
    // the out-of-plane component the two edges can't represent.
    //
    // Per triangle the deformation gradient w.r.t. the target REST frame is
    //   A_f = Vtgt_deformed · Vtgt_rest⁻¹,   and we require A_f == S_f (source).
    // Each of the 3 gradient columns g gives one row (per axis): the deformed
    // edge combination Σ_col Vinv[col][g]·(x_{col}-x0) must equal S column g.
    // Anchor rows (small weight x_i == rest_i) fix the translation gauge.

    std::vector<std::array<double, 3>> trip;
    trip.reserve(size_t(F) * 3 * 4 + size_t(N));

    auto pushGrad = [&](int rowBase, int f) {
        const int i0 = m_faces[size_t(f)*3];
        const int i1 = m_faces[size_t(f)*3+1];
        const int i2 = m_faces[size_t(f)*3+2];
        const int i3 = N + f;                    // the free 4th-vertex column
        const Vec3 a = vert(m_fitted, i0), b = vert(m_fitted, i1), c = vert(m_fitted, i2);
        const Vec3 d = normalV4(a, b, c);
        m_tgtNormalV4[size_t(f)] = d;
        Mat3 Vt = frame(a, b, c, d), Vinv;
        if (!invert3(Vt, Vinv)) {
            // degenerate target triangle — skip its gradient rows (anchors keep it)
            m_tgtRestInv[size_t(f)] = {};
            return;
        }
        m_tgtRestInv[size_t(f)] = Vinv;
        // edges columns are [x1-x0, x2-x0, x4-x0]; gradient column g combines
        // them by Vinv[col][g]. Distribute onto the four real unknowns:
        //   x1 → Vinv[0][g], x2 → Vinv[1][g], x4 → Vinv[2][g],
        //   x0 → -(sum of the three)  (from every -x0 term)
        for (int g = 0; g < 3; ++g) {
            const double w1 = Vinv[size_t(0*3+g)];
            const double w2 = Vinv[size_t(1*3+g)];
            const double w4 = Vinv[size_t(2*3+g)];
            const double w0 = -(w1 + w2 + w4);
            const int row = rowBase + g;
            trip.push_back({double(row), double(i0), w0});
            trip.push_back({double(row), double(i1), w1});
            trip.push_back({double(row), double(i2), w2});
            trip.push_back({double(row), double(i3), w4});
        }
    };

    // rows: 3 per triangle (gradient columns) + ONE anchor row.
    // columns: N real verts + F free 4th-vertices.
    //
    // Deformation gradients are translation-invariant, so the system is only
    // determined up to a global translation. We pin exactly ONE real vertex
    // (index 0) to fix that gauge — anchoring EVERY vertex would fight the
    // shape (the gradients want expr, the anchor wants neutral) and pull the
    // solution back toward the neutral, corrupting the transfer.
    for (int f = 0; f < F; ++f)
        pushGrad(3 * f, f);
    const double anchorW = 1.0;
    trip.push_back({double(3 * F), 0.0, anchorW});

    m_A.fromTriplets(3 * F + 1, N + F, trip);

    // source (template) rest inverse frames — for building S_f per shape
    for (int f = 0; f < F; ++f) {
        const int i0 = m_faces[size_t(f)*3];
        const int i1 = m_faces[size_t(f)*3+1];
        const int i2 = m_faces[size_t(f)*3+2];
        const Vec3 a = vert(m_tmplNeutral, i0), b = vert(m_tmplNeutral, i1),
                   c = vert(m_tmplNeutral, i2);
        const Vec3 d = normalV4(a, b, c);
        Mat3 Vs = frame(a, b, c, d), Vinv;
        m_srcRestInv[size_t(f)] = invert3(Vs, Vinv) ? Vinv : Mat3{};
    }

    m_valid = true;
    return true;
}

std::vector<float> DeformationTransfer::transfer(
    const std::vector<float>& tmplExprDelta) const
{
    if (!m_valid || int(tmplExprDelta.size() / 3) != m_n)
        return {};
    const int F = int(m_faces.size() / 3);

    // template EXPRESSION verts = neutral + delta
    std::vector<float> expr(m_tmplNeutral.size());
    for (size_t i = 0; i < expr.size(); ++i)
        expr[i] = m_tmplNeutral[i] + tmplExprDelta[i];

    // Build rhs for each of the 3 output axes. For triangle f:
    //   S_f = Vsrc_expr · Vsrc_rest⁻¹      (3x3 source deformation gradient).
    // The gradient constraint rows require the target gradient column g == S
    // column g; the free 4th vertex is an unknown (its normal equation exists
    // in the matrix), so the rhs is just S — no rest-approximation term.
    const int nCols = m_n + F;
    std::vector<std::vector<double>> rhs(3, std::vector<double>(size_t(3 * F + 1), 0.0));

    for (int f = 0; f < F; ++f) {
        const int i0 = m_faces[size_t(f)*3];
        const int i1 = m_faces[size_t(f)*3+1];
        const int i2 = m_faces[size_t(f)*3+2];
        // source expression frame
        Vec3 a{expr[size_t(i0)*3], expr[size_t(i0)*3+1], expr[size_t(i0)*3+2]};
        Vec3 b{expr[size_t(i1)*3], expr[size_t(i1)*3+1], expr[size_t(i1)*3+2]};
        Vec3 c{expr[size_t(i2)*3], expr[size_t(i2)*3+1], expr[size_t(i2)*3+2]};
        Vec3 d = normalV4(a, b, c);
        Mat3 Vexpr = frame(a, b, c, d);
        const Mat3& VsInv = m_srcRestInv[size_t(f)];
        const Mat3 S = matmul(Vexpr, VsInv);   // 3x3 source gradient
        for (int axis = 0; axis < 3; ++axis)
            for (int g = 0; g < 3; ++g)
                rhs[size_t(axis)][size_t(3*f+g)] = S[size_t(axis*3+g)];
    }
    // anchor rhs: pin vertex 0 to (fitted rest + the template's vertex-0
    // displacement) — fixes the translation gauge while letting the shape
    // move vertex 0 the same way the source moved its vertex 0. For the
    // identity fit this is exactly expr[0].
    const double anchorW = 1.0;
    for (int axis = 0; axis < 3; ++axis)
        rhs[size_t(axis)][size_t(3*F)] =
            anchorW * (m_fitted[axis] + tmplExprDelta[size_t(axis)]);

    // solve per axis (warm-start real verts at the fitted rest, 4th verts at
    // their rest normal offset). Read back only the N real vertices as a delta.
    std::vector<float> out(size_t(m_n) * 3, 0.0f);
    for (int axis = 0; axis < 3; ++axis) {
        std::vector<double> x(size_t(nCols), 0.0);
        for (int i = 0; i < m_n; ++i)
            x[size_t(i)] = m_fitted[size_t(i)*3+axis];
        for (int f = 0; f < F; ++f)
            x[size_t(m_n + f)] = m_tgtNormalV4[size_t(f)][size_t(axis)];
        solveLeastSquaresCG(m_A, rhs[size_t(axis)], x, 800, 1e-8);
        for (int i = 0; i < m_n; ++i)
            out[size_t(i)*3+axis] =
                float(x[size_t(i)] - m_fitted[size_t(i)*3+axis]);  // delta
    }
    return out;
}

}  // namespace FaceRig
