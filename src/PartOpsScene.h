#ifndef PARTOPSSCENE_H
#define PARTOPSSCENE_H

#include "SubMeshOps.h"

#include <OgreMesh.h>
#include <OgreMatrix4.h>

#include <QString>

#include <string>
#include <vector>

namespace Ogre {
class Entity;
class SceneNode;
}

/**
 * @brief Scene-level Ogre adapter for PartOps explode / join (Slice C #862).
 *
 * `SubMeshOps` is the Ogre-buffer-free pure-data core (join baking + explode
 * offset math, unit-tested headless). `PartOpsMesh` builds a single fresh mesh
 * from `EditableSubMesh`es. This layer sits above both and touches the live
 * scene graph: it turns one fused entity's submeshes into N sibling scene
 * nodes (explode), and merges N selected part nodes back into one fused mesh
 * baking their world transforms (join).
 *
 * Both operations are pure builders — they compute the target scene state but
 * do NOT mutate the scene themselves (no node create/destroy). The undoable
 * commands (`ExplodePartsCommand` / `JoinPartsCommand`) own the scene mutation
 * so undo/redo can replay it; keeping the geometry work here makes it testable
 * without an undo stack.
 */
class PartOpsScene
{
public:
    // -------------------------------------------------------------------------
    // Explode: one fused entity -> one mesh per part submesh.
    // -------------------------------------------------------------------------

    /** One exploded part, ready for the command to realise as a node+entity. */
    struct ExplodePart {
        Ogre::MeshPtr mesh;          ///< single-submesh mesh for this part.
        QString name;                ///< part name (submesh name, e.g. "head").
        Ogre::Vector3 offset;        ///< outward explode translation (local to the source node).
    };

    struct ExplodeResult {
        bool ok = false;
        QString error;
        std::vector<ExplodePart> parts;
    };

    /** Split every submesh of `entity` into its own single-submesh
     *  `Ogre::Mesh` (preserving attributes, materials, and — for a skinned
     *  source — the skeleton + bone assignments), and compute an outward
     *  explode offset per part via `SubMeshOps::explodeOffsets` scaled by
     *  `distance` (0 = coincident, parts stacked at the source origin).
     *
     *  Meshes are named `<baseName>_<partName>` (uniquified by Ogre on create).
     *  Does NOT create scene nodes — returns the built meshes + offsets for the
     *  command to attach. Fails (`ok=false`) on a null entity, unreadable
     *  geometry, or a single-submesh mesh (nothing to explode). */
    static ExplodeResult explodeEntity(Ogre::Entity* entity,
                                        float distance,
                                        const std::string& baseName);

    // -------------------------------------------------------------------------
    // Join: N part entities (with world transforms) -> one fused mesh.
    // -------------------------------------------------------------------------

    struct JoinResult {
        bool ok = false;
        QString error;
        Ogre::MeshPtr mesh;          ///< merged mesh (world transforms baked in).
        int createdSubMeshes = 0;
    };

    /** Read each entity's submeshes + its parent node's FULL world transform
     *  into `SubMeshOps::JoinPart`s (positions baked by the world matrix,
     *  normals/tangents by its inverse-transpose), run `SubMeshOps::joinParts`,
     *  and build one merged mesh named `baseName`.
     *
     *  Same-material submeshes across parts coalesce into one output submesh
     *  (via `joinParts`); distinct materials stay separate. Needs >= 2
     *  entities. Skeletons are NOT reconciled (a documented join limitation) —
     *  the merged mesh is static geometry; a skinned input's bone assignments
     *  are dropped with a warning in `error` left empty on success. */
    static JoinResult joinEntities(const std::vector<Ogre::Entity*>& entities,
                                    const std::string& baseName);
};

#endif // PARTOPSSCENE_H
