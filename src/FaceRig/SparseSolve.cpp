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

void SparseMatrix::diagOfNormalEquations(std::vector<double>& out) const
{
    out.assign(size_t(cols), 0.0);
    for (int r = 0; r < rows; ++r)
        for (int k = m_rowPtr[size_t(r)]; k < m_rowPtr[size_t(r) + 1]; ++k)
            out[size_t(m_col[size_t(k)])] += m_val[size_t(k)] * m_val[size_t(k)];
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

    // JACOBI PRECONDITIONER: M = diag(AᵀA). CG on the normal equations
    // squares the condition number, and this system is badly scaled — the
    // deformation-transfer matrix mixes per-triangle gradient rows with a
    // handful of anchor rows, so unpreconditioned CG crawled: measured on a
    // 27k-vertex head it burned 12000 iterations WITHOUT reaching tol, and
    // the truncated answer was not merely small but distorted (per-vertex
    // amplitude scattered 0.22-1.41 of target). Diagonal scaling is O(nnz)
    // to build, one divide per iteration, and needs no extra storage beyond
    // this vector.
    std::vector<double> invDiag(size_t(A.cols), 0.0);
    A.diagOfNormalEquations(invDiag);
    for (double& d : invDiag)
        d = d > 1e-300 ? 1.0 / d : 1.0;   // a zero column must not divide

    std::vector<double> z(size_t(A.cols));
    for (int i = 0; i < A.cols; ++i)
        z[size_t(i)] = invDiag[size_t(i)] * r[size_t(i)];
    p = z;
    double rz = 0;
    for (int i = 0; i < A.cols; ++i)
        rz += r[size_t(i)] * z[size_t(i)];
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
        const double a = rz / pAp;
        for (int i = 0; i < A.cols; ++i) {
            x[size_t(i)] += a * p[size_t(i)];
            r[size_t(i)] -= a * AtAp[size_t(i)];
        }
        double rsn = 0, rzn = 0;
        for (int i = 0; i < A.cols; ++i) {
            z[size_t(i)] = invDiag[size_t(i)] * r[size_t(i)];
            rsn += r[size_t(i)] * r[size_t(i)];
            rzn += r[size_t(i)] * z[size_t(i)];
        }
        const double beta = rzn / (rz > 1e-300 ? rz : 1e-300);
        for (int i = 0; i < A.cols; ++i)
            p[size_t(i)] = z[size_t(i)] + beta * p[size_t(i)];
        rs = rsn;
        rz = rzn;
    }
}

}  // namespace FaceRig
