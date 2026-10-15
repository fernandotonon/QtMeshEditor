#ifndef REPLACE_ENTITY_MESH_COMMAND_H
#define REPLACE_ENTITY_MESH_COMMAND_H

#include <QString>
#include <QUndoCommand>

#include <OgreMesh.h>

#include <string>

namespace Ogre { class Entity; class SceneNode; }

/**
 * Undoable swap of an entity's whole mesh for a PREBUILT replacement — the
 * QuadriFlow remesh (QuadRemeshEntity) changes vertex count, submesh count
 * and material, which the in-place edit-mode path cannot express.
 *
 * Same swap mechanics as SplitMeshCommand (clear every SelectionSet reference
 * before freeing the entity, recreate it on the same node via
 * Manager::createEntity, reselect the node). Both meshes stay resident for
 * the command's lifetime so redo/undo are instant. The entity is targeted by
 * NAME (node and entity share it), never by a cached pointer.
 */
class ReplaceEntityMeshCommand : public QUndoCommand
{
public:
    ReplaceEntityMeshCommand(std::string entityName, Ogre::MeshPtr replacement,
                             const QString& text, QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool ok() const { return mOk; }

private:
    Ogre::Entity* resolveEntity() const;
    Ogre::Entity* swapTo(const Ogre::MeshPtr& mesh);

    std::string   mEntityName;
    Ogre::MeshPtr mOriginal;
    Ogre::MeshPtr mReplacement;
    bool          mOk = false;
};

#endif // REPLACE_ENTITY_MESH_COMMAND_H
