#ifndef PARTOPSMESH_H
#define PARTOPSMESH_H

#include "SubMeshOps.h"

#include <OgreMesh.h>

#include <QString>

#include <cstdint>
#include <vector>

namespace Ogre {
class Entity;
}

/**
 * @brief Ogre adapter for the PartOps core (#859) — the buffer-touching layer.
 *
 * `SubMeshOps` is Ogre-buffer-free (operates on `EditableSubMesh` data) so it
 * can be unit-tested headless. This adapter bridges it to live Ogre meshes:
 * read an entity's geometry into `EditableSubMesh`es (full attributes + bone
 * assignments), run a `SubMeshOps` operation, and build a fresh `Ogre::Mesh`
 * from the result via the existing `EditableMesh::createNewMesh` path.
 *
 * The GLOBAL triangle order this adapter reads (submesh-then-local, shared
 * vertex data first) is identical to `AutoRig::gatherGeometry`, so a
 * `MeshSegmenter::Result::faceLabels` array produced from that geometry maps
 * 1:1 onto the submeshes this reads — the split routes each labelled triangle
 * to the right part with no re-derivation.
 */
class PartOpsMesh
{
public:
    /** Read an entity's mesh into attribute-complete `EditableSubMesh`es, one
     *  per Ogre submesh, in submesh order. Returns false on a null/empty
     *  entity. Uses `EditableMesh::loadFromEntity` under the hood so every
     *  supported attribute (normal/uv/colour/tangent/bone-assignments/n-gon
     *  faces/UV seams) is captured. */
    static bool readSubMeshes(Ogre::Entity* entity,
                              std::vector<EditableSubMesh>& outSubMeshes);

    /** Build a new detached `Ogre::Mesh` from `subMeshes` (one Ogre SubMesh
     *  each), named from `baseName`. Recomputes normals/bounds. Returns null
     *  on empty input. Reuses `EditableMesh::createNewMesh`.
     *
     *  When `skeletonName` is non-empty, the new mesh is bound to that skeleton
     *  and each submesh's `EditableVertex::boneAssignments` are re-added and
     *  compiled (`Mesh::_compileBoneAssignments`), so a split of a SKINNED mesh
     *  keeps working weights — the #861 "skinned fixtures retain valid bone
     *  assignments" criterion. Bone indices are preserved as-is (the split does
     *  not renumber bones), which is valid because every part shares the source
     *  skeleton. */
    static Ogre::MeshPtr buildMesh(const std::vector<EditableSubMesh>& subMeshes,
                                   const std::string& baseName,
                                   const QString& skeletonName = QString(),
                                   const std::vector<QString>& subMeshNames = {},
                                   bool recomputeNormals = false);

    struct SplitOutcome {
        bool ok = false;
        QString error;
        Ogre::MeshPtr mesh;             ///< the split result as a fresh mesh.
        std::vector<QString> partNames; ///< one per created submesh.
        int createdSubMeshes = 0;
        int duplicatedBoundaryVertices = 0;
    };

    /** Full headless split: read `entity`, run `SubMeshOps::splitByFaceGroups`
     *  with `faceLabels` (must match the entity's global triangle count) and
     *  `groups`, and build a new mesh. Does NOT touch the live entity — the
     *  caller exports the returned mesh (CLI) or swaps it onto the entity via
     *  an undo command (GUI). */
    static SplitOutcome splitEntity(Ogre::Entity* entity,
                                    const std::vector<int>& faceLabels,
                                    const std::vector<SubMeshOps::FaceGroup>& groups,
                                    const SubMeshOps::SplitOptions& opts,
                                    const std::string& baseName);

    struct PrintPrepOutcome {
        bool ok = false;
        QString error;
        Ogre::MeshPtr mesh;              ///< the pegged mesh (parts + connectors).
        std::vector<QString> partNames;  ///< one per submesh (unchanged part names).
        int peggedBoundaries = 0;
        int totalPegs = 0;
        std::vector<QString> warnings;   ///< per-boundary skip reasons.
    };

    /** Prepare an already-SPLIT entity (one submesh per part) for 3D printing by
     *  adding alignment pegs (Slice D #863): read its submeshes + their part
     *  names, run `SubMeshOps::preparePrintPegs`, and build a new mesh whose
     *  parts each carry their male-peg / female-socket connector geometry. The
     *  part names round-trip (each submesh keeps its name); connector geometry is
     *  merged INTO the parts (not new submeshes), so the part count is unchanged
     *  and each part stays one printable object. Preserves the source skeleton
     *  (a skinned character's parts stay riggable). Does NOT touch the live
     *  entity — the caller exports the returned mesh or swaps it via an undo
     *  command. Fails (`ok=false`) on a single-submesh mesh (nothing to peg). */
    static PrintPrepOutcome addPrintPegsToEntity(Ogre::Entity* entity,
                                                 const SubMeshOps::PegOptions& opts,
                                                 const std::string& baseName);
};

#endif // PARTOPSMESH_H
