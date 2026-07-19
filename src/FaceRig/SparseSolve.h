#ifndef FACERIG_SPARSESOLVE_H
#define FACERIG_SPARSESOLVE_H

// Minimal dependency-free sparse linear algebra for the face-rig native
// solvers. A CSR matrix built from triplets + conjugate-gradient on the normal
// equations (AᵀA x = Aᵀb) — no Eigen, no external solver. Pure data,
// headless-tested. Used by DeformationTransfer (#892); NonRigidICP (#891) still
// carries its own equivalent copy and can be migrated onto this in a follow-up.

#include <array>
#include <vector>

namespace FaceRig {

class SparseMatrix {
public:
    int rows = 0, cols = 0;

    // Build from triplets {row, col, value}; duplicates are summed on multiply
    // (CG only needs the products, so we keep them un-coalesced).
    void fromTriplets(int r, int c, const std::vector<std::array<double, 3>>& trip);

    void mul(const std::vector<double>& x, std::vector<double>& y) const;   // y = A x
    void mulT(const std::vector<double>& x, std::vector<double>& y) const;  // y = Aᵀ x

private:
    std::vector<int> m_rowPtr, m_col;
    std::vector<double> m_val;
};

// Solve min ‖A x - b‖² by CG on the normal equations, warm-started at x
// (in/out). b has A.rows entries, x has A.cols.
void solveLeastSquaresCG(const SparseMatrix& A, const std::vector<double>& b,
                         std::vector<double>& x, int maxIters = 400,
                         double tol = 1e-6);

}  // namespace FaceRig

#endif  // FACERIG_SPARSESOLVE_H
