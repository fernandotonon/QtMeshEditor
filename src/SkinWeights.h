#ifndef SKIN_WEIGHTS_H
#define SKIN_WEIGHTS_H

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QList>
#include <cstdint>
#include <vector>

namespace Ogre {
    class Entity;
    class Mesh;
    class Skeleton;
}

// Automatic skinning weights for a mesh + skeleton (issue #402,
// epic #397; Skinning v2 upgrade: issue #819).
//
// #402's original escape plan — libigl's bounded biharmonic weights
// (BBW) — stays blocked: BBW needs a tetrahedral volume mesh from
// TetGen, which is GPL/copyleft and would force the whole binary to
// GPL (closing Homebrew / Snap / WinGet redistribution). Issue #819
// supersedes that plan with the two paths that beat BBW in
// production practice without the license problem:
//
//   • SkinTokens (Slice C, the DEFAULT) — SkinTokens/TokenRig ML
//     skin-weight prediction (VAST-AI, MIT code + weights) via
//     ONNX, skeleton-teacher-forced and geodesically localised;
//     falls back to GeodesicVoxel automatically when the models /
//     ONNX build are unavailable. See src/SkinTokensPredictor.*.
//   • GeodesicVoxel (Slice A) — Maya's "Geodesic Voxel" bind
//     (Dionne & de Lasa, SCA 2013). Distances travel through the
//     mesh's interior voxels, so cross-limb bleed (hand near thigh,
//     inner thighs, fingers) is impossible by construction, and
//     voxel-resolution hole closing makes it work on non-watertight
//     / self-intersecting / multi-component production meshes. The
//     ML path's fallback. See src/GeodesicVoxelBind.{h,cpp}.
//
// InverseDistance — the original #402 closest-point-on-bone smooth
// bind — is kept as the fallback for meshes that enclose no volume
// (planes, cloth, billboards) and as an explicit `--algo` choice.
//
// Every algorithm runs through the same Slice-B post-pass pipeline
// (src/SkinWeightsPost.{h,cpp}): Laplacian relaxation over the
// vertex adjacency (merge-mode manual weights act as Dirichlet
// constraints), then prune + renormalize to maxInfluencesPerVertex.

struct SkinWeightsOptions {
    // Number of bones each vertex is allowed to be influenced by.
    // The Ogre/Mixamo / Unity / Unreal hardware-skinning convention
    // is 4. Lower = lighter shading, blockier deformation; higher
    // = more memory and less GPU-friendly.
    int maxInfluencesPerVertex = 4;

    // Inverse-distance exponent. Higher = sharper falloff (vertex
    // gets glued tighter to its single nearest bone). Lower =
    // smoother blending across bone boundaries. Range typically
    // [1.5, 6.0]; default 4.0.
    double falloff = 4.0;

    // Maximum distance ratio: a bone whose distance to the vertex
    // is more than `maxInfluenceDistance * (mesh bounding-box
    // diagonal)` is excluded. Stops a finger bone from getting any
    // weight on a foot vertex. 0 disables the cap.
    double maxInfluenceDistance = 0.5;

    // Skip bones with zero existing vertex weights when reading
    // back results? Mixamo skeletons ship dozens of helper bones
    // (twist bones, IK targets) that aren't actually skinned.
    // When true, those bones are ignored. Default false (consider
    // every bone) — set true on Mixamo-style rigs to avoid spurious
    // weight on helper bones.
    bool skipUnweightedBones = false;

    // When true (default), existing bone assignments are REPLACED.
    // When false, new weights are merged with existing weights
    // (existing weights take precedence on conflicting vertices).
    // The merge case is useful for "fill in missing weights"
    // workflows where part of the mesh is already manually skinned.
    bool replaceExisting = true;

    // GeodesicVoxel only: grid resolution along the longest AABB
    // axis. Higher resolves thinner parts (fingers) at the cost of
    // memory/time. Range [8, 256]; default 64 (≈ Maya's default).
    int voxelResolution = 64;

    // Slice-B post-pass: Laplacian relaxation iterations over the
    // vertex adjacency graph, applied after every algorithm. Kills
    // the geodesic-voxel staircase and inverse-distance banding.
    // 0 disables. Default 3.
    int smoothIterations = 3;
};

struct SkinWeightsSubmeshReport {
    int submeshIndex                = 0;
    int verticesProcessed           = 0;
    int boneAssignmentsBefore       = 0;
    int boneAssignmentsAfter        = 0;
    int verticesWithMaxInfluences   = 0;
};

struct SkinWeightsReport {
    QString meshName;
    QString skeletonName;
    int totalBones                  = 0;
    int totalVerticesProcessed      = 0;
    int totalAssignmentsBefore      = 0;
    int totalAssignmentsAfter       = 0;
    QList<SkinWeightsSubmeshReport> submeshes;
    bool applied                    = false;
    QString error;

    // Which algorithm actually ran ("geodesic-voxel",
    // "inverse-distance", …) and, when it differs from the request,
    // why (degenerate/planar input, model unavailable, …).
    QString algorithmUsed;
    QString fallbackReason;

