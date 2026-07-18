#ifndef DEFORMATIONTRANSFER_H
#define DEFORMATIONTRANSFER_H

// Deformation transfer (Sumner & Popović 2004) for face auto-rig (#889,
// Slice D #892). Pure data — no Ogre, reuses FaceRig::SparseMatrix — and
// headless-tested.
//
// Given the NRICP correspondence (template verts fitted onto the USER
// identity, in TEMPLATE topology — from NonRigidICP #891) and the template's
// per-shape neutral→expression delta, transfer each expression's per-triangle
// deformation onto the fitted (user-identity) mesh. The output is the
// expression realized on the user's identity, still in template topology, as
// a per-template-vertex delta from the fitted neutral. FaceRigger (#893)
// resamples that to the real user vertices via the correspondence.
//
// Method: per source triangle build the deformation gradient
//     S = [e1' e2' n'] · [e1 e2 n]⁻¹
// (Sumner's 4th "normal" vertex trick: n = (e1×e2)/√|e1×e2|), where the
// unprimed frame is the template NEUTRAL triangle and the primed is the
// template EXPRESSION triangle. Then solve, over the FITTED mesh, for vertex
// positions whose per-triangle deformation gradient matches S — one sparse
// least-squares (the Sumner-Popović matrix), with the fitted neutral as the
// rest state, plus a small anchor term to pin the solution (deformation
// gradients are translation-free).

#include "SparseSolve.h"

#include <vector>

namespace FaceRig {

// Precomputes the per-triangle rest frames of the FITTED mesh once, so all 52
// shapes transfer without rebuilding the (topology-fixed) system.
class DeformationTransfer {
public:
    // tmplNeutral / faces: the TEMPLATE neutral verts (N*3) + tris (F*3).
    // fitted: template verts fitted onto the user identity (N*3) — the NRICP
    // correspondence; SAME topology as the template.
    bool init(const std::vector<float>& tmplNeutral,
              const std::vector<int>& faces,
              const std::vector<float>& fitted);

    bool valid() const { return m_valid; }
    int vertexCount() const { return m_n; }

    // Transfer one template shape (tmplExprDelta = expr - tmplNeutral, N*3) →
    // a per-vertex delta on the FITTED mesh (N*3, added to `fitted` gives the
    // expression on the user identity). Returns empty on failure.
    std::vector<float> transfer(const std::vector<float>& tmplExprDelta) const;

private:
    bool m_valid = false;
    int m_n = 0;
    std::vector<float> m_tmplNeutral;   // template rest (for source gradients)
    std::vector<int> m_faces;
    std::vector<float> m_fitted;        // target rest (user identity)
    // The transfer system A x = c is fixed by topology + fitted rest; only c
    // (built from each shape's source deformation gradient) changes per shape.
    SparseMatrix m_A;
    // per-triangle inverse rest frames of the TEMPLATE neutral (source) and of
    // the FITTED mesh (target), cached for building c.
    std::vector<std::array<double, 9>> m_srcRestInv;   // 3x3 per tri
    std::vector<std::array<double, 9>> m_tgtRestInv;
    std::vector<std::array<double, 3>> m_tgtNormalV4;   // fitted 4th vertex/tri
    std::vector<int> m_anchors;         // one gauge-anchor vertex per island
};

}  // namespace FaceRig

#endif  // DEFORMATIONTRANSFER_H
