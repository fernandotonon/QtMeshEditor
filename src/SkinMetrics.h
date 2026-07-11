#ifndef SKIN_METRICS_H
#define SKIN_METRICS_H

#include "SkinWeights.h"

#include <array>
#include <cstdint>
#include <vector>

// Skin-quality metrics (issue #819, Slice E). Pure-data, Ogre-free —
// shared by the acceptance-suite tests, `qtmesh skin --evaluate`,
// and the Mixamo comparison protocol (docs/SKINNING_QUALITY.md).
//
// Metrics:
//   • Influence histogram — vertices per influence count (0..8),
//     plus the average. Hardware-skinning budgets care about the
//     max; Mixamo-style rigs sit at ≤4.
//   • Weight smoothness — Laplacian energy of the weight fields:
//     mean over mesh edges of ||w_u − w_v||² (weights as sparse
//     vectors over bones). Lower = smoother falloffs; hard 0/1
//     borders (rigid binds) score high.
//   • Volume preservation — deform with linear-blend skinning under
//     a given set of per-bone transforms (e.g. a 90° elbow bend)
//     and compare the signed mesh volume against rest. LBS collapse
//     at joints pulls the ratio below 1.
//
// Cross-limb bleed lives in SkinWeightsPost::bleedFraction (Slice B)
// — pair it with GeodesicVoxelBind's allowed-bone sets.

class SkinMetrics {
public:
    struct InfluenceHistogram {
        // counts[k] = vertices with exactly k influences (0..8).
        std::array<int, 9> counts {};
        double averageInfluences = 0.0;
        int    maxInfluences     = 0;
    };
    static InfluenceHistogram influenceHistogram(
        const std::vector<SkinWeights::VertexWeights>& weights);

    // Mean over undirected edges of the squared L2 distance between
    // the two endpoint weight vectors. 0 when every edge joins
    // identically-weighted vertices; 2 is the theoretical max (two
    // disjoint one-bone binds). Returns -1 with no edges.
    static double laplacianEnergy(
        const std::vector<SkinWeights::VertexWeights>& weights,
        const std::vector<std::vector<int>>& adjacency);

    // Row-major 3x4 affine transform (rotation | translation).
    using Transform = std::array<double, 12>;
    static Transform identityTransform();
    // Rotation of `angleRad` about `axis` (unit) through `pivot`.
    static Transform rotationAbout(const double axis[3],
                                   const double pivot[3],
                                   double angleRad);

    // Linear-blend skinning: out[v] = Σ_k w_k · (M_{bone_k} · p_v).
    // `boneTransforms[b]` maps rest → deformed for bone b. Vertices
    // with no influences stay put.
    static std::vector<float> deformLBS(
        const float* positions, int vertexCount,
        const std::vector<SkinWeights::VertexWeights>& weights,
        const std::vector<Transform>& boneTransforms);

    // Signed volume via the divergence theorem (sum of origin-tet
    // volumes). Meaningful for closed meshes; the fixtures are.
    static double meshVolume(const float* positions, int vertexCount,
                             const std::uint32_t* indices,
                             std::size_t indexCount);
};

#endif // SKIN_METRICS_H
