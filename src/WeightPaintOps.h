#ifndef WEIGHT_PAINT_OPS_H
#define WEIGHT_PAINT_OPS_H

#include "SkinWeights.h"

#include <cstdint>
#include <vector>

/**
 * @brief Skel Slice D (#558) — skin-weight brush + utility ops (Ogre-free).
 *
 * The per-vertex maths behind weight painting: the brush dab (radius / strength
 * / falloff, matching EditModeController's vertex-colour brush so both brushes
 * feel identical), plus the utility ops that are NOT already covered by
 * SkinWeightsPost.
 *
 * Normalize / smooth / limit-weights deliberately live in SkinWeightsPost and
 * are reused rather than reimplemented — see `normalize`, `smooth` and
 * `limitInfluences` here, which are thin wrappers so callers have one place to
 * look and there is one implementation to keep correct.
 *
 * Pure data: inputs are position arrays + VertexWeights, so every op is
 * unit-testable headlessly. Bone identity is the Ogre bone HANDLE throughout
 * (matching SkinEvaluate::extract), never a compacted index.
 */
namespace WeightPaintOps {

/// Which way a dab moves the active bone's weight.
enum class BrushMode {
    Add = 0,     ///< push weight toward 1 on the active bone
    Subtract,    ///< push it toward 0 (influence redistributes on renormalise)
    Blur,        ///< low-pass the active bone's weight toward its 1-ring mean
};

/// Brush shape, mirroring EditModeController::BrushShape so the weight brush
/// honours the same setting the user already set for vertex paint.
enum class BrushShape { Round = 0, Square = 1 };

struct DabOptions {
    double radius = 0.02;      ///< mesh-LOCAL units, same space as vertexPaintRadius
    double strength = 0.5;     ///< 0..1
    double falloff = 0.5;      ///< 0..1, mapped to an exponent (see falloffWeight)
    BrushShape shape = BrushShape::Round;
    BrushMode mode = BrushMode::Add;
    /// Cap applied after every dab. 0 leaves the influence count alone.
    int maxInfluences = 4;
};

/// Per-vertex falloff for a dab, matching EditModeController's vertex-colour
/// brush exactly: exponent = 1 + falloff*5, weight = (1 - d/r)^exponent.
/// Square mode is deliberately flat (constant strength), as it is there.
/// Returns 0 outside the radius.
double falloffWeight(double distance, double radius, double falloff, BrushShape shape);

/// Read the active bone's weight on one vertex (0 when the bone is absent).
double weightOf(const SkinWeights::VertexWeights& vw, int boneHandle);

/// Set the active bone's weight on one vertex, inserting the bone when absent.
/// Returns false only when the row is full (8 influences) and the bone is not
/// already present — a caller may then prune first and retry. Does NOT
/// renormalise; callers batch that so a dab renormalises each row once.
bool setWeight(SkinWeights::VertexWeights& vw, int boneHandle, double weight);

/// Renormalise one row so its weights sum to 1. A row that sums to ~0 is left
/// untouched rather than being given an arbitrary distribution.
void normalizeRow(SkinWeights::VertexWeights& vw);

/// Apply one brush dab centred at `center` (mesh-local).
///
/// `positions` is xyz-per-vertex and sized 3*vertexCount; `weights` is
/// per-vertex and the same length as vertexCount. `lockedBones` holds bone
/// HANDLES whose weight must not change — their value is preserved exactly and
/// the remaining bones absorb the renormalisation, which is what makes
/// "lock bone" work with every brush mode.
///
/// `adjacency` is only needed for BrushMode::Blur (pass the SkinWeightsPost
/// adjacency); an empty adjacency turns Blur into a no-op rather than silently
/// behaving like Add.
///
/// Returns the number of vertices actually modified.
int applyDab(const float* positions, int vertexCount,
             std::vector<SkinWeights::VertexWeights>& weights,
             const double center[3],
             int activeBoneHandle,
             const DabOptions& options,
             const std::vector<std::uint8_t>& lockedBones = {},
             const std::vector<std::vector<int>>& adjacency = {});

/// Flood-fill from `seedVertex` across `adjacency`, setting the active bone's
/// weight with a falloff by GEODESIC hop distance (not straight-line), so the
/// fill follows the surface and does not leak across a gap that happens to be
/// spatially close. `maxHops` bounds the spread; 0 means unbounded.
/// Returns the number of vertices modified.
int fillConnected(std::vector<SkinWeights::VertexWeights>& weights,
                  const std::vector<std::vector<int>>& adjacency,
                  int seedVertex,
                  int activeBoneHandle,
                  double strength,
                  double falloff,
                  int maxHops = 0,
                  const std::vector<std::uint8_t>& lockedBones = {});

// --- utility ops -----------------------------------------------------------

/// Every vertex's weights sum to 1. Thin wrapper over
/// SkinWeightsPost::pruneAndRenormalize with a zero threshold so it only
/// renormalises and never drops influences.
void normalize(std::vector<SkinWeights::VertexWeights>& weights);

/// Laplacian smoothing over the mesh adjacency. Wrapper over
/// SkinWeightsPost::laplacianSmooth; `lockedVertices` are Dirichlet
/// constraints (unchanged, but still influencing neighbours).
void smooth(std::vector<SkinWeights::VertexWeights>& weights,
            const std::vector<std::vector<int>>& adjacency,
            int iterations,
            const std::vector<std::uint8_t>& lockedVertices = {});

/// Clamp every vertex to at most `maxInfluences`, keeping the LARGEST weights
/// and renormalising. Wrapper over SkinWeightsPost::pruneAndRenormalize.
void limitInfluences(std::vector<SkinWeights::VertexWeights>& weights,
                     int maxInfluences);

/// Mirror weights across a world axis by position.
///
/// Slice E (#559) owns the `_l`/`_r` bone-naming convention and has not landed,
/// so this is the issue's documented fallback: for each vertex, find the vertex
/// nearest to its reflection across `axis` (0=X, 1=Y, 2=Z) about `pivot` and
/// copy that vertex's weights onto it. `tolerance` is the maximum accepted
/// distance to the reflected position — beyond it the vertex is left alone
/// rather than given a wrong partner's weights.
///
/// NOTE this mirrors weights BETWEEN VERTICES, not between bone pairs: a vertex
/// on the left gets the weights of its right-hand counterpart, still referring
/// to the same bone handles. On a skeleton with distinct left/right bones that
/// is only correct once bone pairs are also swapped, which needs Slice E. It IS
/// correct for a symmetric mesh weighted to shared/centre bones, and it is what
/// the acceptance criterion asks for.
/// Returns the number of vertices modified.
int mirrorByPosition(const float* positions, int vertexCount,
                     std::vector<SkinWeights::VertexWeights>& weights,
                     int axis,
                     double pivot,
                     double tolerance,
                     const std::vector<std::uint8_t>& lockedBones = {});

/// Sum of one bone's weight across every vertex — cheap way for a test or a
/// readout to show a paint op actually changed something.
double totalWeightOnBone(const std::vector<SkinWeights::VertexWeights>& weights,
                         int boneHandle);

} // namespace WeightPaintOps

#endif // WEIGHT_PAINT_OPS_H
