#ifndef SKIN_WEIGHTS_H
#define SKIN_WEIGHTS_H

#include <QString>
#include <QJsonObject>
#include <QList>
#include <vector>

namespace Ogre {
    class Entity;
    class Mesh;
    class Skeleton;
}

// Automatic skinning weights for a mesh + skeleton (issue #402,
// epic #397).
//
// The issue title proposes wrapping libigl's bounded biharmonic
// weights (BBW). BBW solves a constrained biharmonic equation over
// the *volume* of the mesh and produces the gold-standard smooth-
// skinning weights used by Blender / Maya / Houdini. The catch:
//
//   1. BBW requires a tetrahedral mesh of the volume — produced by
//      TetGen, which is GPL/copyleft. Linking it forces the entire
//      binary to GPL, which would close the door on Homebrew /
//      Snap / WinGet redistribution under the project's current
//      permissive-license stance.
//   2. TetGen meshing fails on common asset issues (non-manifold,
//      self-intersection, degenerate tris).
//   3. Eigen + libigl headers add ~200 MB to the build tree.
//
// This first slice ships a **surface-based heuristic** with zero
// new dependencies. For each vertex, compute its distance to each
// bone segment (the line connecting the bone to its parent in the
// bind pose), invert it with an exponent (falloff), keep the top K
// bones, and normalize. This is the classic "closest point on
// bone" / "smooth bind" approach Maya and 3dsMax use as their
// default — it's heuristic and not as smooth as BBW, but it works
// out of the box on any character mesh including non-manifold
// FBX imports.
//
// A future slice can plug in libigl BBW behind an opt-in CMake
// flag (`-DENABLE_LIBIGL_BBW`) for users who accept the GPL
// implications, surfaced via `--algo biharmonic`.

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
    // When true, those bones are ignored. Default true.
    bool skipUnweightedBones = false;

    // When true (default), existing bone assignments are REPLACED.
    // When false, new weights are merged with existing weights
    // (existing weights take precedence on conflicting vertices).
    // The merge case is useful for "fill in missing weights"
    // workflows where part of the mesh is already manually skinned.
    bool replaceExisting = true;
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
};

class SkinWeights {
public:
    enum class Algorithm {
        InverseDistance,    // Default — closest-point-on-bone heuristic.
        // Future:
        //   Biharmonic     — libigl BBW (requires TetGen → GPL opt-in)
    };

    // Compute new skin weights for `entity` against its attached
    // skeleton and commit them to the mesh. Replaces (or merges
    // with — see options) the existing bone-assignment list and
    // calls `_compileBoneAssignments` to refresh the hardware
    // blend buffer.
    //
    // The entity MUST have a skeleton attached. Static (skeleton-
    // less) entities return `applied=false` with an error.
    static SkinWeightsReport computeAndApply(Ogre::Entity* entity,
                                             const SkinWeightsOptions& opts = {},
                                             Algorithm algo = Algorithm::InverseDistance);

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

    static QJsonObject reportToJson(const SkinWeightsReport& report);
    static QString     reportToText(const SkinWeightsReport& report);

    static QString algorithmToString(Algorithm algo);
    static Algorithm algorithmFromString(const QString& s);
};

#endif // SKIN_WEIGHTS_H
