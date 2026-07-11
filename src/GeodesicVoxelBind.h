#ifndef GEODESIC_VOXEL_BIND_H
#define GEODESIC_VOXEL_BIND_H

#include "SkinWeights.h"

#include <QString>
#include <cstdint>
#include <vector>

// Geodesic Voxel Binding (issue #819, Slice A) — the method Maya
// ships as its "Geodesic Voxel" bind mode (Dionne & de Lasa,
// "Geodesic voxel binding for production character meshes",
// SCA 2013). Designed explicitly for broken production meshes:
//
//   1. Voxelize the mesh surface at `voxelResolution` along the
//      longest AABB axis (triangle/box overlap, Akenine-Möller SAT).
//   2. Flood-fill the EXTERIOR from the grid boundary; everything
//      not exterior and not surface is interior. This closes holes
//      at voxel resolution — the paper's trick that makes
//      non-watertight / self-intersecting meshes work.
//   3. Rasterize each bone segment through the grid (3D-DDA) to
//      seed voxels; a bone lying outside the solid snaps to the
//      nearest solid voxel within a small radius, else it gets no
//      seeds (reported).
//   4. One multi-source Dijkstra over solid (interior + surface)
//      voxels, 26-connectivity, carrying the bone id; each voxel
//      keeps its best K=8 (bone, distance) pairs.
//   5. Per-vertex: sample the containing (or nearest) solid voxel;
//      weight = (1/d)^falloff over the voxel's pairs, reusing
//      SkinWeightsOptions falloff / maxInfluencesPerVertex /
//      maxInfluenceDistance semantics unchanged.
//
// Distances travel through the interior volume, so cross-limb bleed
// (hand near thigh, inner thighs, fingers) is impossible by
// construction — the hard quality ceiling of the Euclidean
// inverse-distance heuristic in SkinWeights.
//
// Pure-data and Ogre-free; called from SkinWeights::computeWeights.

class GeodesicVoxelBind {
public:
    // Per-voxel pair budget. 8 keeps memory at 8 × voxels while
    // covering every realistic influence overlap (final weights are
    // pruned to maxInfluencesPerVertex ≤ 8 anyway).
    static constexpr int kPairsPerVoxel = 8;

    struct Result {
        bool ok = false;              // false → caller should fall back
        QString error;                // why (degenerate input, no interior…)
        int gridX = 0, gridY = 0, gridZ = 0;
        int surfaceVoxels  = 0;
        int interiorVoxels = 0;
        // bones[] indices that produced no seed voxel (bone lies
        // outside the solid beyond the snap radius) — they receive
        // no weights anywhere.
        std::vector<int> bonesWithoutSeeds;
        // Vertices that had no geodesic pair at all (e.g. a floating
        // island with no bone inside it). computeWeights fills them
        // via the inverse-distance fallback.
        int verticesWithoutGeodesicWeights = 0;
    };

    // Computes geodesic-voxel weights. `indices` (triangle list,
    // 3 per face, referencing `vertexPositions`) is REQUIRED — the
    // voxelization needs the surface, not a point cloud. Returns a
    // Result with ok=false (and outWeights untouched) when the input
    // is degenerate: no triangles, or zero interior voxels (planes,
    // cloth, billboards). The caller decides the fallback.
    //
    // On success, `outWeights[v]` holds the top-K normalized weights
    // for every vertex. Vertices unreachable from any bone seed keep
    // count == 0 (see Result::verticesWithoutGeodesicWeights) so the
    // caller can fill them in.
    //
    // `outAllowedBones`, when non-null, receives per-vertex the bone
    // set that is geodesically local to it (the voxel's K pairs,
    // BEFORE the maxInfluences cut) — used by the Slice B bleed
    // metric.
    static Result compute(const float* vertexPositions,
                          int vertexCount,
                          const std::uint32_t* indices,
                          std::size_t indexCount,
                          const std::vector<SkinWeights::BoneSegment>& bones,
                          const SkinWeightsOptions& opts,
                          std::vector<SkinWeights::VertexWeights>& outWeights,
                          std::vector<std::vector<int>>* outAllowedBones = nullptr);
};

#endif // GEODESIC_VOXEL_BIND_H
