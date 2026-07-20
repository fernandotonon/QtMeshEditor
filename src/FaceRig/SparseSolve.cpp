#include "SparseSolve.h"

namespace FaceRig {

void SparseMatrix::fromTriplets(int r, int c,
                                const std::vector<std::array<double, 3>>& trip)
{
    rows = r;
    cols = c;
    std::vector<int> cnt(size_t(r) + 1, 0);
    for (const auto& t : trip)
        cnt[size_t(t[0]) + 1]++;
    for (int i = 0; i < r; ++i)
        cnt[size_t(i) + 1] += cnt[size_t(i)];
    m_rowPtr = cnt;
    m_col.resize(trip.size());
    m_val.resize(trip.size());
    std::vector<int> cur = m_rowPtr;
    for (const auto& t : trip) {
        const int rr = int(t[0]);
        const int dst = cur[size_t(rr)]++;
        m_col[size_t(dst)] = int(t[1]);
        m_val[size_t(dst)] = t[2];
    }
}

void SparseMatrix::mul(const std::vector<double>& x, std::vector<double>& y) const
{
    y.assign(size_t(rows), 0.0);
    for (int r = 0; r < rows; ++r) {
        double s = 0;
        for (int k = m_rowPtr[size_t(r)]; k < m_rowPtr[size_t(r) + 1]; ++k)
            s += m_val[size_t(k)] * x[size_t(m_col[size_t(k)])];
        y[size_t(r)] = s;
    }
}

void SparseMatrix::mulT(const std::vector<double>& x, std::vector<double>& y) const
{
    y.assign(size_t(cols), 0.0);
    for (int r = 0; r < rows; ++r) {
        const double xr = x[size_t(r)];
        for (int k = m_rowPtr[size_t(r)]; k < m_rowPtr[size_t(r) + 1]; ++k)
            y[size_t(m_col[size_t(k)])] += m_val[size_t(k)] * xr;
    }
}

void solveLeastSquaresCG(const SparseMatrix& A, const std::vector<double>& b,
                         std::vector<double>& x, int maxIters, double tol)
{
    std::vector<double> Ax, r, p, Ap, AtAp;
    A.mul(x, Ax);
    std::vector<double> resid(size_t(A.rows));
    for (int i = 0; i < A.rows; ++i)
        resid[size_t(i)] = b[size_t(i)] - Ax[size_t(i)];
    A.mulT(resid, r);                     // r = Aᵀ(b - Ax)
    p = r;
    double rs = 0;
    for (double v : r)
        rs += v * v;
    const double rs0 = rs;
    if (rs0 <= 0.0)
        return;
    for (int it = 0; it < maxIters && rs > tol * tol * rs0; ++it) {
        A.mul(p, Ap);
        A.mulT(Ap, AtAp);                 // AtAp = AᵀA p
        double pAp = 0;
        for (int i = 0; i < A.cols; ++i)
            pAp += p[size_t(i)] * AtAp[size_t(i)];
        if (pAp <= 1e-30)
            break;
        const double a = rs / pAp;
        for (int i = 0; i < A.cols; ++i) {
            x[size_t(i)] += a * p[size_t(i)];
            r[size_t(i)] -= a * AtAp[size_t(i)];
        }
        double rsn = 0;
        for (double v : r)
            rsn += v * v;
        const double beta = rsn / rs;
        for (int i = 0; i < A.cols; ++i)
            p[size_t(i)] = r[size_t(i)] + beta * p[size_t(i)];
        rs = rsn;
    }
}

}  // namespace FaceRig
