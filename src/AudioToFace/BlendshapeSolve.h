#ifndef AUDIOTOFACE_BLENDSHAPESOLVE_H
#define AUDIOTOFACE_BLENDSHAPESOLVE_H

// Audio2Face (#1019): recover ARKit blendshape WEIGHTS from a predicted mesh
// deformation.
//
// The A2F network does not emit blendshapes — it emits a deformation of
// NVIDIA's own character mesh. What makes that usable is that NVIDIA also
// ships that character's ARKit-52 blendshape deltas, so the weights can be
// recovered by asking: which combination of these 52 shapes best reproduces
// the motion the network predicted?
//
//     minimise  |D w - dv|²  +  l2|w|²  +  l1|w|  +  temporal|w - wPrev|²
//                             +  symmetry * Σ (w[a] - w[b])²
//     subject to 0 <= w <= 1
//
// Solving in the NETWORK's mesh space is the key point: the result is a vector
// of weights, and weights carry no topology. They drive any mesh that has
// ARKit targets — including the ones `qtmesh facerig` generates — so no
// correspondence between NVIDIA's character and the user's mesh is ever
// needed. (The naming lines up too: 51 of our 52 names match NVIDIA's
// verbatim; they spell `_neutral` as `neutral` and add a bonus `tongueOut`.)
//
// Pure data — no Ogre, no ONNX, no Qt — so every rule here is unit-testable
// without a scene or a model file.
//
// The formulation and its default strengths come from NVIDIA's MIT-licensed
// `blendshape_solver.h` and the per-character `bs_skin_config.json`.

#include <cstddef>
#include <vector>

namespace AudioToFace {

/// One blendshape's vertex deltas, flattened xyz (3 * vertexCount).
using PoseDelta = std::vector<float>;

struct SolveOptions {
    // Defaults are NVIDIA's shipped values for the v2.3 "Mark" character.
    double l2 = 0.5;          ///< pulls weights toward zero (ridge)
    double l1 = 0.5;          ///< encourages a SPARSE pose — few shapes at once
    double temporal = 0.3;    ///< penalises change from the previous frame
    double symmetry = 100.0;  ///< ties left/right pairs together; deliberately
                              ///< large, since asymmetric speech is rare and
                              ///< an unbalanced smile reads as a defect
    int maxIterations = 200;
    double tolerance = 1e-7;

    /// Per-pose enable. A disabled pose is held at 0: audio cannot predict
    /// brows, blinks or eye-look, and letting the solver use them to explain
    /// mouth motion produces a face that blinks when it talks.
    std::vector<int> active;
    /// Index of each pose's mirror partner, or -1. Both directions should be
    /// present (a<->b), which is how NVIDIA's table is written.
    std::vector<int> symmetryPartner;
    /// Applied to the solved weight before it is returned.
    std::vector<float> multipliers;
    std::vector<float> offsets;
};

struct SolveResult {
    std::vector<float> weights;   ///< one per pose, clamped to [0,1]
    double residual = 0.0;        ///< |D w - dv| / |dv|, 0 = perfect fit
    int iterations = 0;
    bool converged = false;
};

/// Least-squares fit of `deltaVertices` (flattened xyz) onto `poses`.
///
/// `previous` may be empty on the first frame, which disables the temporal
/// term for that frame rather than dragging the solve toward zero.
///
/// Returns weights sized to `poses.size()`; an empty/degenerate input yields
/// all-zero weights rather than an error, so a silent audio frame simply
/// produces a neutral face.
SolveResult solveBlendshapeWeights(const std::vector<PoseDelta>& poses,
                                   const std::vector<float>& deltaVertices,
                                   const std::vector<float>& previous,
                                   const SolveOptions& options);

/// Build the normal-equation system once for a fixed pose basis.
///
/// The basis never changes across a take, but the target does every frame, so
/// the expensive part — the 52x52 Gram matrix over ~61k vertices — is hoisted
/// out of the per-frame loop. Without this a 10-second clip at 30 fps would
/// redo ~2.8 billion multiply-adds it could have done once.
class PoseBasis {
public:
    /// `poses` must all share a vertex count. Empty input leaves the basis
    /// unusable (`poseCount() == 0`), which the solver treats as "no fit".
    void build(const std::vector<PoseDelta>& poses);

    size_t poseCount() const { return m_poseCount; }
    size_t valueCount() const { return m_valueCount; }
    bool valid() const { return m_poseCount > 0 && m_valueCount > 0; }

    /// Solve for one frame against this basis. Same contract as the free
    /// function above, which delegates here.
    SolveResult solve(const std::vector<float>& deltaVertices,
                      const std::vector<float>& previous,
                      const SolveOptions& options) const;

private:
    size_t m_poseCount = 0;
    size_t m_valueCount = 0;
    std::vector<double> m_gram;          ///< poseCount², row-major: Dᵀ D
    /// Copy, so callers need not outlive us. Held BY VALUE and used directly:
    /// an earlier version also kept a `m_poses` pointer aimed at this vector,
    /// which a copy or move of the basis would leave pointing into the source
    /// object -- a dangling read once the source died. There is no second
    /// storage mode, so the pointer bought nothing.
    std::vector<PoseDelta> m_owned;
};

}  // namespace AudioToFace

#endif  // AUDIOTOFACE_BLENDSHAPESOLVE_H
