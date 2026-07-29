#ifndef SUBMESHOPS_H
#define SUBMESHOPS_H

#include "EditableMesh.h"

#include <QString>

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Pure-data mesh-authoring core for the PartOps epic (#859).
 *
 * PartOps turns AI mesh segmentation (`MeshSegmenter`) into real authoring
 * operations: split a fused mesh into per-part submeshes, explode those into
 * separate scene nodes, and join them back.
 *
 * Everything here operates on `std::vector<EditableSubMesh>` — the same
 * attribute-complete editable representation `EditableMesh` loads from an
 * Ogre entity (position/normal/uv/colour/tangent/bone-assignments/n-gon
 * faces/UV seams). That makes the geometry math **Ogre-buffer-free and
 * unit-testable**, and lets the Ogre adapter reuse the existing
 * `EditableMesh::createNewMesh` / `buildSubMeshBuffers` path to realise the
 * result — attribute + bone-weight preservation comes for free because
 * splitting merely copies `EditableVertex` values into per-group submeshes.
 *
 * The GUI (`EditModeController`/`PartOpsController`), CLI (`qtmesh segment
 * --split-parts` etc.), and MCP (`split_mesh_by_segments`, …) surfaces are
 * thin adapters over this core.
 */
class SubMeshOps
{
public:
    // -------------------------------------------------------------------------
    // Segmentation grouping (Slice A #860)
    // -------------------------------------------------------------------------

    /** One detected part: a segmentation label plus the faces assigned to it,
     *  addressed as GLOBAL triangle indices (the flat triangle stream across
     *  every submesh, in submesh-then-local order — matching
     *  `MeshSegmenter::Result::faceLabels`). */
    struct FaceGroup {
        int label = 0;                 ///< MeshSegmenter::Part value.
        QString name;                  ///< display / submesh-suffix name (e.g. "head").
        std::vector<uint32_t> triangles; ///< global triangle indices in this group.
        bool excluded = false;         ///< user excluded it from split/explode.
    };

    /** Group a flat per-face label array (one entry per GLOBAL triangle) into
     *  `FaceGroup`s, one per distinct label that occurs. Names come from
     *  `MeshSegmenter::partName`. Order is stable: groups sorted by label
     *  value, so `unknown`(0) is first. Empty labels → empty result. */
    static std::vector<FaceGroup> groupFacesByLabel(const std::vector<int>& faceLabels);

    /** Total triangle count across every submesh (the length a valid
     *  `faceLabels` array must have). */
    static size_t totalTriangleCount(const std::vector<EditableSubMesh>& subMeshes);

    // -------------------------------------------------------------------------
    // Split (Slice B #861)
    // -------------------------------------------------------------------------

    struct SplitOptions {
        /** Prefix for generated submesh names: `<prefix>.<group>` (e.g.
         *  "Body" → "Body.head"). Empty → just the group name. */
        QString namePrefix = QStringLiteral("Body");
        /** When true, split every group's faces further into connected
         *  components so e.g. two disjoint islands sharing a label become two
         *  submeshes (`head`, `head.1`). Off by default: one submesh per label. */
        bool splitDisconnected = false;
        /** Assign a distinct generated material name per part instead of
         *  preserving the source material. The Ogre adapter creates the
         *  materials; the core only records the intended name. */
        bool assignPartMaterials = false;
        /** Close each part's OPEN cut face (the seam left hollow by the split)
         *  with a triangle fan so every part is a watertight solid — otherwise
         *  an exploded part shows a see-through hole where it was cut from its
         *  neighbour. Default OFF so the pure-split algorithm keeps exact vertex/
         *  triangle counts (unit tests, downstream callers that re-cap
         *  themselves); the user-facing split (SplitMeshCommand) and explode/
         *  print-prep turn it ON. */
        bool capParts = false;
    };

    struct SplitResult {
        bool ok = false;
        QString error;
        /** The new submesh layout (replaces the input entirely). */
        std::vector<EditableSubMesh> subMeshes;
        /** Parallel to `subMeshes`: the part name each came from. */
        std::vector<QString> partNames;
        /** Boundary vertices duplicated so parts are independent (diagnostic). */
        int duplicatedBoundaryVertices = 0;
        int createdSubMeshes = 0;
    };

    /** Split `subMeshes` into one new submesh per accepted `FaceGroup`.
     *
     *  `faceLabels` must have `totalTriangleCount(subMeshes)` entries; each
     *  global triangle is routed to the group whose `label` matches, EXCEPT
     *  faces whose group is `excluded` (those are dropped from the output —
     *  the caller decides whether that's intended). A face whose label has no
     *  accepted group is also dropped.
     *
     *  Every vertex referenced by a group's triangles is copied into that
     *  group's submesh with a fresh local index; a vertex shared by two groups
     *  is therefore DUPLICATED (counted in `duplicatedBoundaryVertices`), so
     *  the resulting submeshes are geometrically independent. All
     *  `EditableVertex` attributes (normal/uv/colour/tangent/bone-assignments)
     *  and the source material carry over unchanged; n-gon `faces` are rebuilt
     *  when the source submesh had them, else triangle-only.
     *
     *  Deterministic; never throws. Returns `ok=false` with `error` set on a
     *  size mismatch or when no group survives. */
    static SplitResult splitByFaceGroups(const std::vector<EditableSubMesh>& subMeshes,
                                         const std::vector<int>& faceLabels,
                                         const std::vector<FaceGroup>& groups,
                                         const SplitOptions& opts);
    /** Overload with default options. Separate (not a `= {}` default argument)
     *  because `SplitOptions` has a `QString` NSDMI, which a defaulted
     *  argument would force the compiler to evaluate at this class-definition
     *  scope where the enclosing type is still incomplete (GCC hard error). */
    static SplitResult splitByFaceGroups(const std::vector<EditableSubMesh>& subMeshes,
                                         const std::vector<int>& faceLabels,
                                         const std::vector<FaceGroup>& groups);

    // -------------------------------------------------------------------------
    // Join (Slice C #862)
    // -------------------------------------------------------------------------

    /** One part to join: its submeshes plus a world transform (rows of a 4x4,
     *  applied to positions; the inverse-transpose to normals/tangents). The
     *  Ogre adapter fills `transform` from each exploded node's world matrix. */
    struct JoinPart {
        std::vector<EditableSubMesh> subMeshes;
        Ogre::Matrix4 transform = Ogre::Matrix4::IDENTITY;
    };

    struct JoinResult {
        bool ok = false;
        QString error;
        std::vector<EditableSubMesh> subMeshes; ///< merged layout.
    };

    /** Merge `parts` into one mesh, baking each part's `transform` into its
     *  vertex positions (and rotating normals/tangents by the transform's
     *  linear part). Submeshes that share a material name are concatenated
     *  into one output submesh; distinct materials stay separate. Bone
     *  assignments are preserved as-is (join does not attempt skeleton
     *  reconciliation — documented limitation). Deterministic. */
    static JoinResult joinParts(const std::vector<JoinPart>& parts);

    // -------------------------------------------------------------------------
    // Explode offsets (Slice C #862)
    // -------------------------------------------------------------------------

    /** Compute an outward explode offset per part: the direction from the
     *  whole-assembly centroid to each part's centroid, scaled by `distance`
     *  times the assembly bounding-box diagonal. Degenerate (part centroid ==
     *  assembly centroid) → zero offset. `partCentroids` in, offsets out
     *  (parallel). Pure math, unit-testable. */
    static std::vector<Ogre::Vector3> explodeOffsets(const std::vector<Ogre::Vector3>& partCentroids,
                                                      const Ogre::AxisAlignedBox& assemblyBounds,
                                                      float distance);

    // -------------------------------------------------------------------------
    // Boundary capping (#863) — close the open cut face of a split part
    // -------------------------------------------------------------------------

    /** Cap the OPEN boundary of a split part so it becomes a watertight solid
     *  (a split leaves the cut face as a hole — bad for 3D printing, and a peg
     *  needs a solid face to attach to). Finds every boundary edge (an edge used
     *  by exactly ONE triangle), chains them into closed loops, and fills each
     *  loop with a CENTROID FAN: one new vertex at the loop's centroid + a
     *  triangle per boundary edge, wound so the cap faces OUTWARD (away from the
     *  part's centroid). Copies a representative boundary vertex's attributes
     *  onto the new centroid verts so the cap shares the part's material/uv
     *  space. Edits `sub` in place; returns the number of caps (loops) filled.
     *  Deterministic; pure-data. Skips loops shorter than 3 edges. */
    static int capOpenBoundaries(EditableSubMesh& sub);
};

#endif // SUBMESHOPS_H
