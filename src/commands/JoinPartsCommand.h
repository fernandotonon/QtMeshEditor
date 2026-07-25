#ifndef JOIN_PARTS_COMMAND_H
#define JOIN_PARTS_COMMAND_H

#include <QUndoCommand>
#include <QString>

#include <OgreMesh.h>
#include <OgreVector3.h>
#include <OgreQuaternion.h>

#include <string>
#include <vector>

namespace Ogre { class Entity; class SceneNode; }

/**
 * Undoable PartOps join (#859/#862): merges the selected part entities into one
 * fused mesh, baking each part's world transform into vertex positions. The
 * inverse of ExplodePartsCommand, but general — it joins ANY set of selected
 * mesh nodes, not just ones from a single explode.
 *
 * redo(): read every selected entity's submeshes + world transform, run
 * `PartOpsScene::joinEntities` (same-material submeshes coalesce), destroy the
 * source part nodes, and create ONE fused node bound to the merged mesh at the
 * origin (positions are already world-baked). undo(): destroy the fused node
 * and recreate each part node with its captured transform + original mesh.
 *
 * Skeletons are not reconciled (documented join limitation) — the merged mesh
 * is static geometry.
 */
class JoinPartsCommand : public QUndoCommand
{
public:
    /** @param entityNames  the part entities to join (== their node names).
     *  @param fusedName     base name for the merged node/entity. */
    JoinPartsCommand(std::vector<std::string> entityNames,
                     QString fusedName,
                     QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool ok() const { return mOk; }
    const QString& error() const { return mError; }
    int createdSubMeshes() const { return mCreatedSubMeshes; }
    const std::string& fusedNodeName() const { return mFusedNodeName; }

private:
    struct SourcePart {
        std::string      name;       ///< original node/entity name.
        std::string      parentName; ///< original parent group node (empty == root).
        Ogre::MeshPtr    mesh;       ///< original mesh (resident for undo).
        Ogre::Vector3    pos = Ogre::Vector3::ZERO;    ///< LOCAL TRS (relative to parent).
        Ogre::Quaternion orient = Ogre::Quaternion::IDENTITY;
        Ogre::Vector3    scale = Ogre::Vector3::UNIT_SCALE;
    };

    void buildOnce();

    std::vector<std::string> mEntityNames;
    QString                  mFusedNameBase;

    std::vector<SourcePart> mSources;   ///< captured for undo (meshes + transforms).
    Ogre::MeshPtr           mFusedMesh; ///< merged mesh (built once).
    std::string             mFusedNodeName; ///< actual (uniquified) fused node name.

    bool mBuilt = false;
    bool mOk = false;
    QString mError;
    int mCreatedSubMeshes = 0;
};

#endif // JOIN_PARTS_COMMAND_H
