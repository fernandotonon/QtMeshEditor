#ifndef MESH_WELD_OPS_H
#define MESH_WELD_OPS_H

#include <QString>

namespace Ogre { class Entity; }

// Co-located-vertex analysis + weld (validation flow, degenerate-triangles
// pattern). Generated/baked meshes routinely carry vertices that share a
// POSITION but are not connected: xatlas UV-seam splits, marching-cubes
// bakes, imported duplicates. Statically they render fine — but once the
// mesh is SKINNED, twins with mismatched bone weights move apart and the
// triangles visibly tear during animation.
//
// Two complementary fixes, applied together by apply():
//   1. WELD identical duplicates — vertices whose ENTIRE vertex record
//      (position + normal + UV + every other attribute) is byte-identical
//      are true duplicates; their indices are remapped to one
//      representative. UV seams survive (seam twins differ in UV, so they
//      are never welded). Orphaned vertices stay in the buffer (no
//      compaction — index remap only, cheap and non-destructive).
//   2. UNIFY skin weights across the remaining co-located twins — same
//      position ⇒ same bone assignments (the representative's), so
//      animation can never separate them. This is the fix for the
//      "triangles separate when animating" report; UVs/normals untouched.
//
// analyze() runs the same walk without mutating, for the validator rows.
// NOT safe to call while the entity's vertex buffers are locked elsewhere.
class MeshWeldOps {
public:
    struct Report {
        int duplicateVertices = 0;       ///< vertices sharing a position with ≥1 other
        int duplicateClusters = 0;       ///< distinct co-located position clusters
        int weldableVertices = 0;        ///< byte-identical duplicates (weld candidates)
        int weightMismatchClusters = 0;  ///< clusters whose members' weights differ
        int weldedVertices = 0;          ///< apply(): indices remapped away
        int weightsUnified = 0;          ///< apply(): vertices whose assignments were rewritten
        bool skinned = false;
        QString error;
        bool ok = false;
    };

    /// epsilon <= 0 → auto: 1e-5 × bounding-box diagonal.
    static Report analyze(Ogre::Entity* entity, float epsilon = 0.0f);
    static Report apply(Ogre::Entity* entity, float epsilon = 0.0f);

private:
    static Report run(Ogre::Entity* entity, float epsilon, bool mutate);
};

#endif // MESH_WELD_OPS_H
