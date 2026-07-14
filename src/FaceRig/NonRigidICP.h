#ifndef NONRIGIDICP_H
#define NONRIGIDICP_H

// Non-rigid ICP (Amberg, Romdhani & Vetter 2007, "Optimal Step Nonrigid ICP")
// for face auto-rig (#889, Slice C #891). Pure data — no Ogre, no ONNX, no
// external linear-algebra dependency (a self-contained sparse CG solver lives
// in the .cpp) — and headless-unit-tested.
//
// Fits a TEMPLATE mesh (the ICT ARKit head, ArkitTemplate) to a USER neutral
// head of arbitrary topology, producing per-template-vertex positions that lie
// on the user surface: the CORRESPONDENCE the deformation transfer (#892)
// needs. Each template vertex gets a 3x4 affine A_i; we minimize
//
//     Σ_i ‖A_i·ṽ_i − closest_point_on_user(A_i·ṽ_i)‖²      (data)
//   + α Σ_(i,j)∈edges ‖A_i − A_j‖²                          (stiffness)
//
// over an annealed stiffness schedule α (high→low). closest_point is a
// point-to-triangle projection against the user mesh (KD-tree over triangle
// centroids for the broad phase). Contract proven in docs/FACE_RIG_SPIKE.md.

#include <cstdint>
#include <vector>

namespace FaceRig {

struct NricpResult {
    // fitted template vertex positions on the user surface (templateVertexCount
    // * 3, xyz interleaved) — the correspondence.
    std::vector<float> fitted;
    // per-template-vertex residual distance to the user surface (in mesh units)
    std::vector<float> residual;
    double meanResidual = 0.0;   // over all template verts
    double maxResidual = 0.0;
    double diag = 0.0;           // user-mesh bounding-box diagonal (for %)
    bool ok = false;
};

struct NricpOptions {
    // annealed stiffness weights (high = rigid, low = free); ~3 inner iters each
    std::vector<double> stiffness = {50, 20, 8, 3, 1, 0.5};
    int itersPerLevel = 3;
    int cgIters = 400;           // CG cap per axis solve
    double cgTol = 1e-6;
};

// tmplV/tmplF: template neutral verts (Nt*3) + tris (Ft*3).
// userV/userF: user neutral verts (Nu*3) + tris (Fu*3).
// Both assumed roughly aligned in orientation (+Y up); the fit does a
// centroid+bbox-scale rigid pre-align, then the non-rigid warp.
NricpResult fit(const std::vector<float>& tmplV, const std::vector<int>& tmplF,
                const std::vector<float>& userV, const std::vector<int>& userF,
                const NricpOptions& opts = {});

}  // namespace FaceRig

#endif  // NONRIGIDICP_H
