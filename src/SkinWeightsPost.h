#ifndef SKIN_WEIGHTS_POST_H
#define SKIN_WEIGHTS_POST_H

#include "SkinWeights.h"

#include <cstdint>
#include <vector>

// Weight post-pass pipeline (issue #819, Slice B). Runs inside
// SkinWeights::computeAndApply after ANY algorithm:
//
//   1. Laplacian relaxation — per-bone weight fields smoothed over
//      the vertex adjacency graph (uniform weights), then
//      re-normalized. Kills the geodesic-voxel staircase and the
//      inverse-distance banding. Locked vertices (pre-existing
//      manual weights in merge mode) act as Dirichlet constraints:
//      they contribute to their neighbours but are never modified.
//   2. Prune + renormalize — drop weights < threshold, keep the top
//      maxInfluencesPerVertex, renormalize to partition of unity.
//   3. Bleed metric (no mutation) — fraction of committed
//      (vertex, bone) weight entries whose bone is NOT geodesically
//      local to the vertex (per GeodesicVoxelBind's allowed-bone
//      sets). Zero by construction for geodesic-voxel weights;
//      meaningful as a report metric for the other algorithms.
//
// Pure-data and Ogre-free.

class SkinWeightsPost {
public:
    // Undirected vertex adjacency built from a triangle index list.
    // adjacency[v] = sorted unique neighbour vertex ids.
    static std::vector<std::vector<int>> buildAdjacency(
        int vertexCount,
        const std::uint32_t* indices,
        std::size_t indexCount);

    // Laplacian relaxation of the weight fields. `locked[v] != 0`
    // marks Dirichlet-constrained vertices (kept bit-identical).
    // Each iteration blends every free vertex halfway toward the
    // average of its neighbours, then renormalizes. No-op when
    // iterations <= 0 or the adjacency is empty.
    static void laplacianSmooth(std::vector<SkinWeights::VertexWeights>& weights,
                                const std::vector<std::vector<int>>& adjacency,
                                int iterations,
                                const std::vector<std::uint8_t>& locked = {});

    // Drop weights < `threshold`, keep the top `maxInfluences`
    // (clamped to [1, 8]), renormalize. The largest weight always
    // survives so no vertex is left with zero influences.
    static void pruneAndRenormalize(std::vector<SkinWeights::VertexWeights>& weights,
                                    int maxInfluences,
                                    double threshold = 0.01);

    // Fraction of (vertex, bone) weight entries whose bone is not in
    // the vertex's allowed set. `allowedBones[v]` comes from
    // GeodesicVoxelBind::compute. Returns -1 when the metric can't
    // be computed (size mismatch / empty). Skips vertices with an
    // empty allowed set (no geodesic data for them).
    static double bleedFraction(const std::vector<SkinWeights::VertexWeights>& weights,
                                const std::vector<std::vector<int>>& allowedBones);
};

#endif // SKIN_WEIGHTS_POST_H
