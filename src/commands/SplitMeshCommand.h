#ifndef SPLIT_MESH_COMMAND_H
#define SPLIT_MESH_COMMAND_H

#include <QUndoCommand>
#include <QString>

#include <OgreMesh.h>

#include <string>
#include <vector>

namespace Ogre { class Entity; class SceneNode; }

/**
 * Undoable PartOps split (#859/#861): replaces a fused entity's mesh with a
 * split mesh whose submeshes are the detected parts (head/torso/…).
 *
 * A split changes the submesh COUNT, which the in-place edit-mode path
 * (`EditMeshTopologyCommand` → `resizeEntityBuffers`) forbids — it only ever
 * rewrites existing SubMesh buffers, never adds/removes SubMeshes. So this
 * command swaps the whole mesh instead: it recreates the entity on the same
 * scene node with a new `Ogre::MeshPtr`.
 *
 * redo(): first call runs segmentation + `PartOpsMesh::splitEntity` to build
 * the split mesh (cached on the command so later redos are instant), then
 * destroys the current entity and creates a new one bound to the split mesh on
 * the same node. undo(): recreates the entity on the node bound to the ORIGINAL
 * mesh (kept resident in MeshManager for the command's lifetime).
 *
 * Node and entity share a name (Manager::createEntity), so the command targets
 * by that name and survives scene rebuilds like the other entity-scoped
 * commands. Runs in Object mode (Edit Mode must be exited first — the caller
 * guarantees this).
 */
class SplitMeshCommand : public QUndoCommand
{
public:
    /** @param entityName  the fused entity to split (== its node name).
     *  @param upAxis       0=X,1=Y,2=Z — forwarded to segmentation.
     *  @param category     MeshSegmenter category id ("auto"/"body"/…).
     *  @param noModel      force the offline geometric/rig-prior segmentation.
     *  @param namePrefix   submesh name prefix ("Body" → "Body.head" material).
     *  @param solidify     give each part real wall volume (thin-shell assets). */
    SplitMeshCommand(std::string entityName,
                     int upAxis,
                     QString category,
                     bool noModel,
                     QString namePrefix,
                     bool solidify = false,
                     QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool ok() const { return mOk; }
    const QString& error() const { return mError; }
    int createdSubMeshes() const { return mCreatedSubMeshes; }
    const std::vector<QString>& partNames() const { return mPartNames; }

private:
    Ogre::Entity* resolveEntity() const;
    // Swap the node's entity to `mesh`; returns the new entity (or null).
    Ogre::Entity* swapEntityMesh(const Ogre::MeshPtr& mesh);

    std::string mEntityName;
    int         mUpAxis = 1;
    QString     mCategory;
    bool        mNoModel = false;
    QString     mNamePrefix;
    bool        mSolidify = false;

    Ogre::SceneNode* mReselectNode = nullptr; ///< transient: node to reselect after a swap.
    Ogre::MeshPtr mOriginalMesh; ///< kept resident so undo can restore it.
    Ogre::MeshPtr mSplitMesh;    ///< built once on first redo.
    bool          mBuilt = false;
    bool          mOk = false;
    QString       mError;
    int           mCreatedSubMeshes = 0;
    std::vector<QString> mPartNames;
};

#endif // SPLIT_MESH_COMMAND_H
