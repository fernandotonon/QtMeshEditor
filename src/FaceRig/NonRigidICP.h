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

#include <array>
#include <cstdint>
#include <functional>
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

// A landmark correspondence: template vertex `tmplVertex` should map onto the
// user-space position `target`. Feeding a handful of these (from facial-landmark
// detection) ANCHORS the fit so it can't converge to a low-residual but
// mis-oriented/mis-scaled drape — the fix for the ARKit template landing on the
// wrong face features. (#889)
struct NricpLandmark {
    int tmplVertex = -1;
    std::array<float, 3> target{0, 0, 0};
};

struct NricpOptions {
    // annealed stiffness weights (high = rigid, low = free); ~3 inner iters each
    std::vector<double> stiffness = {50, 20, 8, 3, 1, 0.5};
    int itersPerLevel = 3;
    int cgIters = 400;           // CG cap per axis solve
    double cgTol = 1e-6;
    // Optional landmark anchors (template vertex → user position). Added as
    // high-weight data rows; the weight is strongest at the rigid (high-alpha)
    // levels so orientation/scale lock first, then relaxes as the fit refines.
    std::vector<NricpLandmark> landmarks;
    // Base anchor weight (× alpha at each level). High enough that the marked
    // features stay PINNED through the fine anneal levels — at 10 the anchors
    // loosened once alpha dropped below 1 and the fitted lip line drifted a few
    // mm below the marked lips (field-observed offset).
    double landmarkWeight = 30.0;
};

// Progress callback for the annealing loop: (level, levelCount). Return false
// to abort the fit early (fit() then returns the best-so-far with ok=false).
using NricpProgressFn = std::function<bool(int level, int levelCount)>;

// tmplV/tmplF: template neutral verts (Nt*3) + tris (Ft*3).
// userV/userF: user neutral verts (Nu*3) + tris (Fu*3).
// Both assumed roughly aligned in orientation (+Y up); the fit does a
// centroid+bbox-scale rigid pre-align, then the non-rigid warp.
// `progress` (optional) fires once per completed stiffness level.
NricpResult fit(const std::vector<float>& tmplV, const std::vector<int>& tmplF,
                const std::vector<float>& userV, const std::vector<int>& userF,
                const NricpOptions& opts = {},
                const NricpProgressFn& progress = {});

}  // namespace FaceRig

#endif  // NONRIGIDICP_H