    // Slice-B bleed metric: fraction of committed (vertex, bone)
    // weight entries whose bone is not geodesically local to the
    // vertex. 0 by construction for geodesic-voxel weights; -1 when
    // not computable (no geodesic field available).
    double bleedFraction = -1.0;

    // Bones that produced no seed voxel (lie outside the solid
    // beyond the snap radius) — they receive no weights anywhere.
    QStringList bonesWithoutSeeds;
};

class SkinWeights {
public:
    enum class Algorithm {
        InverseDistance,   // #402 closest-point-on-bone heuristic
                           // (kept: fallback for volume-less meshes
                           // + explicit choice for planes/cloth).
        GeodesicVoxel,     // #819 Slice A — Maya-style geodesic voxel
                           // binding; the fallback for the ML path and
                           // an explicit choice. Falls back to
                           // InverseDistance on degenerate input.
        SkinTokens,        // #819 Slice C — SkinTokens/TokenRig ML
                           // skinning (the DEFAULT; falls back to
                           // GeodesicVoxel when models/ONNX are
                           // unavailable). "unirig" is kept as a
                           // deprecated string alias.
    };

    // Compute new skin weights for `entity` against its attached
    // skeleton and commit them to the mesh. Replaces (or merges
    // with — see options) the existing bone-assignment list and
    // calls `_compileBoneAssignments` to refresh the hardware
    // blend buffer. The Slice-B post-passes (Laplacian smooth,
    // prune + renormalize) run here after any algorithm.
    //
    // The entity MUST have a skeleton attached. Static (skeleton-
    // less) entities return `applied=false` with an error.
    static SkinWeightsReport computeAndApply(Ogre::Entity* entity,
                                             const SkinWeightsOptions& opts = {},
                                             Algorithm algo = Algorithm::SkinTokens);

    // Pure-data variant: bone segments + vertex positions →
    // sparse weight list (per vertex: K (bone_index, weight)
    // pairs). Used by tests and any future headless caller.
    //
    // `bones` provides one entry per bone giving its world-space
    // head and tail in the bind pose. Leaf / root bones may pass
    // `tail == head` — the algorithm falls back to point-to-point
    // distance in that case.
    struct BoneSegment {
        double headX, headY, headZ;
        double tailX, tailY, tailZ;
    };
    struct VertexWeights {
        int    boneIndices[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
        double weights[8]     = { 0, 0, 0, 0, 0, 0, 0, 0 };
        int    count          = 0;
    };
    static bool computeWeights(const float* vertexPositions,
                               int vertexCount,
                               const std::vector<BoneSegment>& bones,
                               const SkinWeightsOptions& opts,
                               std::vector<VertexWeights>& outWeights);

    // Extra outputs of the algorithm-aware overload below.
    struct ComputeInfo {
        QString algorithmUsed;           // what actually ran
        QString fallbackReason;          // empty when the request ran
        std::vector<int> bonesWithoutSeeds;   // bones[] indices (GVB)
        // Per-vertex geodesically-local bone sets (GVB only) — feed
        // to SkinWeightsPost::bleedFraction.
        std::vector<std::vector<int>> allowedBones;
    };

    // Skeleton hierarchy for the ML (SkinTokens) path — it
    // tokenizes the actual joint tree, which the
    // flat BoneSegment list can't express. Entries must be
    // DFS-ordered (parent before child); `parent` indexes into the
    // same array (-1 = root). Pass via the overload's optional
    // parameter; when absent, SkinTokens falls back to GeodesicVoxel.
    struct SkeletonHierarchy {
        struct Node {
            double x = 0, y = 0, z = 0;
            int parent = -1;
        };
        std::vector<Node> nodes;   // aligned with the bones[] order
    };

    // Algorithm-aware overload (#819). `indices` is the triangle
    // list referencing `vertexPositions` — required by GeodesicVoxel
    // for voxelization (pass nullptr/0 to force the InverseDistance
    // path). GeodesicVoxel falls back to InverseDistance when the
    // mesh encloses no volume (planes, cloth) — never fails on
    // degenerate input; vertices unreachable from any bone seed
    // (floating islands with no bone) are filled in with
    // inverse-distance weights so everything still moves with the
    // rig. SkinTokens needs the joint hierarchy + downloaded models
    // and falls back to GeodesicVoxel otherwise. The Slice-B
    // post-passes are NOT
    // applied here — callers (computeAndApply, tests) run
    // SkinWeightsPost explicitly.
    static bool computeWeights(const float* vertexPositions,
                               int vertexCount,
                               const std::uint32_t* indices,
                               std::size_t indexCount,
                               const std::vector<BoneSegment>& bones,
                               const SkinWeightsOptions& opts,
                               Algorithm algo,
                               std::vector<VertexWeights>& outWeights,
                               ComputeInfo* info = nullptr,
                               const SkeletonHierarchy* hierarchy = nullptr);

    static QJsonObject reportToJson(const SkinWeightsReport& report);
    static QString     reportToText(const SkinWeightsReport& report);

    static QString algorithmToString(Algorithm algo);
    static Algorithm algorithmFromString(const QString& s);
};

#endif // SKIN_WEIGHTS_H
